// Aligned with openclaw: src/channels/ack-reactions.ts
#pragma once

#include <string>
#include <functional>
#include <future>
#include <optional>

namespace hunterclaw {
namespace channels {

// ============================================================================
// AckReactions
// Aligned with OpenClaw: ack-reactions.ts
// ============================================================================

// Aligned with OpenClaw: AckReactionScope
enum class AckReactionScope {
    All,            // "all"
    Direct,         // "direct"
    GroupAll,       // "group-all"
    GroupMentions,  // "group-mentions"
    Off,            // "off"
    None            // "none"
};

// Aligned with OpenClaw: WhatsAppAckReactionMode
enum class WhatsAppAckReactionMode {
    Always,   // "always"
    Mentions, // "mentions"
    Never     // "never"
};

// Aligned with OpenClaw: AckReactionGateParams
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

/**
 * Determine whether to send an ack reaction.
 * Aligned with OpenClaw: shouldAckReaction()
 */
inline bool should_ack_reaction(const AckReactionGateParams& params) {
    const auto scope = params.scope;
    if (scope == AckReactionScope::Off || scope == AckReactionScope::None) {
        return false;
    }
    if (scope == AckReactionScope::All) {
        return true;
    }
    if (scope == AckReactionScope::Direct) {
        return params.is_direct;
    }
    if (scope == AckReactionScope::GroupAll) {
        return params.is_group;
    }
    if (scope == AckReactionScope::GroupMentions) {
        if (!params.is_mentionable_group) return false;
        if (!params.require_mention) return false;
        if (!params.can_detect_mention) return false;
        return params.effective_was_mentioned || params.should_bypass_mention;
    }
    return false;
}

// Aligned with OpenClaw: shouldAckReactionForWhatsApp params
struct WhatsAppAckReactionParams {
    std::string emoji;
    bool is_direct = false;
    bool is_group = false;
    bool direct_enabled = false;
    WhatsAppAckReactionMode group_mode = WhatsAppAckReactionMode::Mentions;
    bool was_mentioned = false;
    bool group_activated = false;
};

/**
 * Determine whether to send a WhatsApp ack reaction.
 * Aligned with OpenClaw: shouldAckReactionForWhatsApp()
 */
inline bool should_ack_reaction_for_whatsapp(const WhatsAppAckReactionParams& params) {
    if (params.emoji.empty()) return false;
    if (params.is_direct) return params.direct_enabled;
    if (!params.is_group) return false;
    if (params.group_mode == WhatsAppAckReactionMode::Never) return false;
    if (params.group_mode == WhatsAppAckReactionMode::Always) return true;
    // Mentions mode
    return should_ack_reaction({
        AckReactionScope::GroupMentions,
        /*is_direct=*/false,
        /*is_group=*/true,
        /*is_mentionable_group=*/true,
        /*require_mention=*/true,
        /*can_detect_mention=*/true,
        /*effective_was_mentioned=*/params.was_mentioned,
        /*should_bypass_mention=*/params.group_activated,
    });
}

// Aligned with OpenClaw: removeAckReactionAfterReply params
struct RemoveAckReactionParams {
    bool remove_after_reply = false;
    bool ack_reaction_sent = false;
    std::string ack_reaction_value;
    std::function<void()> remove;
    std::function<void(const std::string&)> on_error;
};

/**
 * Remove ack reaction after reply is sent.
 * Aligned with OpenClaw: removeAckReactionAfterReply()
 */
inline void remove_ack_reaction_after_reply(const RemoveAckReactionParams& params) {
    if (!params.remove_after_reply) return;
    if (!params.ack_reaction_sent) return;
    if (params.ack_reaction_value.empty()) return;
    if (!params.remove) return;
    try {
        params.remove();
    } catch (const std::exception& e) {
        if (params.on_error) params.on_error(e.what());
    }
}

} // namespace channels
} // namespace hunterclaw
