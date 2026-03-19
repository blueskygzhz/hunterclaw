#pragma once

#include "hunterclaw/queue/delivery_queue.h"
#include <string>
#include <chrono>
#include <optional>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace auto_reply {

// ═══════════════════════════════════════════════════════════════
// Heartbeat
// Aligned with openclaw: src/auto-reply/heartbeat.ts
// ═══════════════════════════════════════════════════════════════

class Heartbeat {
public:
    // ── Constants (aligned with openclaw) ──────────────────────

    // Aligned with openclaw: HEARTBEAT_PROMPT
    static constexpr const char* DEFAULT_HEARTBEAT_PROMPT =
        "Read HEARTBEAT.md if it exists (workspace context). Follow it strictly. "
        "Do not infer or repeat old tasks from prior chats. "
        "If nothing needs attention, reply HEARTBEAT_OK.";

    // Aligned with openclaw: HEARTBEAT_TOKEN (tokens.ts)
    static constexpr const char* HEARTBEAT_TOKEN = "HEARTBEAT_OK";

    // Aligned with openclaw: DEFAULT_HEARTBEAT_EVERY = "30m"
    static constexpr std::chrono::seconds DEFAULT_HEARTBEAT_EVERY{1800};

    // Aligned with openclaw: DEFAULT_HEARTBEAT_ACK_MAX_CHARS = 300
    static constexpr int DEFAULT_HEARTBEAT_ACK_MAX_CHARS = 300;

    // ── StripMode (aligned with openclaw: StripHeartbeatMode) ──

    enum class StripMode {
        Heartbeat,  // "heartbeat" — suppress if rest fits within maxAckChars
        Message,    // "message"   — always return rest (default)
    };

    // ── StripResult (aligned with openclaw: return type of stripHeartbeatToken) ──

    struct StripResult {
        bool should_skip = false;  // true → suppress sending
        std::string text;          // remaining text after stripping
        bool did_strip = false;    // whether the token was found and stripped
    };

    // ── Config ─────────────────────────────────────────────────

    struct Config {
        std::string prompt;
        std::optional<std::string> model;
        int ack_max_chars = DEFAULT_HEARTBEAT_ACK_MAX_CHARS;
        bool show_alerts = true;
        bool show_ok = false;
        bool use_indicator = true;
    };

    // ── Result ─────────────────────────────────────────────────

    struct Result {
        bool is_ok = false;
        bool should_send = false;
        std::string response;
        std::string processed_response;
        bool is_alert = false;
        int64_t timestamp_ms = 0;
    };

    // ── API ────────────────────────────────────────────────────

    /// Aligned with openclaw: stripHeartbeatToken (heartbeat.ts)
    /// Strips HEARTBEAT_TOKEN from raw text.
    /// mode="heartbeat": suppress if remaining text fits within maxAckChars.
    /// mode="message":   always return remaining text (default).
    static StripResult strip_heartbeat_token(
        const std::string& raw,
        StripMode mode = StripMode::Message,
        int max_ack_chars = DEFAULT_HEARTBEAT_ACK_MAX_CHARS
    );

    /// Aligned with openclaw: resolveHeartbeatPrompt (heartbeat.ts)
    static std::string resolve_heartbeat_prompt(
        const std::string& workspace_path,
        const std::optional<std::string>& config_prompt
    );

    /// Aligned with openclaw: isHeartbeatContentEffectivelyEmpty (heartbeat.ts)
    static bool is_heartbeat_content_effectively_empty(const std::string& content);

    /// Aligned with openclaw: resolveHeartbeatReplyPayload (heartbeat-reply-payload.ts)
    /// Returns the last non-empty payload from a reply result array.
    static std::optional<queue::MessagePayload> resolve_heartbeat_reply_payload(
        const std::vector<queue::MessagePayload>& payloads
    );

    /// Check if text contains the heartbeat token
    static bool contains_heartbeat_token(const std::string& text);

    // Helpers (used by heartbeat.cc internals)
    static std::string trim(const std::string& text);
    static bool is_empty_or_whitespace(const std::string& text);
};

// ═══════════════════════════════════════════════════════════════
// HeartbeatRunner
// Aligned with openclaw: src/web/auto-reply/heartbeat-runner.ts
// ═══════════════════════════════════════════════════════════════

class HeartbeatRunner {
public:
    struct RunOptions {
        std::string agent_id;
        std::string workspace_path;
        std::string session_key;
        Heartbeat::Config config;
        std::vector<std::string> recipients;
    };

    struct RunResult {
        bool success = false;
        bool sent_message = false;
        bool used_indicator = false;
        Heartbeat::Result heartbeat_result;
        std::string error;
    };

    /// Aligned with openclaw: runWebHeartbeatOnce
    static RunResult run_once(const RunOptions& options);

    /// Aligned with openclaw: resolveHeartbeatRecipients
    static std::vector<std::string> resolve_heartbeat_recipients(
        const std::string& channel,
        const std::vector<std::string>& configured_recipients
    );

private:
    /// Aligned with openclaw: restore session updatedAt after heartbeat
    static void restore_session_updated_at(
        const std::string& session_key,
        int64_t original_updated_at
    );
};

} // namespace auto_reply
} // namespace hunterclaw
