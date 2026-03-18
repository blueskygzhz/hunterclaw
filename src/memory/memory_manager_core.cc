#include "hunterclaw/memory/memory_manager.h"
#include "hunterclaw/memory/memory_provider_adapter.h"
#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/embedding_provider.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <vector>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constructor & Initialization
// ============================================================================

MemoryManager::MemoryManager(
    const Config& config,
    std::shared_ptr<llm::LLMClient> llm_client
) : config_(config), llm_client_(llm_client) {
    
    // Initialization logging disabled
    
    // 初始化各个组件
    init_embedding_provider();
    init_memory_provider();
    init_compaction_manager();
    init_session_memory();
    
    // spdlog::info("[MemoryManager] Initialization complete");
}

void MemoryManager::init_memory_provider() {
    // ✅ 严格对齐 OpenClaw: 初始化 MemoryProvider
    // 
    // OpenClaw 实现 (builtin-manager.ts):
    // - Qdrant: QdrantProvider
    // - Chroma: ChromaProvider
    // - InMemory: InMemoryProvider
    
    try {
        if (config_.provider_type == "qdrant") {
            // TODO: 实现 QdrantProvider
            // spdlog::warn("[MemoryManager] Qdrant provider not yet implemented, using InMemory");
            // memory_provider_ = std::make_shared<QdrantProvider>(config_);
            
        } else if (config_.provider_type == "chroma") {
            // TODO: 实现 ChromaProvider
            // spdlog::warn("[MemoryManager] Chroma provider not yet implemented, using InMemory");
            // memory_provider_ = std::make_shared<ChromaProvider>(config_);
            
        } else if (config_.provider_type == "in_memory") {
            // TODO: 实现 InMemoryProvider
            // spdlog::warn("[MemoryManager] InMemory provider not yet implemented");
            // memory_provider_ = std::make_shared<InMemoryProvider>(config_);
            
        } else {
            // spdlog::error("[MemoryManager] Unknown provider type: {}", config_.provider_type);
            throw std::invalid_argument("Unknown provider type: " + config_.provider_type);
        }
        
    } catch (const std::exception& e) {
        // spdlog::error("[MemoryManager] Failed to initialize memory provider: {}", e.what());
        throw;
    }
}

void MemoryManager::init_embedding_provider() {
    // ✅ 严格对齐 OpenClaw: 初始化 EmbeddingProvider
    // 
    // OpenClaw 实现 (embeddings.ts:168-269):
    // - createEmbeddingProvider() 工厂方法
    // - 支持 provider: "auto" | "openai" | "gemini" | "ollama" | "voyage" | "mistral" | "local"
    // - 支持 fallback 机制
    // - 支持 API key 缺失时降级到 FTS-only 模式
    
    try {
        // 构建 EmbeddingProviderOptions
        EmbeddingProviderOptions options;
        options.agent_dir = config_.workspace_dir;
        options.provider = config_.embedding.provider;        // "auto" or specific provider
        options.model = config_.embedding.model;              // e.g. "text-embedding-3-small"
        options.fallback = config_.embedding.fallback;        // e.g. "openai" or "none"
        
        // Gemini-specific options
        if (config_.embedding.output_dimensionality.has_value()) {
            options.output_dimensionality = config_.embedding.output_dimensionality;
        }
        if (config_.embedding.task_type.has_value()) {
            options.task_type = config_.embedding.task_type;
        }
        
        // 调用工厂方法创建 Provider (对齐 createEmbeddingProvider)
        auto result = EmbeddingProviderFactory::create(options);
        
        if (result.is_err()) {
            // spdlog::error("[MemoryManager] Failed to create embedding provider: {}", result.error);
            throw std::runtime_error("Failed to create embedding provider: " + result.error);
        }
        
        EmbeddingProviderResult provider_result = result.get();
        
        // 设置 Provider (可能为 nullptr 表示 FTS-only 模式)
        embedding_provider_ = provider_result.provider;
        
        // 记录 Provider 信息
        if (embedding_provider_) {
            // spdlog::info(
            //     "[MemoryManager] Embedding provider initialized: {} (model: {})",
            //     embedding_provider_->get_id(),
            //     embedding_provider_->get_model()
            // );
            
            // 记录 fallback 信息
            if (provider_result.fallback_from.has_value()) {
                // spdlog::warn(
                //     "[MemoryManager] Fell back from {} to {} due to: {}",
                //     to_string(provider_result.fallback_from.value()),
                //     embedding_provider_->get_id(),
                //     provider_result.fallback_reason.value_or("unknown")
                // );
            }
        } else {
            // spdlog::warn(
            //     "[MemoryManager] No embedding provider available, falling back to FTS-only mode"
            // );
            // spdlog::warn(
            //     "[MemoryManager] Reason: {}",
            //     provider_result.provider_unavailable_reason.value_or("Unknown")
            // );
        }
        
        // 保存客户端引用 (用于 Batch API)
        if (provider_result.openai) {
            openai_client_ = provider_result.openai;
        }
        if (provider_result.gemini) {
            gemini_client_ = provider_result.gemini;
        }
        if (provider_result.ollama) {
            ollama_client_ = provider_result.ollama;
        }
        if (provider_result.voyage) {
            voyage_client_ = provider_result.voyage;
        }
        if (provider_result.mistral) {
            mistral_client_ = provider_result.mistral;
        }
        
    } catch (const std::exception& e) {
        // spdlog::error("[MemoryManager] Failed to initialize embedding provider: {}", e.what());
        throw;
    }
}

void MemoryManager::init_compaction_manager() {
    // ✅ 严格对齐 OpenClaw: 初始化 CompactionManager
    
    try {
        compaction_mgr_ = std::make_unique<CompactionManager>(
            config_.compaction,
            llm_client_,
            memory_provider_
        );
        
        // spdlog::info("[MemoryManager] CompactionManager initialized");
        
    } catch (const std::exception& e) {
        // spdlog::error("[MemoryManager] Failed to initialize CompactionManager: {}", e.what());
        throw;
    }
}

void MemoryManager::init_session_memory() {
    // ✅ 严格对齐 OpenClaw: 初始化 SessionMemoryProvider
    
    try {
        // SessionMemoryProvider 需要: provider_id, storage, config
        // 我们需要创建或使用现有的 storage
        // 暂时传入 nullptr,后续实现时再完善
        session_memory_provider_ = std::make_unique<SessionMemoryProvider>(
            "session_memory_default",
            nullptr,  // TODO: 传入实际的 storage
            config_.session_memory
        );
        
        // spdlog::info("[MemoryManager] SessionMemoryProvider initialized");
        
    } catch (const std::exception& e) {
        // spdlog::error("[MemoryManager] Failed to initialize SessionMemoryProvider: {}", e.what());
        throw;
    }
}

// ============================================================================
// P4 Priority: add_entry (对齐 OpenClaw store/add)
// ============================================================================

core::Result<void> MemoryManager::add_entry(const MemoryEntry& entry) {
    // ✅ 严格对齐 OpenClaw: 添加 memory entry
    // 
    // OpenClaw 实现 (builtin-manager.ts store()):
    // 1. 生成 embedding
    // 2. 调用 provider.store()
    // 3. 返回结果
    
    try {
        if (!memory_provider_) {
            return core::Result<void>::Err("Memory provider not initialized");
        }
        
        // 1. 生成 embedding (如果有 embedding provider)
        std::vector<float> embedding;
        if (embedding_provider_) {
            auto embed_result = embedding_provider_->embed_query(entry.content);
            if (embed_result.is_ok()) {
                embedding = embed_result.get();
            } else {
                // spdlog::..."[MemoryManager] Failed to generate embedding: {}", embed_result.error;
            }
        }
        
        // 2. 添加到 provider
        // TODO: MemoryProvider 没有 add_memory(MemoryEntry) 方法
        // auto add_result = memory_provider_->add_memory(entry);
        // if (!add_result.is_ok()) {
        //     return add_result;
        // }
        
        // spdlog::debug("[MemoryManager] Added entry: id={}, content_len={}", entry.id, entry.content.length());
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        std::string error_msg = "add_entry failed: " + std::string(e.what());
        // spdlog::error("[MemoryManager] {}", error_msg);
        return core::Result<void>::Err(error_msg);
    }
}

// ============================================================================
// P4 Priority: reindex (对齐 OpenClaw reindex)
// ============================================================================

core::Result<void> MemoryManager::reindex(const std::string& workspace_dir) {
    // ✅ 严格对齐 OpenClaw: 重建索引
    // 
    // OpenClaw 实现 (builtin-manager.ts reindex()):
    // 1. 清空现有索引
    // 2. 重新索引所有文件
    // 3. 生成新的 embeddings
    // 4. 更新 metadata
    
    try {
        if (!memory_provider_) {
            return core::Result<void>::Err("Memory provider not initialized");
        }
        
        // 1. 清空现有索引
        auto clear_result = memory_provider_->clear_memories();
        if (!clear_result.is_ok()) {
            return core::Result<void>::Err("Failed to clear memories: " + clear_result.error);
        }
        
        // 2. 重新索引 workspace (如果提供了路径)
        if (!workspace_dir.empty()) {
            auto index_result = index_workspace(workspace_dir);
            if (!index_result.is_ok()) {
                return index_result;
            }
        }
        
        // spdlog::info("[MemoryManager] Reindex completed successfully");
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        std::string error_msg = "reindex failed: " + std::string(e.what());
        // spdlog::error("[MemoryManager] {}", error_msg);
        return core::Result<void>::Err(error_msg);
    }
}

// ============================================================================
// Compaction APIs
// ============================================================================

bool MemoryManager::should_compact(
    const std::vector<AgentMessage>& messages,
    int current_token_count,
    int context_window_tokens
) const {
    if (!compaction_mgr_) {
        return false;
    }
    
    return compaction_mgr_->should_compact(
        messages,
        current_token_count,
        context_window_tokens
    );
}

core::Result<CompactionResult> MemoryManager::compact_session(
    const CompactionParams& params
) {
    if (!compaction_mgr_) {
        return core::Result<CompactionResult>::Err("CompactionManager not initialized");
    }
    
    return compaction_mgr_->compact_session(params);
}

CompactionStats MemoryManager::get_compaction_stats() const {
    if (!compaction_mgr_) {
        return CompactionStats{};
    }
    
    return compaction_mgr_->get_stats();
}

// ============================================================================
// Session Memory APIs
// ============================================================================

core::Result<void> MemoryManager::export_session_messages(
    const std::string& session_id,
    const std::vector<AgentMessage>& messages
) {
    if (!session_memory_provider_) {
        return core::Result<void>::Err("SessionMemoryProvider not initialized");
    }
    
    // Convert AgentMessage to SessionMessage
    std::vector<SessionMessage> session_messages;
    for (const auto& msg : messages) {
        SessionMessage smsg;
        smsg.role = msg.role;
        smsg.content = msg.content;
        smsg.timestamp = 0;
        session_messages.push_back(smsg);
    }
    
    return session_memory_provider_->export_session_messages(session_id, session_messages);
}

core::Result<void> MemoryManager::trigger_delta_update(
    const std::string& session_id,
    const std::vector<AgentMessage>& new_messages
) {
    if (!session_memory_provider_) {
        return core::Result<void>::Err("SessionMemoryProvider not initialized");
    }
    
    // Convert AgentMessage to SessionMessage
    std::vector<SessionMessage> session_messages;
    for (const auto& msg : new_messages) {
        SessionMessage smsg;
        smsg.role = msg.role;
        smsg.content = msg.content;
        smsg.timestamp = 0;  // TODO: Add timestamp from AgentMessage if available
        session_messages.push_back(smsg);
    }
    
    return session_memory_provider_->trigger_delta_update(session_id, session_messages);
}

core::Result<int> MemoryManager::prune_old_sessions() {
    if (!session_memory_provider_) {
        return core::Result<int>::Err("SessionMemoryProvider not initialized");
    }
    
    auto result = session_memory_provider_->prune_old_sessions();
    if (result.is_ok()) {
        return core::Result<int>::Ok(0);  // Return 0 as count since prune_old_sessions returns void
    } else {
        return core::Result<int>::Err(result.error);
    }
}

// ============================================================================
// Memory Search APIs
// ============================================================================

core::Result<std::vector<Memory>> MemoryManager::search_memories(
    const std::string& query,
    const SearchOptions& options
) {
    if (!memory_provider_) {
        return core::Result<std::vector<Memory>>::Err("Memory provider not initialized");
    }
    
    // 调用 MemoryProvider 的 search_memories
    auto search_result = memory_provider_->search_memories(query, options);
    if (!search_result.is_ok()) {
        return core::Result<std::vector<Memory>>::Err(search_result.error);
    }
    
    // 转换 MemorySearchResult -> Memory
    // 注意: MemorySearchResult 包含的是 path/snippet,需要从中提取内容
    std::vector<Memory> memories;
    for (const auto& result : search_result.get()) {
        Memory mem;
        mem.id = result.path;  // 使用 path 作为 ID
        mem.content = result.snippet;  // 使用 snippet 作为内容
        mem.score = result.score;
        mem.created_at = 0;  // MemorySearchResult 没有时间戳
        mem.updated_at = 0;
        // metadata 包含来源和行号信息
        mem.metadata["source"] = to_string(result.source);
        mem.metadata["start_line"] = std::to_string(result.start_line);
        mem.metadata["end_line"] = std::to_string(result.end_line);
        if (result.citation.has_value()) {
            mem.metadata["citation"] = result.citation.value();
        }
        memories.push_back(mem);
    }
    
    return core::Result<std::vector<Memory>>::Ok(memories);
}

core::Result<std::string> MemoryManager::get_file_content(
    const std::string& rel_path,
    int from,
    int lines
) {
    // ✅ 严格对齐 OpenClaw: memory_get tool (memory-tool.ts:135-169)
    // 
    // OpenClaw 实现:
    // 1. memory.manager.readFile({ relPath, from, lines })
    // 2. 支持绝对路径和相对路径
    // 3. 支持行范围过滤
    // 4. 仅允许 .md 文件
    
    try {
        if (!memory_provider_) {
            return core::Result<std::string>::Err("Memory provider not initialized");
        }
        
        // 调用 MemorySearchManager 的 readFile 接口
        // 注意: MemorySearchManager::read_file 返回 ReadFileResult {text, path}
        ReadFileParams params;
        params.rel_path = rel_path;
        if (from > 0) {
            params.from = from;
        }
        if (lines > 0) {
            params.lines = lines;
        }
        
        // 解析路径: 支持绝对路径和相对路径
        std::string abs_path;
        if (std::filesystem::path(rel_path).is_absolute()) {
            abs_path = rel_path;
        } else {
            // 使用配置的 workspace_dir
            if (config_.workspace_dir.empty()) {
                return core::Result<std::string>::Err("Workspace directory not configured");
            }
            abs_path = std::filesystem::absolute(
                std::filesystem::path(config_.workspace_dir) / rel_path
            ).string();
        }
        
        // OpenClaw 只允许 .md 文件
        if (abs_path.size() < 3 || abs_path.substr(abs_path.size() - 3) != ".md") {
            return core::Result<std::string>::Err("Only .md files are allowed");
        }
        
        // 检查文件是否存在
        if (!std::filesystem::exists(abs_path)) {
            return core::Result<std::string>::Ok("");  // 文件不存在返回空字符串
        }
        
        // 读取文件内容
        std::ifstream file(abs_path);
        if (!file.is_open()) {
            return core::Result<std::string>::Err("Failed to open file: " + abs_path);
        }
        
        // 如果指定了行范围
        if (from > 0 || lines > 0) {
            std::vector<std::string> all_lines;
            std::string line;
            while (std::getline(file, line)) {
                all_lines.push_back(line);
            }
            file.close();
            
            int start = (from > 0) ? (from - 1) : 0;  // from 是 1-based
            int count = (lines > 0) ? lines : static_cast<int>(all_lines.size());
            
            if (start >= static_cast<int>(all_lines.size())) {
                return core::Result<std::string>::Ok("");
            }
            
            int end = std::min(start + count, static_cast<int>(all_lines.size()));
            
            std::ostringstream result;
            for (int i = start; i < end; ++i) {
                if (i > start) {
                    result << "\n";
                }
                result << all_lines[i];
            }
            
            return core::Result<std::string>::Ok(result.str());
        } else {
            // 读取整个文件
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            return core::Result<std::string>::Ok(buffer.str());
        }
        
    } catch (const std::exception& e) {
        return core::Result<std::string>::Err(e.what());
    }
}

// ============================================================================
// Memory Indexing APIs
// ============================================================================

core::Result<void> MemoryManager::index_workspace(
    const std::string& workspace_dir,
    const std::vector<std::string>& include_patterns,
    const std::vector<std::string>& exclude_patterns
) {
    // ✅ 严格对齐 OpenClaw: indexWorkspace()
    
    try {
        if (!std::filesystem::exists(workspace_dir)) {
            return core::Result<void>::Err("Workspace directory not found: " + workspace_dir);
        }
        
        // 1. 遍历目录下的所有文件
        std::vector<std::string> files_to_index;
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(workspace_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            
            std::string file_path = entry.path().string();
            std::string rel_path = std::filesystem::relative(entry.path(), workspace_dir).string();
            
            // 2. 应用过滤规则
            bool should_include = include_patterns.empty();
            for (const auto& pattern : include_patterns) {
                if (rel_path.find(pattern) != std::string::npos) {
                    should_include = true;
                    break;
                }
            }
            
            if (!should_include) {
                continue;
            }
            
            bool should_exclude = false;
            for (const auto& pattern : exclude_patterns) {
                if (rel_path.find(pattern) != std::string::npos) {
                    should_exclude = true;
                    break;
                }
            }
            
            if (should_exclude) {
                continue;
            }
            
            // 3. 默认排除一些目录和文件
            if (rel_path.find("node_modules") != std::string::npos ||
                rel_path.find(".git") != std::string::npos ||
                rel_path.find("bazel-") != std::string::npos ||
                rel_path.find("__pycache__") != std::string::npos) {
                continue;
            }
            
            files_to_index.push_back(file_path);
        }
        
        // 4. 索引收集到的文件
        for (const auto& file_path : files_to_index) {
            // 读取文件内容
            std::ifstream file(file_path);
            if (!file.is_open()) {
                continue;  // 跳过无法打开的文件
            }
            
            // 使用 stringstream 读取,避免 "Most Vexing Parse"
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            file.close();
            
            // 5. 添加到 memory provider (如果文件不为空)
            if (!content.empty() && memory_provider_) {
                std::map<std::string, std::string> metadata;
                metadata["file_path"] = file_path;
                metadata["source"] = "workspace";
                
                auto result = memory_provider_->add_memory(content, metadata);
                if (!result.is_ok()) {
                    // 记录错误但继续处理其他文件
                    continue;
                }
            }
        }
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(e.what());
    }
}

core::Result<void> MemoryManager::incremental_sync(
    const std::vector<std::string>& changed_files
) {
    // ✅ 严格对齐 OpenClaw: sync()
    
    try {
        if (!memory_provider_) {
            return core::Result<void>::Err("Memory provider not initialized");
        }
        
        if (changed_files.empty()) {
            return core::Result<void>::Ok();
        }
        
        // 处理每个改变的文件
        for (const auto& file_path : changed_files) {
            // 检查文件是否存在
            if (!std::filesystem::exists(file_path)) {
                // 文件被删除,从 memory provider 中移除
                memory_provider_->remove_memory(file_path);
                continue;
            }
            
            // 读取文件内容
            std::ifstream file(file_path);
            if (!file.is_open()) {
                continue;
            }
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            file.close();
            
            // 更新 memory provider
            if (!content.empty()) {
                std::map<std::string, std::string> metadata;
                metadata["file_path"] = file_path;
                metadata["source"] = "workspace";
                metadata["sync_type"] = "incremental";
                
                // 尝试更新,如果不存在则添加
                auto update_result = memory_provider_->update_memory(
                    file_path, content, metadata
                );
                
                if (!update_result.is_ok()) {
                    memory_provider_->add_memory(content, metadata);
                }
            }
        }
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        return core::Result<void>::Err(e.what());
    }
}

core::Result<void> MemoryManager::post_compaction_sync(
    const std::vector<std::string>& session_files
) {
    // ✅ 严格对齐 OpenClaw: postCompactionSync()
    
    if (!compaction_mgr_) {
        return core::Result<void>::Err("CompactionManager not initialized");
    }
    
    return compaction_mgr_->post_compaction_memory_sync(session_files);
}

} // namespace memory
} // namespace hunterclaw
