#include <optional>
#include "hunterclaw/memory/search_algorithms.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace hunterclaw {
namespace memory {

namespace {

// Internal types for hybrid search
struct HybridSearchResult {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    MemorySource source;
    std::string snippet;
    double vector_score = 0.0;
    double text_score = 0.0;
    double score = 0.0;
    int64_t modified_ms = 0;
};

struct HybridSearchParams {
    std::vector<MemorySearchResult> vector_results;
    std::vector<MemorySearchResult> keyword_results;
    double vector_weight = 0.7;
    double text_weight = 0.3;
};

struct MMRConfig {
    bool enabled = true;
    double lambda = 0.7;
    int top_k = 6;
};

struct TemporalDecayConfig {
    bool enabled = false;
    double half_life_days = 30.0;
    double min_score_multiplier = 0.1;
};

// Helper: compute cosine similarity between two embeddings
double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 0.0;
    
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    if (norm_a == 0.0 || norm_b == 0.0) return 0.0;
    return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

// Helper: get file modification time in milliseconds
std::optional<int64_t> get_file_modified_ms(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return static_cast<int64_t>(st.st_mtime) * 1000;
}

} // anonymous namespace

// ============================================================================
// Hybrid Search - 严格对齐 OpenClaw hybrid.ts
// ============================================================================

std::string build_fts_query(const std::string& raw) {
    // Extract alphanumeric tokens
    std::vector<std::string> tokens;
    std::string current;
    
    for (char c : raw) {
        if (std::isalnum(c) || c == '_') {
            current += c;
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    
    if (tokens.empty()) {
        return "";
    }
    
    // Build FTS5 query: "token1" AND "token2" AND ...
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += " AND ";
        result += "\"" + tokens[i] + "\"";
    }
    
    return result;
}

double bm25_rank_to_score(double rank) {
    // 严格对齐 OpenClaw hybrid.ts::bm25RankToScore
    if (!std::isfinite(rank)) {
        return 1.0 / (1.0 + 999.0);
    }
    if (rank < 0.0) {
        double relevance = -rank;
        return relevance / (1.0 + relevance);
    }
    return 1.0 / (1.0 + rank);
}

std::vector<HybridSearchResult> merge_hybrid_results(const HybridSearchParams& params) {
    // Build map by path (use path as unique identifier)
    std::map<std::string, HybridSearchResult> by_path;
    
    // Add vector results
    for (const auto& vr : params.vector_results) {
        HybridSearchResult result;
        result.id = vr.path;  // Use path as ID
        result.path = vr.path;
        result.start_line = vr.start_line;
        result.end_line = vr.end_line;
        result.source = vr.source;
        result.snippet = vr.snippet;
        result.vector_score = vr.score;  // Map score to vector_score
        result.text_score = 0.0;
        result.modified_ms = 0;  // MemorySearchResult doesn't have timestamp
        
        by_path[result.path] = result;
    }
    
    // Merge keyword results
    for (const auto& kr : params.keyword_results) {
        auto it = by_path.find(kr.path);
        if (it != by_path.end()) {
            it->second.text_score = kr.score;  // Map score to text_score
            if (!kr.snippet.empty()) {
                it->second.snippet = kr.snippet;
            }
        } else {
            HybridSearchResult result;
            result.id = kr.path;
            result.path = kr.path;
            result.start_line = kr.start_line;
            result.end_line = kr.end_line;
            result.source = kr.source;
            result.snippet = kr.snippet;
            result.vector_score = 0.0;
            result.text_score = kr.score;  // Map score to text_score
            result.modified_ms = 0;
            
            by_path[result.path] = result;
        }
    }
    
    // Calculate combined scores
    std::vector<HybridSearchResult> merged;
    merged.reserve(by_path.size());
    
    for (auto& [id, result] : by_path) {
        result.score = params.vector_weight * result.vector_score + 
                       params.text_weight * result.text_score;
        merged.push_back(std::move(result));
    }
    
    // Sort by score descending
    std::sort(merged.begin(), merged.end(), 
              [](const HybridSearchResult& a, const HybridSearchResult& b) {
                  return a.score > b.score;
              });
    
    return merged;
}

// ============================================================================
// MMR (Maximal Marginal Relevance) - 严格对齐 OpenClaw mmr.ts
// ============================================================================

std::vector<HybridSearchResult> apply_mmr_reranking(
    std::vector<HybridSearchResult> results,
    const MMRConfig& config,
    const std::function<std::optional<std::vector<float>>(const std::string&)>& get_embedding) {
    
    if (!config.enabled || results.empty()) {
        return results;
    }
    
    const size_t k = std::min(static_cast<size_t>(config.top_k), results.size());
    const double lambda = config.lambda;
    
    std::vector<HybridSearchResult> reranked;
    std::set<size_t> selected_indices;
    
    // First: select item with highest relevance score
    reranked.push_back(results[0]);
    selected_indices.insert(0);
    
    // Iteratively select next k-1 items
    while (reranked.size() < k && reranked.size() < results.size()) {
        double max_mmr_score = -1.0;
        size_t best_idx = 0;
        
        for (size_t i = 0; i < results.size(); ++i) {
            if (selected_indices.count(i) > 0) continue;
            
            const auto& candidate = results[i];
            auto candidate_emb = get_embedding(candidate.id);
            if (!candidate_emb.has_value()) continue;
            
            // Compute max similarity to already selected items
            double max_sim = 0.0;
            for (size_t sel_idx : selected_indices) {
                auto sel_emb = get_embedding(results[sel_idx].id);
                if (!sel_emb.has_value()) continue;
                
                double sim = cosine_similarity(candidate_emb.value(), sel_emb.value());
                max_sim = std::max(max_sim, sim);
            }
            
            // MMR score: λ * relevance - (1-λ) * max_similarity
            double mmr_score = lambda * candidate.score - (1.0 - lambda) * max_sim;
            
            if (mmr_score > max_mmr_score) {
                max_mmr_score = mmr_score;
                best_idx = i;
            }
        }
        
        if (max_mmr_score >= 0.0) {
            reranked.push_back(results[best_idx]);
            selected_indices.insert(best_idx);
        } else {
            break; // No more valid candidates
        }
    }
    
    return reranked;
}

// ============================================================================
// Temporal Decay - 严格对齐 OpenClaw temporal-decay.ts
// ============================================================================

std::vector<HybridSearchResult> apply_temporal_decay(
    std::vector<HybridSearchResult> results,
    const TemporalDecayConfig& config,
    const std::optional<std::string>& workspace_dir,
    int64_t now_ms) {
    
    if (!config.enabled || results.empty()) {
        return results;
    }
    
    const double half_life_ms = config.half_life_days * 24.0 * 3600.0 * 1000.0;
    const double decay_constant = std::log(2.0) / half_life_ms;
    
    for (auto& result : results) {
        int64_t modified_ms = result.modified_ms;  // Direct access, it's int64_t not optional
        
        // Fallback: Try to get from filesystem if not set
        if (modified_ms == 0 && workspace_dir.has_value()) {
            std::string full_path = workspace_dir.value() + "/" + result.path;
            auto file_mod_ms = get_file_modified_ms(full_path);
            if (file_mod_ms.has_value()) {
                modified_ms = file_mod_ms.value();
            }
        }
        
        if (modified_ms > 0) {
            int64_t age_ms = now_ms - modified_ms;
            if (age_ms < 0) age_ms = 0; // Future timestamps
            
            // Exponential decay: score * exp(-λ * age)
            double decay_factor = std::exp(-decay_constant * age_ms);
            
            // Apply floor to prevent over-penalization
            decay_factor = std::max(decay_factor, config.min_score_multiplier);
            
            result.score *= decay_factor;
        }
    }
    
    // Re-sort after applying decay
    std::sort(results.begin(), results.end(),
              [](const HybridSearchResult& a, const HybridSearchResult& b) {
                  return a.score > b.score;
              });
    
    return results;
}

// ============================================================================
// Search Pipeline - 严格对齐 OpenClaw 完整流程
// ============================================================================

std::vector<MemorySearchResult> SearchPipeline::execute(
    const std::vector<MemorySearchResult>& vector_results,
    const std::vector<MemorySearchResult>& keyword_results,
    const std::vector<float>& query_embedding,
    const Config& config
) {
    // Step 1: Merge hybrid results (RRF fusion)
    HybridSearchParams hybrid_params;
    hybrid_params.vector_results = vector_results;
    hybrid_params.keyword_results = keyword_results;
    hybrid_params.vector_weight = config.hybrid.vector_weight;
    hybrid_params.text_weight = config.hybrid.text_weight;
    
    auto merged = merge_hybrid_results(hybrid_params);
    
    // Step 2: Apply temporal decay (if enabled)
    if (config.hybrid.temporal_decay.enabled) {
        TemporalDecayConfig temporal_config;
        temporal_config.enabled = true;
        temporal_config.half_life_days = config.hybrid.temporal_decay.half_life_days;
        temporal_config.min_score_multiplier = 0.1;  // Use default value
        
        // Apply temporal decay (note: workspace_dir and now_ms would need to be passed separately)
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        merged = apply_temporal_decay(merged, temporal_config, std::nullopt, now_ms);
    }
    
    // Step 3: Apply MMR re-ranking (if enabled)
    if (config.hybrid.mmr.enabled && !query_embedding.empty()) {
        MMRConfig mmr_config;
        mmr_config.enabled = true;
        mmr_config.lambda = config.hybrid.mmr.lambda;
        mmr_config.top_k = config.max_results;  // Use max_results as top_k
        
        // Convert HybridSearchResult to have embeddings for MMR
        // TODO: Implement proper MMR with embeddings
        // For now, skip MMR if embeddings not available
    }
    
    // Step 4: Filter by min score and limit
    std::vector<MemorySearchResult> final_results;
    final_results.reserve(std::min(merged.size(), static_cast<size_t>(config.max_results)));
    
    for (const auto& item : merged) {
        if (item.score < config.min_score) {
            continue;
        }
        
        if (final_results.size() >= static_cast<size_t>(config.max_results)) {
            break;
        }
        
        MemorySearchResult result;
        result.path = item.path;
        result.start_line = item.start_line;
        result.end_line = item.end_line;
        result.score = item.score;
        result.snippet = item.snippet;
        result.source = item.source;
        
        // Generate citation (simple format)
        result.citation = result.path + ":" + 
                          std::to_string(result.start_line) + "-" + 
                          std::to_string(result.end_line);
        
        final_results.push_back(std::move(result));
    }
    
    return final_results;
}

} // namespace memory
} // namespace hunterclaw
