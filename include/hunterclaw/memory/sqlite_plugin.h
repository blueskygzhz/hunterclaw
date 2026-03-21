#pragma once

// ============================================================================
// SQLite Memory Plugin
//
// OpenClaw alignment:
//   This is the built-in memory plugin (kind="memory") backed by SQLite.
//   It implements MemorySearchManager (types.ts / types.h).
//
//   In OpenClaw, the built-in SQLite memory manager is MemoryIndexManager
//   (src/memory/manager.ts). It is NOT a plugin class — it's a plain class
//   that implements MemorySearchManager.
//
//   HunterClaw maps this as:
//   - SQLiteMemoryManager: implements MemorySearchManager (= MemoryIndexManager)
//   - make_sqlite_memory_plugin_definition(): registers it as a memory plugin
// ============================================================================

#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <string>
#include <map>

namespace hunterclaw {
namespace memory {

// ============================================================================
// SQLiteMemoryManager
//
// Implements MemorySearchManager backed by local SQLite.
// Aligns with OpenClaw src/memory/manager.ts (MemoryIndexManager).
//
// Features:
//   - FTS5 full-text search (BM25 keyword matching)
//   - sqlite-vec vector search (cosine similarity)
//   - Hybrid search (vector + keyword, configurable weights)
//   - MMR reranking (Maximal Marginal Relevance)
//   - Temporal decay for old memories
//   - Embedding cache (avoid redundant computation)
//   - Incremental sync (file watcher + session listener)
// ============================================================================
class SQLiteMemoryManager : public MemorySearchManager {
public:
    // Configuration (对齐 ResolvedMemorySearchConfig in types.h)
    struct Config {
        std::string db_path;
        std::string embedding_provider = "openai";  // "openai"|"ollama"|"gemini"|"voyage"|"auto"
        bool hybrid_search = true;
        double vector_weight = 0.7;
        double text_weight = 0.3;
        bool mmr_enabled = true;
        bool temporal_decay = false;
        int half_life_days = 30;
    };

    explicit SQLiteMemoryManager(const Config& config);
    ~SQLiteMemoryManager() override;

    // MemorySearchManager interface (对齐 types.ts:61-81)
    std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = {}
    ) override;

    ReadFileResult read_file(const ReadFileParams& params) override;

    MemoryProviderStatus status() override;

    void sync(const SyncParams& params = {}) override;

    MemoryEmbeddingProbeResult probe_embedding_availability() override;

    bool probe_vector_availability() override;

    void close() override;

    // ========================================================================
    // Factory (对齐 MemoryIndexManager::get() in manager.ts)
    // ========================================================================

    // Create from plugin config map (used by MemoryPluginDefinition::create_manager)
    static std::shared_ptr<SQLiteMemoryManager> create(
        const std::string& agent_id,
        const std::string& workspace_dir,
        const std::map<std::string, std::string>& plugin_config
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// SQLite Memory Plugin Definition
//
// Register this plugin via MemoryPluginRegistry to make SQLite memory
// available as a memory backend.
//
// Usage:
//   MemoryPluginRegistry::instance().register_plugin(
//       make_sqlite_memory_plugin_definition()
//   );
// ============================================================================
MemoryPluginDefinition make_sqlite_memory_plugin_definition();

} // namespace memory
} // namespace hunterclaw
