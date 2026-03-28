#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/memory/memory_index_manager.h"  // MemoryIndexManager (openclaw manager.ts)
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/logging.h"
#include "hunterclaw/react/react_engine.h"  // ✅ ReactEngine
#include "hunterclaw/llm/llm_client.h"       // ✅ LLMClient
#include "hunterclaw/tools/tool.h"            // ✅ ToolManager
#include "hunterclaw/tools/tool_manager.h"    // ✅ ToolManager::create
#include "hunterclaw/sessions/transcript_io.h"  // ✅ 对齐 OpenClaw: transcript 读写
#include <spdlog/spdlog.h>
#include <filesystem>
#include <sstream>
#include <vector>

namespace hunterclaw {
namespace agents {

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
        
        auto tool_mgr = tools::ToolManager::create(tool_context);
        
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
        
        auto llm_client = llm::LLMClientFactory::create(llm_config);
        
        if (!llm_client) {
            throw std::runtime_error("Failed to create LLMClient from config");
        }
        
        // 配置 ReactEngine
        react::ReactConfig react_config;
        react_config.max_steps = 20;
        react_config.reasoning_mode = react::ReactConfig::ReasoningMode::Off;
        react_config.parallel_tool_calls = false;
        
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

    // 如果当前正在处理，将消息入队（非阻塞）
    // 对齐 OpenClaw: maybeQueueSubagentAnnounce()
    if (is_processing_.load()) {
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

    // 标记为处理中
    is_processing_.store(true);
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
    
    spdlog::get("default")->info("[AgentInstance] Processing message: session={}, message='{}'",
        ctx_.session_id, message);
    
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
        
        // 8. 发射 WebSocket 事件 (对齐 OpenClaw:526-533)
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
                
                auto compact_result = react_engine_->trigger_compaction(history);
                
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
                auto compact_result = react_engine_->trigger_compaction(history);
                
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
