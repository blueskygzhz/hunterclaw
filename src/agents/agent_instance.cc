#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace hunterclaw {
namespace agents {

AgentInstance::AgentInstance(
    const std::string& session_id,
    const std::string& agent_id,
    std::shared_ptr<core::Config> config,
    std::shared_ptr<skills::SkillManager> skill_mgr,
    std::shared_ptr<channels::ChannelManager> channel_mgr
) : config_(config),
    skill_mgr_(skill_mgr),
    channel_mgr_(channel_mgr) {
    
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
        // TODO: 从 ToolRegistry/ToolManager 获取工具管理器
        // 暂时使用占位 (需要全局 ToolManager 单例或从外部注入)
        std::shared_ptr<tools::ToolManager> tool_mgr;  // = get_tool_manager();
        
        // TODO: 从 Config 创建 LLMClient
        std::shared_ptr<llm::LLMClient> llm_client;  // = create_llm_client(ctx_.config.model);
        
        if (tool_mgr && llm_client) {
            // 配置 ReactEngine
            react::ReactConfig react_config;
            react_config.max_steps = 20;
            react_config.reasoning_mode = react::ReactConfig::ReasoningMode::Off;
            react_config.parallel_tool_calls = false;
            
            react_engine_ = react::ReactEngine::create(tool_mgr, llm_client, react_config);
            spdlog::info("[AgentInstance] ReactEngine initialized");
        } else {
            spdlog::warn("[AgentInstance] ReactEngine initialization skipped (missing dependencies)");
        }
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
    touch();
    
    spdlog::info("[AgentInstance] Processing message: session={}, message='{}'",
        ctx_.session_id, message);
    
    // ========== ReactEngine 集成 (对齐 OpenClaw) ==========
    
    if (!react_engine_) {
        return core::Result<std::string>::Err("ReactEngine not initialized");
    }
    
    // 1. 准备流式回调
    react::StreamCallbacks callbacks;
    callbacks.session_id = ctx_.session_id;
    
    // 可选: 发射事件到外部监听器
    callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
        // 推理流
        spdlog::debug("[AgentInstance] Reasoning: {}", reasoning);
    };
    
    callbacks.on_assistant_delta = [this](const std::string& text) {
        // 助手输出流
        spdlog::trace("[AgentInstance] Delta: {}", text);
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
    
    // 2. 加载历史消息 (从 Memory)
    std::vector<react::LLMMessage> history;
    if (ctx_.memory) {
        // TODO: 从 Memory 加载历史对话
        // auto memory_results = ctx_.memory->search(message);
        // ... 转换为 LLMMessage
    }
    
    // 3. 执行 ReAct 循环
    try {
        auto result = react_engine_->run(message, history, callbacks);
        
        if (!result.success) {
            return core::Result<std::string>::Err(result.error);
        }
        
        // 4. 保存到 Memory
        if (ctx_.memory) {
            // TODO: 将对话保存到 Memory
            // ctx_.memory->add_memory(message, {{"role", "user"}});
            // ctx_.memory->add_memory(result.final_response, {{"role", "assistant"}});
        }
        
        spdlog::info("[AgentInstance] Message processed: steps={}, tool_calls={}, duration={}ms",
            result.total_steps, result.tool_calls, result.duration_ms);
        
        return core::Result<std::string>::Ok(result.final_response);
        
    } catch (const std::exception& e) {
        spdlog::error("[AgentInstance] ReactEngine error: {}", e.what());
        return core::Result<std::string>::Err(std::string("ReactEngine failed: ") + e.what());
    }
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
    bool cleanup_temp_files = true;  // TODO: 从配置读取
    
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
        
        // TODO: 从 Agent 配置读取 embedding 设置
        std::string embedding_provider = "ollama";  // 默认 ollama
        std::string embedding_model = "nomic-embed-text";
        
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
        
        auto memory_mgr = memory::MemoryManager::create(storage, embedding, search_config);
        
        // 5. 将 MemoryManager 转换为 MemoryProvider (适配)
        // TODO: 创建 MemoryProvider 包装器
        // ctx_.memory = std::make_shared<MemoryProviderAdapter>(memory_mgr);
        
        spdlog::info("[AgentInstance] Memory initialized: session={}, db={}",
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

} // namespace agents
} // namespace hunterclaw
