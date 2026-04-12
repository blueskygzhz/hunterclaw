// ============================================================================
// context_pruning.cc
// 严格对齐 OpenClaw: pi-hooks/context-pruning/ (pruner.ts 10.6KB + settings.ts 3.5KB)
//                   + post-compaction-context.ts (8.2KB)
// ============================================================================

#include "hunterclaw/context/context_pruning.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <regex>
#include <map>

namespace hunterclaw {
namespace context {

// ============================================================================
// Constants (对齐 OC: post-compaction-context.ts)
// ============================================================================

static constexpr int MAX_CONTEXT_CHARS = 3000;
static const std::vector<std::string> DEFAULT_POST_COMPACTION_SECTIONS =
    {"Session Startup", "Red Lines"};
static const std::vector<std::string> LEGACY_POST_COMPACTION_SECTIONS =
    {"Every Session", "Safety"};

// ============================================================================
// Settings Implementation
// ============================================================================

EffectiveContextPruningSettings default_context_pruning_settings() {
    return EffectiveContextPruningSettings{
        false, 0, 3000, {}, {}
    };
}

EffectiveContextPruningSettings compute_effective_settings(
    const ContextPruningConfig& config
) {
    EffectiveContextPruningSettings settings;
    settings.enabled = config.enabled;
    settings.max_turns = config.max_turns;
    settings.max_tool_result_chars = config.max_tool_result_chars > 0
        ? config.max_tool_result_chars : 3000;
    settings.tool_matches = config.tool_matches;
    settings.preserve_tool_names = config.preserve_tool_names;
    return settings;
}

// ============================================================================
// Pruner Implementation (对齐 OC: pruner.ts)
// ============================================================================

PruneResult prune_context_messages(
    const std::vector<PrunableMessage>& messages,
    const EffectiveContextPruningSettings& settings
) {
    if (!settings.enabled || messages.empty()) {
        return {messages, 0, 0};
    }

    std::vector<PrunableMessage> result;
    int pruned = 0;
    int truncated = 0;

    // Find compaction boundary (if any)
    int compaction_idx = -1;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].is_compaction_marker) {
            compaction_idx = i;
            break;
        }
    }

    // Start from after compaction marker (or beginning)
    int start = compaction_idx >= 0 ? compaction_idx : 0;

    // Track tool invocations for keep_latest_only
    std::map<std::string, int> tool_latest_idx;
    for (int i = start; i < static_cast<int>(messages.size()); ++i) {
        if (messages[i].role == "tool" && !messages[i].tool_name.empty()) {
            tool_latest_idx[messages[i].tool_name] = i;
        }
    }

    // Apply max_turns limit
    int turn_count = 0;
    int turn_start = static_cast<int>(messages.size());
    if (settings.max_turns > 0) {
        for (int i = static_cast<int>(messages.size()) - 1; i >= start; --i) {
            if (messages[i].role == "user") {
                turn_count++;
                if (turn_count >= settings.max_turns) {
                    turn_start = i;
                    break;
                }
            }
        }
    }

    // Copy messages before compaction marker as-is
    for (int i = 0; i < start; ++i) {
        result.push_back(messages[i]);
    }

    // Process messages after compaction
    for (int i = start; i < static_cast<int>(messages.size()); ++i) {
        const auto& msg = messages[i];

        // Prune if before turn window
        if (settings.max_turns > 0 && i < turn_start && !msg.is_compaction_marker) {
            pruned++;
            continue;
        }

        // Check tool pruning rules
        if (msg.role == "tool" && !msg.tool_name.empty()) {
            // Check preserve list
            if (settings.preserve_tool_names.count(msg.tool_name)) {
                // Always keep preserved tools
            } else {
                // Check tool match rules
                bool matched = false;
                for (const auto& match : settings.tool_matches) {
                    if (match.tool_name == msg.tool_name ||
                        match.tool_name == "*") {
                        matched = true;
                        // keep_latest_only: skip if not the latest
                        if (match.keep_latest_only) {
                            auto it = tool_latest_idx.find(msg.tool_name);
                            if (it != tool_latest_idx.end() && it->second != i) {
                                pruned++;
                                continue;
                            }
                        }
                        break;
                    }
                }
                (void)matched;
            }
        }

        // Truncate long tool results
        PrunableMessage out = msg;
        if (msg.role == "tool" &&
            static_cast<int>(msg.content.size()) > settings.max_tool_result_chars) {
            out.content = msg.content.substr(0, settings.max_tool_result_chars) +
                "\n... [truncated]";
            truncated++;
        }

        result.push_back(out);
    }

    return {result, pruned, truncated};
}

// ============================================================================
// Post-Compaction Context (对齐 OC: post-compaction-context.ts)
// ============================================================================

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// Parse markdown sections (## heading) from content
std::map<std::string, std::string> parse_markdown_sections(const std::string& content) {
    std::map<std::string, std::string> sections;
    std::istringstream stream(content);
    std::string line;
    std::string current_heading;
    std::ostringstream current_body;

    while (std::getline(stream, line)) {
        if (line.size() >= 2 && line[0] == '#' && line[1] == '#') {
            // Save previous section
            if (!current_heading.empty()) {
                sections[current_heading] = trim(current_body.str());
            }
            // Extract heading text
            auto heading_start = line.find_first_not_of("# ");
            current_heading = heading_start != std::string::npos
                ? trim(line.substr(heading_start)) : "";
            current_body.str("");
            current_body.clear();
        } else if (!current_heading.empty()) {
            current_body << line << "\n";
        }
    }
    // Save last section
    if (!current_heading.empty()) {
        sections[current_heading] = trim(current_body.str());
    }
    return sections;
}

std::string format_date_stamp(int64_t now_ms, const std::string& timezone) {
    (void)timezone; // simplified: use UTC
    auto tp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(now_ms > 0 ? now_ms :
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_val;
    gmtime_r(&time_t_val, &tm_val);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday);
    return buf;
}

} // namespace

std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir,
    const std::string& timezone,
    int64_t now_ms
) {
    auto agents_path = std::filesystem::path(workspace_dir) / "AGENTS.md";

    if (!std::filesystem::exists(agents_path)) {
        return std::nullopt;
    }

    try {
        std::ifstream f(agents_path);
        if (!f.is_open()) return std::nullopt;

        std::ostringstream ss;
        ss << f.rdbuf();
        auto content = ss.str();

        if (content.empty()) return std::nullopt;

        // Parse sections
        auto sections = parse_markdown_sections(content);
        if (sections.empty()) return std::nullopt;

        // Find matching sections (default or legacy)
        std::vector<std::string> matching_sections;
        for (const auto& section_name : DEFAULT_POST_COMPACTION_SECTIONS) {
            auto lower = to_lower(section_name);
            for (const auto& [heading, body] : sections) {
                if (to_lower(heading) == lower && !body.empty()) {
                    matching_sections.push_back(body);
                }
            }
        }

        // Fallback to legacy sections
        if (matching_sections.empty()) {
            for (const auto& section_name : LEGACY_POST_COMPACTION_SECTIONS) {
                auto lower = to_lower(section_name);
                for (const auto& [heading, body] : sections) {
                    if (to_lower(heading) == lower && !body.empty()) {
                        matching_sections.push_back(body);
                    }
                }
            }
        }

        if (matching_sections.empty()) return std::nullopt;

        // Join sections, apply date substitution
        std::ostringstream result;
        for (size_t i = 0; i < matching_sections.size(); ++i) {
            if (i > 0) result << "\n\n";
            result << matching_sections[i];
        }

        auto text = result.str();

        // Truncate to MAX_CONTEXT_CHARS
        if (static_cast<int>(text.size()) > MAX_CONTEXT_CHARS) {
            text = text.substr(0, MAX_CONTEXT_CHARS) + "\n[truncated]";
        }

        // Substitute YYYY-MM-DD placeholders
        auto date_stamp = format_date_stamp(now_ms, timezone);
        std::string placeholder = "YYYY-MM-DD";
        size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::string::npos) {
            text.replace(pos, placeholder.size(), date_stamp);
            pos += date_stamp.size();
        }

        return text.empty() ? std::nullopt : std::optional(text);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace context
} // namespace hunterclaw
