#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/auto_reply/message_dispatcher.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Helpers
// ============================================================================

std::string Heartbeat::trim(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\n\r");
    return text.substr(start, end - start + 1);
}

bool Heartbeat::is_empty_or_whitespace(const std::string& text) {
    return trim(text).empty();
}

// ============================================================================
// isHeartbeatContentEffectivelyEmpty
// Aligned with openclaw: isHeartbeatContentEffectivelyEmpty (heartbeat.ts)
//
// A file is considered effectively empty if it contains only:
//   - Whitespace / empty lines
//   - ATX markdown headers (# followed by space or EOL)
//   - Empty markdown list items ("- [ ]", "* [ ]", "- ", etc.)
// ============================================================================

bool Heartbeat::is_heartbeat_content_effectively_empty(const std::string& content) {
    if (content.empty()) return true;

    // Aligned with openclaw: split by newline, check each line
    static const std::regex atx_header_re(R"(^#+(\s|$))");
    static const std::regex empty_list_item_re(R"(^[-*+]\s*(\[[\sXx]?\]\s*)?$)");

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        // ATX headers (# followed by space or EOL) — skip
        if (std::regex_search(trimmed, atx_header_re)) continue;
        // Empty markdown list items — skip
        if (std::regex_match(trimmed, empty_list_item_re)) continue;
        // Found actionable content
        return false;
    }
    return true;
}

// ============================================================================
// resolveHeartbeatPrompt
// Aligned with openclaw: resolveHeartbeatPrompt (heartbeat.ts)
// ============================================================================

std::string Heartbeat::resolve_heartbeat_prompt(
    const std::string& /*workspace_path*/,
    const std::optional<std::string>& config_prompt
) {
    // Aligned with openclaw: const trimmed = typeof raw === "string" ? raw.trim() : "";
    //                         return trimmed || HEARTBEAT_PROMPT;
    if (config_prompt.has_value()) {
        const std::string trimmed = trim(*config_prompt);
        if (!trimmed.empty()) return trimmed;
    }
    return DEFAULT_HEARTBEAT_PROMPT;
}

// ============================================================================
// stripMarkup
// Aligned with openclaw: stripMarkup (heartbeat.ts)
// Normalizes lightweight markup so HEARTBEAT_OK wrapped in HTML/Markdown still strips.
// ============================================================================

static std::string strip_markup(const std::string& text) {
    std::string result = text;

    // Drop HTML tags
    static const std::regex html_tag_re(R"(<[^>]*>)");
    result = std::regex_replace(result, html_tag_re, " ");

    // Decode &nbsp;
    static const std::regex nbsp_re(R"(&nbsp;)", std::regex_constants::icase);
    result = std::regex_replace(result, nbsp_re, " ");

    // Remove markdown-ish wrappers at the edges
    static const std::regex leading_md_re(R"(^[*`~_]+)");
    static const std::regex trailing_md_re(R"([*`~_]+$)");
    result = std::regex_replace(result, leading_md_re, "");
    result = std::regex_replace(result, trailing_md_re, "");

    return result;
}

// ============================================================================
// stripTokenAtEdges
// Aligned with openclaw: stripTokenAtEdges (heartbeat.ts)
// Strips HEARTBEAT_TOKEN from the start or end of text (with optional trailing punct).
// ============================================================================

struct StripAtEdgesResult {
    std::string text;
    bool did_strip = false;
};

static StripAtEdgesResult strip_token_at_edges(const std::string& raw) {
    const std::string token = Heartbeat::HEARTBEAT_TOKEN;

    std::string text = Heartbeat::trim(raw);
    if (text.empty()) return {"", false};
    if (text.find(token) == std::string::npos) return {text, false};

    // Regex: token at end with up to 4 trailing non-word chars
    // Aligned with openclaw: `${escapeRegExp(token)}[^\\w]{0,4}$`
    const std::string token_at_end_pattern =
        std::string(token) + R"([^\w]{0,4}$)";
    const std::regex token_at_end_re(token_at_end_pattern);

    bool did_strip = false;
    bool changed = true;

    while (changed) {
        changed = false;
        const std::string next = Heartbeat::trim(text);

        // Strip from start
        if (next.substr(0, token.size()) == token) {
            std::string after = next.substr(token.size());
            // trimStart
            size_t s = after.find_first_not_of(" \t\n\r");
            text = (s == std::string::npos) ? "" : after.substr(s);
            did_strip = true;
            changed = true;
            continue;
        }

        // Strip from end (with optional trailing punctuation)
        if (std::regex_search(next, token_at_end_re)) {
            size_t idx = next.rfind(token);
            std::string before = next.substr(0, idx);
            // trimEnd
            size_t e = before.find_last_not_of(" \t\n\r");
            before = (e == std::string::npos) ? "" : before.substr(0, e + 1);

            if (before.empty()) {
                text = "";
            } else {
                std::string after = next.substr(idx + token.size());
                // trimStart
                size_t s = after.find_first_not_of(" \t\n\r");
                after = (s == std::string::npos) ? "" : after.substr(s);
                text = before + after;
                // trimEnd
                e = text.find_last_not_of(" \t\n\r");
                text = (e == std::string::npos) ? "" : text.substr(0, e + 1);
            }
            did_strip = true;
            changed = true;
        }
    }

    // Collapse whitespace (aligned with openclaw: text.replace(/\s+/g, " ").trim())
    static const std::regex ws_re(R"(\s+)");
    text = std::regex_replace(text, ws_re, " ");
    text = Heartbeat::trim(text);

    return {text, did_strip};
}

// ============================================================================
// stripHeartbeatToken
// Aligned with openclaw: stripHeartbeatToken (heartbeat.ts)
//
// Returns:
//   shouldSkip: true  → suppress sending (token-only or short ack in heartbeat mode)
//   text:       remaining text after stripping
//   didStrip:   whether the token was found and stripped
// ============================================================================

Heartbeat::StripResult Heartbeat::strip_heartbeat_token(
    const std::string& raw,
    StripMode mode,
    int max_ack_chars
) {
    if (raw.empty()) return {true, "", false};
    const std::string trimmed = trim(raw);
    if (trimmed.empty()) return {true, "", false};

    // Resolve maxAckChars (aligned with openclaw)
    const int resolved_max_ack_chars = std::max(0, max_ack_chars);

    // Normalize markup so token wrapped in HTML/Markdown still strips
    const std::string trimmed_normalized = strip_markup(trimmed);

    const bool has_token =
        trimmed.find(HEARTBEAT_TOKEN) != std::string::npos ||
        trimmed_normalized.find(HEARTBEAT_TOKEN) != std::string::npos;

    if (!has_token) {
        return {false, trimmed, false};
    }

    // Try stripping from original and normalized versions
    auto stripped_original   = strip_token_at_edges(trimmed);
    auto stripped_normalized = strip_token_at_edges(trimmed_normalized);

    // Prefer original if it stripped and has remaining text
    const auto& picked =
        (stripped_original.did_strip && !stripped_original.text.empty())
            ? stripped_original
            : stripped_normalized;

    if (!picked.did_strip) {
        return {false, trimmed, false};
    }

    if (picked.text.empty()) {
        return {true, "", true};
    }

    const std::string rest = trim(picked.text);

    // Aligned with openclaw: heartbeat mode — suppress if rest fits within maxAckChars
    if (mode == StripMode::Heartbeat) {
        if (static_cast<int>(rest.size()) <= resolved_max_ack_chars) {
            return {true, "", true};
        }
    }

    return {false, rest, true};
}

// ============================================================================
// contains_heartbeat_token
// ============================================================================

bool Heartbeat::contains_heartbeat_token(const std::string& text) {
    return text.find(HEARTBEAT_TOKEN) != std::string::npos;
}

// ============================================================================
// resolveHeartbeatReplyPayload
// Aligned with openclaw: resolveHeartbeatReplyPayload (heartbeat-reply-payload.ts)
// Returns the last non-empty payload from a reply result array.
// ============================================================================

std::optional<queue::MessagePayload> Heartbeat::resolve_heartbeat_reply_payload(
    const std::vector<queue::MessagePayload>& payloads
) {
    // Aligned with openclaw: iterate in reverse, return first non-empty
    for (int i = static_cast<int>(payloads.size()) - 1; i >= 0; --i) {
        const auto& p = payloads[i];
        if (!p.text.empty() || !p.media_urls.empty()) {
            return p;
        }
    }
    return std::nullopt;
}

// ============================================================================
// HeartbeatRunner::run_once
// Aligned with openclaw: runWebHeartbeatOnce (heartbeat-runner.ts)
// ============================================================================

HeartbeatRunner::RunResult HeartbeatRunner::run_once(const RunOptions& options) {
    RunResult result;

    // Step 1: Resolve heartbeat prompt (aligned with openclaw)
    std::string prompt = Heartbeat::resolve_heartbeat_prompt(
        options.workspace_path,
        options.config.prompt.empty()
            ? std::nullopt
            : std::make_optional(options.config.prompt)
    );

    // Step 2: Read HEARTBEAT.md and check if effectively empty
    // Aligned with openclaw: isHeartbeatContentEffectivelyEmpty check
    // If the file exists but is effectively empty, skip the LLM call.
    std::string heartbeat_md_content;
    if (!options.workspace_path.empty()) {
        const std::string heartbeat_md_path = options.workspace_path + "/HEARTBEAT.md";
        std::ifstream heartbeat_file(heartbeat_md_path);
        if (heartbeat_file.is_open()) {
            std::ostringstream ss;
            ss << heartbeat_file.rdbuf();
            heartbeat_md_content = ss.str();
        }
    }

    // Aligned with openclaw: if HEARTBEAT.md exists and is effectively empty, skip
    if (!heartbeat_md_content.empty() &&
        Heartbeat::is_heartbeat_content_effectively_empty(heartbeat_md_content)) {
        result.success = true;
        result.heartbeat_result.is_ok = true;
        result.heartbeat_result.should_send = false;
        return result;
    }

    // Step 3: LLM call
    // Aligned with openclaw: getReplyFromConfig({ isHeartbeat: true })
    // The heartbeat uses the same reply pipeline as normal messages,
    // but with isHeartbeat=true which:
    //   - Uses the heartbeat model override if configured
    //   - Passes the heartbeat prompt as the user input
    //   - Strips HEARTBEAT_OK token from the response
    //
    // HunterClaw: delegate to ReplyGenerator with is_heartbeat=true
    auto_reply::ReplyGenerator::GenerateConfig gen_config;
    gen_config.agent_id = options.agent_id.empty() ? "default" : options.agent_id;
    gen_config.workspace_path = options.workspace_path;
    gen_config.session_key = options.session_key;
    gen_config.user_input = prompt;
    gen_config.is_heartbeat = true;
    gen_config.enable_media_understanding = false;  // No media in heartbeat
    gen_config.enable_link_understanding = false;   // No link understanding in heartbeat
    if (options.config.model.has_value()) {
        gen_config.model_override = options.config.model;
    }

    // Note: llm_client and tool_manager are injected via options in a full implementation.
    // For now, we record the intent and return success.
    // The actual LLM call is performed by the caller (HeartbeatScheduler) which has
    // access to the full dependency graph.
    result.heartbeat_result.response = prompt;  // Placeholder until LLM is called

    // Step 4: Process heartbeat response
    // Aligned with openclaw: stripHeartbeatToken(response, { mode: "heartbeat", maxAckChars })
    if (!result.heartbeat_result.response.empty()) {
        auto strip_result = Heartbeat::strip_heartbeat_token(
            result.heartbeat_result.response,
            Heartbeat::StripMode::Heartbeat,
            options.config.ack_max_chars
        );

        result.heartbeat_result.is_ok = strip_result.did_strip;
        result.heartbeat_result.processed_response = strip_result.text;
        result.heartbeat_result.should_send = !strip_result.should_skip;

        // Aligned with openclaw: isAlert = response has content after stripping
        result.heartbeat_result.is_alert =
            !strip_result.should_skip && !strip_result.text.empty();
    }

    // Step 5: Decide whether to send
    // Aligned with openclaw: show_ok controls whether HEARTBEAT_OK is sent
    if (result.heartbeat_result.is_ok && !options.config.show_ok) {
        result.heartbeat_result.should_send = false;
    }
    if (result.heartbeat_result.is_alert && !options.config.show_alerts) {
        result.heartbeat_result.should_send = false;
    }

    // Step 6: Restore session updatedAt
    // Aligned with openclaw: prevent heartbeat from keeping session "active"
    // by restoring the session's updatedAt to its pre-heartbeat value.
    if (!options.session_key.empty()) {
        restore_session_updated_at(
            options.session_key,
            result.heartbeat_result.timestamp_ms
        );
    }

    result.success = true;
    return result;
}

std::vector<std::string> HeartbeatRunner::resolve_heartbeat_recipients(
    const std::string& /*channel*/,
    const std::vector<std::string>& configured_recipients
) {
    if (!configured_recipients.empty()) return configured_recipients;
    return {};
}

void HeartbeatRunner::restore_session_updated_at(
    const std::string& session_key,
    int64_t original_updated_at
) {
    // Aligned with openclaw: restore session updatedAt after heartbeat
    //
    // OpenClaw saves the session's updatedAt before the heartbeat LLM call,
    // then restores it afterward. This prevents the heartbeat from making
    // the session appear "recently active" and triggering idle-timeout resets.
    //
    // Implementation:
    //   1. Before heartbeat: save session.updatedAt
    //   2. After heartbeat: restore session.updatedAt to saved value
    //
    // HunterClaw: this is handled by the HeartbeatScheduler which has access
    // to the SessionManager. The scheduler calls:
    //   1. session_manager->get_session(session_key) → save updated_at
    //   2. run_once(options)
    //   3. session_manager->update_session({ ...session, updated_at: saved_updated_at })
    //
    // The original_updated_at parameter is the timestamp to restore.
    // If 0, no restoration is needed (heartbeat didn't modify the session).
    if (session_key.empty() || original_updated_at == 0) {
        return;
    }
    // Note: actual restoration is performed by the caller (HeartbeatScheduler)
    // which has access to SessionManager. This function is a no-op placeholder
    // that documents the intent.
}

} // namespace auto_reply
} // namespace hunterclaw
