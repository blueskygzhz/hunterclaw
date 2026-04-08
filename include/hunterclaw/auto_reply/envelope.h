// Aligned with openclaw: src/auto-reply/envelope.ts
#pragma once

#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// EnvelopeFormatOptions (aligned with openclaw: EnvelopeFormatOptions)
// ============================================================================

struct EnvelopeFormatOptions {
    std::string timezone = "local";  // "local" | "utc" | "user" | IANA timezone
    bool include_timestamp = true;
    bool include_elapsed = true;
};

EnvelopeFormatOptions resolve_envelope_format_options(
    const std::string& timezone = "local",
    bool include_timestamp = true,
    bool include_elapsed = true
);

// ============================================================================
// AgentEnvelopeParams (aligned with openclaw: AgentEnvelopeParams)
// ============================================================================

struct AgentEnvelopeParams {
    std::string channel;
    std::string from;
    std::string body;
    int64_t timestamp_ms = 0;
    int64_t previous_timestamp_ms = 0;
    std::string host;
    std::string ip;
    EnvelopeFormatOptions options;
};

// Format: [Channel From +elapsed Timestamp] Body
std::string format_agent_envelope(const AgentEnvelopeParams& params);

// ============================================================================
// InboundEnvelopeParams (aligned with openclaw: formatInboundEnvelope params)
// ============================================================================

struct InboundEnvelopeParams {
    std::string channel;
    std::string from;
    std::string body;
    int64_t timestamp_ms = 0;
    int64_t previous_timestamp_ms = 0;
    std::string chat_type;       // "direct" | "group"
    std::string sender_label;    // Display name for group sender
    bool from_me = false;        // Is this a self-message?
    EnvelopeFormatOptions options;
};

std::string format_inbound_envelope(const InboundEnvelopeParams& params);

} // namespace auto_reply
} // namespace hunterclaw
