#include "hunterclaw/react/preflight_memory.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <regex>

namespace hunterclaw {
namespace react {

namespace {
constexpr int TRANSCRIPT_TAIL_CHUNK_BYTES = 64 * 1024;
constexpr int TRANSCRIPT_OUTPUT_READ_BUFFER_TOKENS = 8192;

// Simple token estimation: ~4 chars per token
int estimate_tokens(const std::string& text) {
    if (text.empty()) return 0;
    return std::max(1, static_cast<int>(text.size() / 4));
}
} // anon

// ============================================================================
// 对齐 agent-runner-memory.ts:75-86
// ============================================================================
std::optional<int> estimate_prompt_tokens_for_memory_flush(const std::string& prompt) {
    if (prompt.empty()) return std::nullopt;
    // Trim
    auto start = prompt.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::nullopt;
    int tokens = estimate_tokens(prompt);
    if (tokens <= 0) return std::nullopt;
    return tokens;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:88-99
// ============================================================================
int resolve_effective_prompt_tokens(
    int base_prompt_tokens,
    int last_output_tokens,
    int prompt_token_estimate
) {
    int base = std::max(0, base_prompt_tokens);
    int output = std::max(0, last_output_tokens);
    int estimate = std::max(0, prompt_token_estimate);
    return base + output + estimate;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:254-284: 读取 session log 尾部 usage
// ============================================================================
std::optional<SessionTranscriptUsageSnapshot> read_last_nonzero_usage_from_session_log(
    const std::string& log_path
) {
    namespace fs = std::filesystem;
    if (!fs::exists(log_path)) return std::nullopt;

    try {
        std::ifstream file(log_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return std::nullopt;

        auto file_size = file.tellg();
        if (file_size <= 0) return std::nullopt;

        // Read last TRANSCRIPT_TAIL_CHUNK_BYTES
        int64_t read_size = std::min(static_cast<int64_t>(file_size),
                                      static_cast<int64_t>(TRANSCRIPT_TAIL_CHUNK_BYTES));
        file.seekg(-read_size, std::ios::end);

        std::string chunk(read_size, '\0');
        file.read(&chunk[0], read_size);

        // Parse lines backwards looking for usage
        std::istringstream stream(chunk);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty()) lines.push_back(line);
        }

        // Search backwards for usage JSON
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            // Look for "usage" or "prompt_tokens" in line
            if (it->find("\"usage\"") != std::string::npos ||
                it->find("prompt_tokens") != std::string::npos) {
                // Basic extraction — real impl would use JSON parser
                SessionTranscriptUsageSnapshot snapshot;
                // Extract prompt_tokens
                {
                    auto pos = it->find("\"prompt_tokens\"");
                    if (pos == std::string::npos) pos = it->find("\"input\"");
                    if (pos != std::string::npos) {
                        auto colon = it->find(':', pos);
                        if (colon != std::string::npos) {
                            auto start = it->find_first_of("0123456789", colon);
                            if (start != std::string::npos) {
                                auto end = it->find_first_not_of("0123456789", start);
                                auto num_str = it->substr(start, end - start);
                                int val = std::stoi(num_str);
                                if (val > 0) snapshot.prompt_tokens = val;
                            }
                        }
                    }
                }
                // Extract output tokens
                {
                    auto pos = it->find("\"output\"");
                    if (pos == std::string::npos) pos = it->find("\"completion_tokens\"");
                    if (pos != std::string::npos) {
                        auto colon = it->find(':', pos);
                        if (colon != std::string::npos) {
                            auto start = it->find_first_of("0123456789", colon);
                            if (start != std::string::npos) {
                                auto end = it->find_first_not_of("0123456789", start);
                                auto num_str = it->substr(start, end - start);
                                int val = std::stoi(num_str);
                                if (val > 0) snapshot.output_tokens = val;
                            }
                        }
                    }
                }
                if (snapshot.prompt_tokens.has_value() || snapshot.output_tokens.has_value()) {
                    return snapshot;
                }
            }
        }
    } catch (...) {}
    return std::nullopt;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:212-252
// ============================================================================
SessionLogSnapshot read_session_log_snapshot(
    const std::string& log_path,
    bool include_byte_size,
    bool include_usage
) {
    SessionLogSnapshot snapshot;
    namespace fs = std::filesystem;

    if (include_byte_size) {
        try {
            auto size = fs::file_size(log_path);
            snapshot.byte_size = static_cast<int64_t>(size);
        } catch (...) {
            snapshot.byte_size = std::nullopt;
        }
    }

    if (include_usage) {
        snapshot.usage = read_last_nonzero_usage_from_session_log(log_path);
    }

    return snapshot;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:286-312
// ============================================================================
std::optional<int> estimate_prompt_tokens_from_session_transcript(
    const std::string& session_id,
    const std::string& /*store_path*/,
    const std::string& /*session_file*/
) {
    // Simplified: in production, would read transcript file and estimate tokens
    if (session_id.empty()) return std::nullopt;
    return std::nullopt; // Fallback to other methods
}

// ============================================================================
// 对齐 agent-runner-memory.ts:331-484: runPreflightCompactionIfNeeded
// ============================================================================
PreflightCompactionResult run_preflight_compaction_if_needed(
    const PreflightRunParams& params
) {
    PreflightCompactionResult result;

    if (params.session_key.empty() || params.session_id.empty()) return result;
    if (params.is_heartbeat || params.is_cli) return result;

    // Resolve token count for compaction check
    auto prompt_estimate = estimate_prompt_tokens_for_memory_flush(
        params.prompt_for_estimate.value_or(""));

    bool has_fresh_tokens = params.total_tokens_fresh &&
        params.persisted_total_tokens.has_value() &&
        *params.persisted_total_tokens > 0;

    if (has_fresh_tokens) return result; // Fresh tokens → skip transcript fallback

    // Try transcript-based estimation
    auto transcript_tokens = estimate_prompt_tokens_from_session_transcript(
        params.session_id, params.store_path, params.session_file);

    std::optional<int> projected_count;
    if (transcript_tokens.has_value()) {
        projected_count = resolve_effective_prompt_tokens(
            *transcript_tokens, 0, prompt_estimate.value_or(0));
    }

    // Check threshold
    int threshold = params.context_window_tokens -
                    params.reserve_tokens_floor -
                    params.soft_threshold_tokens;
    if (threshold <= 0) return result;

    bool should_compact = projected_count.has_value() && *projected_count >= threshold;
    if (!should_compact) return result;

    // Execute compaction
    if (params.compact_session) {
        bool ok = params.compact_session(params.session_id, projected_count.value_or(0));
        if (ok) {
            result.compacted = true;
            if (params.on_compaction_complete) {
                params.on_compaction_complete(
                    params.compaction_count.value_or(0) + 1);
            }
        }
    }

    return result;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:486-829: runMemoryFlushIfNeeded
// ============================================================================
MemoryFlushResult run_memory_flush_if_needed(const MemoryFlushParams& params) {
    MemoryFlushResult result;

    if (!params.memory_flush_writable || params.is_heartbeat || params.is_cli) {
        return result;
    }
    if (params.session_key.empty()) return result;

    // Resolve token counts
    auto prompt_estimate = estimate_prompt_tokens_for_memory_flush(
        params.prompt_for_estimate.value_or(""));

    int persisted = params.persisted_total_tokens.value_or(0);
    bool has_fresh = params.total_tokens_fresh && persisted > 0;

    int flush_threshold = params.context_window_tokens -
                          params.reserve_tokens_floor -
                          params.soft_threshold_tokens;

    // Check if we need transcript-based token estimation
    std::optional<int> transcript_prompt_tokens;
    std::optional<int> transcript_output_tokens;
    std::optional<int64_t> transcript_byte_size;

    bool should_read_transcript = !has_fresh;
    bool should_check_force_flush = params.force_flush_transcript_bytes > 0;

    if (should_read_transcript || should_check_force_flush) {
        // Would read session log in production
        // Simplified: use persisted tokens if available
    }

    // Check force flush by transcript size
    bool force_flush = false;
    if (should_check_force_flush && transcript_byte_size.has_value()) {
        force_flush = *transcript_byte_size >= params.force_flush_transcript_bytes;
    }

    // Resolve projected token count
    int prompt_snapshot = has_fresh ? persisted : 0;
    if (transcript_prompt_tokens.has_value() && *transcript_prompt_tokens > prompt_snapshot) {
        prompt_snapshot = *transcript_prompt_tokens;
    }

    std::optional<int> projected;
    if (prompt_snapshot > 0) {
        projected = resolve_effective_prompt_tokens(
            prompt_snapshot,
            transcript_output_tokens.value_or(0),
            prompt_estimate.value_or(0));
    }

    // Check memory flush gate
    bool should_flush = false;
    if (projected.has_value() && *projected >= flush_threshold) {
        // Check if already flushed for current compaction
        int comp_count = params.compaction_count.value_or(0);
        int flush_comp_count = params.memory_flush_compaction_count.value_or(-1);
        if (flush_comp_count != comp_count) {
            should_flush = true;
        }
    }
    if (force_flush) {
        int comp_count = params.compaction_count.value_or(0);
        int flush_comp_count = params.memory_flush_compaction_count.value_or(-1);
        if (flush_comp_count != comp_count) {
            should_flush = true;
        }
    }

    if (!should_flush) return result;

    // Execute memory flush
    if (params.run_flush_agent) {
        try {
            params.run_flush_agent(params.flush_prompt, params.flush_system_prompt);
            result.flushed = true;
            if (params.on_flush_complete) {
                params.on_flush_complete(
                    params.compaction_count.value_or(0),
                    params.session_id);
            }
        } catch (...) {
            // Memory flush is best-effort
        }
    }

    return result;
}

// ============================================================================
// 对齐 post-compaction-context.ts (236行): readPostCompactionContext
// 从 AGENTS.md 按 section 名提取，支持 legacy 回退、日期替换、截断保护
// ============================================================================

namespace {
constexpr int MAX_CONTEXT_CHARS = 3000;

// 对齐 post-compaction-context.ts:164-236: extractSections
std::vector<std::string> extract_sections(
    const std::string& content,
    const std::vector<std::string>& section_names,
    std::vector<std::string>& found_names
) {
    std::vector<std::string> results;
    std::istringstream stream(content);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);

    auto to_lower_local = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    };

    for (const auto& name : section_names) {
        std::vector<std::string> section_lines;
        bool in_section = false;
        int section_level = 0;
        bool in_code_block = false;
        auto target_lower = to_lower_local(name);

        for (const auto& l : lines) {
            // Track fenced code blocks
            auto trimmed_start = l.find_first_not_of(" \t");
            if (trimmed_start != std::string::npos && l.substr(trimmed_start, 3) == "```") {
                in_code_block = !in_code_block;
                if (in_section) section_lines.push_back(l);
                continue;
            }
            if (in_code_block) {
                if (in_section) section_lines.push_back(l);
                continue;
            }

            // Check heading: ## or ###
            std::smatch heading_match;
            static const std::regex heading_re(R"(^(#{2,3})\s+(.+?)\s*$)");
            if (std::regex_match(l, heading_match, heading_re)) {
                int level = static_cast<int>(heading_match[1].str().size());
                auto heading_text = heading_match[2].str();

                if (!in_section) {
                    if (to_lower_local(heading_text) == target_lower) {
                        in_section = true;
                        section_level = level;
                        section_lines = {l};
                        continue;
                    }
                } else {
                    if (level <= section_level) break;
                    section_lines.push_back(l);
                    continue;
                }
            }

            if (in_section) section_lines.push_back(l);
        }

        if (!section_lines.empty()) {
            // Join and trim
            std::string joined;
            for (size_t i = 0; i < section_lines.size(); ++i) {
                if (i > 0) joined += "\n";
                joined += section_lines[i];
            }
            // Trim trailing whitespace
            auto end = joined.find_last_not_of(" \t\n\r");
            if (end != std::string::npos) joined = joined.substr(0, end + 1);
            if (!joined.empty()) {
                results.push_back(joined);
                found_names.push_back(name);
            }
        }
    }
    return results;
}

// 对齐: formatDateStamp → YYYY-MM-DD
std::string format_date_stamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}
} // anon

std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir
) {
    namespace fs = std::filesystem;
    if (workspace_dir.empty()) return std::nullopt;

    // 对齐: read AGENTS.md
    std::string agents_path = workspace_dir + "/AGENTS.md";
    try {
        if (!fs::exists(agents_path)) return std::nullopt;
        std::ifstream f(agents_path);
        if (!f.is_open()) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (content.empty()) return std::nullopt;

        // 对齐: DEFAULT_POST_COMPACTION_SECTIONS + LEGACY fallback
        std::vector<std::string> default_sections = {"Session Startup", "Red Lines"};
        std::vector<std::string> legacy_sections = {"Every Session", "Safety"};

        std::vector<std::string> found_names;
        auto sections = extract_sections(content, default_sections, found_names);

        // Legacy fallback
        if (sections.empty()) {
            sections = extract_sections(content, legacy_sections, found_names);
        }
        if (sections.empty()) return std::nullopt;

        // 对齐: combine + YYYY-MM-DD replacement + truncation
        std::string combined;
        for (size_t i = 0; i < sections.size(); ++i) {
            if (i > 0) combined += "\n\n";
            combined += sections[i];
        }

        // Replace YYYY-MM-DD with real date
        auto date_stamp = format_date_stamp();
        {
            size_t pos;
            while ((pos = combined.find("YYYY-MM-DD")) != std::string::npos) {
                combined.replace(pos, 10, date_stamp);
            }
        }

        // Truncation
        if (static_cast<int>(combined.size()) > MAX_CONTEXT_CHARS) {
            combined = combined.substr(0, MAX_CONTEXT_CHARS) + "\n...[truncated]...";
        }

        // 对齐: format prose
        std::string display_names;
        for (size_t i = 0; i < found_names.size(); ++i) {
            if (i > 0) display_names += ", ";
            display_names += found_names[i];
        }

        return "[Post-compaction context refresh]\n\n"
               "Session was just compacted. The conversation summary above is a hint, "
               "NOT a substitute for your startup sequence. "
               "Run your Session Startup sequence - read the required files before responding to the user.\n\n"
               "Critical rules from AGENTS.md:\n\n" +
               combined + "\n\nCurrent date: " + date_stamp;

    } catch (...) {
        return std::nullopt;
    }
}

} // namespace react
} // namespace hunterclaw
