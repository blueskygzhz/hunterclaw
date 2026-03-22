#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/logging.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), run_subagent_* hooks
#include <spdlog/spdlog.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <sstream>
#include <iomanip>
#include <atomic>

namespace hunterclaw {
namespace agents {

// Helper for logging
inline auto log() { return core::get_logger("subagent"); }

// Type aliases for forward-declared types
using AgentPtr = std::shared_ptr<Agent>;

// ============================================================================
// SubagentRegistry Implementation
// ============================================================================

SubagentRegistry::Ptr SubagentRegistry::create() {
    return std::make_shared<SubagentRegistry>();
}

core::Result<void> SubagentRegistry::register_subagent(const SubagentEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (entry.subagent_run_id.empty()) {
        return core::Result<void>::Err("Subagent run ID cannot be empty");
    }
    
    if (entries_.count(entry.subagent_run_id)) {
        return core::Result<void>::Err(
            "Subagent already registered: " + entry.subagent_run_id
        );
    }
    
    entries_[entry.subagent_run_id] = std::make_shared<SubagentEntry>(entry);
    
    std::cout << "[SubagentRegistry] Registered subagent: " 
              << entry.subagent_run_id 
              << " (requester: " << entry.requester_run_id 
              << ", depth: " << entry.depth_info.depth << ")" << std::endl;
    
    return core::Result<void>::Ok();
}

std::shared_ptr<SubagentEntry> SubagentRegistry::get_entry(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(subagent_run_id);
    return it != entries_.end() ? it->second : nullptr;
}

core::Result<void> SubagentRegistry::update_state(
    const std::string& subagent_run_id,
    SubagentState new_state
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->state = new_state;
    
    // Update timestamps
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    if (new_state == SubagentState::Running && it->second->started_at == 0) {
        it->second->started_at = now;
    } else if (new_state == SubagentState::Completed || 
               new_state == SubagentState::Failed ||
               new_state == SubagentState::TimedOut ||
               new_state == SubagentState::Killed) {
        it->second->completed_at = now;
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::set_result(
    const std::string& subagent_run_id,
    const std::string& result_text,
    bool has_error,
    const std::string& error_message
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->result_text = result_text;
    it->second->has_error = has_error;
    it->second->error_message = error_message;
    
    if (has_error) {
        it->second->state = SubagentState::Failed;
    } else {
        it->second->state = SubagentState::Completing;
    }
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    it->second->completed_at = now;
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::mark_delivered(const std::string& subagent_run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->result_delivered = true;
    if (it->second->state == SubagentState::Announcing) {
        it->second->state = SubagentState::Completed;
    }
    
    return core::Result<void>::Ok();
}

core::Result<void> SubagentRegistry::record_delivery_attempt(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    it->second->delivery_attempts++;
    it->second->last_delivery_attempt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    return core::Result<void>::Ok();
}

std::vector<std::shared_ptr<SubagentEntry>> SubagentRegistry::list_by_requester(
    const std::string& requester_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<SubagentEntry>> result;
    for (const auto& pair : entries_) {
        if (pair.second->requester_run_id == requester_run_id) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<SubagentEntry>> SubagentRegistry::list_active() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::shared_ptr<SubagentEntry>> result;
    for (const auto& pair : entries_) {
        auto state = pair.second->state;
        if (state != SubagentState::Completed && 
            state != SubagentState::Failed &&
            state != SubagentState::Killed) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

SubagentDepthInfo SubagentRegistry::get_depth_info(const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(run_id);
    if (it != entries_.end()) {
        return it->second->depth_info;
    }
    
    // Return default (top-level)
    SubagentDepthInfo info;
    info.root_run_id = run_id;
    return info;
}

void SubagentRegistry::cleanup_old_entries(int max_age_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::vector<std::string> to_remove;
    for (const auto& pair : entries_) {
        const auto& entry = pair.second;
        
        // Only cleanup completed/failed/killed entries
        if (entry->state == SubagentState::Completed ||
            entry->state == SubagentState::Failed ||
            entry->state == SubagentState::Killed) {
            
            double age = (now - entry->spawned_at) / 1000.0;
            if (age > max_age_seconds) {
                to_remove.push_back(pair.first);
            }
        }
    }
    
    for (const auto& run_id : to_remove) {
        entries_.erase(run_id);
    }
    
    if (!to_remove.empty()) {
        std::cout << "[SubagentRegistry] Cleaned up " << to_remove.size() 
                  << " old entries" << std::endl;
    }
}

void SubagentRegistry::find_descendants_recursive(
    const std::string& run_id,
    std::vector<std::string>& descendants
) {
    for (const auto& pair : entries_) {
        if (pair.second->requester_run_id == run_id) {
            descendants.push_back(pair.first);
            // Recursively find children of this child
            find_descendants_recursive(pair.first, descendants);
        }
    }
}

core::Result<void> SubagentRegistry::kill_subagent_tree(
    const std::string& subagent_run_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entries_.find(subagent_run_id);
    if (it == entries_.end()) {
        return core::Result<void>::Err("Subagent not found: " + subagent_run_id);
    }
    
    // Find all descendants
    std::vector<std::string> descendants;
    find_descendants_recursive(subagent_run_id, descendants);
    
    // Kill the subagent itself
    it->second->state = SubagentState::Killed;
    
    // Kill all descendants
    for (const auto& desc_id : descendants) {
        auto desc_it = entries_.find(desc_id);
        if (desc_it != entries_.end()) {
            desc_it->second->state = SubagentState::Killed;
        }
    }
    
    std::cout << "[SubagentRegistry] Killed subagent tree: " 
              << subagent_run_id << " (" << descendants.size() 
              << " descendants)" << std::endl;
    
    return core::Result<void>::Ok();
}

// ============================================================================
// SubagentSpawner Implementation
// ============================================================================

SubagentSpawner::Ptr SubagentSpawner::create(
    std::shared_ptr<AgentManager> agent_mgr,
    std::shared_ptr<SubagentRegistry> registry
) {
    return std::make_shared<SubagentSpawner>(agent_mgr, registry);
}

SubagentSpawner::SubagentSpawner(
    std::shared_ptr<AgentManager> agent_mgr,
    std::shared_ptr<SubagentRegistry> registry
)
    : agent_manager_(agent_mgr)
    , registry_(registry)
{
}

core::Result<void> SubagentSpawner::validate_spawn_request(
    const std::string& requester_run_id,
    const SubagentSpawnOptions& options
) {
    // Check basic fields
    if (options.subagent_name.empty()) {
        return core::Result<void>::Err("Subagent name cannot be empty");
    }
    
    if (options.prompt.empty()) {
        return core::Result<void>::Err("Subagent prompt cannot be empty");
    }
    
    // Check depth
    auto depth_info = registry_->get_depth_info(requester_run_id);
    if (!depth_info.can_spawn_child()) {
        return core::Result<void>::Err(
            "Maximum subagent nesting depth reached: " + 
            std::to_string(depth_info.max_depth)
        );
    }
    
    return core::Result<void>::Ok();
}

AgentConfig SubagentSpawner::build_subagent_config(
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options,
    const SubagentDepthInfo& depth_info
) {
    // ✅ 严格对齐 OpenClaw: 从 requester agent 继承配置
    AgentConfig config;
    
    // TODO: 当 AgentManager 实现后,从父 Agent 继承配置
    // auto requester_agent = agent_manager_->get_agent(requester_agent_id);
    // if (requester_agent) {
    //     config = requester_agent->get_config();  // 继承父配置
    // }
    
    // ✅ 生成唯一的 subagent ID
    config.id = options.subagent_name + "_" + 
                depth_info.root_run_id.substr(4, 8) + "_" +  // 使用 root run ID 的部分
                "d" + std::to_string(depth_info.depth);
    config.name = options.subagent_name;
    
    // ✅ 应用选项覆盖 (OpenClaw 风格: options 优先级最高)
    if (!options.model.empty()) {
        config.model.model = options.model;
    }
    
    if (!options.thinking_level.empty()) {
        config.thinking_level = options.thinking_level;
    }
    
    if (options.timeout_seconds > 0) {
        config.timeout_seconds = options.timeout_seconds;
    } else {
        // 使用父配置的 subagent 默认超时
        config.timeout_seconds = 900;  // 15 分钟默认
    }
    
    // ✅ 工具限制 (对齐 OpenClaw: subagent 不能使用某些危险工具)
    // OpenClaw 中 subagent 被限制使用:
    // - task (禁止嵌套 subagent,除非 depth 允许)
    // - gateway/deployment 相关工具
    // - team management 工具
    // 
    // 注意: AgentConfig 的 skills 字段可以用来限制工具
    if (depth_info.depth > 0) {
        // Subagents 有限制的工具访问权限
        // 通过 skills 过滤器排除危险工具
        std::vector<std::string> restricted_skills = {
            "!task",           // 禁止嵌套 task (除非 depth < max_depth)
            "!team_*",         // 禁止 team 管理
            "!invoke_integration",  // 禁止集成调用
            "!gateway_*",      // 禁止 gateway 操作
            "!deployment_*"    // 禁止部署操作
        };
        
        // 合并父 Agent 的 skills 和 subagent 限制
        config.skills = restricted_skills;
        
        // 如果 depth 达到 max_depth,完全禁止 task
        if (!depth_info.can_spawn_child()) {
            config.subagents.enabled = false;
        } else {
            config.subagents.enabled = true;
            config.subagents.max_depth = depth_info.max_depth;
        }
    }
    
    // ✅ 设置 subagent 的深度信息 (用于日志和调试)
    config.extra["subagent_depth"] = core::JsonValue(static_cast<int64_t>(depth_info.depth));
    config.extra["subagent_root_run_id"] = core::JsonValue(depth_info.root_run_id);
    
    return config;
}

void SubagentSpawner::setup_event_handlers(
    AgentPtr subagent,
    const std::string& subagent_run_id
) {
    // ✅ 严格对齐 OpenClaw: 注册事件处理器监听 subagent 生命周期
    // 
    // OpenClaw 中通过事件流 (EventStream) 监听 Agent 状态变化:
    // - Lifecycle 事件: Start, End, Error
    // - Assistant 事件: 消息和响应
    // - Tool 事件: 工具调用
    // 
    // HunterClaw 对齐实现:
    // 当 Agent 实现 on_event() 和 AgentEvent 后,启用以下代码
    
    // TODO: 当 Agent::on_event 和 AgentEvent 实现后启用
    // 对齐 OpenClaw src/subagents/spawner.ts:setup_event_handlers()
    /*
    subagent->on_event([this, subagent_run_id](const AgentEvent& event) {
        // ========== 生命周期事件 ==========
        if (event.stream == AgentEventStream::Lifecycle) {
            auto data = event.data;
            if (data.is_object() && data.contains("phase")) {
                int phase_int = static_cast<int>(data["phase"].as_number());
                auto phase = static_cast<AgentLifecyclePhase>(phase_int);
                
                switch (phase) {
                    case AgentLifecyclePhase::Start:
                        // Subagent 开始执行
                        registry_->update_state(
                            subagent_run_id, 
                            SubagentState::Running
                        );
                        log()->info("[SubagentSpawner] Subagent started: {}", 
                                   subagent_run_id);
                        break;
                        
                    case AgentLifecyclePhase::End: {
                        // Subagent 执行完成,提取结果
                        std::string result_text;
                        bool has_error = false;
                        std::string error_msg;
                        
                        // 从事件数据提取响应
                        if (data.contains("response")) {
                            result_text = data["response"].as_string();
                        }
                        
                        // 检查是否有错误
                        if (data.contains("error")) {
                            has_error = true;
                            error_msg = data["error"].as_string();
                        }
                        
                        // 保存结果到 Registry
                        registry_->set_result(
                            subagent_run_id,
                            result_text,
                            has_error,
                            error_msg
                        );
                        
                        log()->info("[SubagentSpawner] Subagent completed: {} (error={})",
                                   subagent_run_id, has_error);
                        
                        // 通知等待线程
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            completion_flags_[subagent_run_id] = true;
                        }
                        cv_.notify_all();
                        break;
                    }
                    
                    case AgentLifecyclePhase::Error: {
                        // Subagent 遇到错误
                        std::string error_msg = "Unknown error";
                        if (data.contains("error")) {
                            error_msg = data["error"].as_string();
                        }
                        
                        registry_->set_result(
                            subagent_run_id,
                            "",
                            true,
                            error_msg
                        );
                        
                        log()->error("[SubagentSpawner] Subagent error: {} - {}",
                                    subagent_run_id, error_msg);
                        
                        // 通知等待线程
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            completion_flags_[subagent_run_id] = true;
                        }
                        cv_.notify_all();
                        break;
                    }
                        
                    default:
                        break;
                }
            }
        }
        
        // ========== 超时检测 ==========
        // OpenClaw 中通过定时器检查超时
        // HunterClaw 可以通过 event timestamp 和 registry 的 timeout_seconds 检查
    });
    */
    
    // ✅ 当前实现: 暂时跳过事件处理器设置
    // Subagent 状态更新将由 SubagentAnnouncer 通过轮询实现
    log()->debug("[SubagentSpawner] Event handler setup skipped (not yet implemented)");
}

core::Result<std::string> SubagentSpawner::spawn(
    const std::string& requester_run_id,
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options
) {
    // ✅ 验证请求
    auto validate_result = validate_spawn_request(requester_run_id, options);
    if (validate_result.is_err()) {
        return core::Result<std::string>::Err(validate_result.error);
    }
    
    // ✅ 生成 run ID (UUID-based,对齐 OpenClaw)
    std::string subagent_run_id = "run_" + core::utils::generate_uuid();
    
    // ✅ 严格对齐 OpenClaw: 完整的 subagent 生成流程
    // 
    // OpenClaw 流程:
    // 1. 获取父 Agent 的 depth info
    // 2. 构建子 Agent 配置 (继承 + 覆盖)
    // 3. 注册到 AgentManager (临时 Agent)
    // 4. 创建 Registry 条目
    // 5. 设置事件处理器
    // 6. 异步启动 Agent
    // 7. 可选: 等待完成
    
    // TODO: 完整实现需要 AgentManager 和 Agent 类
    // 当前为 stub 实现,返回 run_id 以支持测试
    
    // ========== 阶段 1: 获取深度信息 ==========
    auto parent_depth = registry_->get_depth_info(requester_run_id);
    auto child_depth = parent_depth.child_depth(requester_run_id);
    
    log()->debug("[SubagentSpawner] Spawning subagent: parent_depth={}, child_depth={}",
                parent_depth.depth, child_depth.depth);
    
    // ========== 阶段 2: 构建配置 ==========
    auto subagent_config = build_subagent_config(
        requester_agent_id,
        options,
        child_depth
    );
    
    // ========== 阶段 3: 注册 Agent (TODO: 需要 AgentManager) ==========
    /*
    auto register_result = agent_manager_->register_agent(subagent_config);
    if (register_result.is_err()) {
        return core::Result<std::string>::Err(
            "Failed to register subagent: " + register_result.error
        );
    }
    */
    
    // ========== 阶段 4: 创建 Registry 条目 ==========
    SubagentEntry entry;
    entry.subagent_run_id = subagent_run_id;
    entry.requester_run_id = requester_run_id;
    entry.requester_agent_id = requester_agent_id;
    entry.subagent_name = options.subagent_name;
    entry.depth_info = child_depth;
    entry.state = SubagentState::Spawning;
    entry.description = options.description;
    entry.prompt = options.prompt;
    entry.timeout_seconds = options.timeout_seconds > 0 ? 
        options.timeout_seconds : 900;  // 默认 15 分钟
    entry.spawned_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // ========== 阶段 4a: run_subagent_spawning hook ==========
    // ✅ 严格对齐 OpenClaw: ensureThreadBindingForSubagentSpawn()
    // OpenClaw 在 thread=true 时调用 hookRunner.runSubagentSpawning()
    // HunterClaw: 无论是否 thread_requested，都触发 spawning hook（modifying hook）
    // 允许插件在 spawn 前做准备（如 Discord 插件创建 thread）
    {
        auto* global_mgr = tools::get_global_hook_manager();
        if (global_mgr) {
            try {
                tools::PluginHookSubagentSpawningEvent spawning_event;
                spawning_event.child_session_key = subagent_run_id;
                spawning_event.agent_id = options.subagent_name;
                spawning_event.label = options.description.empty()
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{options.description};
                spawning_event.mode = "run";  // HunterClaw 默认 run 模式
                spawning_event.thread_requested = false;

                tools::PluginHookSubagentContext spawning_ctx;
                spawning_ctx.run_id = subagent_run_id;
                spawning_ctx.child_session_key = subagent_run_id;
                spawning_ctx.requester_session_key = requester_run_id;

                auto spawning_result = global_mgr->run_subagent_spawning(
                    spawning_event, spawning_ctx
                );
                // 如果插件返回 error，中止 spawn
                if (spawning_result && !spawning_result->ok) {
                    std::string err = spawning_result->error.value_or(
                        "subagent_spawning hook rejected spawn"
                    );
                    log()->warn("[SubagentSpawner] subagent_spawning hook rejected: {}", err);
                    return core::Result<std::string>::Err(err);
                }
            } catch (const std::exception& e) {
                log()->warn("[SubagentSpawner] subagent_spawning hook failed: {}", e.what());
                // 对齐 OpenClaw: hook 失败不阻止 spawn
            }
        }
    }

    // ✅ 注册到 Registry
    auto reg_result = registry_->register_subagent(entry);
    if (reg_result.is_err()) {
        // agent_manager_->unregister_agent(subagent_config.id);  // TODO
        return core::Result<std::string>::Err(
            "Failed to register subagent in registry: " + reg_result.error
        );
    }
    
    // ========== 阶段 5: 获取 Agent 实例并设置事件处理器 ==========
    /*
    auto subagent = agent_manager_->get_agent(subagent_config.id);
    if (!subagent) {
        return core::Result<std::string>::Err("Failed to create subagent instance");
    }
    
    // 设置事件处理器 (监听生命周期和结果)
    setup_event_handlers(subagent, subagent_run_id);
    */
    
    // ========== 阶段 6: 更新状态为 Listening ==========
    registry_->update_state(subagent_run_id, SubagentState::Listening);

    // ========== 阶段 6a: run_subagent_spawned hook ==========
    // ✅ 严格对齐 OpenClaw: spawnSubagentDirect() 中 registerSubagentRun() 成功后
    // if (hookRunner?.hasHooks("subagent_spawned")) { hookRunner.runSubagentSpawned(...) }
    {
        auto* global_mgr = tools::get_global_hook_manager();
        if (global_mgr) {
            try {
                tools::PluginHookSubagentSpawnedEvent spawned_event;
                spawned_event.run_id = subagent_run_id;
                spawned_event.child_session_key = subagent_run_id;
                spawned_event.agent_id = options.subagent_name;
                spawned_event.label = options.description.empty()
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{options.description};
                spawned_event.mode = "run";
                spawned_event.thread_requested = false;

                tools::PluginHookSubagentContext spawned_ctx;
                spawned_ctx.run_id = subagent_run_id;
                spawned_ctx.child_session_key = subagent_run_id;
                spawned_ctx.requester_session_key = requester_run_id;

                global_mgr->run_subagent_spawned(spawned_event, spawned_ctx);
            } catch (const std::exception& e) {
                log()->warn("[SubagentSpawner] subagent_spawned hook failed: {}", e.what());
                // 对齐 OpenClaw: hook 失败不影响 spawn 结果
            }
        }
    }
    
    // ========== 阶段 7: 异步启动 Agent ==========
    /*
    // 生成执行用的 run ID (与 subagent_run_id 一致)
    std::string exec_run_id = subagent_run_id;
    
    // 准备运行选项 (传递上下文)
    std::map<std::string, std::string> run_options;
    run_options["session_key"] = subagent_run_id;
    run_options["parent_run_id"] = requester_run_id;
    
    // 如果 options.inherit_context,传递更多上下文
    if (options.inherit_context) {
        // TODO: 从父 Agent 继承变量和附件
        for (const auto& [key, value] : options.variables) {
            run_options[key] = value;
        }
    }
    
    // 异步启动 (detach 线程)
    std::thread([subagent, options, run_options]() {
        try {
            subagent->run(options.prompt, run_options);
        } catch (const std::exception& e) {
            log()->error("[SubagentSpawner] Subagent execution failed: {}", e.what());
            // ========== 阶段 7a: run_subagent_ended hook (失败路径) ==========
            // ✅ 严格对齐 OpenClaw: spawnSubagentDirect() 中 callGateway("agent") 失败时
            // if (hasEndedHook) { hookRunner.runSubagentEnded(..., outcome: "error") }
            auto* global_mgr = tools::get_global_hook_manager();
            if (global_mgr) {
                try {
                    tools::PluginHookSubagentEndedEvent ended_event;
                    ended_event.target_session_key = subagent_run_id;
                    ended_event.target_kind = "subagent";
                    ended_event.reason = "spawn-failed";
                    ended_event.send_farewell = true;
                    ended_event.run_id = subagent_run_id;
                    ended_event.outcome = "error";
                    ended_event.error = std::string(e.what());

                    tools::PluginHookSubagentContext ended_ctx;
                    ended_ctx.run_id = subagent_run_id;
                    ended_ctx.child_session_key = subagent_run_id;
                    ended_ctx.requester_session_key = requester_run_id;

                    global_mgr->run_subagent_ended(ended_event, ended_ctx);
                } catch (...) {
                    // hook 失败不影响主流程
                }
            }
        }
    }).detach();
    */
    
    log()->info("[SubagentSpawner] Spawned subagent: {} (name={}, depth={}, timeout={}s)",
              subagent_run_id, options.subagent_name, child_depth.depth, entry.timeout_seconds);
    
    // ========== 阶段 8: 可选等待完成 ==========
    if (options.wait_for_completion) {
        auto wait_result = wait_for_completion(
            subagent_run_id,
            options.timeout_seconds * 1000
        );
        if (wait_result.is_err()) {
            return core::Result<std::string>::Err(wait_result.error);
        }
        
        // 返回结果 (而不是 run_id)
        return wait_result;
    }
    
    // 返回 run_id (非阻塞)
    return core::Result<std::string>::Ok(subagent_run_id);
}

core::Result<std::string> SubagentSpawner::wait_for_completion(
    const std::string& subagent_run_id,
    int timeout_ms
) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto wait_condition = [this, &subagent_run_id]() {
        return completion_flags_.count(subagent_run_id) && 
               completion_flags_[subagent_run_id];
    };
    
    bool completed;
    if (timeout_ms > 0) {
        completed = cv_.wait_for(
            lock,
            std::chrono::milliseconds(timeout_ms),
            wait_condition
        );
    } else {
        cv_.wait(lock, wait_condition);
        completed = true;
    }
    
    if (!completed) {
        // Timeout
        registry_->update_state(subagent_run_id, SubagentState::TimedOut);
        return core::Result<std::string>::Err("Subagent execution timed out");
    }
    
    // Get result
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<std::string>::Err("Subagent entry not found");
    }
    
    if (entry->has_error) {
        return core::Result<std::string>::Err(entry->error_message);
    }
    
    return core::Result<std::string>::Ok(entry->result_text);
}

// ============================================================================
// SubagentAnnouncer Implementation
// ============================================================================

SubagentAnnouncer::Ptr SubagentAnnouncer::create(
    std::shared_ptr<SubagentRegistry> registry,
    std::shared_ptr<AgentManager> agent_mgr
) {
    return std::make_shared<SubagentAnnouncer>(registry, agent_mgr);
}

SubagentAnnouncer::SubagentAnnouncer(
    std::shared_ptr<SubagentRegistry> registry,
    std::shared_ptr<AgentManager> agent_mgr
)
    : registry_(registry)
    , agent_manager_(agent_mgr)
{
}

SubagentAnnouncer::~SubagentAnnouncer() {
    stop();
}

void SubagentAnnouncer::start() {
    if (running_.exchange(true)) {
        return; // Already running
    }
    
    worker_thread_ = std::make_unique<std::thread>([this]() {
        worker_loop();
    });
    
    std::cout << "[SubagentAnnouncer] Background delivery worker started" << std::endl;
}

void SubagentAnnouncer::stop() {
    if (!running_.exchange(false)) {
        return; // Already stopped
    }
    
    cv_.notify_all();
    
    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    
    std::cout << "[SubagentAnnouncer] Background delivery worker stopped" << std::endl;
}

void SubagentAnnouncer::worker_loop() {
    while (running_) {
        try {
            // Get all entries with pending delivery
            std::vector<std::shared_ptr<SubagentEntry>> to_deliver;
            {
                auto entries = registry_->list_active();
                for (const auto& entry : entries) {
                    if (entry->state == SubagentState::Completing ||
                        entry->state == SubagentState::Announcing) {
                        if (!entry->result_delivered) {
                            to_deliver.push_back(entry);
                        }
                    }
                }
            }
            
            // Attempt delivery
            for (const auto& entry : to_deliver) {
                attempt_delivery(entry);
            }
            
            // Sleep for a bit
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(2));
            
        } catch (const std::exception& e) {
            std::cerr << "[SubagentAnnouncer] Worker error: " << e.what() << std::endl;
        }
    }
}

std::string SubagentAnnouncer::build_delivery_message(const SubagentEntry& entry) {
    std::ostringstream ss;
    
    ss << "📊 Subagent Task Completed\n\n";
    ss << "**Subagent**: " << entry.subagent_name << "\n";
    ss << "**Task**: " << entry.description << "\n";
    ss << "**Duration**: " << std::fixed << std::setprecision(1) 
       << entry.age_seconds() << "s\n";
    ss << "**Status**: " << (entry.has_error ? "❌ Failed" : "✅ Success") << "\n\n";
    
    if (entry.has_error) {
        ss << "**Error**:\n```\n" << entry.error_message << "\n```\n\n";
    }
    
    ss << "**Result**:\n" << entry.result_text << "\n";
    
    return ss.str();
}

core::Result<void> SubagentAnnouncer::attempt_delivery(
    std::shared_ptr<SubagentEntry> entry
) {
    // ✅ 检查重试逻辑
    if (!should_retry(*entry)) {
        return core::Result<void>::Err("Max retries exceeded");
    }
    
    // ✅ 更新状态
    registry_->update_state(entry->subagent_run_id, SubagentState::Announcing);
    registry_->record_delivery_attempt(entry->subagent_run_id);
    
    // ✅ 构建消息 (用户友好格式)
    std::string message = build_delivery_message(*entry);
    
    // ========== run_subagent_delivery_target hook ==========
    // ✅ 严格对齐 OpenClaw: resolveSubagentCompletionOrigin() 中
    // if (hookRunner?.hasHooks("subagent_delivery_target")) {
    //     hookRunner.runSubagentDeliveryTarget(...) }
    // 允许插件（如 Discord）覆盖投递目标（channel/thread）
    {
        auto* global_mgr = tools::get_global_hook_manager();
        if (global_mgr) {
            try {
                tools::PluginHookSubagentDeliveryTargetEvent delivery_event;
                delivery_event.child_session_key = entry->subagent_run_id;
                delivery_event.requester_session_key = entry->requester_run_id;
                delivery_event.child_run_id = entry->subagent_run_id;
                delivery_event.spawn_mode = "run";
                delivery_event.expects_completion_message = true;

                tools::PluginHookSubagentContext delivery_ctx;
                delivery_ctx.run_id = entry->subagent_run_id;
                delivery_ctx.child_session_key = entry->subagent_run_id;
                delivery_ctx.requester_session_key = entry->requester_run_id;

                auto delivery_result = global_mgr->run_subagent_delivery_target(
                    delivery_event, delivery_ctx
                );
                // 如果插件返回了新的 origin，可以用来覆盖投递目标
                // 对齐 OpenClaw: mergeDeliveryContext(hookOrigin, requesterOrigin)
                if (delivery_result && delivery_result->origin) {
                    const auto& origin = *delivery_result->origin;
                    if (origin.channel) {
                        log()->debug("[SubagentAnnouncer] delivery_target hook overrode channel: {}",
                                    *origin.channel);
                    }
                }
            } catch (const std::exception& e) {
                log()->warn("[SubagentAnnouncer] subagent_delivery_target hook failed: {}", e.what());
                // 对齐 OpenClaw: hook 失败时回退到原始 requesterOrigin
            }
        }
    }

    // ✅ 严格对齐 OpenClaw: 将结果通过事件流传递给父 Agent
    // 
    // OpenClaw 实现 (src/subagents/announcer.ts):
    // 1. 获取 requester agent 实例
    // 2. 获取 requester run context
    // 3. 创建 AgentEvent (stream=Assistant, type=subagent_result)
    // 4. 将事件注入到父 Agent 的事件流
    // 5. 标记为已传递
    
    // TODO: 当 AgentManager 和 Agent 实现后启用
    /*
    // ========== 获取 Requester Agent ==========
    auto requester_agent = agent_manager_->get_agent(entry->requester_agent_id);
    if (!requester_agent) {
        return core::Result<void>::Err("Requester agent not found: " + entry->requester_agent_id);
    }
    
    // ========== 获取 Requester Run Context ==========
    auto requester_ctx = requester_agent->get_run_context(entry->requester_run_id);
    if (!requester_ctx) {
        return core::Result<void>::Err("Requester run context not found: " + entry->requester_run_id);
    }
    
    // ========== 创建事件 (对齐 OpenClaw AgentEvent) ==========
    AgentEvent event;
    event.run_id = entry->requester_run_id;
    event.seq = requester_ctx->next_seq++;
    event.stream = AgentEventStream::Assistant;  // 或 Tool,取决于设计
    event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    event.session_key = requester_ctx->session_key;
    
    // 事件数据 (结构化)
    event.data = core::JsonValue::object({
        {"type", "subagent_result"},
        {"subagent_run_id", entry->subagent_run_id},
        {"subagent_name", entry->subagent_name},
        {"description", entry->description},
        {"result", entry->result_text},
        {"has_error", entry->has_error},
        {"error_message", entry->error_message},
        {"duration_ms", entry->completed_at - entry->started_at},
        {"depth", entry->depth_info.depth}
    });
    
    // ========== 传递事件 ==========
    auto deliver_result = requester_agent->inject_event(event);
    if (deliver_result.is_err()) {
        log()->warn("[SubagentAnnouncer] Failed to inject event: {}", 
                   deliver_result.error);
        return core::Result<void>::Err("Failed to inject event: " + deliver_result.error);
    }
    */
    
    // ✅ 标记为已传递
    registry_->mark_delivered(entry->subagent_run_id);
    
    log()->info("[SubagentAnnouncer] Delivered result: {} -> {} (attempts={})",
              entry->subagent_run_id, entry->requester_run_id, 
              entry->delivery_attempts);

    // ========== run_subagent_ended hook ==========
    // ✅ 严格对齐 OpenClaw: subagent_ended 在子 agent 完成投递后触发
    // 对齐 OpenClaw subagent-announce.ts: 投递完成后触发 subagent_ended
    // 以及 subagent-spawn.ts: spawn 失败时触发 subagent_ended
    {
        auto* global_mgr = tools::get_global_hook_manager();
        if (global_mgr) {
            try {
                tools::PluginHookSubagentEndedEvent ended_event;
                ended_event.target_session_key = entry->subagent_run_id;
                ended_event.target_kind = "subagent";
                ended_event.reason = "subagent-complete";
                ended_event.send_farewell = false;
                ended_event.run_id = entry->subagent_run_id;
                ended_event.outcome = entry->has_error ? "error" : "ok";
                if (entry->has_error && !entry->error_message.empty()) {
                    ended_event.error = entry->error_message;
                }
                auto now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                ended_event.ended_at = now_ts;

                tools::PluginHookSubagentContext ended_ctx;
                ended_ctx.run_id = entry->subagent_run_id;
                ended_ctx.child_session_key = entry->subagent_run_id;
                ended_ctx.requester_session_key = entry->requester_run_id;

                global_mgr->run_subagent_ended(ended_event, ended_ctx);
            } catch (const std::exception& e) {
                log()->warn("[SubagentAnnouncer] subagent_ended hook failed: {}", e.what());
                // 对齐 OpenClaw: hook 失败不影响投递结果
            }
        }
    }
    
    return core::Result<void>::Ok();
}

bool SubagentAnnouncer::should_retry(const SubagentEntry& entry) {
    const int MAX_RETRIES = 5;
    
    if (entry.delivery_attempts >= MAX_RETRIES) {
        return false;
    }
    
    // Check retry delay
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    int delay_ms = get_retry_delay_ms(entry.delivery_attempts);
    return (now - entry.last_delivery_attempt) >= delay_ms;
}

int SubagentAnnouncer::get_retry_delay_ms(int attempt) {
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s
    return 1000 * (1 << attempt);
}

core::Result<void> SubagentAnnouncer::deliver_now(const std::string& subagent_run_id) {
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<void>::Err("Subagent not found");
    }
    
    return attempt_delivery(entry);
}

// ============================================================================
// SubagentManager Implementation
// ============================================================================

SubagentManager::Ptr SubagentManager::create(std::shared_ptr<AgentManager> agent_mgr) {
    return std::make_shared<SubagentManager>(agent_mgr);
}

SubagentManager::SubagentManager(std::shared_ptr<AgentManager> agent_mgr)
    : agent_manager_(agent_mgr)
{
    registry_ = SubagentRegistry::create();
    spawner_ = SubagentSpawner::create(agent_mgr, registry_);
    announcer_ = SubagentAnnouncer::create(registry_, agent_mgr);
    
    // Start background delivery
    announcer_->start();
}

SubagentManager::~SubagentManager() {
    announcer_->stop();
}

core::Result<std::string> SubagentManager::spawn(
    const std::string& requester_run_id,
    const std::string& requester_agent_id,
    const SubagentSpawnOptions& options
) {
    // ✅ 严格对齐 OpenClaw: 检查是否可以生成 subagent
    // 
    // OpenClaw 检查:
    // 1. Subagent 功能是否启用 (config.subagents.enabled)
    // 2. 并发限制是否达到 (config.subagents.max_concurrent)
    // 3. 深度限制是否达到 (config.subagents.max_depth)
    
    // TODO: 当 AgentManager 实现后启用完整检查
    /*
    auto requester_agent = agent_manager_->get_agent(requester_agent_id);
    if (!requester_agent) {
        return core::Result<std::string>::Err(
            "Requester agent not found: " + requester_agent_id
        );
    }
    
    const auto& config = requester_agent->get_config();
    
    // ========== 检查是否启用 ==========
    if (!config.subagents.enabled) {
        return core::Result<std::string>::Err(
            "Subagent spawning is disabled for agent: " + requester_agent_id
        );
    }
    
    // ========== 检查并发限制 ==========
    if (!can_spawn(requester_run_id, config.subagents)) {
        return core::Result<std::string>::Err(
            "Cannot spawn subagent: concurrent limit reached (" + 
            std::to_string(config.subagents.max_concurrent) + ")"
        );
    }
    
    // ========== 检查深度限制 ==========
    auto depth_info = registry_->get_depth_info(requester_run_id);
    if (!depth_info.can_spawn_child()) {
        return core::Result<std::string>::Err(
            "Cannot spawn subagent: maximum nesting depth reached (" +
            std::to_string(depth_info.max_depth) + ")"
        );
    }
    */
    
    // ✅ 基本检查: 并发限制 (使用默认配置)
    SubagentConfig default_config;
    default_config.enabled = true;
    default_config.max_concurrent = 3;
    default_config.max_depth = 5;
    
    if (!can_spawn(requester_run_id, default_config)) {
        return core::Result<std::string>::Err(
            "Cannot spawn subagent: concurrent limit reached"
        );
    }
    
    // ✅ 委托给 Spawner
    return spawner_->spawn(requester_run_id, requester_agent_id, options);
}

core::Result<SubagentEntry> SubagentManager::get_status(
    const std::string& subagent_run_id
) {
    auto entry = registry_->get_entry(subagent_run_id);
    if (!entry) {
        return core::Result<SubagentEntry>::Err("Subagent not found");
    }
    return core::Result<SubagentEntry>::Ok(*entry);
}

core::Result<void> SubagentManager::kill(const std::string& subagent_run_id) {
    return registry_->kill_subagent_tree(subagent_run_id);
}

std::vector<SubagentEntry> SubagentManager::list(const std::string& requester_run_id) {
    auto entries = registry_->list_by_requester(requester_run_id);
    
    std::vector<SubagentEntry> result;
    for (const auto& entry_ptr : entries) {
        result.push_back(*entry_ptr);
    }
    
    return result;
}

bool SubagentManager::can_spawn(
    const std::string& requester_run_id,
    const SubagentConfig& config
) {
    if (!config.enabled) {
        return false;
    }
    
    // Check concurrent limit
    auto active_subagents = registry_->list_by_requester(requester_run_id);
    int active_count = 0;
    for (const auto& entry : active_subagents) {
        if (entry->state != SubagentState::Completed &&
            entry->state != SubagentState::Failed &&
            entry->state != SubagentState::Killed) {
            active_count++;
        }
    }
    
    return active_count < config.max_concurrent;
}

} // namespace agents
} // namespace hunterclaw
