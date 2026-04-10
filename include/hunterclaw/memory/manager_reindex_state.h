#pragma once

// ============================================================================
// Manager Reindex State — 严格对齐 OpenClaw manager-reindex-state.ts (103 行)
//
// 重建索引的状态管理：meta 比较、scope hash、full reindex 判定。
// ============================================================================

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// 对齐 manager-reindex-state.ts:7-17
struct MemoryIndexMeta {
    std::string model;
    std::string provider;
    std::optional<std::string> provider_key;
    std::vector<std::string> sources;  // "memory" | "sessions"
    std::optional<std::string> scope_hash;
    int chunk_tokens = 400;
    int chunk_overlap = 80;
    std::optional<int> vector_dims;
    std::string fts_tokenizer = "unicode61";
};

/**
 * Resolve configured sources for meta comparison.
 * (对齐 manager-reindex-state.ts:19-24 resolveConfiguredSourcesForMeta)
 */
std::vector<std::string> resolve_configured_sources_for_meta(
    const std::vector<std::string>& sources
);

/**
 * Normalize meta sources for comparison.
 * (对齐 manager-reindex-state.ts:26-39 normalizeMetaSources)
 */
std::vector<std::string> normalize_meta_sources(const MemoryIndexMeta& meta);

/**
 * Check if configured sources differ from stored meta sources.
 * (对齐 manager-reindex-state.ts:41-50 configuredMetaSourcesDiffer)
 */
bool configured_meta_sources_differ(
    const MemoryIndexMeta& meta,
    const std::vector<std::string>& configured_sources
);

/**
 * Resolve scope hash from workspace dir + extra paths + multimodal config.
 * (对齐 manager-reindex-state.ts:52-74 resolveConfiguredScopeHash)
 */
std::string resolve_configured_scope_hash(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths = {},
    bool multimodal_enabled = false
);

/**
 * Should a full memory reindex be performed?
 * (对齐 manager-reindex-state.ts:76-103 shouldRunFullMemoryReindex)
 */
struct FullReindexCheckParams {
    const MemoryIndexMeta* meta = nullptr;  // null if no existing meta
    std::string provider_id;
    std::string provider_model;
    std::string provider_key;
    std::vector<std::string> configured_sources;
    std::string configured_scope_hash;
    int chunk_tokens = 400;
    int chunk_overlap = 80;
    bool vector_ready = false;
    std::string fts_tokenizer = "unicode61";
};

bool should_run_full_memory_reindex(const FullReindexCheckParams& params);

}  // namespace memory
}  // namespace hunterclaw
