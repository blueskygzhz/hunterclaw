#pragma once
// Aligned with openclaw: src/channels/ack-reactions.ts (104 lines)
//
// Ack reactions: emoji feedback sent immediately when a user message is received.
//   - shouldAckReaction: universal gate (scope/direct/group/mention)
//   - shouldAckReactionForWhatsApp: WhatsApp-specific logic
//   - removeAckReactionAfterReply: remove ack reaction after reply is sent

#include <string>
#include <functional>

namespace hunterclaw {
namespace channels {

// Aligned with openclaw: AckReactionScope
enum class AckReactionScope {
    All,             // React to all messages
    Direct,          // React only in direct messages
    GroupAll,        // React to all group messages
    GroupMentions,   // React only when mentioned in group (default)
    Off,             // Disabled
    None             // Alias for Off
};

inline AckReactionScope parse_ack_reaction_scope(const std::string& s) {
    if (s == "all") return AckReactionScope::All;
    if (s == "direct") return AckReactionScope::Direct;
    if (s == "group-all" || s == "group_all") return AckReactionScope::GroupAll;
    if (s == "group-mentions" || s == "group_mentions") return AckReactionScope::GroupMentions;
    if (s == "off") return AckReactionScope::Off;
    if (s == "none") return AckReactionScope::None;
    return AckReactionScope::GroupMentions;  // default
}

// Aligned with openclaw: WhatsAppAckReactionMode
enum class WhatsAppAckReactionMode {
    Always,
    Mentions,
    Never
};

inline WhatsAppAckReactionMode parse_whatsapp_ack_mode(const std::string& s) {
    if (s == "always") return WhatsAppAckReactionMode::Always;
    if (s == "mentions") return WhatsAppAckReactionMode::Mentions;
    if (s == "never") return WhatsAppAckReactionMode::Never;
    return WhatsAppAckReactionMode::Mentions;  // default
}

// Aligned with openclaw: AckReactionGateParams
struct AckReactionGateParams {
    AckReactionScope scope = AckReactionScope::GroupMentions;
    bool is_direct = false;
    bool is_group = false;
    bool is_mentionable_group = false;
    bool require_mention = false;
    bool can_detect_mention = false;
    bool effective_was_mentioned = false;
    bool should_bypass_mention = false;
};

// Aligned with openclaw: shouldAckReaction (channels/ack-reactions.ts:16-43)
inline bool should_ack_reaction(const AckReactionGateParams& params) {
    AckReactionScope scope = params.scope;
    if (scope == AckReactionScope::Off || scope == AckReactionScope::None) return false;
    if (scope == AckReactionScope::All) return true;
    if (scope == AckReactionScope::Direct) return params.is_direct;
    if (scope == AckReactionScope::GroupAll) return params.is_group;
    if (scope == AckReactionScope::GroupMentions) {
        if (!params.is_mentionable_group) return false;
        if (!params.require_mention) return false;
        if (!params.can_detect_mention) return false;
        return params.effective_was_mentioned || params.should_bypass_mention;
    }
    return false;
}

// Aligned with openclaw: shouldAckReactionForWhatsApp (channels/ack-reactions.ts:45-79)
struct WhatsAppAckReactionParams {
    std::string emoji;
    bool is_direct = false;
    bool is_group = false;
    bool direct_enabled = true;
    WhatsAppAckReactionMode group_mode = WhatsAppAckReactionMode::Mentions;
    bool was_mentioned = false;
    bool group_activated = false;
};

inline bool should_ack_reaction_for_whatsapp(const WhatsAppAckReactionParams& params) {
    if (params.emoji.empty()) return false;
    if (params.is_direct) return params.direct_enabled;
    if (!params.is_group) return false;
    if (params.group_mode == WhatsAppAckReactionMode::Never) return false;
    if (params.group_mode == WhatsAppAckReactionMode::Always) return true;
    // group_mode == Mentions
    AckReactionGateParams gate;
    gate.scope = AckReactionScope::GroupMentions;
    gate.is_direct = false;
    gate.is_group = true;
    gate.is_mentionable_group = true;
    gate.require_mention = true;
    gate.can_detect_mention = true;
    gate.effective_was_mentioned = params.was_mentioned;
    gate.should_bypass_mention = params.group_activated;
    return should_ack_reaction(gate);
}

// Aligned with openclaw: removeAckReactionAfterReply (channels/ack-reactions.ts:81-103)
inline void remove_ack_reaction_after_reply(
    bool remove_after_reply,
    bool ack_did_send,
    const std::string& ack_reaction_value,
    std::function<void()> remove_fn,
    std::function<void(const std::string&)> on_error = nullptr
) {
    if (!remove_after_reply) return;
    if (!ack_did_send) return;
    if (ack_reaction_value.empty()) return;
    try {
        if (remove_fn) remove_fn();
    } catch (const std::exception& e) {
        if (on_error) on_error(e.what());
    }
}

} // namespace channels
} // namespace hunterclaw
