#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/memory/types.h"
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

// ============================================================================
// Auto-Reindex with Atomic Database Swapping
// ============================================================================
//
// 严格对齐 OpenClaw src/memory/manager-sync-ops.ts
//
// 核心特性:
// 1. 自动检测 Provider/Model/Dimensions 变化
// 2. 原子性重建索引（失败时保持原索引）
// 3. Embedding Cache 复用（加速重建）
// 4. Safe/Unsafe 两种模式（生产/测试）
//
// Atomic Reindex 保证:
// - 重建失败不影响现有索引
// - 临时数据库隔离操作
// - 文件交换的原子性
// - 状态恢复机制
//
// 对齐文件:
// - OpenClaw manager-sync-ops.ts:1148-1361
//   * runSafeReindex (1148-1252): Safe atomic reindex
//   * runUnsafeReindex (1254-1301): Unsafe fast reindex
//   * resetIndex (1305-1315): Clear all index data
//   * swapIndexFiles (328-338): Atomic file swap
//   * seedEmbeddingCache (278-327): Copy cache from old DB
//
// ============================================================================

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// Embedding cache entry
struct EmbeddingCacheEntry {
    std::string content_hash;
    std::string provider;
    std::string model;
    std::string provider_key;
    std::vector<float> embedding;
    int64_t updated_at;
};

// MemoryIndexMeta (简化版，对齐 manager-sync-ops.ts:54-63)
struct MemoryIndexMeta {
    std::string model;
    std::string provider;
    std::optional<std::string> provider_key;
    std::optional<std::vector<MemorySource>> sources;
    std::optional<std::string> scope_hash;
    int chunk_tokens;
    int chunk_overlap;
    std::optional<int> vector_dims;
};

// ============================================================================
// Auto-Reindex Detection (OpenClaw-compatible)
// ============================================================================

struct MemoryProviderInfo {
    std::string provider;          // "openai" | "gemini" | "mistral" | "local"
    std::string model;             // "text-embedding-3-small" | "gemini-embedding-2-preview"
    int output_dimensions = -1;    // Gemini 特殊支持
    std::string endpoint;          // Custom endpoint URL
};

struct ReindexReason {
    enum Type {
        None,
        ProviderChanged,
        ModelChanged,
        DimensionsChanged,
        EndpointChanged
    };
    
    Type type = None;
    std::string old_value;
    std::string new_value;
    
    std::string to_string() const {
        switch (type) {
            case ProviderChanged: return "Provider changed: " + old_value + " → " + new_value;
            case ModelChanged: return "Model changed: " + old_value + " → " + new_value;
            case DimensionsChanged: return "Dimensions changed: " + old_value + " → " + new_value;
            case EndpointChanged: return "Endpoint changed: " + old_value + " → " + new_value;
            default: return "No reindex needed";
        }
    }
};

// Auto-reindex detector
class AutoReindexDetector {
public:
    AutoReindexDetector(const std::string& state_file);
    
    // Check if reindex is needed
    core::Result<ReindexReason> check_reindex_needed(const MemoryProviderInfo& current_info);
    
    // Save current state
    core::Result<bool> save_state(const MemoryProviderInfo& info);
    
    // Load saved state
    core::Result<MemoryProviderInfo> load_state();
    
    // Clear state (after successful reindex)
    core::Result<bool> clear_state();
    
private:
    std::string state_file_;
    
    // Compare two provider infos
    ReindexReason compare_provider_info(
        const MemoryProviderInfo& old_info,
        const MemoryProviderInfo& new_info
    );
};

// Memory status (matches OpenClaw)
struct MemoryStatus {
    std::string backend = "builtin";  // "builtin" | "external"
    int files_count = 0;
    int chunks_count = 0;
    std::string provider = "none";
    std::string model;
    int output_dimensions = -1;
    std::string requested_provider;
    bool reindex_in_progress = false;
    bool fts_available = false;
    std::string fts_error;
    bool vector_available = false;
    std::string vector_load_error;
    int vector_dims = -1;
};

// Memory manager with auto-reindex
class MemoryManagerWithAutoReindex {
public:
    MemoryManagerWithAutoReindex(
        const std::string& data_dir,
        std::shared_ptr<AutoReindexDetector> detector
    );
    
    // Initialize memory (check reindex)
    core::Result<bool> initialize(const MemoryProviderInfo& provider_info);
    
    // Get status
    MemoryStatus status() const;
    
    // Trigger reindex (safe or unsafe based on environment)
    core::Result<bool> reindex(bool force = false);
    
    // Sync (incremental index)
    core::Result<bool> sync(const std::string& reason = "manual");
    
    // Set database path (required for atomic reindex)
    void set_database_path(const std::string& path) { db_path_ = path; }
    
    // Set storage instance
    void set_storage(std::shared_ptr<SQLiteVectorStorage> storage) { 
        storage_ = storage; 
    }
    
    // Set sync callback for file synchronization
    using SyncCallback = std::function<core::Result<bool>(const SyncParams&)>;
    void set_sync_callback(SyncCallback callback) { 
        sync_callback_ = callback; 
    }
    
private:
    std::string data_dir_;
    std::shared_ptr<AutoReindexDetector> detector_;
    MemoryStatus current_status_;
    MemoryProviderInfo current_provider_;
    std::string db_path_;  // SQLite database path
    std::shared_ptr<SQLiteVectorStorage> storage_;  // 数据库存储实例
    SyncCallback sync_callback_;  // 文件同步回调
    
    // Check if reindex is needed
    bool needs_reindex(const MemoryProviderInfo& new_provider);
    
    // Atomic reindex (对齐 manager-sync-ops.ts:1148-1252)
    core::Result<bool> run_safe_reindex();
    
    // Unsafe reindex for tests (对齐 manager-sync-ops.ts:1254-1301)
    core::Result<bool> run_unsafe_reindex();
    
    // Reset index (对齐 manager-sync-ops.ts:1305-1315)
    core::Result<bool> reset_index();
    
    // File operations (对齐 manager-sync-ops.ts:328-361)
    core::Result<bool> swap_index_files(
        const std::string& target_path,
        const std::string& temp_path
    );
    
    core::Result<bool> move_index_files(
        const std::string& source_base,
        const std::string& target_base
    );
    
    core::Result<bool> remove_index_files(const std::string& base_path);
    
    // Seed embedding cache from source DB (对齐 manager-sync-ops.ts:278-327)
    core::Result<bool> seed_embedding_cache(
        SQLiteVectorStorage* source_storage,
        SQLiteVectorStorage* target_storage
    );
    
    // Write metadata to database
    core::Result<bool> write_meta(
        const MemoryIndexMeta& meta,
        SQLiteVectorStorage* storage
    );
    
    // Incremental sync
    core::Result<bool> do_incremental_sync();
};

// ============================================================================
// Gemini Dimensions Support (特殊处理)
// ============================================================================

namespace gemini_support {

// Gemini 支持的输出维度
constexpr int VALID_DIMENSIONS[] = {256, 512, 768, 1024, 1536, 2048};

// Check if dimensions are valid for Gemini
inline bool is_valid_dimensions(int dims) {
    for (int valid : VALID_DIMENSIONS) {
        if (dims == valid) return true;
    }
    return false;
}

// Get default dimensions for model
inline int get_default_dimensions(const std::string& model) {
    if (model.find("gemini-embedding") != std::string::npos) {
        return 768;  // Gemini default
    }
    return -1;  // Other providers use model's native dimensions
}

}  // namespace gemini_support

}  // namespace memory
}  // namespace hunterclaw
