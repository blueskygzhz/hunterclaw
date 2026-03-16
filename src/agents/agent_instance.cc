#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/memory/memory_provider_adapter.h"  // ✅ P3: MemoryProviderAdapter
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/logging.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <sstream>
#include <vector>

namespace hunterclaw {
namespace agents {

// Helper for logging
inline auto log() { return core::get_logger("agent_instance"); }

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
    
    spdlog::info("[AgentInstance] Initializing: session={}, agent={}",
        session_id, agent_id);
    
    // 加载 Agent 配置
    auto config_result = resolve_agent_config(*config, agent_id);
    if (config_result.is_err()) {
        throw std::runtime_error("Failed to resolve agent config: " + config_result.error);
    }
    ctx_.config = config_result.value();
    
    // 初始化资源
    initialize_workspace();
    initialize_memory();
    initialize_tools();
    
    // ========== 初始化 ReactEngine (对齐 OpenClaw) ==========
    try {
        // ✅ 创建 ToolManager (独立实例,不使用全局单例)
        tools::ToolContext tool_context;
        tool_context.workspace_dir = ctx_.workspace_dir;
        tool_context.session_id = ctx_.session_id;
        tool_context.agent_id = ctx_.agent_id;
        
        auto tool_mgr = tools::ToolManager::create(tool_context);
        
        // ✅ 从 Agent 配置创建 LLMClient
        llm::LLMConfig llm_config;
        llm_config.provider = config_->llm.provider;
        llm_config.model = ctx_.config.model;  // Agent 级别的 model 配置
        llm_config.api_key = config_->llm.api_key;
        llm_config.endpoint = config_->llm.endpoint;
        llm_config.max_tokens = config_->llm.max_tokens > 0 
            ? config_->llm.max_tokens 
            : 4096;  // 默认值
        llm_config.temperature = config_->llm.temperature;
        
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
        spdlog::info("[AgentInstance] ReactEngine initialized: model={}, provider={}", 
                   llm_config.model, llm_config.provider);
        
    } catch (const std::exception& e) {
        spdlog::error("[AgentInstance] ReactEngine initialization failed: {}", e.what());
        // 不抛出异常,允许 Agent 在没有 ReactEngine 的情况下继续
    }
    
    spdlog::info("[AgentInstance] Initialized: session={}, agent={}, workspace={}",
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
    // ✅ 使用 Overflow 重试循环 (对齐 OpenClaw)
    return process_message_with_overflow_retry(message, options);
}

void AgentInstance::cleanup() {
    spdlog::info("[AgentInstance] Cleaning up: session={}, agent={}",
        ctx_.session_id, ctx_.agent_id);
    
    // 发射结束事件
    emit_lifecycle_event(AgentLifecyclePhase::End, {
        {"reason", "cleanup"}
    });
    
    // ========== 资源清理 (对齐 OpenClaw) ==========
    
    // 1. 停止运行中的任务
    if (react_engine_) {
        try {
            react_engine_->cancel();
            spdlog::debug("[AgentInstance] Cancelled running ReactEngine tasks");
        } catch (const std::exception& e) {
            spdlog::error("[AgentInstance] Failed to cancel ReactEngine: {}", e.what());
        }
    }
    
    // 2. 关闭 Memory 连接 (flush + close)
    if (ctx_.memory) {
        try {
            // TODO: 调用 Memory 的 close/flush 方法
            // ctx_.memory->flush();
            // ctx_.memory->close();
            ctx_.memory.reset();  // 释放智能指针
            spdlog::debug("[AgentInstance] Closed memory connection");
        } catch (const std::exception& e) {
            spdlog::error("[AgentInstance] Failed to close memory: {}", e.what());
        }
    }
    
    // 3. 清理临时文件 (可选,取决于配置)
    bool cleanup_temp_files = true;  // 默认清理
    
    // ✅ 从配置读取
    if (config_->workspace.cleanup_temp_files.has_value()) {
        cleanup_temp_files = *config_->workspace.cleanup_temp_files;
    }
    
    if (cleanup_temp_files) {
        try {
            std::string temp_dir = ctx_.workspace_dir + "/temp";
            if (std::filesystem::exists(temp_dir)) {
                std::filesystem::remove_all(temp_dir);
                spdlog::debug("[AgentInstance] Cleaned temp directory: {}", temp_dir);
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AgentInstance] Failed to clean temp files: {}", e.what());
        }
    }
    
    // 4. 释放 ReactEngine 和 ToolRegistry
    react_engine_.reset();
    tool_registry_.reset();
    
    // 5. 更新生命周期状态
    ctx_.phase = AgentLifecyclePhase::End;
    
    spdlog::info("[AgentInstance] Cleanup complete: session={}", ctx_.session_id);
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
        
        spdlog::debug("[AgentInstance] Workspace created: {}", ctx_.workspace_dir);
    } catch (const std::exception& e) {
        spdlog::error("[AgentInstance] Failed to create workspace: {}", e.what());
        throw;
    }
}

void AgentInstance::initialize_memory() {
    // ========== 初始化 Session 级别 Memory (对齐 OpenClaw) ==========
    
    try {
        // 1. 构建 Memory 存储路径
        // 格式: {workspace}/{agent_id}/memory/{session_id}.db
        std::string memory_db_path = ctx_.workspace_dir + "/memory/" + ctx_.session_id + ".db";
        
        // 确保目录存在
        std::filesystem::create_directories(ctx_.workspace_dir + "/memory");
        
        spdlog::info("[AgentInstance] Initializing memory: path={}", memory_db_path);
        
        // 2. 创建 SQLite Vector Storage
        auto storage = memory::SQLiteVectorStorage::create(memory_db_path, 1536);
        if (!storage) {
            spdlog::error("[AgentInstance] Failed to create SQLite storage");
            return;
        }
        
        // 3. 创建 Embedding Provider (从配置)
        std::shared_ptr<memory::EmbeddingProvider> embedding;
        
        // ✅ 从 Agent 配置或全局配置读取 embedding 设置
        std::string embedding_provider = "ollama";  // 默认 ollama
        std::string embedding_model = "nomic-embed-text";
        
        // 尝试从全局配置读取
        if (config_->memory.embedding_provider.has_value()) {
            embedding_provider = *config_->memory.embedding_provider;
        }
        if (config_->memory.embedding_model.has_value()) {
            embedding_model = *config_->memory.embedding_model;
        }
        
        if (embedding_provider == "openai") {
            std::string api_key = config_->llm.api_key;  // 复用 LLM API Key
            embedding = memory::EmbeddingProviderFactory::create_openai(api_key, "text-embedding-3-small");
        } else if (embedding_provider == "ollama") {
            embedding = memory::EmbeddingProviderFactory::create_ollama(embedding_model);
        }
        
        if (!embedding) {
            spdlog::warn("[AgentInstance] Failed to create embedding provider, memory disabled");
            return;
        }
        
        // 4. 创建 MemoryManager
        memory::SearchConfig search_config;
        search_config.max_results = 6;
        search_config.min_score = 0.35;
        search_config.hybrid.enabled = true;
        search_config.hybrid.vector_weight = 0.7;
        search_config.hybrid.text_weight = 0.3;
        search_config.hybrid.mmr.enabled = true;
        search_config.hybrid.mmr.lambda = 0.7;
        
        // ✅ 保存 MemoryManager 引用
        memory_mgr_ = memory::MemoryManager::create(storage, embedding, search_config);
        
        // ✅ 传递 CompactionManager 给 ReactEngine (对齐 OpenClaw)
        if (react_engine_ && memory_mgr_) {
            // 获取 CompactionManager 引用
            auto& compaction_mgr = memory_mgr_->compaction_manager();
            
            // 使用非拥有智能指针 (避免循环引用)
            auto compaction_ptr = std::shared_ptr<memory::CompactionManager>(
                &compaction_mgr,
                [](auto*){} // 空 deleter - 不真正删除
            );
            
            react_engine_->set_compaction_manager(compaction_ptr);
            
            // ✅ 配置 Context Window (对齐 OpenClaw)
            react::ReactEngine::ContextWindowConfig ctx_config;
            ctx_config.context_window_tokens = 200000;
            ctx_config.reserve_tokens_floor = 20000;
            ctx_config.soft_threshold_tokens = 4000;
            react_engine_->set_context_window_config(ctx_config);
            
            spdlog::info("[AgentInstance] CompactionManager integrated with ReactEngine");
        }
        
        // 5. 将 MemoryManager 转换为 MemoryProvider (适配)
        // ✅ 严格对齐 OpenClaw: 创建 MemoryProvider 适配器 (P3 优先级实现)
        // 
        // OpenClaw 中使用统一的 MemoryProvider 接口:
        // - store(item): 存储记忆
        // - search(query, options): 搜索记忆
        // - get(id): 获取记忆
        // - delete(id): 删除记忆
        // 
        // HunterClaw 实现:
        // - MemoryManager: 完整的 memory 管理器 (compaction, session, search)
        // - MemoryProviderAdapter: 适配器模式,包装 MemoryManager 为 MemoryProvider
        // - ctx_.memory: 统一接口,供 AgentInstanceContext 使用
        // 
        // 优势:
        // 1. 统一接口: AgentInstanceContext 只依赖 MemoryProvider
        // 2. 解耦: MemoryManager 的复杂功能隐藏在适配器后
        // 3. 兼容性: 未来可切换不同的 Memory 实现 (QMD, remote)
        
        // ✅ 创建适配器 (P3 优先级完成)
        ctx_.memory = std::make_shared<memory::MemoryProviderAdapter>(memory_mgr_);
        
        spdlog::info("[AgentInstance] Memory initialized: session={}, db={}, adapter=enabled",
            ctx_.session_id, memory_db_path);
        
    } catch (const std::exception& e) {
        spdlog::error("[AgentInstance] Memory initialization failed: {}", e.what());
        // 不抛出异常,允许 Agent 在没有 Memory 的情况下运行
    }
}

void AgentInstance::initialize_tools() {
    // ========== 从 ToolRegistry 获取并过滤工具 (对齐 OpenClaw) ==========
    
    // 1. 获取全局 ToolRegistry (TODO: 实现全局单例或依赖注入)
    // tool_registry_ = tools::ToolRegistry::instance();
    
    std::vector<std::string> all_tools;
    
    if (tool_registry_) {
        // 从 ToolRegistry 获取所有工具
        all_tools = tool_registry_->list_tools();
        spdlog::debug("[AgentInstance] Loaded {} tools from registry", all_tools.size());
    } else {
        // 回退: 使用占位工具列表
        all_tools = {
            "read_file", "write_file", "search_files", "list_dir",
            "execute_command", "web_search", "web_fetch",
            "calculator", "ask_followup_question"
        };
        spdlog::warn("[AgentInstance] ToolRegistry not available, using fallback tool list");
    }
    
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
    
    spdlog::info("[AgentInstance] Tools filtered: {} allowed (from {} total)",
        filtered_tools.size(), all_tools.size());
    
    // 3. 打印过滤后的工具列表 (调试)
    if (spdlog::get_level() <= spdlog::level::debug) {
        std::string tools_str;
        for (const auto& tool : filtered_tools) {
            if (!tools_str.empty()) tools_str += ", ";
            tools_str += tool;
        }
        spdlog::debug("[AgentInstance] Available tools: [{}]", tools_str);
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
            spdlog::error("[AgentInstance] Event callback error: {}", e.what());
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// ✅ Overflow 重试循环 (对齐 OpenClaw)
// 参考: openclaw-analysis/src/agents/pi-embedded-runner/run.ts:980-1034
// ═══════════════════════════════════════════════════════════════

core::Result<std::string> AgentInstance::process_message_with_overflow_retry(
    const std::string& message,
    const std::map<std::string, std::string>& options
) {
    touch();
    
    spdlog::info("[AgentInstance] Processing message: session={}, message='{}'",
        ctx_.session_id, message);
    
    if (!react_engine_) {
        return core::Result<std::string>::Err("ReactEngine not initialized");
    }
    
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
            spdlog::trace("[AgentInstance] Reasoning duplicate, skipping");
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
        emit_lifecycle_event(AgentLifecyclePhase::Reasoning, {
            {"stream", "thinking"},
            {"text", formatted},    // 完整文本
            {"delta", delta}        // 增量文本
        });
        
        spdlog::debug("[AgentInstance] Reasoning stream: total_len={}, delta_len={}", 
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
    callbacks.on_assistant_delta = [this](const std::string& delta) {
        // 1. 累积 delta (对齐 OpenClaw deltaBuffer)
        assistant_buffer_ += delta;
        
        // 2. 节流检查 (100ms 最小间隔，避免过度推送)
        // OpenClaw 没有显式节流，但通过 block chunking 实现了类似效果
        auto now = std::chrono::steady_clock::now();
        const auto MIN_EMIT_INTERVAL = std::chrono::milliseconds(100);
        
        if (now - last_assistant_emit_time_ < MIN_EMIT_INTERVAL) {
            spdlog::trace("[AgentInstance] Assistant delta throttled: buffer_len={}", 
                         assistant_buffer_.length());
            return;  // 跳过本次发送
        }
        
        // 3. TODO: 剥离 <think> 标签 (OpenClaw stripBlockTags)
        // 简化版本: 暂时不处理 <think> 标签
        std::string cleaned = assistant_buffer_;
        
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
            emit_lifecycle_event(AgentLifecyclePhase::Responding, {
                {"stream", "assistant"},
                {"text", cleaned},       // 完整文本
                {"delta", delta_text}    // 增量文本
            });
            
            last_streamed_assistant_ = cleaned;
            last_assistant_emit_time_ = now;
            
            spdlog::debug("[AgentInstance] Assistant stream: total_len={}, delta_len={}", 
                         cleaned.length(), delta_text.length());
        } else {
            spdlog::trace("[AgentInstance] Assistant delta no-op: no new text");
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
    callbacks.on_block_reply = [this](const std::string& block) {
        // 1. 动态抑制检测 (对齐 OpenClaw:478)
        if (suppress_block_chunks_) {
            spdlog::trace("[AgentInstance] Block reply suppressed by suppress_block_chunks_");
            return;
        }
        
        // 2. Strip <think> 和 <final> 标签 (对齐 OpenClaw:484)
        // TODO: 实现完整的 stripBlockTags 逻辑
        std::string cleaned = block;
        // 简化版本: 移除 <think>...</think> 标签
        // 正则表达式: <think>.*?</think>
        // 暂时跳过，直接使用原始文本
        
        // Trim 结尾空白 (对齐 OpenClaw:484)
        while (!cleaned.empty() && std::isspace(cleaned.back())) {
            cleaned.pop_back();
        }
        
        if (cleaned.empty()) {
            spdlog::trace("[AgentInstance] Block reply empty after cleaning");
            return;
        }
        
        // 3. 去重检查 (对齐 OpenClaw:488-490)
        if (cleaned == last_block_reply_text_) {
            spdlog::trace("[AgentInstance] Block reply duplicate: already sent");
            return;
        }
        
        // 4. Messaging tool 冲突检测 (对齐 OpenClaw:494-498)
        // 检查是否已通过 messaging tool 发送过
        for (const auto& sent_text : messaging_tool_sent_texts_) {
            if (cleaned == sent_text) {
                spdlog::debug("[AgentInstance] Block reply skip: already sent via messaging tool");
                return;
            }
        }
        
        // 5. 更新状态 (对齐 OpenClaw:504-506)
        last_block_reply_text_ = cleaned;
        // assistantTexts.push(chunk) - 对应 C++ 中可以记录到 memory
        
        // 6. TODO: Parse reply directives (对齐 OpenClaw:510-521)
        // - replyDirectiveAccumulator.consume(chunk)
        // - 提取 mediaUrls, audioAsVoice, replyToId 等
        // 简化版本: 直接使用 text
        
        // 7. 跳过空 payload (对齐 OpenClaw:523-525)
        // (已通过上面的 empty 检查)
        
        // 8. 发射 WebSocket 事件 (对齐 OpenClaw:526-533)
        emit_lifecycle_event(AgentLifecyclePhase::Responding, {
            {"type", "block_reply"},
            {"text", cleaned},
            // TODO: 添加可选字段
            // {"mediaUrls", mediaUrls},
            // {"audioAsVoice", audioAsVoice},
            // {"replyToId", replyToId},
            // {"replyToTag", replyToTag},
            // {"replyToCurrent", replyToCurrent}
        });
        
        spdlog::debug("[AgentInstance] Block reply sent: len={}", cleaned.length());
    };
    
    callbacks.on_tool_start = [this](const std::string& tool_name, const core::JsonValue& args) {
        spdlog::info("[AgentInstance] Tool start: {}", tool_name);
        emit_lifecycle_event(AgentLifecyclePhase::ToolExecution, {
            {"tool", tool_name},
            {"action", "start"}
        });
    };
    
    callbacks.on_tool_end = [this](const std::string& tool_name, const tools::ToolResult& result) {
        spdlog::info("[AgentInstance] Tool end: {} (success={})", tool_name, result.success);
        emit_lifecycle_event(AgentLifecyclePhase::ToolExecution, {
            {"tool", tool_name},
            {"action", "end"},
            {"success", result.success}
        });
    };
    
    // ✅ 严格对齐 OpenClaw: 加载历史消息
    std::vector<react::LLMMessage> history;
    if (memory_mgr_) {
        // ✅ OpenClaw 风格: 通过 Vector Search 检索相关历史
        // 
        // OpenClaw 实现 (src/agents/agent.ts):
        // 1. 使用当前消息作为查询
        // 2. 搜索 top-K 相关历史对话
        // 3. 按时间戳排序
        // 4. 转换为 LLMMessage 格式
        // 5. 限制总 token 数 (避免超出上下文窗口)
        
        try {
            // 搜索相关历史 (使用当前消息作为查询)
            memory::SearchOptions search_opts;
            search_opts.max_results = 6;          // 最多 6 条相关记忆
            search_opts.min_score = 0.35;         // 最低相似度 0.35
            search_opts.filters["session_id"] = ctx_.session_id;  // 限制到当前会话
            
            auto search_result = memory_mgr_->search(message, search_opts);
            
            if (!search_result.empty()) {
                // 按时间戳排序 (从旧到新)
                std::vector<memory::MemoryItem> sorted_items = search_result;
                std::sort(sorted_items.begin(), sorted_items.end(),
                    [](const memory::MemoryItem& a, const memory::MemoryItem& b) {
                        return a.timestamp_ms < b.timestamp_ms;
                    });
                
                // 转换为 LLMMessage
                for (const auto& item : sorted_items) {
                    react::LLMMessage msg;
                    
                    // 从 metadata 获取角色
                    std::string role = "user";  // 默认
                    if (item.metadata.count("role")) {
                        role = item.metadata.at("role");
                    }
                    
                    msg.role = role;
                    msg.content = item.content;
                    
                    history.push_back(msg);
                }
                
                spdlog::debug("[AgentInstance] Loaded {} historical messages", history.size());
            } else {
                spdlog::debug("[AgentInstance] No relevant history found");
            }
        } catch (const std::exception& e) {
            spdlog::warn("[AgentInstance] Failed to load history: {}", e.what());
        }
    } else {
        // 没有 Memory,当前是独立对话
        spdlog::debug("[AgentInstance] No memory available, starting fresh conversation");
    }
    
    // ✅ Overflow 重试循环 (对齐 OpenClaw)
    for (int attempt = 0; attempt < MAX_OVERFLOW_ATTEMPTS; ++attempt) {
        try {
            // 尝试执行 ReAct
            auto result = react_engine_->run(message, history, callbacks);
            
            // 检查是否成功
            if (result.success) {
                // ✅ 保存到 Memory
                if (memory_mgr_) {
                    try {
                        // 保存用户消息和 Agent 响应
                        memory::MemoryItem user_msg;
                        user_msg.content = message;
                        user_msg.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count();
                        user_msg.metadata["role"] = "user";
                        user_msg.metadata["session_id"] = ctx_.session_id;
                        
                        memory::MemoryItem agent_msg;
                        agent_msg.content = result.final_response;
                        agent_msg.timestamp_ms = user_msg.timestamp_ms + result.duration_ms;
                        agent_msg.metadata["role"] = "assistant";
                        agent_msg.metadata["session_id"] = ctx_.session_id;
                        agent_msg.metadata["steps"] = std::to_string(result.total_steps);
                        agent_msg.metadata["tool_calls"] = std::to_string(result.tool_calls);
                        
                        memory_mgr_->add(user_msg);
                        memory_mgr_->add(agent_msg);
                        
                        spdlog::debug("[AgentInstance] Saved conversation to memory");
                    } catch (const std::exception& e) {
                        spdlog::warn("[AgentInstance] Failed to save to memory: {}", e.what());
                    }
                }
                
                spdlog::info("[AgentInstance] Message processed: steps={}, tool_calls={}, duration={}ms",
                    result.total_steps, result.tool_calls, result.duration_ms);
                
                return core::Result<std::string>::Ok(result.final_response);
            }
            
            // ✅ 检测 Context Overflow Error (对齐 OpenClaw)
            if (react_engine_->is_context_overflow_error(result.error)) {
                spdlog::warn(
                    "[AgentInstance] Context overflow detected (attempt {}/{}): {}",
                    attempt + 1, MAX_OVERFLOW_ATTEMPTS, result.error
                );
                
                // 提取 overflow token count (用于调试)
                auto observed_tokens = react_engine_->extract_overflow_token_count(result.error);
                if (observed_tokens.has_value()) {
                    spdlog::info("[AgentInstance] Observed overflow token count: {}", *observed_tokens);
                }
                
                // ✅ 触发 Compaction (对齐 OpenClaw)
                if (memory_mgr_) {
                    spdlog::info("[AgentInstance] Triggering compaction to resolve overflow...");
                    
                    // 直接调用 ReactEngine 的 trigger_compaction
                    // (history 会被更新)
                    auto compact_result = react_engine_->trigger_compaction(history);
                    
                    if (compact_result.compacted) {
                        spdlog::info("[AgentInstance] Compaction successful, retrying (attempt {}/{})",
                                   attempt + 1, MAX_OVERFLOW_ATTEMPTS);
                        continue;  // 重试
                    } else {
                        spdlog::error("[AgentInstance] Compaction failed: {}", compact_result.reason);
                    }
                } else {
                    spdlog::error("[AgentInstance] MemoryManager not initialized, cannot compact");
                }
                
                // Compaction 失败 - 如果还有重试机会则继续,否则返回错误
                if (attempt + 1 < MAX_OVERFLOW_ATTEMPTS) {
                    continue;
                } else {
                    return core::Result<std::string>::Err(
                        "Context overflow after max compaction retries (" + 
                        std::to_string(MAX_OVERFLOW_ATTEMPTS) + ")"
                    );
                }
            }
            
            // 其他错误 - 直接返回
            return core::Result<std::string>::Err(result.error);
            
        } catch (const std::exception& e) {
            spdlog::error("[AgentInstance] ReactEngine exception: {}", e.what());
            
            // 检测是否是 overflow 相关异常
            std::string error_msg = e.what();
            if (react_engine_->is_context_overflow_error(error_msg)) {
                spdlog::warn("[AgentInstance] Context overflow exception (attempt {}/{})",
                           attempt + 1, MAX_OVERFLOW_ATTEMPTS);
                
                // 尝试 compaction 并重试
                if (memory_mgr_ && attempt + 1 < MAX_OVERFLOW_ATTEMPTS) {
                    auto compact_result = react_engine_->trigger_compaction(history);
                    if (compact_result.compacted) {
                        continue;  // 重试
                    }
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
