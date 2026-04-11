#pragma once

// 严格对齐 OpenClaw: channels/plugins/session-conversation.ts (275行)
// + channels/chat-meta-shared.ts (88行) + channels/chat-meta.ts (14行)
// + channels/sender-label.ts (56行) + channels/sender-identity.ts (42行)
// + channels/location.ts (77行) + channels/logging.ts (33行)
// + channels/channel-config.ts (183行) + channels/config-presence.ts (123行)
// + channels/model-overrides.ts (253行)

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Session Conversation (对齐 OC: session-conversation.ts)
// ============================================================================

struct ResolvedSessionConversation {
    std::string id;
    std::optional<std::string> thread_id;
    std::string base_conversation_id;
    std::vector<std::string> parent_conversation_candidates;
};

struct ResolvedSessionConversationRef {
    std::string channel;
    std::string kind;  // "group" | "channel"
    std::string raw_id;
    std::string id;
    std::optional<std::string> thread_id;
    std::string base_session_key;
    std::string base_conversation_id;
    std::vector<std::string> parent_conversation_candidates;
};

std::optional<ResolvedSessionConversation> resolve_session_conversation(
    const std::string& channel, const std::string& kind, const std::string& raw_id);

std::optional<ResolvedSessionConversationRef> resolve_session_conversation_ref(
    const std::optional<std::string>& session_key);

std::optional<std::string> resolve_session_parent_session_key(
    const std::optional<std::string>& session_key);

// ============================================================================
// Sender Label (对齐 OC: sender-label.ts)
// ============================================================================

struct SenderLabelParams {
    std::optional<std::string> name;
    std::optional<std::string> username;
    std::optional<std::string> tag;
    std::optional<std::string> e164;
    std::optional<std::string> id;
};

std::optional<std::string> resolve_sender_label(const SenderLabelParams& params);
std::vector<std::string> list_sender_label_candidates(const SenderLabelParams& params);

// ============================================================================
// Sender Identity (对齐 OC: sender-identity.ts)
// ============================================================================

std::vector<std::string> validate_sender_identity(
    const std::optional<std::string>& chat_type,
    const std::optional<std::string>& sender_id,
    const std::optional<std::string>& sender_name,
    const std::optional<std::string>& sender_username,
    const std::optional<std::string>& sender_e164);

// ============================================================================
// Location (对齐 OC: location.ts)
// ============================================================================

enum class LocationSource { PIN, PLACE, LIVE };

struct NormalizedLocation {
    double latitude = 0.0;
    double longitude = 0.0;
    std::optional<double> accuracy;
    std::optional<std::string> name;
    std::optional<std::string> address;
    bool is_live = false;
    LocationSource source = LocationSource::PIN;
    std::optional<std::string> caption;
};

std::string format_location_text(const NormalizedLocation& location);

// ============================================================================
// Channel Logging (对齐 OC: logging.ts)
// ============================================================================

using LogFn = std::function<void(const std::string&)>;

void log_inbound_drop(const LogFn& log, const std::string& channel,
                       const std::string& reason, const std::string& target = "");
void log_typing_failure(const LogFn& log, const std::string& channel,
                         const std::string& error, const std::string& target = "",
                         const std::string& action = "");
void log_ack_failure(const LogFn& log, const std::string& channel,
                      const std::string& error, const std::string& target = "");

// ============================================================================
// Channel Config (对齐 OC: channel-config.ts)
// ============================================================================

std::string normalize_channel_slug(const std::string& value);
std::vector<std::string> build_channel_key_candidates(
    const std::vector<std::optional<std::string>>& keys);

// ============================================================================
// Config Presence (对齐 OC: config-presence.ts)
// ============================================================================

bool has_meaningful_channel_config(const nlohmann::json& value);
bool has_potential_configured_channels(const core::Config& cfg);
std::vector<std::string> list_potential_configured_channel_ids(const core::Config& cfg);

// ============================================================================
// Channel Model Overrides (对齐 OC: model-overrides.ts)
// ============================================================================

struct ChannelModelOverride {
    std::string channel;
    std::string model;
    std::optional<std::string> match_key;
    std::optional<std::string> match_source;  // "direct" | "parent" | "wildcard"
};

std::optional<ChannelModelOverride> resolve_channel_model_override(
    const core::Config& cfg,
    const std::optional<std::string>& channel,
    const std::optional<std::string>& group_id = std::nullopt,
    const std::optional<std::string>& group_chat_type = std::nullopt,
    const std::optional<std::string>& group_channel = std::nullopt,
    const std::optional<std::string>& group_subject = std::nullopt,
    const std::optional<std::string>& parent_session_key = std::nullopt);

} // namespace channels
} // namespace hunterclaw
