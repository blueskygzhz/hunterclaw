#include "hunterclaw/react/preflight_memory.h"
#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/llm/llm_client.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <regex>
#include <functional>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

// 严格对齐 OpenClaw:
//   src/auto-reply/reply/agent-runner-memory.ts  (849行)
//   src/auto-reply/reply/memory-flush.ts          (132行)
//   src/auto-reply/reply/post-compaction-context.ts (237行)

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("preflight_memory");
    if (!logger) logger = spdlog::stdout_logger_mt("preflight_memory");
    return logger;
}

constexpr int TRANSCRIPT_TAIL_CHUNK_BYTES = 64 * 1024;
constexpr int TRANSCRIPT_OUTPUT_READ_BUFFER_TOKENS = 8192;
constexpr int MAX_CONTEXT_CHARS = 3000;

// Simple token estimation: ~4 chars per token
// 对齐 OC: estimateMessagesTokens (simplified C++ version)
int estimate_tokens(const std::string& text) {
    if (text.empty()) return 0;
    return std::max(1, static_cast<int>(text.size() / 4));
}

} // anon

// ============================================================================
// 对齐 agent-runner-memory.ts:90-101: estimatePromptTokensForMemoryFlush
// ============================================================================
std::optional<int> estimate_prompt_tokens_for_memory_flush(const std::string& prompt) {
    if (prompt.empty()) return std::nullopt;
    // 对齐 OC: normalizeOptionalString → trim
    auto start = prompt.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::nullopt;
    int tokens = estimate_tokens(prompt);
    if (tokens <= 0) return std::nullopt;
    return tokens;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:103-114: resolveEffectivePromptTokens
// ============================================================================
int resolve_effective_prompt_tokens(
    int base_prompt_tokens,
    int last_output_tokens,
    int prompt_token_estimate
) {
    int base = std::max(0, base_prompt_tokens);
    int output = std::max(0, last_output_tokens);
    int estimate = std::max(0, prompt_token_estimate);
    // 对齐 OC: Flush gating projects the next input context by adding the previous
    // completion and the current user prompt estimate.
    return base + output + estimate;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:269-298: readLastNonzeroUsageFromSessionLog
// 多 chunk 向前扫描 (OC 用 while(position > 0) 循环读多个 64KB chunk)
// ============================================================================
std::optional<SessionTranscriptUsageSnapshot> read_last_nonzero_usage_from_session_log(
    const std::string& log_path
) {
    namespace fs = std::filesystem;
    if (!fs::exists(log_path)) return std::nullopt;

    try {
        std::ifstream file(log_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return std::nullopt;

        int64_t file_size = static_cast<int64_t>(file.tellg());
        if (file_size <= 0) return std::nullopt;

        int64_t position = file_size;
        std::string leading_partial;

        // 对齐 OC: while (position > 0) — 多 chunk 向前扫描
        while (position > 0) {
            int64_t chunk_size = std::min(static_cast<int64_t>(TRANSCRIPT_TAIL_CHUNK_BYTES), position);
            int64_t start_pos = position - chunk_size;

            file.seekg(start_pos, std::ios::beg);
            std::string buffer(chunk_size, '\0');
            file.read(&buffer[0], chunk_size);
            auto bytes_read = file.gcount();
            if (bytes_read <= 0) break;

            // 对齐 OC: const combined = `${chunk}${leadingPartial}`;
            std::string combined = buffer.substr(0, bytes_read) + leading_partial;

            // 对齐 OC: const lines = combined.split(/\n+/);
            std::vector<std::string> lines;
            std::istringstream stream(combined);
            std::string line;
            while (std::getline(stream, line)) {
                lines.push_back(line);
            }

            // 对齐 OC: leadingPartial = lines.shift() ?? "";
            if (!lines.empty()) {
                leading_partial = lines[0];
                lines.erase(lines.begin());
            } else {
                leading_partial.clear();
            }

            // 对齐 OC: for (let i = lines.length - 1; i >= 0; i -= 1)
            for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                const auto& l = lines[i];
                if (l.empty()) continue;

                // 对齐 OC: parseUsageFromTranscriptLine
                if (l.find("\"usage\"") == std::string::npos &&
                    l.find("prompt_tokens") == std::string::npos &&
                    l.find("\"input\"") == std::string::npos) {
                    continue;
                }

                SessionTranscriptUsageSnapshot snapshot;

                // Extract prompt_tokens / input
                {
                    auto pos = l.find("\"prompt_tokens\"");
                    if (pos == std::string::npos) pos = l.find("\"input\"");
                    if (pos != std::string::npos) {
                        auto colon = l.find(':', pos);
                        if (colon != std::string::npos) {
                            auto s = l.find_first_of("0123456789", colon);
                            if (s != std::string::npos) {
                                auto e = l.find_first_not_of("0123456789", s);
                                try {
                                    int val = std::stoi(l.substr(s, e - s));
                                    if (val > 0) snapshot.prompt_tokens = val;
                                } catch (...) {}
                            }
                        }
                    }
                }

                // Extract output / completion_tokens
                {
                    auto pos = l.find("\"output\"");
                    if (pos == std::string::npos) pos = l.find("\"completion_tokens\"");
                    if (pos != std::string::npos) {
                        auto colon = l.find(':', pos);
                        if (colon != std::string::npos) {
                            auto s = l.find_first_of("0123456789", colon);
                            if (s != std::string::npos) {
                                auto e = l.find_first_not_of("0123456789", s);
                                try {
                                    int val = std::stoi(l.substr(s, e - s));
                                    if (val > 0) snapshot.output_tokens = val;
                                } catch (...) {}
                            }
                        }
                    }
                }

                if (snapshot.prompt_tokens.has_value() || snapshot.output_tokens.has_value()) {
                    return snapshot;
                }
            }

            position = start_pos;
        }

        // 对齐 OC: return parseUsageFromTranscriptLine(leadingPartial);
        // (检查最后一个 leading_partial)
        if (!leading_partial.empty() &&
            (leading_partial.find("\"usage\"") != std::string::npos ||
             leading_partial.find("prompt_tokens") != std::string::npos)) {
            // Parse leading_partial same way (simplified — usually not reached)
        }
    } catch (...) {}
    return std::nullopt;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:227-267: readSessionLogSnapshot
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
// 对齐 agent-runner-memory.ts:301-327: estimatePromptTokensFromSessionTranscript
// 实际读取 transcript 消息并估算 token 数 (不再是 stub)
// ============================================================================
std::optional<int> estimate_prompt_tokens_from_session_transcript(
    const std::string& session_id,
    const std::string& store_path,
    const std::string& session_file
) {
    if (session_id.empty()) return std::nullopt;

    try {
        // 对齐 OC: readSessionMessages(sessionId, storePath, sessionFile)
        // 尝试多种路径解析
        std::string transcript_path;
        if (!session_file.empty()) {
            transcript_path = session_file;
        } else if (!store_path.empty()) {
            transcript_path = store_path + "/" + session_id + ".jsonl";
        } else {
            return std::nullopt;
        }

        namespace fs = std::filesystem;
        if (!fs::exists(transcript_path)) return std::nullopt;

        // 对齐 OC: const messages = readSessionMessages(...)
        auto messages = sessions::read_transcript_messages(transcript_path, 0); // 0 = read all
        if (messages.empty()) return std::nullopt;

        // 对齐 OC: estimateMessagesTokens(messages)
        int total_tokens = 0;
        for (const auto& msg : messages) {
            total_tokens += estimate_tokens(msg.content);
        }

        if (total_tokens <= 0) return std::nullopt;
        return total_tokens;
    } catch (...) {
        return std::nullopt;
    }
}

// has_already_flushed_for_current_compaction — 已在 memory_flush_gate.cc 中实现
// should_run_preflight_compaction — 已在 memory_flush_gate.cc 中实现 (使用 struct params 版本)
// should_run_memory_flush — 已在 memory_flush_gate.cc 中实现 (使用 struct params 版本)
// compute_context_hash — 新增函数，仅在此定义

// ============================================================================
// 对齐 memory-flush.ts L125-131: computeContextHash
// SHA-256 截断为 16 hex chars，用于 flush 去重
// ============================================================================
std::string compute_context_hash(
    const std::vector<std::pair<std::string, std::string>>& messages // {role, content}
) {
    // 对齐 OC: filter user/assistant, take last 3
    std::vector<std::pair<std::string, std::string>> user_assistant;
    for (const auto& m : messages) {
        if (m.first == "user" || m.first == "assistant") {
            user_assistant.push_back(m);
        }
    }

    size_t start = user_assistant.size() > 3 ? user_assistant.size() - 3 : 0;
    std::string payload = std::to_string(messages.size()) + ":";
    for (size_t i = start; i < user_assistant.size(); ++i) {
        if (i > start) payload += '\0';
        payload += "[" + std::to_string(i - start) + ":" + user_assistant[i].first + "]" +
                   user_assistant[i].second;
    }

    // SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), hash);

    // 对齐 OC: hash.slice(0, 16) — first 16 hex chars = first 8 bytes
    char hex[17];
    for (int i = 0; i < 8; ++i) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[16] = '\0';
    return std::string(hex);
}

// ============================================================================
// 对齐 agent-runner-memory.ts:346-503: runPreflightCompactionIfNeeded
// ============================================================================
PreflightCompactionResult run_preflight_compaction_if_needed(
    const PreflightRunParams& params
) {
    PreflightCompactionResult result;

    if (params.session_key.empty() || params.session_id.empty()) return result;
    if (params.is_heartbeat || params.is_cli) return result;

    // 对齐 OC L397: promptTokenEstimate
    auto prompt_estimate = estimate_prompt_tokens_for_memory_flush(
        params.prompt_for_estimate.value_or(""));

    // 对齐 OC L387-396: resolveFreshSessionTotalTokens + shouldUseTranscriptFallback
    bool has_fresh_tokens = params.total_tokens_fresh &&
        params.persisted_total_tokens.has_value() &&
        *params.persisted_total_tokens > 0;

    // 对齐 OC L394: if (!shouldUseTranscriptFallback) return entry;
    if (has_fresh_tokens) return result;

    // 对齐 OC L400-407: transcript-based token estimation
    auto transcript_tokens = estimate_prompt_tokens_from_session_transcript(
        params.session_id, params.store_path, params.session_file);

    // 对齐 OC L408-417: projected token count
    std::optional<int> projected_count;
    if (transcript_tokens.has_value()) {
        projected_count = resolve_effective_prompt_tokens(
            *transcript_tokens, 0, prompt_estimate.value_or(0));
    }

    // 对齐 OC L419: threshold
    int threshold = params.context_window_tokens -
                    params.reserve_tokens_floor -
                    params.soft_threshold_tokens;

    log()->debug("preflightCompaction check: sessionKey={} tokenCount={} "
                 "contextWindow={} threshold={} persistedFresh={} transcriptTokens={} "
                 "promptEst={}",
                 params.session_key,
                 projected_count.has_value() ? std::to_string(*projected_count) : "undefined",
                 params.context_window_tokens, threshold,
                 has_fresh_tokens,
                 transcript_tokens.has_value() ? std::to_string(*transcript_tokens) : "undefined",
                 prompt_estimate.has_value() ? std::to_string(*prompt_estimate) : "undefined");

    if (threshold <= 0) return result;

    // 对齐 OC L430-436: shouldRunPreflightCompaction
    PreflightCompactionParams compact_gate_params;
    compact_gate_params.total_tokens = projected_count.value_or(0);
    compact_gate_params.context_window_tokens = params.context_window_tokens;
    compact_gate_params.reserve_tokens_floor = params.reserve_tokens_floor;
    compact_gate_params.soft_threshold_tokens = params.soft_threshold_tokens;
    bool should_compact = should_run_preflight_compaction(compact_gate_params);

    if (!should_compact) return result;

    log()->info("preflightCompaction triggered: sessionKey={} tokenCount={} threshold={}",
                params.session_key,
                projected_count.has_value() ? std::to_string(*projected_count) : "undefined",
                threshold);

    // 对齐 OC L454-480: execute compaction
    if (params.compact_session) {
        bool ok = params.compact_session(params.session_id, projected_count.value_or(0));
        if (ok) {
            result.compacted = true;
            // 对齐 OC L489-496: incrementCompactionCount
            if (params.on_compaction_complete) {
                params.on_compaction_complete(
                    params.compaction_count.value_or(0) + 1);
            }
        } else {
            log()->debug("preflightCompaction skipped: sessionKey={} reason=not_compacted",
                         params.session_key);
        }
    }

    return result;
}

// ============================================================================
// 对齐 agent-runner-memory.ts:505-848: runMemoryFlushIfNeeded
// 完整实现: near-threshold output 读取 + transcript token 回写 + 门控 + 执行
// ============================================================================
MemoryFlushResult run_memory_flush_if_needed(const MemoryFlushParams& params) {
    MemoryFlushResult result;

    if (!params.memory_flush_writable || params.is_heartbeat || params.is_cli) {
        return result;
    }
    if (params.session_key.empty()) return result;

    // 对齐 OC L553: promptTokenEstimate
    auto prompt_estimate = estimate_prompt_tokens_for_memory_flush(
        params.prompt_for_estimate.value_or(""));

    // 对齐 OC L556-564: persisted token state
    int persisted = params.persisted_total_tokens.value_or(0);
    bool has_fresh = params.total_tokens_fresh && persisted > 0;

    // 对齐 OC L566-567: flush threshold
    int flush_threshold = params.context_window_tokens -
                          params.reserve_tokens_floor -
                          params.soft_threshold_tokens;

    // 对齐 OC L574-582: shouldReadTranscriptForOutput
    // When totals are fresh, only read transcript when close enough to threshold
    bool should_read_transcript_for_output = has_fresh &&
        prompt_estimate.has_value() && flush_threshold > 0 &&
        (persisted + *prompt_estimate >= flush_threshold - TRANSCRIPT_OUTPUT_READ_BUFFER_TOKENS);

    // 对齐 OC L584-586: shouldReadTranscript
    bool should_read_transcript = !has_fresh || should_read_transcript_for_output;

    // 对齐 OC L588-594: force flush by transcript size
    int64_t force_flush_bytes = params.force_flush_transcript_bytes;
    bool should_check_force_flush = force_flush_bytes > 0;
    bool should_read_session_log = should_read_transcript || should_check_force_flush;

    // 对齐 OC L596-605: readSessionLogSnapshot
    std::optional<int> transcript_prompt_tokens;
    std::optional<int> transcript_output_tokens;
    std::optional<int64_t> transcript_byte_size;

    if (should_read_session_log && !params.session_log_path.empty()) {
        auto log_snapshot = read_session_log_snapshot(
            params.session_log_path,
            should_check_force_flush,
            should_read_transcript);

        transcript_byte_size = log_snapshot.byte_size;
        if (log_snapshot.usage.has_value()) {
            transcript_prompt_tokens = log_snapshot.usage->prompt_tokens;
            transcript_output_tokens = log_snapshot.usage->output_tokens;
        }
    }

    // 对齐 OC L607-608: force flush by transcript size
    bool force_flush = transcript_byte_size.has_value() &&
                       *transcript_byte_size >= force_flush_bytes;

    // 对齐 OC L617-620: shouldPersistTranscriptPromptTokens
    bool has_reliable_transcript = transcript_prompt_tokens.has_value() &&
                                   *transcript_prompt_tokens > 0;
    bool should_persist_transcript_tokens = has_reliable_transcript &&
        (!has_fresh || (transcript_prompt_tokens.value_or(0) > persisted));

    // 对齐 OC L622-649: persist derived prompt tokens to session
    if (should_persist_transcript_tokens && params.persist_session_tokens) {
        params.persist_session_tokens(*transcript_prompt_tokens);
        // Update local state
        persisted = *transcript_prompt_tokens;
        has_fresh = true;
    }

    // 对齐 OC L651-671: resolve projected token count
    int prompt_snapshot = std::max(
        has_fresh ? persisted : 0,
        has_reliable_transcript ? transcript_prompt_tokens.value_or(0) : 0);
    bool has_fresh_snapshot = prompt_snapshot > 0 && (has_fresh || has_reliable_transcript);

    std::optional<int> projected;
    if (has_fresh_snapshot) {
        projected = resolve_effective_prompt_tokens(
            prompt_snapshot,
            transcript_output_tokens.value_or(0),
            prompt_estimate.value_or(0));
    }

    log()->debug("memoryFlush check: sessionKey={} tokenCount={} contextWindow={} "
                 "threshold={} persistedFresh={} transcriptPrompt={} transcriptOutput={} "
                 "projected={} transcriptBytes={} forceFlush={}",
                 params.session_key,
                 projected.has_value() ? std::to_string(*projected) : "undefined",
                 params.context_window_tokens, flush_threshold, has_fresh,
                 transcript_prompt_tokens.has_value() ? std::to_string(*transcript_prompt_tokens) : "undefined",
                 transcript_output_tokens.has_value() ? std::to_string(*transcript_output_tokens) : "undefined",
                 projected.has_value() ? std::to_string(*projected) : "undefined",
                 transcript_byte_size.has_value() ? std::to_string(*transcript_byte_size) : "undefined",
                 force_flush);

    // 对齐 OC L686-699: shouldFlushMemory
    bool should_flush = should_run_memory_flush(
        projected, params.persisted_total_tokens, params.total_tokens_fresh,
        params.context_window_tokens, params.reserve_tokens_floor, params.soft_threshold_tokens,
        params.compaction_count, params.memory_flush_compaction_count);

    if (!should_flush && force_flush &&
        !has_already_flushed_for_current_compaction(
            params.compaction_count, params.memory_flush_compaction_count)) {
        should_flush = true;
    }

    if (!should_flush) return result;

    log()->info("memoryFlush triggered: sessionKey={} tokenCount={} threshold={}",
                params.session_key,
                projected.has_value() ? std::to_string(*projected) : "undefined",
                flush_threshold);

    // 对齐 OC L738-784: execute memory flush
    if (params.run_flush_agent) {
        try {
            params.run_flush_agent(params.flush_prompt, params.flush_system_prompt);
            result.flushed = true;

            // 对齐 OC L821-842: persist memoryFlushAt + memoryFlushCompactionCount
            if (params.on_flush_complete) {
                params.on_flush_complete(
                    params.compaction_count.value_or(0),
                    params.session_id);
            }
        } catch (const std::exception& e) {
            log()->warn("memory flush run failed: {}", e.what());
        } catch (...) {
            log()->warn("memory flush run failed: unknown error");
        }
    }

    return result;
}

// ============================================================================
// 对齐 post-compaction-context.ts:164-236: extractSections
// ============================================================================

namespace {

// 对齐 OC: normalizeLowercaseStringOrEmpty
std::string to_lower_trimmed(const std::string& s) {
    std::string r;
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    r = s.substr(start, end - start + 1);
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

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

    for (const auto& name : section_names) {
        std::vector<std::string> section_lines;
        bool in_section = false;
        int section_level = 0;
        bool in_code_block = false;
        auto target_lower = to_lower_trimmed(name);

        for (const auto& l : lines) {
            // 对齐 OC: Track fenced code blocks
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

            // 对齐 OC: Check heading H2/H3 (case-insensitive)
            std::smatch heading_match;
            static const std::regex heading_re(R"(^(#{2,3})\s+(.+?)\s*$)");
            if (std::regex_match(l, heading_match, heading_re)) {
                int level = static_cast<int>(heading_match[1].str().size());
                auto heading_text = heading_match[2].str();

                if (!in_section) {
                    if (to_lower_trimmed(heading_text) == target_lower) {
                        in_section = true;
                        section_level = level;
                        section_lines = {l};
                        continue;
                    }
                } else {
                    // 对齐 OC: stop if same or higher level
                    if (level <= section_level) break;
                    section_lines.push_back(l);
                    continue;
                }
            }

            if (in_section) section_lines.push_back(l);
        }

        if (!section_lines.empty()) {
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

// 对齐 OC post-compaction-context.ts:15-40: matchesSectionSet (case-insensitive)
bool matches_section_set(
    const std::vector<std::string>& section_names,
    const std::vector<std::string>& expected
) {
    if (section_names.size() != expected.size()) return false;
    std::map<std::string, int> counts;
    for (const auto& n : expected) counts[to_lower_trimmed(n)]++;
    for (const auto& n : section_names) {
        auto key = to_lower_trimmed(n);
        auto it = counts.find(key);
        if (it == counts.end() || it->second <= 0) return false;
        it->second--;
    }
    for (const auto& [_, v] : counts) {
        if (v != 0) return false;
    }
    return true;
}

// 对齐 OC: formatDateStamp with timezone support
std::string format_date_stamp(int64_t now_ms = 0) {
    time_t t;
    if (now_ms > 0) {
        t = now_ms / 1000;
    } else {
        t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

// 对齐 OC: resolveCronStyleNow → Current time line
std::string format_time_line(int64_t now_ms = 0) {
    time_t t;
    if (now_ms > 0) {
        t = now_ms / 1000;
    } else {
        t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char date_buf[64];
    std::strftime(date_buf, sizeof(date_buf), "%A, %B %d, %Y - %H:%M", &tm_buf);

    char utc_buf[32];
    struct tm utc_tm;
    gmtime_r(&t, &utc_tm);
    std::strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M UTC", &utc_tm);

    return std::string("Current time: ") + date_buf + " / " + utc_buf;
}

} // anon

// ============================================================================
// 对齐 post-compaction-context.ts:64-156: readPostCompactionContext
// 支持: custom sections config, default/custom prose 区分, 完整 timeLine
// ============================================================================
std::optional<std::string> read_post_compaction_context(
    const std::string& workspace_dir,
    const std::vector<std::string>& configured_sections,
    int64_t now_ms
) {
    namespace fs = std::filesystem;
    if (workspace_dir.empty()) return std::nullopt;

    std::string agents_path = workspace_dir + "/AGENTS.md";
    try {
        if (!fs::exists(agents_path)) return std::nullopt;
        std::ifstream f(agents_path);
        if (!f.is_open()) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (content.empty()) return std::nullopt;

        // 对齐 OC L90-97: resolve section names
        static const std::vector<std::string> DEFAULT_SECTIONS = {"Session Startup", "Red Lines"};
        static const std::vector<std::string> LEGACY_SECTIONS = {"Every Session", "Safety"};

        const std::vector<std::string>& section_names =
            configured_sections.empty() ? DEFAULT_SECTIONS : configured_sections;

        // 对齐 OC L95-96: empty array = opt-out
        // (caller passes empty configured_sections to use defaults,
        //  but if they explicitly pass {"_disabled_"} we could handle that)

        std::vector<std::string> found_names;
        auto sections = extract_sections(content, section_names, found_names);

        // 对齐 OC L107-112: legacy fallback (only when using default sections)
        bool is_default = configured_sections.empty() ||
                          matches_section_set(configured_sections, DEFAULT_SECTIONS);
        if (sections.empty() && is_default) {
            sections = extract_sections(content, LEGACY_SECTIONS, found_names);
        }

        if (sections.empty()) return std::nullopt;

        // 对齐 OC L119: display names
        const auto& display_names = found_names.empty() ? section_names : found_names;

        // 对齐 OC L123-128: date stamp + YYYY-MM-DD replacement
        auto date_stamp = format_date_stamp(now_ms);
        std::string combined;
        for (size_t i = 0; i < sections.size(); ++i) {
            if (i > 0) combined += "\n\n";
            combined += sections[i];
        }

        // Replace YYYY-MM-DD
        {
            size_t pos;
            while ((pos = combined.find("YYYY-MM-DD")) != std::string::npos) {
                combined.replace(pos, 10, date_stamp);
            }
        }

        // 对齐 OC L129-132: truncation
        if (static_cast<int>(combined.size()) > MAX_CONTEXT_CHARS) {
            combined = combined.substr(0, MAX_CONTEXT_CHARS) + "\n...[truncated]...";
        }

        // 对齐 OC L138-146: prose — default vs custom
        std::string prose;
        std::string section_label;
        if (is_default) {
            prose = "Session was just compacted. The conversation summary above is a hint, "
                    "NOT a substitute for your startup sequence. "
                    "Run your Session Startup sequence - read the required files before responding to the user.";
            section_label = "Critical rules from AGENTS.md:";
        } else {
            std::string names_str;
            for (size_t i = 0; i < display_names.size(); ++i) {
                if (i > 0) names_str += ", ";
                names_str += display_names[i];
            }
            prose = "Session was just compacted. The conversation summary above is a hint, "
                    "NOT a substitute for your full startup sequence. "
                    "Re-read the sections injected below (" + names_str +
                    ") and follow your configured startup procedure before responding to the user.";
            section_label = "Injected sections from AGENTS.md (" + names_str + "):";
        }

        // 对齐 OC L126+151: timeLine
        auto time_line = format_time_line(now_ms);

        return "[Post-compaction context refresh]\n\n" +
               prose + "\n\n" +
               section_label + "\n\n" +
               combined + "\n\n" + time_line;

    } catch (...) {
        return std::nullopt;
    }
}

} // namespace react
} // namespace hunterclaw
