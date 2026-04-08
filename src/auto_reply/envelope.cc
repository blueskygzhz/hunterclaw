// Aligned with openclaw: src/auto-reply/envelope.ts
// Message envelope formatting: [Channel From +elapsed Timestamp] Body

#include "hunterclaw/auto_reply/envelope.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// sanitize_envelope_header_part
// Aligned with openclaw: sanitizeEnvelopeHeaderPart
// Neutralize brackets, collapse whitespace, strip newlines.
// ============================================================================

static std::string sanitize_envelope_header_part(const std::string& value) {
    std::string result = value;

    // Replace newlines with space
    static const std::regex newline_re(R"(\r\n|\r|\n)");
    result = std::regex_replace(result, newline_re, " ");

    // Neutralize brackets
    for (auto& c : result) {
        if (c == '[') c = '(';
        if (c == ']') c = ')';
    }

    // Collapse whitespace
    static const std::regex ws_re(R"(\s+)");
    result = std::regex_replace(result, ws_re, " ");

    // Trim
    size_t s = result.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    size_t e = result.find_last_not_of(" \t");
    return result.substr(s, e - s + 1);
}

// ============================================================================
// format_time_ago
// Aligned with openclaw: formatTimeAgo (infra/format-time/format-relative.ts)
// Produces human-readable relative time: "2m", "1h", "3d", etc.
// ============================================================================

static std::string format_time_ago(int64_t elapsed_ms, bool include_suffix = true) {
    if (elapsed_ms < 0) return "";

    const int64_t seconds = elapsed_ms / 1000;
    const int64_t minutes = seconds / 60;
    const int64_t hours = minutes / 60;
    const int64_t days = hours / 24;

    std::string value;
    if (days > 0) {
        value = std::to_string(days) + "d";
    } else if (hours > 0) {
        value = std::to_string(hours) + "h";
    } else if (minutes > 0) {
        value = std::to_string(minutes) + "m";
    } else {
        value = std::to_string(seconds) + "s";
    }

    return include_suffix ? value + " ago" : value;
}

// ============================================================================
// format_timestamp
// Aligned with openclaw: formatTimestamp internal helper
// Formats a timestamp based on timezone mode.
// ============================================================================

static std::string format_timestamp_local(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return "";

    auto tp = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(timestamp_ms));
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);

    struct tm tm_val;
    localtime_r(&time_t_val, &tm_val);

    // Weekday prefix (aligned with openclaw: Intl weekday: "short")
    static const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string weekday = weekdays[tm_val.tm_wday];

    // Format: YYYY-MM-DDTHH:mm:ss.SSS±HH:MM
    auto ms = timestamp_ms % 1000;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_val);

    char tz_buf[8];
    std::strftime(tz_buf, sizeof(tz_buf), "%z", &tm_val);
    std::string tz_str(tz_buf);
    // Convert +0800 → +08:00
    if (tz_str.size() >= 5) {
        tz_str = tz_str.substr(0, 3) + ":" + tz_str.substr(3);
    }

    char ms_buf[8];
    snprintf(ms_buf, sizeof(ms_buf), ".%03d", static_cast<int>(ms));

    return weekday + " " + std::string(buf) + ms_buf + tz_str;
}

// ============================================================================
// resolve_envelope_format_options
// Aligned with openclaw: resolveEnvelopeFormatOptions
// ============================================================================

EnvelopeFormatOptions resolve_envelope_format_options(
    const std::string& timezone,
    bool include_timestamp,
    bool include_elapsed
) {
    return {timezone, include_timestamp, include_elapsed};
}

// ============================================================================
// format_agent_envelope
// Aligned with openclaw: formatAgentEnvelope (envelope.ts)
//
// Produces: [Channel From +elapsed Timestamp] Body
// ============================================================================

std::string format_agent_envelope(const AgentEnvelopeParams& params) {
    std::string channel = sanitize_envelope_header_part(
        params.channel.empty() ? "Channel" : params.channel);

    std::vector<std::string> parts;
    parts.push_back(channel);

    // Elapsed time calculation
    std::string elapsed;
    if (params.options.include_elapsed &&
        params.timestamp_ms > 0 && params.previous_timestamp_ms > 0) {
        int64_t elapsed_ms = params.timestamp_ms - params.previous_timestamp_ms;
        if (elapsed_ms >= 0) {
            elapsed = format_time_ago(elapsed_ms, false);
        }
    }

    // From label
    if (!params.from.empty()) {
        std::string from = sanitize_envelope_header_part(params.from);
        if (!elapsed.empty()) {
            parts.push_back(from + " +" + elapsed);
        } else {
            parts.push_back(from);
        }
    } else if (!elapsed.empty()) {
        parts.push_back("+" + elapsed);
    }

    // Host / IP
    if (!params.host.empty()) {
        parts.push_back(sanitize_envelope_header_part(params.host));
    }
    if (!params.ip.empty()) {
        parts.push_back(sanitize_envelope_header_part(params.ip));
    }

    // Timestamp
    if (params.options.include_timestamp && params.timestamp_ms > 0) {
        std::string ts = format_timestamp_local(params.timestamp_ms);
        if (!ts.empty()) {
            parts.push_back(ts);
        }
    }

    // Build header
    std::string header = "[";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) header += " ";
        header += parts[i];
    }
    header += "]";

    return header + " " + params.body;
}

// ============================================================================
// format_inbound_envelope
// Aligned with openclaw: formatInboundEnvelope (envelope.ts)
//
// Adds sender label prefix for group chats.
// ============================================================================

std::string format_inbound_envelope(const InboundEnvelopeParams& params) {
    bool is_direct = params.chat_type.empty() || params.chat_type == "direct";

    // Resolve sender label for group messages
    std::string body = params.body;
    if (!is_direct && !params.sender_label.empty()) {
        body = sanitize_envelope_header_part(params.sender_label) + ": " + params.body;
    } else if (is_direct && params.from_me) {
        body = "(self): " + params.body;
    }

    AgentEnvelopeParams agent_params;
    agent_params.channel = params.channel;
    agent_params.from = params.from;
    agent_params.body = body;
    agent_params.timestamp_ms = params.timestamp_ms;
    agent_params.previous_timestamp_ms = params.previous_timestamp_ms;
    agent_params.options = params.options;

    return format_agent_envelope(agent_params);
}

} // namespace auto_reply
} // namespace hunterclaw
