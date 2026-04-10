#include "hunterclaw/memory/manager_reindex_state.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <openssl/sha.h>
#include <iomanip>

// 严格对齐 OpenClaw memory/manager-reindex-state.ts (103 行)

namespace hunterclaw {
namespace memory {

// 对齐 manager-reindex-state.ts:19-24
std::vector<std::string> resolve_configured_sources_for_meta(
    const std::vector<std::string>& sources
) {
    std::set<std::string> valid;
    for (const auto& s : sources) {
        if (s == "memory" || s == "sessions") valid.insert(s);
    }
    if (valid.empty()) return {"memory"};
    return std::vector<std::string>(valid.begin(), valid.end());
}

// 对齐 manager-reindex-state.ts:26-39
std::vector<std::string> normalize_meta_sources(const MemoryIndexMeta& meta) {
    if (meta.sources.empty()) return {"memory"};
    std::set<std::string> valid;
    for (const auto& s : meta.sources) {
        if (s == "memory" || s == "sessions") valid.insert(s);
    }
    if (valid.empty()) return {"memory"};
    return std::vector<std::string>(valid.begin(), valid.end());
}

// 对齐 manager-reindex-state.ts:41-50
bool configured_meta_sources_differ(
    const MemoryIndexMeta& meta,
    const std::vector<std::string>& configured_sources
) {
    auto meta_sources = normalize_meta_sources(meta);
    if (meta_sources.size() != configured_sources.size()) return true;
    for (size_t i = 0; i < meta_sources.size(); ++i) {
        if (meta_sources[i] != configured_sources[i]) return true;
    }
    return false;
}

// 对齐 manager-reindex-state.ts:52-74
std::string resolve_configured_scope_hash(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths,
    bool multimodal_enabled
) {
    std::vector<std::string> sorted_paths = extra_paths;
    std::sort(sorted_paths.begin(), sorted_paths.end());
    
    std::ostringstream data;
    data << "{\"extraPaths\":[";
    for (size_t i = 0; i < sorted_paths.size(); ++i) {
        if (i > 0) data << ",";
        data << "\"" << sorted_paths[i] << "\"";
    }
    data << "],\"multimodal\":{\"enabled\":" << (multimodal_enabled ? "true" : "false") << "}}";
    
    std::string input = data.str();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    std::ostringstream hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return hex.str();
}

// 对齐 manager-reindex-state.ts:76-103
bool should_run_full_memory_reindex(const FullReindexCheckParams& params) {
    if (!params.meta) return true;
    
    std::string expected_model = params.provider_id.empty() ? "fts-only" : params.provider_model;
    std::string expected_provider = params.provider_id.empty() ? "none" : params.provider_id;
    
    if (params.meta->model != expected_model) return true;
    if (params.meta->provider != expected_provider) return true;
    if (params.meta->provider_key.value_or("") != params.provider_key) return true;
    if (configured_meta_sources_differ(*params.meta, params.configured_sources)) return true;
    if (params.meta->scope_hash.value_or("") != params.configured_scope_hash) return true;
    if (params.meta->chunk_tokens != params.chunk_tokens) return true;
    if (params.meta->chunk_overlap != params.chunk_overlap) return true;
    if (params.vector_ready && !params.meta->vector_dims.has_value()) return true;
    if ((params.meta->fts_tokenizer.empty() ? "unicode61" : params.meta->fts_tokenizer) != params.fts_tokenizer) return true;
    
    return false;
}

}  // namespace memory
}  // namespace hunterclaw
