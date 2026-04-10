#include "hunterclaw/memory/tools_citations.h"
#include <algorithm>
#include <set>
#include <sstream>

// 严格对齐 OpenClaw tools.citations.ts (91 行)

namespace hunterclaw {
namespace memory {

// 对齐 tools.citations.ts:9-15
MemoryCitationsMode resolve_memory_citations_mode(const std::string& mode_str) {
    if (mode_str == "on") return MemoryCitationsMode::On;
    if (mode_str == "off") return MemoryCitationsMode::Off;
    return MemoryCitationsMode::Auto;
}

// 对齐 tools.citations.ts:31-37
std::string format_memory_citation(const MemorySearchResult& entry) {
    std::string line_range;
    if (entry.start_line == entry.end_line) {
        line_range = "#L" + std::to_string(entry.start_line);
    } else {
        line_range = "#L" + std::to_string(entry.start_line) +
                     "-L" + std::to_string(entry.end_line);
    }
    return entry.path + line_range;
}

// 对齐 tools.citations.ts:17-29
std::vector<MemorySearchResult> decorate_citations(
    const std::vector<MemorySearchResult>& results,
    bool include
) {
    std::vector<MemorySearchResult> out;
    out.reserve(results.size());
    for (const auto& entry : results) {
        MemorySearchResult r = entry;
        if (!include) {
            r.citation = "";
        } else {
            std::string cit = format_memory_citation(entry);
            r.citation = cit;
            std::string trimmed = entry.snippet;
            auto s = trimmed.find_first_not_of(" \t\n\r");
            auto e = trimmed.find_last_not_of(" \t\n\r");
            trimmed = (s == std::string::npos) ? "" : trimmed.substr(s, e - s + 1);
            r.snippet = trimmed + "\n\nSource: " + cit;
        }
        out.push_back(r);
    }
    return out;
}

// 对齐 tools.citations.ts:39-63
std::vector<MemorySearchResult> clamp_results_by_injected_chars(
    const std::vector<MemorySearchResult>& results,
    int budget
) {
    if (budget <= 0) return results;
    int remaining = budget;
    std::vector<MemorySearchResult> clamped;
    for (const auto& entry : results) {
        if (remaining <= 0) break;
        int len = static_cast<int>(entry.snippet.size());
        if (len <= remaining) {
            clamped.push_back(entry);
            remaining -= len;
        } else {
            MemorySearchResult r = entry;
            r.snippet = entry.snippet.substr(0, std::max(0, remaining));
            clamped.push_back(r);
            break;
        }
    }
    return clamped;
}

// 对齐 tools.citations.ts:65-91
static std::string derive_chat_type_from_key(const std::string& session_key) {
    if (session_key.empty()) return "direct";
    // Simple heuristic: check for "channel" or "group" tokens
    std::string lower = session_key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::set<std::string> tokens;
    std::istringstream ss(lower);
    std::string tok;
    while (std::getline(ss, tok, ':')) {
        auto s = tok.find_first_not_of(" \t");
        if (s != std::string::npos) {
            auto e = tok.find_last_not_of(" \t");
            tokens.insert(tok.substr(s, e - s + 1));
        }
    }
    if (tokens.count("channel")) return "channel";
    if (tokens.count("group")) return "group";
    return "direct";
}

bool should_include_citations(
    MemoryCitationsMode mode,
    const std::string& session_key
) {
    if (mode == MemoryCitationsMode::On) return true;
    if (mode == MemoryCitationsMode::Off) return false;
    return derive_chat_type_from_key(session_key) == "direct";
}

}  // namespace memory
}  // namespace hunterclaw
