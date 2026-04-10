#include "hunterclaw/memory/corpus_supplement.h"
#include <algorithm>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Global state (对齐 memory-state.ts:154-157 memoryPluginState)
// ============================================================================

static std::mutex corpus_mutex;
static std::vector<MemoryCorpusSupplementRegistration> corpus_supplements;

// ============================================================================
// registerMemoryCorpusSupplement (对齐 memory-state.ts:159-168)
// ============================================================================

void register_memory_corpus_supplement(
    const std::string& plugin_id,
    std::shared_ptr<MemoryCorpusSupplement> supplement
) {
    std::lock_guard<std::mutex> lock(corpus_mutex);

    // 对齐 openclaw: filter(r => r.pluginId !== pluginId), then push
    std::vector<MemoryCorpusSupplementRegistration> next;
    for (const auto& reg : corpus_supplements) {
        if (reg.plugin_id != plugin_id) {
            next.push_back(reg);
        }
    }
    next.push_back({plugin_id, std::move(supplement)});
    corpus_supplements = std::move(next);
}

// ============================================================================
// listMemoryCorpusSupplements (对齐 memory-state.ts:186-188)
// ============================================================================

std::vector<MemoryCorpusSupplementRegistration> list_memory_corpus_supplements() {
    std::lock_guard<std::mutex> lock(corpus_mutex);
    return corpus_supplements;  // copy
}

// ============================================================================
// searchMemoryCorpusSupplements (对齐 tools.shared.ts:139-165)
// ============================================================================

std::vector<MemoryCorpusSearchResult> search_memory_corpus_supplements(
    const std::string& query,
    std::optional<int> max_results,
    const std::string& agent_session_key,
    const std::string& corpus
) {
    // 对齐: if (params.corpus === "memory") return []
    if (corpus == "memory") {
        return {};
    }

    auto supplements = list_memory_corpus_supplements();
    if (supplements.empty()) {
        return {};
    }

    // 对齐: await Promise.all(supplements.map(r => r.supplement.search(params))).flat()
    std::vector<MemoryCorpusSearchResult> all_results;
    for (const auto& reg : supplements) {
        try {
            CorpusSearchParams params;
            params.query = query;
            params.max_results = max_results;
            if (!agent_session_key.empty()) {
                params.agent_session_key = agent_session_key;
            }
            auto results = reg.supplement->search(params);
            all_results.insert(all_results.end(), results.begin(), results.end());
        } catch (...) {
            // Best-effort: individual supplement failures don't break the search
        }
    }

    // 对齐: results.toSorted((l,r) => r.score !== l.score ? r.score - l.score : l.path.localeCompare(r.path))
    std::sort(all_results.begin(), all_results.end(),
        [](const MemoryCorpusSearchResult& a, const MemoryCorpusSearchResult& b) {
            if (a.score != b.score) return b.score < a.score;  // descending score
            return a.path < b.path;
        });

    // 对齐: .slice(0, Math.max(1, params.maxResults ?? 10))
    int limit = std::max(1, max_results.value_or(10));
    if (static_cast<int>(all_results.size()) > limit) {
        all_results.resize(limit);
    }

    return all_results;
}

// ============================================================================
// getMemoryCorpusSupplementResult (对齐 tools.shared.ts:167-184)
// ============================================================================

std::optional<MemoryCorpusGetResult> get_memory_corpus_supplement_result(
    const std::string& lookup,
    std::optional<int> from_line,
    std::optional<int> line_count,
    const std::string& agent_session_key,
    const std::string& corpus
) {
    // 对齐: if (params.corpus === "memory") return null
    if (corpus == "memory") {
        return std::nullopt;
    }

    // 对齐: for (const registration of listMemoryCorpusSupplements()) {
    //          const result = await registration.supplement.get(params);
    //          if (result) return result;
    //        }
    for (const auto& reg : list_memory_corpus_supplements()) {
        try {
            CorpusGetParams params;
            params.lookup = lookup;
            params.from_line = from_line;
            params.line_count = line_count;
            if (!agent_session_key.empty()) {
                params.agent_session_key = agent_session_key;
            }
            auto result = reg.supplement->get(params);
            if (result.has_value()) {
                return result;
            }
        } catch (...) {
            // Best-effort
        }
    }

    return std::nullopt;
}

}  // namespace memory
}  // namespace hunterclaw
