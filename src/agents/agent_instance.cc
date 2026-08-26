#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/memory/memory_index_manager.h"  // MemoryIndexManager (openclaw manager.ts)
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/logging.h"
#include "hunterclaw/infra/agent_events.h"  // ✅ 对齐 OpenClaw: infra/agent-events.ts
#include "hunterclaw/react/react_engine.h"  // ✅ ReactEngine
#include "hunterclaw/context/context_pruning.h"  // ✅ 对齐 OC: pi-hooks/context-pruning
#include "hunterclaw/llm/llm_client.h"       // ✅ LLMClient
#include "hunterclaw/tools/tool.h"            // ✅ ToolManager
#include "hunterclaw/tools/tool_manager.h"    // ✅ ToolManager::create
#include "hunterclaw/mcp/mcporter_client.h"   // ✅ McporterConfig
#include "hunterclaw/mcp/mcp_config.h"        // ✅ list_configured_mcp_servers
#include "hunterclaw/sessions/transcript_io.h"  // ✅ 对齐 OpenClaw: transcript 读写
#include "hunterclaw/sessions/session_manager.h" // ✅ 对齐 OC: SessionEntry (compactionCount/memoryFlushCompactionCount)
#include "hunterclaw/memory/memory_flush_plan.h"  // ✅ 对齐 OC extensions/memory-core/flush-plan.ts
#include "hunterclaw/auto_reply/session_usage.h"  // ✅ 对齐 OC agent-runner-utils.ts: updateSessionAccounting
#include <spdlog/spdlog.h>
#include <filesystem>
#include <sstream>
#include <vector>
#include <cstdlib>

namespace hunterclaw {
namespace agents {

// 🎯 Live Model Catalog 发现钩子存储 (可注入, 避免 agents→core 循环)
namespace {
ModelContextDiscoveryHook& discovery_hook() {
    static ModelContextDiscoveryHook hook;
    return hook;
}
}  // namespace

void set_model_context_discovery_hook(ModelContextDiscoveryHook hook) {
    discovery_hook() = std::move(hook);
}

void run_model_context_discovery(const std::string& provider_id,
                                 const std::string& base_url,
                                 const std::string& api_key) {
    auto& hook = discovery_hook();
    if (hook) {
        try {
            hook(provider_id, base_url, api_key);
        } catch (...) {
            // advisory: 发现失败不影响 agent 运行
        }
    }
}

// Helper for logging
inline auto log() { return core::get_logger("agent_instance"); }

// ========== OpenClaw 对齐: 剥离 Block 标签 ==========
// 参考: openclaw-analysis/src/agents/block-utils.ts:48-85
std::string strip_block_tags(const std::string& text) {
    std::string result = text;
    
    // 移除 <think>...</think> 标签及内容
    // 使用简单的字符串查找替换（避免引入正则表达式依赖）
    size_t pos = 0;
    while ((pos = result.find("<think>", pos)) != std::string::npos) {
        size_t end_pos = result.find("</think>", pos);
        if (end_pos != std::string::npos) {
            // 找到匹配的闭合标签，删除整个块
            result.erase(pos, end_pos - pos + 8);  // 8 = strlen("</think>")
        } else {
            // 没有闭合标签，只删除开始标签
            result.erase(pos, 7);  // 7 = strlen("<think>")
            break;
        }
    }
    
    // 移除 <final>...</final> 标签（但保留内容）
    pos = 0;
    while ((pos = result.find("<final>", pos)) != std::string::npos) {
        result.erase(pos, 7);  // 删除开始标签
        size_t end_pos = result.find("</final>", pos);
        if (end_pos != std::string::npos) {
            result.erase(end_pos, 8);  // 删除闭合标签
        }
    }
    
    // Trim 首尾空白
    auto start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }
    auto end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

// ========== OpenClaw 对齐: 格式化推理消息 ==========
// 参考: openclaw-analysis/src/agents/pi-embedded-utils.ts:273-287
std::string format_reasoning_message_openclaw(const std::string& text) {
    // 1. Trim 空白
    std::string trimmed = text;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    
    if (start == std::string::npos) {
        return "";  // 全是空白
    }
    
    trimmed = trimmed.substr(start, end - start + 1);
    
    if (trimmed.empty()) {
        return "";
    }
    
    // 2. 按行包裹斜体 markdown (_line_)
    // 原因: Telegram 的 _italic_ 不支持跨行，需要每行单独包裹
    std::vector<std::string> lines;
    std::stringstream ss(trimmed);
    std::string line;
    
    while (std::getline(ss, line)) {
        if (!line.empty()) {
            lines.push_back("_" + line + "_");
        } else {
            lines.push_back("");  // 保留空行
        }
    }
    
    // 3. 拼接结果，添加 "Reasoning:" 前缀
    std::string result = "Reasoning:\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i < lines.size() - 1) {
            result += "\n";
        }
    }
    
    return result;
}

AgentInstance::AgentInstance(
    const std::string& session_id,
    const std::string& agent_id,
    std::shared_ptr<core::Config> config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
) : config_(config),
    skill_mgr_(skill_mgr),
    channel_mgr_(channel_mgr),
    last_assistant_emit_time_(std::chrono::steady_clock::now()),  // ✅ 初始化时间戳
    suppress_block_chunks_(false) {  // ✅ 初始化 block reply 抑制标志
    
    ctx_.session_id = session_id;
    ctx_.agent_id = agent_id;
    ctx_.created_at = core::utils::now();
    ctx_.last_active_at = ctx_.created_at;
    ctx_.phase = AgentLifecyclePhase::Start;
    
    spdlog::get("default")->info("[AgentInstance] Initializing: session={}, agent={}",
        session_id, agent_id);
    
    // 加载 Agent 配置
    auto config_result = resolve_agent_config(*config, agent_id);
    if (config_result.is_err()) {
        throw std::runtime_error("Failed to resolve agent config: " + config_result.error);
    }
    ctx_.config = config_result.get();
    
    // 初始化资源
    initialize_workspace();
    initialize_memory();
    initialize_tools();
    
    // ========== 初始化 ReactEngine (对齐 OpenClaw) ==========
    try {
        // ✅ 创建 ToolManager (独立实例,不使用全局单例)
        tools::ToolContext tool_context;
        tool_context.workspace_dir = ctx_.workspace_dir;
        tool_context.session_key = ctx_.session_id;  // ✅ 直接使用 session_id
        tool_context.agent_id = ctx_.agent_id;
        // 🎯 对齐 OpenClaw: 本地 owner 认证的 agent turn 具owner 身份
        // (senderIsOwner:true)。owner-only 工具(cron/gateway/sessions/nodes等)
        // 的门禁仅用于降级"跨 gateway HTTP 面的非 owner 调用者"。
        tool_context.sender_is_owner = true;
        
        auto tool_mgr = tools::ToolManager::create(tool_context);
        
        // ============================================================================
        // MCP 集成:自动从 ~/.hunterclaw/config.json 读取 MCP server 配置并启用
        // ============================================================================
        //   - 底层实现: NativeMcpClient (纯 C++,零外部依赖)
        //   - 可通过环境变量 HUNTERCLAW_MCP=0 强制禁用
        //   - 发现的 MCP 工具会通过 McpToolAdapter 注册到 tool_mgr
        // ============================================================================
        try {
            const char* mcp_disabled = std::getenv("HUNTERCLAW_MCP");
            bool mcp_enabled_by_env = (mcp_disabled == nullptr ||
                                       std::string(mcp_disabled) != "0");

            if (mcp_enabled_by_env) {
                auto servers = mcp::list_configured_mcp_servers();
                if (servers.ok && !servers.mcp_servers.empty()) {
                    mcp::McporterConfig mcp_cfg;
                    mcp_cfg.enabled = true;
                    mcp_cfg.start_daemon = true;
                    mcp_cfg.timeout_ms = 30000;

                    spdlog::get("default")->info(
                        "[AgentInstance] Enabling MCP (NativeMcpClient): {} server(s) configured",
                        servers.mcp_servers.size()
                    );

                    auto result = tool_mgr->enable_mcp(mcp_cfg);
                    if (result.is_ok()) {
                        spdlog::get("default")->info(
                            "[AgentInstance] MCP registered {} tool(s) for session={}",
                            result.value.value_or(0), session_id
                        );
                    } else {
                        spdlog::get("default")->warn(
                            "[AgentInstance] MCP enable failed: {}", result.error
                        );
                    }
                } else {
                    spdlog::get("default")->debug(
                        "[AgentInstance] No MCP servers configured, skipping MCP integration"
                    );
                }
            } else {
                spdlog::get("default")->info(
                    "[AgentInstance] MCP disabled by HUNTERCLAW_MCP=0"
                );
            }
        } catch (const std::exception& e) {
            spdlog::get("default")->warn(
                "[AgentInstance] MCP integration error (non-fatal): {}", e.what()
            );
        }
        
        // ✅ 从 Agent 配置创建 LLMClient
        llm::LLMConfig llm_config;
        
        // 从 Agent ModelConfig 读取配置
        llm_config.provider = ctx_.config.model.provider;
        llm_config.model = ctx_.config.model.model;
        
        // ✅ 从 ModelConfig 读取完整配置
        llm_config.api_key = ctx_.config.model.api_key;
        llm_config.endpoint = ctx_.config.model.endpoint;
        llm_config.max_tokens = ctx_.config.model.max_tokens > 0 ? ctx_.config.model.max_tokens : 4096;
        llm_config.temperature = ctx_.config.model.temperature;
        
        spdlog::get("default")->debug("[AgentInstance] Creating LLMClient: provider={}, model={}",
            llm_config.provider, llm_config.model);

        // 🎯 触发 Live Model Catalog 发现: 按当前 provider 从其 /models 端点动态
        // 拉取模型上下文窗口并灌入 ContextTokenCache (advisory, 失败静默, TTL 缓存),
        // 使压缩窗口能"从服务器按model 返回", 静态 catalog 仅兜底。
        run_model_context_discovery(llm_config.provider, llm_config.endpoint,
                                    llm_config.api_key);

        auto llm_client = llm::LLMClientFactory::create(llm_config);
        
        if (!llm_client) {
            throw std::runtime_error("Failed to create LLMClient from config");
        }
        
        // 配置 ReactEngine
        react::ReactConfig react_config;
        react_config.max_steps = 20;
        react_config.reasoning_mode = react::ReactConfig::ReasoningMode::Off;
        react_config.parallel_tool_calls = false;

        // 🎯 严格对齐 OC: pi-embedded-runner/extensions.ts buildContextPruningFactory
        // 读取 agent 配置中的 contextPruning JSON 块（对齐 OC:
        //   config.agents.defaults.contextPruning），当 mode=cache-ttl 时启用
        {
            auto it = ctx_.config.extra.find("contextPruning");
            if (it != ctx_.config.extra.end()) {
                react_config.context_pruning =
                    context::parse_context_pruning_config_from_json(it->second);
                if (react_config.context_pruning.mode !=
                    context::ContextPruningMode::OFF) {
                    spdlog::get("default")->info(
                        "[AgentInstance] context pruning enabled: mode=cache-ttl "
                        "ttl_ms={} keepLastAssistants={} softTrim={}/{} hardClear={}",
                        react_config.context_pruning.ttl_ms,
                        react_config.context_pruning.keep_last_assistants,
                        react_config.context_pruning.soft_trim_ratio,
                        react_config.context_pruning.hard_clear_ratio,
                        react_config.context_pruning.hard_clear.enabled);
                }
            }
        }

        react_engine_ = react::ReactEngine::create(tool_mgr, llm_client, react_config);
        spdlog::get("default")->info("[AgentInstance] ReactEngine initialized: model={}, provider={}", 
                   llm_config.model, llm_config.provider);
        
    } catch (const std::exception& e) {
        spdlog::get("default")->error("[AgentInstance] ReactEngine initialization failed: {}", e.what());
        // 不抛出异常,允许 Agent 在没有 ReactEngine 的情况下继续
    }
    
    spdlog::get("default")->info("[AgentInstance] Initialized: session={}, agent={}, workspace={}",
        session_id, agent_id, ctx_.workspace_dir);
    
    // 发射启动事件
    emit_lifecycle_event(AgentLifecyclePhase::Start, {});
}

AgentInstance::~AgentInstance() {
    // 清理在 cleanup() 中完成
}

core::Result<std::string> AgentInstance::process_message(
    const std::string& message,
    const std::map<std::string, std::string>& options
) {
    // ✅ 对齐 OpenClaw: 并发安全保护
    // OpenClaw 依赖 Node.js 单线程事件循环天然串行，C++ 需要显式保护
    // 对齐 OpenClaw: sendSubagentAnnounceDirectly 在 Agent 空闲时直接调用
    //             maybeQueueSubagentAnnounce 在 Agent 忙时入队等待
    if (!react_engine_) {
        return core::Result<std::string>::Err("ReactEngine not initialized");
    }

    // 原子地尝试将 is_processing_ 从 false 切换到 true（compare_exchange）
    // 对齐 OpenClaw: maybeQueueSubagentAnnounce()
    // 修复 race condition：load() + store(true) 之间存在窗口，两个线程可能同时通过检查。
    // compare_exchange_strong 保证只有一个线程能成功将 false→true，其余线程入队。
    bool expected = false;
    if (!is_processing_.compare_exchange_strong(expected, true)) {
        // 已有线程在处理，入队等待
        enqueue_message(message, options);
        spdlog::get("default")->debug(
            "[AgentInstance] Agent busy, message queued: session={}, queue_size={}",
            ctx_.session_id,
            [this]() {
                std::lock_guard<std::mutex> lock(pending_messages_mutex_);
                return pending_messages_.size();
            }()
        );
        return core::Result<std::string>::Ok("queued");
    }

    // compare_exchange 成功：is_processing_ 已被原子地设为 true
    auto result = process_message_with_overflow_retry(message, options);
    is_processing_.store(false);

    // 处理完成后，消费队列中的消息
    // 对齐 OpenClaw: 事件循环在 await 完成后自动处理下一个排队消息
    drain_pending_messages();

    return result;
}

void AgentInstance::enqueue_message(
    const std::string& message,
    const std::map<std::string, std::string>& options
) {
    std::lock_guard<std::mutex> lock(pending_messages_mutex_);
    pending_messages_.push({message, options});
    spdlog::get("default")->debug(
        "[AgentInstance] Message enqueued: session={}, queue_size={}",
        ctx_.session_id, pending_messages_.size()
    );
}

void AgentInstance::drain_pending_messages() {
    // 对齐 OpenClaw: 事件循环在当前任务完成后自动处理下一个排队消息
    // 注意：这里不能持锁调用 process_message_with_overflow_retry，
    //       因为它会再次调用 drain_pending_messages，形成递归处理链
    while (true) {
        PendingMessage pending;
        {
            std::lock_guard<std::mutex> lock(pending_messages_mutex_);
            if (pending_messages_.empty()) {
                break;
            }
            pending = pending_messages_.front();
            pending_messages_.pop();
        }

        spdlog::get("default")->info(
            "[AgentInstance] Processing queued message: session={}, remaining={}",
            ctx_.session_id,
            [this]() {
                std::lock_guard<std::mutex> lock(pending_messages_mutex_);
                return pending_messages_.size();
            }()
        );

        is_processing_.store(true);
        process_message_with_overflow_retry(pending.message, pending.options);
        is_processing_.store(false);
    }
}

void AgentInstance::cleanup() {
    spdlog::get("default")->info("[AgentInstance] Cleaning up: session={}, agent={}",
        ctx_.session_id, ctx_.agent_id);
    
    // 发射结束事件
    core::JsonValue data = core::JsonValue::object();
    data["reason"] = "cleanup";
    emit_lifecycle_event(AgentLifecyclePhase::End, data);
    
    // ========== 资源清理 (对齐 OpenClaw) ==========
    
    // 1. 停止运行中的任务
    if (react_engine_) {
        try {
            react_engine_->cancel();
            spdlog::get("default")->debug("[AgentInstance] ReactEngine cancelled");
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentInstance] Failed to cancel ReactEngine: {}", e.what());
        }
    }
    
    // 2. 关闭 Memory 连接 (通过 RAII 自动清理)
    if (ctx_.memory) {
        try {
            // MemoryProvider 依赖 RAII 自动清理
            // 释放智能指针会触发析构函数
            ctx_.memory.reset();
            spdlog::get("default")->debug("[AgentInstance] Closed memory connection");
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentInstance] Failed to close memory: {}", e.what());
        }
    }
    
    // 2.5 保存 MemoryManager 状态 (通过 RAII 自动清理)
    if (memory_mgr_) {
        try {
            // MemoryManager 依赖 RAII 自动清理
            // 析构函数会保存必要的状态
            memory_mgr_.reset();
            spdlog::get("default")->debug("[AgentInstance] Closed MemoryManager");
        } catch (const std::exception& e) {
            spdlog::get("default")->warn("[AgentInstance] Failed to close MemoryManager: {}", e.what());
        }
    }
    
    // 3. 清理临时文件 (默认启用)
    // ✅ 对齐 OpenClaw: 总是清理临时文件，保持工作空间整洁
    bool cleanup_temp_files = true;
    
    if (cleanup_temp_files) {
        try {
            std::string temp_dir = ctx_.workspace_dir + "/temp";
            if (std::filesystem::exists(temp_dir)) {
                std::filesystem::remove_all(temp_dir);
                spdlog::get("default")->debug("[AgentInstance] Cleaned temp directory: {}", temp_dir);
            }
        } catch (const std::exception& e) {
            spdlog::get("default")->warn("[AgentInstance] Failed to clean temp files: {}", e.what());
        }
    }
    
    // 4. 释放 ReactEngine
    react_engine_.reset();
    
    // 5. 更新生命周期状态
    ctx_.phase = AgentLifecyclePhase::End;
    
    spdlog::get("default")->info("[AgentInstance] Cleanup complete: session={}", ctx_.session_id);
}

void AgentInstance::on_event(EventCallback callback) {
    event_callbacks_.push_back(callback);
}

// ========== 私有方法 ==========

void AgentInstance::initialize_workspace() {
    // Workspace 格式: {base_workspace}/{agent_id}/sessions/{session_id}
    std::string base_workspace = resolve_agent_workspace_dir(*config_, ctx_.agent_id);
    ctx_.workspace_dir = base_workspace + "/sessions/" + ctx_.session_id;
    
    // 创建独立工作空间
    try {
        std::filesystem::create_directories(ctx_.workspace_dir);
            std::filesystem::create_directories(ctx_.workspace_dir + "/files");
            std::filesystem::create_directories(ctx_.workspace_dir + "/temp");
            
            spdlog::get("default")->debug("[AgentInstance] Workspace created: {}", ctx_.workspace_dir);
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentInstance] Failed to create workspace: {}", e.what());
        throw;
    }
}

void AgentInstance::initialize_memory() {
    // ========== 初始化 Memory (严格对齐 OpenClaw manager.ts) ==========
    //
    // openclaw 设计:
    // - MemoryIndexManager::get(params) 是唯一入口 (manager.ts:135)
    // - MemoryIndexManager 直接实现 MemorySearchManager 接口
    // - 没有 MemoryManager 包装层，没有 MemoryProviderAdapter
    // - Session memory 由 MemoryIndexManager 内部的 sessionsDirty/sessionDeltas 机制处理

    try {
        // 1. 构建 workspace 路径 (对齐 openclaw resolveAgentWorkspaceDir)
        std::string memory_workspace = ctx_.workspace_dir;
        std::filesystem::create_directories(memory_workspace);

        log()->info("[AgentInstance] Initializing memory: agent={}, workspace={}",
                    ctx_.agent_id, memory_workspace);

        // 2. 构建 MemoryIndexManager 参数 (对齐 manager.ts:135-175)
        memory::MemoryIndexManager::Params params;
        params.agent_id = ctx_.agent_id;
        params.workspace_dir = memory_workspace;
        // settings 使用默认值 (ResolvedMemorySearchConfig)

        // 3. 获取或创建 MemoryIndexManager (带缓存，对齐 INDEX_CACHE)
        auto mgr_result = memory::MemoryIndexManager::get(params);
        if (mgr_result.is_err()) {
            log()->warn("[AgentInstance] Memory initialization failed: {}", mgr_result.error);
            // 不抛出异常，允许 Agent 在没有 Memory 的情况下运行
            return;
        }

        // 4. MemoryIndexManager 直接是 MemorySearchManager，无需适配器
        memory_mgr_ = mgr_result.get();
        ctx_.memory = memory_mgr_;

        log()->info("[AgentInstance] Memory initialized: agent={}, workspace={}",
                    ctx_.agent_id, memory_workspace);

    } catch (const std::exception& e) {
        log()->error("[AgentInstance] Memory initialization failed: {}", e.what());
        // 不抛出异常，允许 Agent 在没有 Memory 的情况下运行
    }
}

void AgentInstance::initialize_tools() {
    // ========== 从配置获取并过滤工具 (对齐 OpenClaw) ==========
    
    // 1. 使用回退工具列表 (包含常用工具)
    // ✅ 对齐 OpenClaw: 工具列表由 Agent 配置的 skills 字段过滤
    // ToolManager 负责工具注册和执行，这里只需要定义可用工具集合
    std::vector<std::string> all_tools = {
        "read_file", "write_file", "search_files", "list_dir",
        "execute_command", "web_search", "web_fetch",
        "calculator", "ask_followup_question"
    };
    
    spdlog::get("default")->debug("[AgentInstance] Available tools: {} total", all_tools.size());
    
    // 2. 应用 Agent 的 skills 过滤器
    std::vector<std::string> filtered_tools;
    
    for (const auto& tool_id : all_tools) {
        bool allowed = false;
        
        for (const auto& pattern : ctx_.config.skills) {
            // 通配符: 允许所有
            if (pattern == "*") {
                allowed = true;
                break;
            }
            
            // 完全匹配
            if (pattern == tool_id) {
                allowed = true;
                break;
            }
            
            // 前缀匹配 (例如: "search*" 匹配 "search_files", "search_content")
            if (pattern.back() == '*') {
                std::string prefix = pattern.substr(0, pattern.size() - 1);
                if (tool_id.substr(0, prefix.size()) == prefix) {
                    allowed = true;
                    break;
                }
            }
            
            // 排除规则 (例如: "!execute_command" 禁用命令执行)
            if (pattern.front() == '!' && pattern.substr(1) == tool_id) {
                allowed = false;
                break;
            }
        }
        
        if (allowed) {
            filtered_tools.push_back(tool_id);
        }
    }
    
    ctx_.tools = filtered_tools;
    
    spdlog::get("default")->info("[AgentInstance] Tools filtered: {} allowed (from {} total)",
        filtered_tools.size(), all_tools.size());
    
    // 3. 打印过滤后的工具列表 (调试)
    // ✅ spdlog 已完全支持，添加详细的工具列表日志
    if (spdlog::get("default")->should_log(spdlog::level::debug)) {
        std::string tools_str;
        for (const auto& tool : filtered_tools) {
            if (!tools_str.empty()) tools_str += ", ";
            tools_str += tool;
        }
        spdlog::get("default")->debug("[AgentInstance] Available tools: [{}]", tools_str);
    }
}

void AgentInstance::emit_lifecycle_event(
    AgentLifecyclePhase phase,
    const core::JsonValue& data
) {
    ctx_.phase = phase;
    
    AgentEvent event;
    event.run_id = ctx_.current_run_id.value_or("");
    event.seq = ctx_.event_sequence_++;  // 递增序列号
    event.stream = AgentEventStream::Lifecycle;
    event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        core::utils::now().time_since_epoch()
    ).count();
    event.session_key = ctx_.session_id;
    event.data = data;
    
    // 触发回调
    for (auto& callback : event_callbacks_) {
        try {
            callback(event);
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentInstance] Event callback error: {}", e.what());
        }
    }
}

// ✅ touch() 已在头文件定义为 inline，无需重复实现
// ✅ emit_lifecycle_event() 已完整实现

// ═══════════════════════════════════════════════════════════════
// ✅ Overflow 重试循环 (对齐 OpenClaw)
// 参考: openclaw-analysis/src/agents/pi-embedded-runner/run.ts:980-1034
// ═══════════════════════════════════════════════════════════════

// ✅ 已启用: 完整的 overflow 重试循环实现
core::Result<std::string> AgentInstance::process_message_with_overflow_retry(
    const std::string& message,
    const std::map<std::string, std::string>& options
) {
    touch();

    // ✅ 对齐 OpenClaw: agent-runner-execution.ts
    // const runId = params.opts?.runId ?? crypto.randomUUID();
    // 优先使用调用方传入的 run_id（来自 subagent announce 的幂等键），
    // 否则生成新的随机 UUID。
    // 这保证了：
    // 1. 幂等性：相同子 run 的 announce 重试时 run_id 相同，可去重
    // 2. 生命周期事件正确：phase=start/end 事件携带正确的 run_id
    // 3. 可追踪性：父 agent 的这次执行可以被 abort 或监控
    {
        auto it = options.find("run_id");
        if (it != options.end() && !it->second.empty()) {
            ctx_.current_run_id = it->second;
        } else {
            ctx_.current_run_id = core::utils::generate_uuid();
        }
    }
    
    spdlog::get("default")->info("[AgentInstance] Processing message: session={}, run_id={}, message='{}'",
        ctx_.session_id, ctx_.current_run_id.value_or(""), message);
    
    if (!react_engine_) {
        return core::Result<std::string>::Err("ReactEngine not initialized");
    }
    
    // ✅ 对齐 OpenClaw: 读取 deliver 选项
    // OpenClaw: callGateway("agent", { deliver: true/false })
    // deliver=true  → 父 Agent 是顶层用户 session，LLM 回复需要广播给用户
    // deliver=false → 父 Agent 是 subagent session，LLM 回复仅内部传递，不广播
    // 默认 deliver=true（普通 chat.send 路径不传此选项）
    bool deliver = true;
    {
        auto it = options.find("deliver");
        if (it != options.end()) {
            deliver = (it->second != "false" && it->second != "0");
        }
    }
    
    // ✅ 对齐 OpenClaw: NO_REPLY token 检测
    // 当 LLM 判断结果已经发过（重复），回复 ONLY: NO_REPLY
    // HunterClaw 通过 suppress_block_chunks_ 抑制广播
    // 注意：使用局部变量而非 static，避免 lambda 捕获 static 变量的编译错误
    const std::string SILENT_REPLY_TOKEN = "NO_REPLY";
    
    spdlog::get("default")->debug("[AgentInstance] process_message: deliver={}, session={}",
        deliver, ctx_.session_id);
    
    // ✅ Overflow 重试配置 (对齐 OpenClaw)
    const int MAX_OVERFLOW_ATTEMPTS = 3;
    
    // 准备流式回调
    react::StreamCallbacks callbacks;
    callbacks.session_id = ctx_.session_id;
    
    // ✅ 严格对齐 OpenClaw: on_reasoning_stream 实现
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:556-586
    callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
        // 1. 格式化推理消息 (添加 "Reasoning:" 前缀 + 斜体包裹)
        std::string formatted = format_reasoning_message_openclaw(reasoning);
        if (formatted.empty()) {
            return;
        }
        
        // 2. 去重检查 (避免重复发送)
        if (formatted == last_streamed_reasoning_) {
            spdlog::get("default")->trace("[AgentInstance] Reasoning duplicate, skipping");
            return;
        }
        
        // 3. 计算增量 (只发送新增部分)
        std::string prior = last_streamed_reasoning_;
        std::string delta;
        
        if (!prior.empty() && formatted.find(prior) == 0) {
            // 新文本是旧文本的前缀扩展 → 只发送增量
            delta = formatted.substr(prior.length());
        } else {
            // 非前缀或首次 → 发送全量
            delta = formatted;
        }
        
        last_streamed_reasoning_ = formatted;
        
        // 4. 发射事件 (WebSocket 实时推送)
        core::JsonValue data = core::JsonValue::object();
        data["stream"] = "thinking";
        data["text"] = formatted;
        data["delta"] = delta;
        
        emit_lifecycle_event(AgentLifecyclePhase::Reasoning, data);
        
        spdlog::get("default")->debug("[AgentInstance] Reasoning stream: total_len={}, delta_len={}", 
                     formatted.length(), delta.length());
    };
    
    // ✅ 严格对齐 OpenClaw: on_assistant_delta 实现
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.handlers.messages.ts:146-243
    // 
    // OpenClaw 实现要点:
    // 1. 累积 delta 到 deltaBuffer
    // 2. 剥离 <think> 标签 (stripBlockTags)
    // 3. 计算增量 (delta = cleaned - previous)
    // 4. 发射 WebSocket 事件 (emitAgentEvent)
    // 5. 可选: Block Chunking (按段落/代码块切分)
    //
    // 本实现: 简化对齐版本 (带节流机制)
    callbacks.on_assistant_delta = [this, deliver, SILENT_REPLY_TOKEN](const std::string& delta) {
        // ✅ 对齐 OpenClaw: deliver=false 时不广播 delta
        if (!deliver) {
            return;
        }
        
        // 1. 累积 delta (对齐 OpenClaw deltaBuffer)
        assistant_buffer_ += delta;
        
        // 2. 节流检查 (100ms 最小间隔，避免过度推送)
        // OpenClaw 没有显式节流，但通过 block chunking 实现了类似效果
        auto now = std::chrono::steady_clock::now();
        const auto MIN_EMIT_INTERVAL = std::chrono::milliseconds(100);
        
        if (now - last_assistant_emit_time_ < MIN_EMIT_INTERVAL) {
            spdlog::get("default")->trace("[AgentInstance] Assistant delta throttled: buffer_len={}", 
                         assistant_buffer_.length());
            return;  // 跳过本次发送
        }
        
        // 3. 剥离 <think> 标签 (OpenClaw stripBlockTags)
        std::string cleaned = strip_block_tags(assistant_buffer_);
        
        // 4. 计算增量 (对齐 OpenClaw)
        std::string delta_text;
        if (!last_streamed_assistant_.empty() && 
            cleaned.find(last_streamed_assistant_) == 0) {
            // 新文本是旧文本的前缀扩展 → 只发送增量
            delta_text = cleaned.substr(last_streamed_assistant_.length());
        } else {
            // 非前缀或首次 → 发送全量
            delta_text = cleaned;
        }
        
        // 5. 发射 WebSocket 事件 (对齐 OpenClaw emitAgentEvent)
        if (!delta_text.empty()) {
            core::JsonValue data = core::JsonValue::object();
            data["stream"] = "assistant";
            data["text"] = cleaned;
            data["delta"] = delta_text;
            
            emit_lifecycle_event(AgentLifecyclePhase::Responding, data);
            
            last_streamed_assistant_ = cleaned;
            last_assistant_emit_time_ = now;
            
            spdlog::get("default")->debug("[AgentInstance] Assistant stream: total_len={}, delta_len={}", 
                         cleaned.length(), delta_text.length());
        } else {
            spdlog::get("default")->trace("[AgentInstance] Assistant delta no-op: no new text");
        }
    };
    
    // ✅ 严格对齐 OpenClaw: on_block_reply 实现
    // 参考: openclaw-analysis/src/agents/pi-embedded-subscribe.ts:480-533
    // 
    // OpenClaw 实现要点:
    // 1. Strip block tags (<think>, <final>) - 避免泄露推理内容
    // 2. 去重检查 (与上次发送的 text 比较)
    // 3. Messaging tool 冲突检测 (避免重复发送)
    // 4. Parse reply directives (mediaUrls, audioAsVoice, replyToId 等)
    // 5. 发射 WebSocket 事件 (emitBlockReplySafely)
    //
    // 本实现: 核心对齐版本 (简化了 reply directives 解析)
    callbacks.on_block_reply = [this, deliver, SILENT_REPLY_TOKEN](const std::string& block) {
        // 1. 动态抑制检测 (对齐 OpenClaw:478)
        if (suppress_block_chunks_) {
            spdlog::get("default")->trace("[AgentInstance] Block reply suppressed by suppress_block_chunks_");
            return;
        }
        
        // ✅ 对齐 OpenClaw: deliver=false 时不广播给用户
        // 这是 subagent announce 注入父 agent 时的路径（内部传递）
        if (!deliver) {
            spdlog::get("default")->debug("[AgentInstance] Block reply suppressed: deliver=false (subagent internal)");
            return;
        }
        
        // 2. Strip <think> 和 <final> 标签 (对齐 OpenClaw:484)
        std::string cleaned = strip_block_tags(block);
        
        // Trim 结尾空白 (对齐 OpenClaw:484)
        while (!cleaned.empty() && std::isspace(cleaned.back())) {
            cleaned.pop_back();
        }
        
        if (cleaned.empty()) {
            spdlog::get("default")->trace("[AgentInstance] Block reply empty after cleaning");
            return;
        }
        
        // 3. 去重检查 (对齐 OpenClaw:488-490)
        if (cleaned == last_block_reply_text_) {
            spdlog::get("default")->trace("[AgentInstance] Block reply duplicate: already sent");
            return;
        }
        
        // ✅ 对齐 OpenClaw: NO_REPLY token 检测
        // LLM 判断结果已发过时，回复 ONLY: NO_REPLY，此时抑制广播
        if (cleaned == SILENT_REPLY_TOKEN || cleaned.find(SILENT_REPLY_TOKEN) != std::string::npos) {
            spdlog::get("default")->debug("[AgentInstance] Block reply suppressed: NO_REPLY token detected");
            suppress_block_chunks_ = true;  // 后续 delta 也抑制
            return;
        }
        
        // 4. Messaging tool 冲突检测 (对齐 OpenClaw:494-498)
        // 检查是否已通过 messaging tool 发送过
        for (const auto& sent_text : messaging_tool_sent_texts_) {
            if (cleaned == sent_text) {
                spdlog::get("default")->debug("[AgentInstance] Block reply skip: already sent via messaging tool");
                return;
            }
        }
        
        // 5. 更新状态 (对齐 OpenClaw:504-506)
        last_block_reply_text_ = cleaned;
        // assistantTexts.push(chunk) - 对应 C++ 中可以记录到 memory
        
        // 6. ✅ Reply Directives 解析 (对齐 OpenClaw:510-521)
        // OpenClaw 支持的指令：
        // - mediaUrls: 附件URL列表 (图片、文件等)
        // - audioAsVoice: 语音消息标识
        // - replyToId: 回复消息ID
        // - disableNotification: 静音通知
        // 
        // HunterClaw 简化版本: 直接使用 text，不解析特殊指令
        // 原因: Reply Directives 主要用于 Telegram 等消息平台
        // HTTP/WebSocket 接口不需要这些平台特定功能
        
        // 7. 跳过空 payload (对齐 OpenClaw:523-525)
        // (已通过上面的 empty 检查)
        
        // 8. ✅ 严格对齐 OpenClaw: 发送 block reply
        // OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver → channel.send()
        // HunterClaw:
        //   - 路径 1: deliver_fn_ 注入（channel-specific，由 method_agent 注入）
        //   - 路径 2: emit_lifecycle_event（WebSocket 广播，用于 chat.send 路径）
        if (deliver_fn_) {
            // ✅ 对齐 OpenClaw: deliver_fn_ 是 channel-specific 的发送函数
            // 由 method_agent 在 deliver=true 时注入（subagent 完成后通知用户）
            try {
                deliver_fn_(cleaned);
            } catch (const std::exception& e) {
                spdlog::get("default")->warn("[AgentInstance] deliver_fn_ failed: {}", e.what());
            }
        }

        // ✅ 同时广播 WebSocket 事件（用于 chat.send 路径的实时推送）
        core::JsonValue data = core::JsonValue::object();
        data["type"] = "block_reply";
        data["text"] = cleaned;

        emit_lifecycle_event(AgentLifecyclePhase::Responding, data);

        spdlog::get("default")->debug("[AgentInstance] Block reply sent: len={}", cleaned.length());
    };
    
    callbacks.on_tool_start = [this](const std::string& tool_name, const core::JsonValue& args) {
        spdlog::get("default")->info("[AgentInstance] Tool start: {}", tool_name);
        core::JsonValue data = core::JsonValue::object();
        data["tool"] = tool_name;
        data["action"] = "start";
        emit_lifecycle_event(AgentLifecyclePhase::ToolExecution, data);
    };
    
    callbacks.on_tool_end = [this](const std::string& tool_name, const tools::ToolResult& result) {
        spdlog::get("default")->info("[AgentInstance] Tool end: {} (success={})", tool_name, result.success);
        core::JsonValue data = core::JsonValue::object();
        data["tool"] = tool_name;
        data["action"] = "end";
        data["success"] = result.success;
        emit_lifecycle_event(AgentLifecyclePhase::ToolExecution, data);
    };

    // ✅ 严格对齐 OpenClaw: 桥接 ReactEngine 局部事件 → 全局 infra::AgentEvent 总线
    // 参考: openclaw-analysis/src/infra/agent-events.ts: emitAgentEvent
    //
    // OpenClaw 链路:
    //   ReactEngine delta → callbacks.on_agent_event → emitAgentEvent(runId, stream, data)
    //   → 全局总线广播 → ParentStreamRelay::on_agent_event 触发
    //   → flush_pending() → enqueue_system_event → 父 Agent 心跳唤醒
    //
    // 没有此桥接时:
    //   - callbacks.on_agent_event 为 nullptr，ReactEngine 内部跳过
    //   - infra::emit_agent_event 从未被调用
    //   - ParentStreamRelay 订阅了全局总线但永远收不到事件
    //   - ACP 父 Agent 无法感知子 Agent 进度，60s stall 检测触发告警
    {
        std::string current_run_id = ctx_.current_run_id.value_or("");
        callbacks.on_agent_event = [current_run_id](const react::StreamCallbacks::AgentEvent& event) {
            if (current_run_id.empty()) return;
            infra::AgentEvent global_event;
            global_event.run_id = current_run_id;
            global_event.stream = event.stream;
            global_event.data = event.data.to_json();
            infra::emit_agent_event(global_event);
        };
    }

    // ✅ 严格对齐 OpenClaw: 计算 transcript 路径
    // OpenClaw: transcriptIO.load(sessionKey) 在每次 run 前加载历史
    // workspace_dir = {base}/{agent_id}/sessions/{session_id}
    // sessions_dir  = {base}/{agent_id}/sessions
    // transcript    = {sessions_dir}/agent-{agent_id}/session-{session_id}.jsonl
    std::string sessions_dir = std::filesystem::path(ctx_.workspace_dir).parent_path().string();
    std::string transcript_path = sessions::resolve_transcript_path(
        ctx_.session_id,
        ctx_.agent_id,
        sessions_dir
    );

    // ✅ 对齐 OpenClaw: 确保 transcript 文件存在
    sessions::ensure_transcript_file(transcript_path, ctx_.session_id);

    // ✅ 对齐 OpenClaw: 写入用户消息到 transcript
    // OpenClaw: transcriptIO.append(sessionKey, { role: "user", content: message })
    {
        std::string run_id = ctx_.current_run_id.value_or(core::utils::generate_uuid());
        sessions::append_user_message_to_transcript(transcript_path, message, run_id);
    }

    // ✅ 对齐 OpenClaw: 从 transcript 加载历史消息
    // OpenClaw: transcriptIO.load(sessionKey) → messages[] → 传给 ReactEngine::run()
    // 这是关键：每次 process_message 都从 transcript 加载完整历史，
    // 保证父 Agent 收到 [task_completion] 时能看到之前所有的对话上下文
    std::vector<react::LLMMessage> history = sessions::read_transcript_messages(transcript_path, 200);

    // 去掉最后一条（刚写入的用户消息），避免 ReactEngine 重复添加
    if (!history.empty() && history.back().role == llm::LLMMessageRole::User) {
        history.pop_back();
    }

    spdlog::get("default")->debug("[AgentInstance] Loaded {} history messages from transcript: session={}",
        history.size(), ctx_.session_id);

    // ✅ 对齐 OpenClaw agent-step.ts:93,113 —— extraSystemPrompt
    //
    // 跨会话步骤（A2A ping-pong / announce / subagent）需要为**本次 run** 附加
    // 一段系统指令，用于交代当前角色、轮次与控制约定（例如
    // 「若要停止对话请仅回复 REPLY_SKIP」）。它是 per-run 的，不进 transcript，
    // 也不改变会话的常驻 system prompt。
    //
    // 此前该字段在 gateway 与 AgentInstance 两层都未被消费，导致 A2A 的
    // reply/announce 上下文从未送达模型 —— 模型不知道 REPLY_SKIP 之类的约定，
    // 整套 A2A 控制流（终止 ping-pong、静默 announce）全部失效。
    //
    // 以 System 消息追加到 history 末尾（即紧贴本轮 user 输入之前），语义等价于
    // 真源把 extraSystemPrompt 作为附加系统指令下发。刻意不覆盖首条 System
    // 消息：那是会话常驻 prompt，覆盖会丢失 agent 身份与工具约定。
    {
        auto it = options.find("extra_system_prompt");
        if (it != options.end() && !it->second.empty()) {
            react::LLMMessage extra_sys;
            extra_sys.role = llm::LLMMessageRole::System;
            extra_sys.content = it->second;
            history.push_back(extra_sys);
            spdlog::get("default")->debug(
                "[AgentInstance] Appended extraSystemPrompt ({} bytes): session={}",
                it->second.size(), ctx_.session_id);
        }
    }

    if (ctx_.memory) {
        try {
            memory::SyncParams sync_params;
            sync_params.reason = "session-start";
            ctx_.memory->sync(sync_params);
        } catch (const std::exception& e) {
            spdlog::get("default")->warn("[AgentInstance] Exception warming memory: {}", e.what());
        }
    }

    // ✅ 对齐 OpenClaw: 设置 transcript_path 到 callbacks（用于 tool_result_persist hook）
    callbacks.transcript_path = transcript_path;

    // ════════════════════════════════════════════════════════════════════
    // 🎯 严格对齐 OC agent-runner-memory.ts: memory flush 执行路径注入
    //
    // OC 调用链：runMemoryFlushIfNeeded → memoryDeps.runEmbeddedPiAgent({
    //   trigger: "memory", memoryFlushWritePath, prompt, extraSystemPrompt, ... })
    //
    // HC 等价架构：
    //   1) build_memory_flush_plan()  ← 对齐 OC extensions/memory-core/src/flush-plan.ts
    //      产出 flush_prompt / flush_system_prompt / 默认阈值 / memory/YYYY-MM-DD.md 路径
    //   2) 读 SessionManager 取 compaction_count / memory_flush_compaction_count
    //      （支持 hasAlreadyFlushedForCurrentCompaction 去重）
    //   3) 注入 run_flush_agent_fn：起一个独立 ReactEngine run，
    //      以 flush_prompt 为 user_input、flush_system_prompt 并入 system，
    //      允许它通过 Write/Edit 工具把 durable memory 写到 workspace_dir/memory/YYYY-MM-DD.md
    //   4) 注入 persist_session_tokens_fn / on_flush_complete_fn
    //      写回 SessionManager（totalTokens/memoryFlushCompactionCount/memoryFlushAt）
    //
    // 关键隔离：flush 子 session 使用**全新空 history**（对齐 OC runEmbeddedPiAgent 的
    // trigger: "memory" 入口——不读原 session transcript 作为 context，以免 context 污染
    // 再触发一次 compact）；但 workspace_dir 共享（才能落盘到 memory/YYYY-MM-DD.md）。
    // ════════════════════════════════════════════════════════════════════
    {
        // Step 1: 生成 flush plan（默认配置，无 override）
        memory::MemoryFlushPlanOverrides plan_overrides;
        auto maybe_plan = memory::build_memory_flush_plan(plan_overrides, /*now_ms=*/0);

        if (maybe_plan.has_value()) {
            const auto& plan = *maybe_plan;

            // Step 2: 读取 session entry（用于 compaction_count 去重）
            //   HC AgentInstance 不强依赖 SessionManager；无则跳过去重，门控仍能走。
            std::optional<int> compaction_count;
            std::optional<int> memory_flush_compaction_count;
            std::optional<int> persisted_total_tokens;
            bool total_tokens_fresh = false;

            // 从 workspace 上级目录解析默认 storage_dir（对齐 OpenClaw 布局）
            std::string storage_dir = std::filesystem::path(ctx_.workspace_dir)
                                         .parent_path().parent_path().string();
            auto session_mgr = sessions::SessionManager::create(storage_dir,
                                                                sessions::SessionConfig{});
            if (session_mgr) {
                auto entry_r = session_mgr->get_session(ctx_.session_id);
                if (entry_r.is_ok()) {
                    const auto& e = entry_r.get();
                    compaction_count = e.compaction_count;
                    memory_flush_compaction_count = e.memory_flush_compaction_count;
                    if (e.total_tokens > 0) {
                        persisted_total_tokens = e.total_tokens;
                        total_tokens_fresh = e.total_tokens_fresh;
                    }
                }
            }

            // Step 3: 填充 StreamCallbacks memory-flush 字段
            callbacks.workspace_dir      = ctx_.workspace_dir;
            callbacks.persisted_total_tokens = persisted_total_tokens;
            callbacks.total_tokens_fresh = total_tokens_fresh;
            callbacks.compaction_count   = compaction_count;
            callbacks.memory_flush_compaction_count = memory_flush_compaction_count;
            callbacks.flush_prompt        = plan.prompt;
            callbacks.flush_system_prompt = plan.system_prompt;
            // 对齐 OC: relativePath 是相对 workspace 的 "memory/YYYY-MM-DD.md"
            callbacks.flush_write_path =
                (std::filesystem::path(ctx_.workspace_dir) / plan.relative_path).string();
            callbacks.force_flush_transcript_bytes = plan.force_flush_transcript_bytes;
            callbacks.memory_flush_writable = true;
            callbacks.is_heartbeat = false;  // AgentInstance 不是 heartbeat 入口
            callbacks.is_cli       = false;
            callbacks.provider = ctx_.config.model.provider;
            callbacks.model    = ctx_.config.model.model;

            // Step 4: run_flush_agent_fn —— 对齐 OC memoryDeps.runEmbeddedPiAgent({trigger:"memory"})
            //
            // 用 AgentInstance 持有的 react_engine_ 再跑一次，但 **history 为空**
            // （对齐 OC 独立子 session 语义，避免污染当前 ReAct 流）。
            // 它通过 Write/Edit 工具访问同一 workspace 将记忆落盘。
            auto engine  = react_engine_;
            auto session = ctx_.session_id;
            auto agent   = ctx_.agent_id;
            auto workspace = ctx_.workspace_dir;
            callbacks.run_flush_agent_fn =
                [engine, session, agent, workspace]
                (const std::string& prompt, const std::string& system_prompt) {
                    if (!engine) return;
                    react::StreamCallbacks flush_cb;
                    flush_cb.session_id = session + "-memflush";  // 子会话标识
                    flush_cb.agent_id   = agent;
                    flush_cb.workspace_dir = workspace;
                    flush_cb.trigger       = "memory";  // 对齐 OC runEmbeddedPiAgent trigger
                    // 关键：不设 transcript_path → 本次子 run 不写用户 transcript
                    //       不设 compact_session 字段 → 子 run 不再触发嵌套 compact

                    // 把 system_prompt 合并到 callbacks（engine 支持经 before_prompt_build
                    // 等 hook 注入 system；这里简化：把 system_prompt 前置到 user prompt）
                    std::string merged_prompt =
                        system_prompt.empty()
                            ? prompt
                            : std::string("[System]\n") + system_prompt +
                              "\n\n[Task]\n" + prompt;

                    try {
                        auto res = engine->run(merged_prompt, /*history=*/{}, flush_cb);
                        spdlog::get("default")->info(
                            "[AgentInstance] memory flush sub-run done: "
                            "session={} success={} steps={}",
                            session, res.success, res.total_steps);
                    } catch (const std::exception& e) {
                        spdlog::get("default")->warn(
                            "[AgentInstance] memory flush sub-run failed: {}", e.what());
                    }
                };

            // Step 5: persist_session_tokens_fn —— 对齐 OC L622-649
            auto sess_mgr_weak = std::weak_ptr<sessions::SessionManager>(session_mgr);
            auto sid_for_persist = ctx_.session_id;
            callbacks.persist_session_tokens_fn =
                [sess_mgr_weak, sid_for_persist](int derived_total_tokens) {
                    auto mgr = sess_mgr_weak.lock();
                    if (!mgr) return;
                    auto entry_r = mgr->get_session(sid_for_persist);
                    if (!entry_r.is_ok()) return;
                    auto e = entry_r.get();
                    e.total_tokens = derived_total_tokens;
                    e.total_tokens_fresh = true;
                    (void)mgr->update_session(e);
                };

            // Step 6: on_flush_complete_fn —— 对齐 OC L821-842
            callbacks.on_flush_complete_fn =
                [sess_mgr_weak, sid_for_persist]
                (int new_compaction_count, const std::string& new_session_id) {
                    auto mgr = sess_mgr_weak.lock();
                    if (!mgr) return;
                    auto entry_r = mgr->get_session(sid_for_persist);
                    if (!entry_r.is_ok()) return;
                    auto e = entry_r.get();
                    e.memory_flush_compaction_count = new_compaction_count;
                    e.memory_flush_at_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    // 对齐 OC: 可选 session rotation（有新 session_id 则更新）
                    if (!new_session_id.empty()) {
                        e.session_id = new_session_id;
                    }
                    (void)mgr->update_session(e);
                };

            // Step 7: on_run_complete_fn —— 严格对齐 OC agent-runner-utils.ts:
            //   updateSessionAccounting(agent, sessionKey, runAccounting)
            //
            // 每轮 LLM run 结束时由 ReactEngine 调用，把 (input/output/cache_read/
            // cache_write + model) 累加写回 session entry 的 totalTokens，并刷新
            // totalTokensFresh = (runTotal > 0)。
            //
            // ⚠️ 这条链路修复了 preflight_memory fresh 门控的「只在重启第一轮生效」
            //    回归：没有此回调时 persisted_total_tokens 在同一进程内永远不刷新，
            //    fresh 门控被动退化。
            auto agent_for_acct = ctx_.agent_id;
            callbacks.on_run_complete_fn =
                [sess_mgr_weak, sid_for_persist, agent_for_acct]
                (const react::StreamCallbacks::RunCompleteUsage& usage) {
                    auto mgr = sess_mgr_weak.lock();
                    if (!mgr) return;
                    auto_reply::SessionRunAccounting acct;
                    acct.started_at         = 0;  // 目前未使用 duration, 保持 0
                    acct.completed_at       = 0;
                    acct.input_tokens       = static_cast<int>(usage.input_tokens);
                    acct.output_tokens      = static_cast<int>(usage.output_tokens);
                    acct.cache_read_tokens  = static_cast<int>(usage.cache_read_tokens);
                    acct.cache_write_tokens = static_cast<int>(usage.cache_write_tokens);
                    acct.model_provider     = usage.model_provider;
                    acct.model              = usage.model;
                    acct.is_heartbeat       = usage.is_heartbeat;
                    auto_reply::update_session_accounting(
                        *mgr, sid_for_persist, agent_for_acct, acct);
                };
        }
    }

    // ✅ Overflow 重试循环 (对齐 OpenClaw)
    for (int attempt = 0; attempt < MAX_OVERFLOW_ATTEMPTS; ++attempt) {
        try {
            // 尝试执行 ReAct（传入从 transcript 加载的历史）
            auto result = react_engine_->run(message, history, callbacks);

            // 检查是否成功
            if (result.success) {
                // ✅ 对齐 OpenClaw: 写入 assistant 消息到 transcript
                // OpenClaw: transcriptIO.append(sessionKey, { role: "assistant", content: response })
                if (!result.final_response.empty()) {
                    sessions::AppendTranscriptOptions opts;
                    opts.message = result.final_response;
                    opts.create_if_missing = false;
                    sessions::append_assistant_message_to_transcript(transcript_path, opts);
                }

                if (ctx_.memory) {
                    memory::SyncParams sync_params;
                    sync_params.reason = "session-end";
                    ctx_.memory->sync(sync_params);
                }

                spdlog::get("default")->info("[AgentInstance] Message processed: steps={}, tool_calls={}, duration={}ms",
                    result.total_steps, result.tool_calls, result.duration_ms);

                return core::Result<std::string>::Ok(result.final_response);
            }

            // ✅ 检测 Context Overflow Error (对齐 OpenClaw)
            bool is_overflow = react_engine_->is_context_overflow_error(result.error);
            
            if (is_overflow) {
                spdlog::get("default")->warn(
                    "[AgentInstance] Context overflow detected (attempt {}/{}): {}",
                    attempt + 1, MAX_OVERFLOW_ATTEMPTS, result.error
                );
                
                // 提取 overflow token count (用于调试)
                auto observed_tokens = react_engine_->extract_overflow_token_count(result.error);
                if (observed_tokens.has_value()) {
                    spdlog::get("default")->info("[AgentInstance] Observed overflow token count: {}", *observed_tokens);
                }
                
                // ✅ 触发 Compaction (对齐 OpenClaw)
                spdlog::get("default")->info("[AgentInstance] Triggering compaction to resolve overflow...");
                
                auto compact_result = react_engine_->trigger_compaction(history, transcript_path);
                
                if (compact_result.compacted) {
                    spdlog::get("default")->info(
                        "[AgentInstance] Compaction successful: {} -> {} messages, saved {} tokens",
                        compact_result.messages_before,
                        compact_result.messages_after,
                        compact_result.tokens_saved
                    );
                    
                    // 重试
                    continue;
                } else {
                    spdlog::get("default")->error(
                        "[AgentInstance] Compaction failed: {}",
                        compact_result.reason
                    );
                    
                    // Compaction 失败 - 如果还有重试机会则继续,否则返回错误
                    if (attempt + 1 < MAX_OVERFLOW_ATTEMPTS) {
                        continue;
                    } else {
                        return core::Result<std::string>::Err(
                            "Context overflow after max compaction retries (" + 
                            std::to_string(MAX_OVERFLOW_ATTEMPTS) + "): " + compact_result.reason
                        );
                    }
                }
            }
            
            // 其他错误 - 直接返回
            return core::Result<std::string>::Err(result.error);
            
        } catch (const std::exception& e) {
            spdlog::get("default")->error("[AgentInstance] ReactEngine exception: {}", e.what());
            
            // 检测是否是 overflow 相关异常
            std::string error_msg = e.what();
            bool is_overflow = react_engine_->is_context_overflow_error(error_msg);
            
            if (is_overflow && attempt + 1 < MAX_OVERFLOW_ATTEMPTS) {
                spdlog::get("default")->warn("[AgentInstance] Context overflow exception (attempt {}/{})",
                           attempt + 1, MAX_OVERFLOW_ATTEMPTS);
                
                // 尝试 compaction 并重试
                auto compact_result = react_engine_->trigger_compaction(history, transcript_path);
                
                if (compact_result.compacted) {
                    spdlog::get("default")->info(
                        "[AgentInstance] Compaction after exception: {} -> {} messages",
                        compact_result.messages_before,
                        compact_result.messages_after
                    );
                    continue;  // 重试
                }
            }
            
            // 非 overflow 异常或重试失败 - 返回错误
            return core::Result<std::string>::Err(
                std::string("ReactEngine failed: ") + e.what()
            );
        }
    }
    
    // 不应该到达这里
    return core::Result<std::string>::Err(
        "Context overflow after max retries (" + 
        std::to_string(MAX_OVERFLOW_ATTEMPTS) + ")"
    );
}

} // namespace agents
} // namespace hunterclaw
