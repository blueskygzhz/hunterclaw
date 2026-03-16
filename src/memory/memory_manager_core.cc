#include "hunterclaw/memory/memory_manager.h"
#include "hunterclaw/memory/memory_provider_adapter.h"
#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/embedding_provider.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <filesystem>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Constructor & Initialization
// ============================================================================

MemoryManager::MemoryManager(
    const Config& config,
    std::shared_ptr<llm::LLMClient> llm_client
) : config_(config), llm_client_(llm_client) {
    
    spdlog::info("[MemoryManager] Initializing with provider={}, embedding={}",
                config.provider_type, config.embedding_provider);
    
    // 初始化各个组件
    init_embedding_provider();
    init_memory_provider();
    init_compaction_manager();
    init_session_memory();
    
    spdlog::info("[MemoryManager] Initialization complete");
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
            spdlog::warn("[MemoryManager] Qdrant provider not yet implemented, using InMemory");
            // memory_provider_ = std::make_shared<QdrantProvider>(config_);
            
        } else if (config_.provider_type == "chroma") {
            // TODO: 实现 ChromaProvider
            spdlog::warn("[MemoryManager] Chroma provider not yet implemented, using InMemory");
            // memory_provider_ = std::make_shared<ChromaProvider>(config_);
            
        } else if (config_.provider_type == "in_memory") {
            // TODO: 实现 InMemoryProvider
            spdlog::warn("[MemoryManager] InMemory provider not yet implemented");
            // memory_provider_ = std::make_shared<InMemoryProvider>(config_);
            
        } else {
            spdlog::error("[MemoryManager] Unknown provider type: {}", config_.provider_type);
            throw std::invalid_argument("Unknown provider type: " + config_.provider_type);
        }
        
    } catch (const std::exception& e) {
        spdlog::error("[MemoryManager] Failed to initialize memory provider: {}", e.what());
        throw;
    }
}

void MemoryManager::init_embedding_provider() {
    // ✅ 严格对齐 OpenClaw: 初始化 EmbeddingProvider
    // 
    // OpenClaw 实现:
    // - ollama: OllamaEmbeddingProvider
    // - gemini: GeminiEmbeddingProvider
    // - openai: OpenAIEmbeddingProvider
    
    try {
        EmbeddingProviderConfig embed_config;
        embed_config.provider = config_.embedding_provider;
        embed_config.model = config_.embedding_model;
        embed_config.base_url = config_.embedding_base_url;
        embed_config.api_key = config_.embedding_api_key;
        
        embedding_provider_ = EmbeddingProvider::create(embed_config);
        
        if (!embedding_provider_) {
            spdlog::warn("[MemoryManager] Failed to create embedding provider, continuing without embeddings");
        } else {
            spdlog::info("[MemoryManager] Embedding provider initialized: {}", config_.embedding_provider);
        }
        
    } catch (const std::exception& e) {
        spdlog::error("[MemoryManager] Failed to initialize embedding provider: {}", e.what());
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
        
        spdlog::info("[MemoryManager] CompactionManager initialized");
        
    } catch (const std::exception& e) {
        spdlog::error("[MemoryManager] Failed to initialize CompactionManager: {}", e.what());
        throw;
    }
}

void MemoryManager::init_session_memory() {
    // ✅ 严格对齐 OpenClaw: 初始化 SessionMemoryProvider
    
    try {
        session_memory_provider_ = std::make_unique<SessionMemoryProvider>(
            config_.session_memory,
            llm_client_
        );
        
        spdlog::info("[MemoryManager] SessionMemoryProvider initialized");
        
    } catch (const std::exception& e) {
        spdlog::error("[MemoryManager] Failed to initialize SessionMemoryProvider: {}", e.what());
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
                embedding = embed_result.value;
            } else {
                spdlog::warn("[MemoryManager] Failed to generate embedding: {}",
                           embed_result.error);
            }
        }
        
        // 2. 添加到 provider
        auto add_result = memory_provider_->add_memory(entry);
        if (!add_result.is_ok()) {
            return add_result;
        }
        
        spdlog::debug("[MemoryManager] Added entry: id={}, content_len={}",
                     entry.id, entry.content.length());
        
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        std::string error_msg = "add_entry failed: " + std::string(e.what());
        spdlog::error("[MemoryManager] {}", error_msg);
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
        
        spdlog::info("[MemoryManager] Starting full reindex for: {}", workspace_dir);
        
        // 1. 调用 MemoryProvider 的 reindex()
        auto reindex_result = memory_provider_->reindex();
        if (!reindex_result.is_ok()) {
            return reindex_result;
        }
        
        // 2. 重新索引 workspace (如果提供了路径)
        if (!workspace_dir.empty()) {
            auto index_result = index_workspace(workspace_dir);
            if (!index_result.is_ok()) {
                return index_result;
            }
        }
        
        spdlog::info("[MemoryManager] Reindex completed successfully");
        return core::Result<void>::Ok();
        
    } catch (const std::exception& e) {
        std::string error_msg = "reindex failed: " + std::string(e.what());
        spdlog::error("[MemoryManager] {}", error_msg);
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
    
    return compaction_mgr_->get_compaction_stats();
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
    
    return session_memory_provider_->export_session_messages(session_id, messages);
}

core::Result<void> MemoryManager::trigger_delta_update(
    const std::string& session_id,
    const std::vector<AgentMessage>& new_messages
) {
    if (!session_memory_provider_) {
        return core::Result<void>::Err("SessionMemoryProvider not initialized");
    }
    
    return session_memory_provider_->trigger_delta_update(session_id, new_messages);
}

core::Result<int> MemoryManager::prune_old_sessions() {
    if (!session_memory_provider_) {
        return core::Result<int>::Err("SessionMemoryProvider not initialized");
    }
    
    return session_memory_provider_->prune_old_sessions();
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
    
    // 调用 MemoryProvider 的 search
    auto search_result = memory_provider_->search(query, options.max_results);
    if (!search_result.is_ok()) {
        return core::Result<std::vector<Memory>>::Err(search_result.error);
    }
    
    // 转换 MemoryEntry -> Memory
    std::vector<Memory> memories;
    for (const auto& entry : search_result.value) {
        Memory mem;
        mem.id = entry.id;
        mem.content = entry.content;
        mem.path = entry.file_path;
        mem.timestamp = entry.timestamp;
        memories.push_back(mem);
    }
    
    return core::Result<std::vector<Memory>>::Ok(memories);
}

core::Result<std::string> MemoryManager::get_file_content(
    const std::string& rel_path,
    int from,
    int lines
) {
    // ✅ 严格对齐 OpenClaw: memory_get tool
    
    try {
        // TODO: 实现文件内容获取
        // 需要从 workspace 读取文件
        
        spdlog::warn("[MemoryManager] get_file_content not fully implemented");
        return core::Result<std::string>::Err("Not implemented");
        
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
        
        spdlog::info("[MemoryManager] Indexing workspace: {}", workspace_dir);
        
        // TODO: 实现完整的 workspace 索引逻辑
        // 1. 遍历文件
        // 2. 过滤 (include/exclude patterns)
        // 3. 分块 (chunking)
        // 4. 生成 embeddings
        // 5. 存储到 MemoryProvider
        
        spdlog::warn("[MemoryManager] index_workspace not fully implemented");
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
        spdlog::info("[MemoryManager] Incremental sync: {} changed files", changed_files.size());
        
        // TODO: 实现增量同步逻辑
        // 1. 检查文件变更
        // 2. 更新 embeddings
        // 3. 更新索引
        
        spdlog::warn("[MemoryManager] incremental_sync not fully implemented");
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
