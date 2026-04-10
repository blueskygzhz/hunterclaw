#include "hunterclaw/tools/memory_tools.h"
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/memory/short_term_promotion.h"
#include "hunterclaw/memory/corpus_supplement.h"  // ✅ Corpus supplement 路由 (对齐 tools.shared.ts)
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::memory;
using namespace hunterclaw::sessions;

// ============================================================================
// 辅助函数 (严格对齐 OpenClaw memory-tool.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: formatCitation(entry: MemorySearchResult): string
static std::string format_citation(const MemorySearchResult& entry) {
    std::string line_range;
    if (entry.start_line == entry.end_line) {
        line_range = "#L" + std::to_string(entry.start_line);
    } else {
        line_range = "#L" + std::to_string(entry.start_line) +
                     "-L" + std::to_string(entry.end_line);
    }
    return entry.path + line_range;
}

// 🎯 对齐 OpenClaw: deriveChatTypeFromSessionKey(sessionKey?: string)
// Returns "direct" | "group" | "channel"
static std::string derive_chat_type_from_session_key(const std::string& session_key) {
    if (session_key.empty()) {
        return "direct";
    }
    auto parsed = parse_agent_session_key(session_key);
    if (!parsed || parsed->rest.empty()) {
        return "direct";
    }
    // 对齐 OpenClaw: const tokens = new Set(parsed.rest.toLowerCase().split(":").filter(Boolean))
    std::string rest_lower = parsed->rest;
    std::transform(rest_lower.begin(), rest_lower.end(), rest_lower.begin(), ::tolower);
    std::set<std::string> tokens;
    std::istringstream ss(rest_lower);
    std::string tok;
    while (std::getline(ss, tok, ':')) {
        if (!tok.empty()) tokens.insert(tok);
    }
    if (tokens.count("channel")) return "channel";
    if (tokens.count("group"))   return "group";
    return "direct";
}

// 🎯 对齐 OpenClaw: shouldIncludeCitations({ mode, sessionKey })
static bool should_include_citations(const std::string& mode,
                                     const std::string& session_key) {
    if (mode == "on")  return true;
    if (mode == "off") return false;
    // auto: show citations in direct chats; suppress in groups/channels by default.
    std::string chat_type = derive_chat_type_from_session_key(session_key);
    return chat_type == "direct";
}

// 🎯 对齐 OpenClaw: decorateCitations(results, include)
static std::vector<MemorySearchResult> decorate_citations(
    const std::vector<MemorySearchResult>& results,
    bool include
) {
    std::vector<MemorySearchResult> out;
    out.reserve(results.size());
    for (const auto& entry : results) {
        MemorySearchResult decorated = entry;
        if (!include) {
            // 对齐 OpenClaw: { ...entry, citation: undefined }
            decorated.citation = std::nullopt;
        } else {
            // 对齐 OpenClaw: const citation = formatCitation(entry)
            std::string citation = format_citation(entry);
            // 对齐 OpenClaw: const snippet = `${entry.snippet.trim()}\n\nSource: ${citation}`
            std::string trimmed = entry.snippet;
            size_t s = trimmed.find_first_not_of(" \t\r\n");
            if (s == std::string::npos) trimmed = "";
            else {
                size_t e = trimmed.find_last_not_of(" \t\r\n");
                trimmed = trimmed.substr(s, e - s + 1);
            }
            decorated.snippet  = trimmed + "\n\nSource: " + citation;
            decorated.citation = citation;
        }
        out.push_back(std::move(decorated));
    }
    return out;
}

// 🎯 对齐 OpenClaw: clampResultsByInjectedChars(results, budget?)
static std::vector<MemorySearchResult> clamp_results_by_injected_chars(
    const std::vector<MemorySearchResult>& results,
    std::optional<int> budget
) {
    if (!budget.has_value() || *budget <= 0) {
        return results;
    }
    int remaining = *budget;
    std::vector<MemorySearchResult> clamped;
    for (const auto& entry : results) {
        if (remaining <= 0) break;
        // 对齐 OpenClaw: const snippet = entry.snippet ?? ""
        const std::string& snippet = entry.snippet;
        int len = static_cast<int>(snippet.size());
        if (len <= remaining) {
            clamped.push_back(entry);
            remaining -= len;
        } else {
            MemorySearchResult trimmed_entry = entry;
            // 对齐 OpenClaw: snippet.slice(0, Math.max(0, remaining))
            trimmed_entry.snippet = snippet.substr(0, static_cast<size_t>(std::max(0, remaining)));
            clamped.push_back(std::move(trimmed_entry));
            break;
        }
    }
    return clamped;
}

// 🎯 对齐 OpenClaw: buildMemorySearchUnavailableResult(error)
static ToolResult build_memory_search_unavailable_result(const std::string& error) {
    // 对齐 OpenClaw: const reason = (error ?? "memory search unavailable").trim() || "..."
    std::string reason = error;
    size_t s = reason.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) reason = "memory search unavailable";
    else {
        size_t e = reason.find_last_not_of(" \t\r\n");
        reason = reason.substr(s, e - s + 1);
    }
    if (reason.empty()) reason = "memory search unavailable";

    // 对齐 OpenClaw: const isQuotaError = /insufficient_quota|quota|429/.test(reason.toLowerCase())
    std::string reason_lower = reason;
    std::transform(reason_lower.begin(), reason_lower.end(), reason_lower.begin(), ::tolower);
    static const std::regex quota_re{"insufficient_quota|quota|429"};
    bool is_quota_error = std::regex_search(reason_lower, quota_re);

    std::string warning = is_quota_error
        ? "Memory search is unavailable because the embedding provider quota is exhausted."
        : "Memory search is unavailable due to an embedding/provider error.";
    std::string action = is_quota_error
        ? "Top up or switch embedding provider, then retry memory_search."
        : "Check embedding provider configuration and retry memory_search.";

    nlohmann::json j = {
        {"results",     nlohmann::json::array()},
        {"disabled",    true},
        {"unavailable", true},
        {"error",       reason},
        {"warning",     warning},
        {"action",      action},
    };
    return tool_result::json(core::JsonValue(j));
}

// ============================================================================
// MemorySearchTool Implementation
// ============================================================================

MemorySearchTool::MemorySearchTool(std::shared_ptr<MemorySearchManager> manager)
    : manager_(manager) {}

ToolResult MemorySearchTool::execute(const core::JsonValue& params) {
    // 🎯 对齐 OpenClaw: readStringParam(params, "query", { required: true })
    if (!params.has("query") || !params["query"].is_string()) {
        return tool_result::error("Parameter 'query' is required and must be a string");
    }
    std::string query = params["query"].as_string();

    // 🎯 对齐 OpenClaw: readNumberParam(params, "maxResults") / readNumberParam(params, "minScore")
    std::optional<int>    max_results;
    std::optional<double> min_score;
    if (params.has("maxResults") && params["maxResults"].is_number()) {
        max_results = static_cast<int>(params["maxResults"].as_number());
    }
    if (params.has("minScore") && params["minScore"].is_number()) {
        min_score = params["minScore"].as_number();
    }

    // 🎯 对齐 OpenClaw: readStringParam(params, "corpus") → "memory" | "wiki" | "all"
    std::string corpus = "memory";
    if (params.has("corpus") && params["corpus"].is_string()) {
        corpus = params["corpus"].as_string();
    }

    // 🎯 对齐 OpenClaw tools.ts:167-168
    // shouldQueryMemory = requestedCorpus !== "wiki"
    // shouldQuerySupplements = requestedCorpus === "wiki" || requestedCorpus === "all"
    bool should_query_memory = (corpus != "wiki");
    bool should_query_supplements = (corpus == "wiki" || corpus == "all");

    // 🎯 对齐 OpenClaw: getMemoryManagerContext — if "error" in memory → unavailable result
    if (should_query_memory && !manager_ && !should_query_supplements) {
        return build_memory_search_unavailable_result("memory manager not initialized");
    }

    try {
        // 🎯 对齐 OpenClaw: resolveMemoryCitationsMode(cfg) → "auto" | "on" | "off"
        const std::string citations_mode = "auto";

        // 🎯 对齐 OpenClaw: shouldIncludeCitations({ mode, sessionKey })
        std::string session_key;
        if (params.has("_sessionKey") && params["_sessionKey"].is_string()) {
            session_key = params["_sessionKey"].as_string();
        }
        bool include_citations = should_include_citations(citations_mode, session_key);

        // 🎯 对齐 OpenClaw tools.ts:179-217 — memory search (conditional on shouldQueryMemory)
        std::vector<MemorySearchResult> raw_results;
        std::vector<MemorySearchResult> surfaced_memory_results;
        std::string provider_str;
        std::string search_mode;
        MemoryProviderStatus status;
        bool has_status = false;

        if (should_query_memory && manager_) {
            SearchOptions opts;
            opts.max_results = max_results;
            opts.min_score   = min_score;
            if (!session_key.empty()) opts.session_key = session_key;
            raw_results = manager_->search(query, opts);

            status = manager_->status();
            has_status = true;

            auto decorated = decorate_citations(raw_results, include_citations);

            // 对齐: status.backend === "qmd" → clampResultsByInjectedChars
            if (status.backend == "qmd") {
                surfaced_memory_results = clamp_results_by_injected_chars(decorated, std::nullopt);
            } else {
                surfaced_memory_results = decorated;
            }

            provider_str = status.provider;
            if (status.custom.has_value() && !status.custom->search_mode.empty()) {
                search_mode = status.custom->search_mode;
            }

            // 🎯 对齐 OpenClaw tools.ts:206-212 queueShortTermRecallTracking
            if (!surfaced_memory_results.empty()) {
                try {
                    std::string workspace_dir;
                    if (status.workspace_dir.has_value()) {
                        workspace_dir = *status.workspace_dir;
                    }
                    if (!workspace_dir.empty()) {
                        std::thread([workspace_dir, query, raw_results]() {
                            try {
                                memory::record_short_term_recalls(
                                    workspace_dir, query, raw_results);
                            } catch (...) {}
                        }).detach();
                    }
                } catch (...) {}
            }
        }

        // 🎯 对齐 OpenClaw tools.ts:218-225 — supplement search
        std::vector<memory::MemoryCorpusSearchResult> supplement_results;
        if (should_query_supplements) {
            supplement_results = memory::search_memory_corpus_supplements(
                query, max_results, session_key, corpus);
        }

        // 🎯 对齐 OpenClaw tools.ts:226-233 — merge + sort + slice
        nlohmann::json merged_results = nlohmann::json::array();

        // Memory results with corpus:"memory" tag
        for (const auto& r : surfaced_memory_results) {
            nlohmann::json item = {
                {"corpus",    "memory"},
                {"path",      r.path},
                {"startLine", r.start_line},
                {"endLine",   r.end_line},
                {"score",     r.score},
                {"snippet",   r.snippet},
                {"source",    to_string(r.source)},
            };
            if (r.citation.has_value()) {
                item["citation"] = *r.citation;
            }
            merged_results.push_back(item);
        }

        // Supplement results (already have corpus field)
        for (const auto& sr : supplement_results) {
            nlohmann::json item = {
                {"corpus",    sr.corpus},
                {"path",      sr.path},
                {"score",     sr.score},
                {"snippet",   sr.snippet},
            };
            if (sr.title.has_value()) item["title"] = *sr.title;
            if (sr.kind.has_value()) item["kind"] = *sr.kind;
            if (sr.start_line.has_value()) item["startLine"] = *sr.start_line;
            if (sr.end_line.has_value()) item["endLine"] = *sr.end_line;
            if (sr.citation.has_value()) item["citation"] = *sr.citation;
            if (sr.source.has_value()) item["source"] = *sr.source;
            if (sr.provenance_label.has_value()) item["provenanceLabel"] = *sr.provenance_label;
            merged_results.push_back(item);
        }

        // 对齐: .toSorted((l,r) => r.score !== l.score ? r.score - l.score : l.path.localeCompare(r.path))
        // .slice(0, Math.max(1, maxResults ?? 10))
        std::vector<nlohmann::json> sorted_vec(merged_results.begin(), merged_results.end());
        std::sort(sorted_vec.begin(), sorted_vec.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
                double sa = a.value("score", 0.0);
                double sb = b.value("score", 0.0);
                if (sa != sb) return sb < sa;
                return a.value("path", "") < b.value("path", "");
            });
        int limit = std::max(1, max_results.value_or(10));
        if (static_cast<int>(sorted_vec.size()) > limit) {
            sorted_vec.resize(limit);
        }

        nlohmann::json final_results = nlohmann::json::array();
        for (auto& item : sorted_vec) {
            final_results.push_back(std::move(item));
        }

        // 🎯 对齐 OpenClaw tools.ts:234-241 — response
        nlohmann::json response = {
            {"results",   final_results},
            {"citations", citations_mode},
        };
        if (!provider_str.empty()) {
            response["provider"] = provider_str;
        }
        if (has_status && status.model.has_value()) {
            response["model"] = *status.model;
        }
        if (has_status && status.fallback.has_value()) {
            nlohmann::json fallback_json = {{"from", status.fallback->from}};
            if (status.fallback->reason.has_value()) {
                fallback_json["reason"] = *status.fallback->reason;
            }
            response["fallback"] = fallback_json;
        }
        if (!search_mode.empty()) {
            response["mode"] = search_mode;
        }

        return tool_result::json(core::JsonValue(response));

    } catch (const std::exception& e) {
        // 🎯 对齐 OpenClaw: catch (err) → buildMemorySearchUnavailableResult(message)
        return build_memory_search_unavailable_result(e.what());
    }
}

// ============================================================================
// MemoryGetTool Implementation
// ============================================================================

MemoryGetTool::MemoryGetTool(std::shared_ptr<MemorySearchManager> manager)
    : manager_(manager) {}

ToolResult MemoryGetTool::execute(const core::JsonValue& params) {
    // 🎯 对齐 OpenClaw: readStringParam(params, "path", { required: true })
    if (!params.has("path") || !params["path"].is_string()) {
        return tool_result::error("Parameter 'path' is required and must be a string");
    }
    std::string rel_path = params["path"].as_string();

    // 🎯 对齐 OpenClaw: readNumberParam(params, "from", { integer: true })
    std::optional<int> from;
    std::optional<int> lines;
    if (params.has("from") && params["from"].is_number()) {
        from = static_cast<int>(params["from"].as_number());
    }
    if (params.has("lines") && params["lines"].is_number()) {
        lines = static_cast<int>(params["lines"].as_number());
    }

    // 🎯 对齐 OpenClaw tools.ts:267-271: readStringParam(params, "corpus")
    std::string requested_corpus = "memory";
    if (params.has("corpus") && params["corpus"].is_string()) {
        requested_corpus = params["corpus"].as_string();
    }

    // 🎯 对齐 OpenClaw tools.ts:273-289: if (requestedCorpus === "wiki") → supplement only
    if (requested_corpus == "wiki") {
        auto supplement = memory::get_memory_corpus_supplement_result(
            rel_path,
            from,
            lines,
            "",  // agentSessionKey
            requested_corpus
        );
        if (supplement.has_value()) {
            nlohmann::json j = {
                {"corpus",    supplement->corpus},
                {"path",      supplement->path},
                {"text",      supplement->content},
                {"fromLine",  supplement->from_line},
                {"lineCount", supplement->line_count},
            };
            if (supplement->title.has_value()) j["title"] = *supplement->title;
            if (supplement->kind.has_value()) j["kind"] = *supplement->kind;
            if (supplement->provenance_label.has_value()) j["provenanceLabel"] = *supplement->provenance_label;
            return tool_result::json(core::JsonValue(j));
        }
        nlohmann::json j = {
            {"path",     rel_path},
            {"text",     ""},
            {"disabled", true},
            {"error",    "wiki corpus result not found"},
        };
        return tool_result::json(core::JsonValue(j));
    }

    // 🎯 对齐 OpenClaw: getMemoryManagerContext — if "error" in memory → disabled result
    if (!manager_) {
        nlohmann::json j = {
            {"path",     rel_path},
            {"text",     ""},
            {"disabled", true},
            {"error",    "memory manager not initialized"},
        };
        return tool_result::json(core::JsonValue(j));
    }

    try {
        // 🎯 对齐 OpenClaw: memory.manager.readFile({ relPath, from?, lines? })
        ReadFileParams read_params;
        read_params.rel_path = rel_path;
        read_params.from     = from;
        read_params.lines    = lines;

        auto result = manager_->read_file(read_params);

        // 🎯 对齐 OpenClaw: return jsonResult(result)
        // ReadFileResult: { path, text, error? }
        nlohmann::json response = {
            {"path", result.path},
            {"text", result.text},
        };
        if (result.error.has_value()) {
            response["disabled"] = true;
            response["error"]    = *result.error;
        }
        return tool_result::json(core::JsonValue(response));

    } catch (const std::exception& e) {
        // 🎯 对齐 OpenClaw: catch (err) → { path, text: "", disabled: true, error: message }
        nlohmann::json j = {
            {"path",     rel_path},
            {"text",     ""},
            {"disabled", true},
            {"error",    std::string(e.what())},
        };
        return tool_result::json(core::JsonValue(j));
    }
}

// ============================================================================
// MemoryToolFactory Implementation
// ============================================================================

std::vector<std::shared_ptr<Tool>> MemoryToolFactory::create_memory_tools(
    std::shared_ptr<MemorySearchManager> manager
) {
    return {
        create_memory_search(manager),
        create_memory_get(manager),
    };
}

std::shared_ptr<Tool> MemoryToolFactory::create_memory_search(
    std::shared_ptr<MemorySearchManager> manager
) {
    return std::make_shared<MemorySearchTool>(manager);
}

std::shared_ptr<Tool> MemoryToolFactory::create_memory_get(
    std::shared_ptr<MemorySearchManager> manager
) {
    return std::make_shared<MemoryGetTool>(manager);
}

} // namespace tools
} // namespace hunterclaw
