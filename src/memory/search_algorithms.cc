#include <optional>
#include "hunterclaw/memory/search_algorithms.h"
#include "hunterclaw/memory/temporal_decay.h"
#include <algorithm>
#include <cmath>
#include <regex>
#include <chrono>
#include <numeric>

// 严格对齐 OpenClaw src/memory/hybrid.ts + mmr.ts

namespace hunterclaw {
namespace memory {

// ============================================================================
// buildFtsQuery (对齐 hybrid.ts:buildFtsQuery)
// ============================================================================
std::string build_fts_query(const std::string& raw) {
    // 对齐 openclaw: raw.match(/[\p{L}\p{N}_]+/gu)
    // 使用 UTF-8 aware 分词：提取字母、数字、下划线组成的 token
    // 对于 ASCII 用 \w+，对于非 ASCII 字节（UTF-8 多字节序列）直接保留
    std::vector<std::string> tokens;
    std::string current;

    // 遍历 UTF-8 字节序列
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    const unsigned char* end = p + raw.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) {
            // ASCII
            if (std::isalnum(c) || c == '_') {
                current.push_back(static_cast<char>(c));
            } else {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            }
            ++p;
        } else {
            // Multi-byte UTF-8 character (non-ASCII) → treat as word character
            // Determine byte length
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            for (int i = 0; i < len && p < end; ++i, ++p) {
                current.push_back(static_cast<char>(*p));
            }
        }
    }
    if (!current.empty()) tokens.push_back(current);

    if (tokens.empty()) return "";

    // 对齐 openclaw: quoted.join(" AND ")
    // 对齐 openclaw: t.replaceAll('"', "") — 直接删除引号，不转义
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += " AND ";
        std::string t = tokens[i];
        // Remove all double-quote characters (对齐 replaceAll('"', ""))
        t.erase(std::remove(t.begin(), t.end(), '"'), t.end());
        if (t.empty()) continue;
        result += '"' + t + '"';
    }
    return result;
}

// ============================================================================
// bm25RankToScore (对齐 hybrid.ts:bm25RankToScore)
// ============================================================================
double bm25_rank_to_score(double rank) {
    if (!std::isfinite(rank)) {
        return 1.0 / (1.0 + 999.0);
    }
    if (rank < 0.0) {
        double relevance = -rank;
        return relevance / (1.0 + relevance);
    }
    return 1.0 / (1.0 + rank);
}

// ============================================================================
// tokenize (对齐 mmr.ts:tokenize)
// ============================================================================
std::set<std::string> tokenize(const std::string& text) {
    std::set<std::string> result;
    std::regex token_re(R"([a-z0-9_]+)");
    // 转小写
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    auto begin = std::sregex_iterator(lower.begin(), lower.end(), token_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        result.insert(it->str());
    }
    return result;
}

// ============================================================================
// jaccardSimilarity (对齐 mmr.ts:jaccardSimilarity)
// ============================================================================
double jaccard_similarity(const std::set<std::string>& set_a, const std::set<std::string>& set_b) {
    if (set_a.empty() && set_b.empty()) return 1.0;
    if (set_a.empty() || set_b.empty()) return 0.0;

    // 计算交集大小
    size_t intersection = 0;
    const auto& smaller = (set_a.size() <= set_b.size()) ? set_a : set_b;
    const auto& larger  = (set_a.size() <= set_b.size()) ? set_b : set_a;
    for (const auto& token : smaller) {
        if (larger.count(token)) intersection++;
    }

    size_t union_size = set_a.size() + set_b.size() - intersection;
    return union_size == 0 ? 0.0 : static_cast<double>(intersection) / union_size;
}

// ============================================================================
// textSimilarity (对齐 mmr.ts:textSimilarity)
// ============================================================================
double text_similarity(const std::string& content_a, const std::string& content_b) {
    return jaccard_similarity(tokenize(content_a), tokenize(content_b));
}

// ============================================================================
// computeMMRScore (对齐 mmr.ts:computeMMRScore)
// ============================================================================
double compute_mmr_score(double relevance, double max_similarity, double lambda) {
    return lambda * relevance - (1.0 - lambda) * max_similarity;
}

// ============================================================================
// mmrRerank (对齐 mmr.ts:mmrRerank)
// ============================================================================
std::vector<MMRItem> mmr_rerank(std::vector<MMRItem> items, const MMRConfig& config) {
    // 对齐 openclaw: early exits
    if (!config.enabled || items.size() <= 1) {
        return items;
    }

    double lambda = std::max(0.0, std::min(1.0, config.lambda));

    // 对齐 openclaw: if (clampedLambda === 1) return sorted by relevance
    if (lambda == 1.0) {
        std::sort(items.begin(), items.end(),
                  [](const MMRItem& a, const MMRItem& b){ return a.score > b.score; });
        return items;
    }

    // 对齐 openclaw: pre-tokenize all items
    std::map<std::string, std::set<std::string>> token_cache;
    for (const auto& item : items) {
        token_cache[item.id] = tokenize(item.content);
    }

    // 对齐 openclaw: normalize scores to [0, 1]
    double max_score = -std::numeric_limits<double>::infinity();
    double min_score =  std::numeric_limits<double>::infinity();
    for (const auto& item : items) {
        max_score = std::max(max_score, item.score);
        min_score = std::min(min_score, item.score);
    }
    double score_range = max_score - min_score;

    auto normalize_score = [&](double score) -> double {
        if (score_range == 0.0) return 1.0;
        return (score - min_score) / score_range;
    };

    // 对齐 openclaw: iterative selection
    std::vector<MMRItem> selected;
    std::vector<bool> used(items.size(), false);
    selected.reserve(items.size());

    while (selected.size() < items.size()) {
        size_t best_idx = 0;
        double best_mmr = -std::numeric_limits<double>::infinity();
        bool found = false;

        for (size_t i = 0; i < items.size(); ++i) {
            if (used[i]) continue;

            const auto& candidate = items[i];
            double normalized_relevance = normalize_score(candidate.score);

            // 计算与已选结果的最大 Jaccard 相似度
            double max_sim = 0.0;
            const auto& cand_tokens = token_cache[candidate.id];
            for (const auto& sel : selected) {
                const auto& sel_tokens = token_cache[sel.id];
                double sim = jaccard_similarity(cand_tokens, sel_tokens);
                max_sim = std::max(max_sim, sim);
            }

            double mmr = compute_mmr_score(normalized_relevance, max_sim, lambda);

            // 对齐 openclaw: tiebreaker by original score
            if (mmr > best_mmr ||
                (mmr == best_mmr && candidate.score > (found ? items[best_idx].score : -std::numeric_limits<double>::infinity()))) {
                best_mmr = mmr;
                best_idx = i;
                found = true;
            }
        }

        if (!found) break;
        selected.push_back(items[best_idx]);
        used[best_idx] = true;
    }

    return selected;
}

// ============================================================================
// applyMMRToHybridResults (对齐 mmr.ts:applyMMRToHybridResults)
// ============================================================================
std::vector<HybridMergedResult> apply_mmr_to_hybrid_results(
    std::vector<HybridMergedResult> results,
    const MMRConfig& config
) {
    if (results.empty()) return results;

    // 对齐 openclaw: create MMR items with unique IDs
    std::map<std::string, size_t> id_to_idx;
    std::vector<MMRItem> mmr_items;
    mmr_items.reserve(results.size());

    for (size_t i = 0; i < results.size(); ++i) {
        std::string id = results[i].path + ":" +
                         std::to_string(results[i].start_line) + ":" +
                         std::to_string(i);
        id_to_idx[id] = i;
        mmr_items.push_back({id, results[i].score, results[i].snippet});
    }

    auto reranked = mmr_rerank(std::move(mmr_items), config);

    // 对齐 openclaw: map back to original items
    std::vector<HybridMergedResult> output;
    output.reserve(reranked.size());
    for (const auto& item : reranked) {
        auto it = id_to_idx.find(item.id);
        if (it != id_to_idx.end()) {
            output.push_back(results[it->second]);
        }
    }
    return output;
}

// ============================================================================
// mergeHybridResults (对齐 hybrid.ts:mergeHybridResults)
// ============================================================================
std::vector<HybridMergedResult> merge_hybrid_results(
    const std::vector<HybridVectorResult>& vector_results,
    const std::vector<HybridKeywordResult>& keyword_results,
    double vector_weight,
    double text_weight,
    const std::string& workspace_dir,
    const MMRConfig& mmr,
    const TemporalDecayConfig& temporal_decay,
    int64_t now_ms
) {
    // 对齐 openclaw: byId map (用 id 作为 key)
    struct Entry {
        std::string id;
        std::string path;
        int start_line = 0;
        int end_line = 0;
        std::string source;
        std::string snippet;
        double vector_score = 0.0;
        double text_score = 0.0;
    };
    std::map<std::string, Entry> by_id;

    // 添加 vector 结果
    for (const auto& r : vector_results) {
        by_id[r.id] = {r.id, r.path, r.start_line, r.end_line,
                       r.source, r.snippet, r.vector_score, 0.0};
    }

    // 合并 keyword 结果
    for (const auto& r : keyword_results) {
        auto it = by_id.find(r.id);
        if (it != by_id.end()) {
            it->second.text_score = r.text_score;
            if (!r.snippet.empty()) {
                it->second.snippet = r.snippet;
            }
        } else {
            by_id[r.id] = {r.id, r.path, r.start_line, r.end_line,
                           r.source, r.snippet, 0.0, r.text_score};
        }
    }

    // 计算合并分数
    std::vector<HybridMergedResult> merged;
    merged.reserve(by_id.size());
    for (const auto& [id, entry] : by_id) {
        double score = vector_weight * entry.vector_score + text_weight * entry.text_score;
        merged.push_back({entry.path, entry.start_line, entry.end_line,
                          score, entry.snippet, entry.source});
    }

    // 对齐 openclaw: applyTemporalDecayToHybridResults
    if (temporal_decay.enabled) {
        std::vector<TemporalDecayEntry> decay_entries;
        decay_entries.reserve(merged.size());
        for (const auto& r : merged) {
            decay_entries.push_back({r.path, r.score, r.source});
        }
        auto decayed = apply_temporal_decay_to_hybrid_results(
            std::move(decay_entries), temporal_decay, workspace_dir, now_ms);

        // 将衰减后的分数写回
        std::map<std::string, double> decayed_scores;
        for (const auto& d : decayed) {
            decayed_scores[d.source + ":" + d.path] = d.score;
        }
        for (auto& r : merged) {
            auto it = decayed_scores.find(r.source + ":" + r.path);
            if (it != decayed_scores.end()) r.score = it->second;
        }
    }

    // 对齐 openclaw: toSorted((a, b) => b.score - a.score)
    std::sort(merged.begin(), merged.end(),
              [](const HybridMergedResult& a, const HybridMergedResult& b) {
                  return a.score > b.score;
              });

    // 对齐 openclaw: applyMMRToHybridResults (if enabled)
    if (mmr.enabled) {
        merged = apply_mmr_to_hybrid_results(std::move(merged), mmr);
    }

    return merged;
}

// ============================================================================
// SearchPipeline::execute (对齐 manager-search.ts 搜索流程)
// ============================================================================
std::vector<MemorySearchResult> SearchPipeline::execute(
    const std::vector<MemorySearchResult>& vector_results,
    const std::vector<MemorySearchResult>& keyword_results,
    const std::vector<float>& /*query_embedding*/,  // 不再需要（MMR 用 Jaccard）
    const Config& config
) {
    // 转换为 HybridVectorResult / HybridKeywordResult
    std::vector<HybridVectorResult> vec_in;
    vec_in.reserve(vector_results.size());
    for (size_t i = 0; i < vector_results.size(); ++i) {
        const auto& r = vector_results[i];
        std::string id = r.path + ":" + std::to_string(r.start_line) + ":v" + std::to_string(i);
        vec_in.push_back({id, r.path, r.start_line, r.end_line,
                          to_string(r.source), r.snippet, r.score});
    }

    std::vector<HybridKeywordResult> kw_in;
    kw_in.reserve(keyword_results.size());
    for (size_t i = 0; i < keyword_results.size(); ++i) {
        const auto& r = keyword_results[i];
        std::string id = r.path + ":" + std::to_string(r.start_line) + ":k" + std::to_string(i);
        kw_in.push_back({id, r.path, r.start_line, r.end_line,
                         to_string(r.source), r.snippet, r.score});
    }

    MMRConfig mmr_cfg;
    mmr_cfg.enabled = config.hybrid.mmr.enabled;
    mmr_cfg.lambda  = config.hybrid.mmr.lambda;

    TemporalDecayConfig td_cfg;
    td_cfg.enabled        = config.hybrid.temporal_decay.enabled;
    td_cfg.half_life_days = config.hybrid.temporal_decay.half_life_days;

    auto merged = merge_hybrid_results(
        vec_in, kw_in,
        config.hybrid.vector_weight,
        config.hybrid.text_weight,
        "",  // workspace_dir (caller should pass if needed)
        mmr_cfg,
        td_cfg,
        0
    );

    // 过滤 + 截断
    std::vector<MemorySearchResult> final_results;
    final_results.reserve(std::min(merged.size(), static_cast<size_t>(config.max_results)));

    for (const auto& item : merged) {
        if (item.score < config.min_score) continue;
        if (static_cast<int>(final_results.size()) >= config.max_results) break;

        MemorySearchResult result;
        result.path       = item.path;
        result.start_line = item.start_line;
        result.end_line   = item.end_line;
        result.score      = item.score;
        result.snippet    = item.snippet;
        result.citation   = item.path + ":" +
                            std::to_string(item.start_line) + "-" +
                            std::to_string(item.end_line);
        final_results.push_back(std::move(result));
    }

    return final_results;
}

} // namespace memory
} // namespace hunterclaw
