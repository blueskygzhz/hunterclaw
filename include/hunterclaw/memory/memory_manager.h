#pragma once

#include "hunterclaw/memory/compaction_manager.h"
#include "hunterclaw/memory/memory_provider.h"
#include "hunterclaw/memory/session_memory_provider.h"
#include <memory>
#include <string>

namespace hunterclaw {
namespace memory {

// Forward declaration
class EmbeddingProvider;

/**
 * MemoryManager - 集成 compaction, session memory, embedding providers
 * 
 * 严格对齐 OpenClaw:
 * - BuiltinMemoryManager (builtin-manager.ts)
 * - CompactionManager (compact.ts + compaction_manager.cc)
 * - SessionMemoryProvider (session_memory_provider.cc)
 */
class MemoryManager {
public:
    struct Config {
        // Compaction 配置
        CompactionConfig compaction;
        
        // Session Memory 配置
        SessionMemoryConfig session_memory;
        
        // Memory Provider 配置
        std::string provider_type = "qdrant";  // "qdrant", "chroma", "in_memory"
        std::string provider_base_url;
        std::string provider_api_key;
        
        // Embedding Provider 配置
        std::string embedding_provider = "ollama";  // "ollama", "gemini", "openai"
        std::string embedding_model;
        std::string embedding_base_url;
        std::string embedding_api_key;
        
        // Workspace 配置 (对齐 OpenClaw)
        std::string workspace_dir;  // 工作空间根目录
    };
    
    explicit MemoryManager(
        const Config& config,
        std::shared_ptr<llm::LLMClient> llm_client = nullptr
    );
    
    // ========================================
    // Compaction APIs
    // ========================================
    
    /**
     * 检查是否需要压缩
     */
    bool should_compact(
        const std::vector<AgentMessage>& messages,
        int current_token_count,
        int context_window_tokens
    ) const;
    
    /**
     * 执行完整的 compaction 流程
     */
    core::Result<CompactionResult> compact_session(
        const CompactionParams& params
    );
    
    /**
     * 获取 compaction 统计
     */
    CompactionStats get_compaction_stats() const;
    
    // ========================================
    // Session Memory APIs
    // ========================================
    
    /**
     * 导出 session 消息到 memory (对齐 OpenClaw SessionMemoryProvider)
     */
    core::Result<void> export_session_messages(
        const std::string& session_id,
        const std::vector<AgentMessage>& messages
    );
    
    /**
     * Trigger delta 更新 (在 auto-reply 完成后调用)
     */
    core::Result<void> trigger_delta_update(
        const std::string& session_id,
        const std::vector<AgentMessage>& new_messages
    );
    
    /**
     * 清理过期 sessions
     */
    core::Result<int> prune_old_sessions();
    
    // ========================================
    // Memory Search APIs
    // ========================================
    
    /**
     * 搜索 memories (对齐 OpenClaw memory_search tool)
     */
    core::Result<std::vector<Memory>> search_memories(
        const std::string& query,
        const SearchOptions& options = SearchOptions{}
    );
    
    /**
     * 获取文件内容 (对齐 OpenClaw memory_get tool)
     */
    core::Result<std::string> get_file_content(
        const std::string& rel_path,
        int from = 0,
        int lines = 0
    );
    
    // ========================================
    // Memory Indexing APIs
    // ========================================
    
    /**
     * 索引 workspace 文件
     */
    core::Result<void> index_workspace(
        const std::string& workspace_dir,
        const std::vector<std::string>& include_patterns = {},
        const std::vector<std::string>& exclude_patterns = {}
    );
    
    /**
     * 增量索引 (对齐 OpenClaw sync)
     */
    core::Result<void> incremental_sync(
        const std::vector<std::string>& changed_files = {}
    );
    
    /**
     * Post-compaction memory sync (只更新 transcript files)
     */
    core::Result<void> post_compaction_sync(
        const std::vector<std::string>& session_files
    );
    
    // ========================================
    // P4 Priority: Direct Entry Management
    // ========================================
    
    /**
     * ✅ P4: 直接添加 memory entry (对齐 OpenClaw store)
     * 
     * 使用场景:
     * - 添加对话记忆 (conversation memory)
     * - 添加用户注释 (user annotations)
     * - 添加自定义 metadata
     * 
     * OpenClaw 对齐: builtin-manager.ts store()
     */
    core::Result<void> add_entry(const MemoryEntry& entry);
    
    /**
     * ✅ P4: 完整重建索引 (对齐 OpenClaw reindex)
     * 
     * 使用场景:
     * - Provider 配置变更
     * - Embedding model 变更
     * - Chunking 设置变更
     * - 修复索引损坏
     * 
     * OpenClaw 对齐: builtin-manager.ts reindex()
     * 
     * @param workspace_dir 工作空间路径 (可选,为空则只清空索引)
     */
    core::Result<void> reindex(const std::string& workspace_dir = "");
    
    // ========================================
    // Accessors
    // ========================================
    
    CompactionManager& compaction_manager() { return *compaction_mgr_; }
    const CompactionManager& compaction_manager() const { return *compaction_mgr_; }
    
    SessionMemoryProvider* session_memory_provider() { return session_memory_provider_.get(); }
    const SessionMemoryProvider* session_memory_provider() const { return session_memory_provider_.get(); }
    
    MemoryProvider* memory_provider() { return memory_provider_.get(); }
    const MemoryProvider* memory_provider() const { return memory_provider_.get(); }

private:
    Config config_;
    std::shared_ptr<llm::LLMClient> llm_client_;
    
    // Core components
    std::unique_ptr<CompactionManager> compaction_mgr_;
    std::unique_ptr<SessionMemoryProvider> session_memory_provider_;
    std::shared_ptr<MemoryProvider> memory_provider_;
    std::shared_ptr<EmbeddingProvider> embedding_provider_;
    
    // 初始化辅助
    void init_memory_provider();
    void init_embedding_provider();
    void init_compaction_manager();
    void init_session_memory();
};

} // namespace memory
} // namespace hunterclaw
