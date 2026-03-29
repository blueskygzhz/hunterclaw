// ============================================================================
// heartbeat_visibility.cc
// Aligned with openclaw: src/infra/heartbeat-visibility.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_visibility.h"

namespace hunterclaw {
namespace infra {

static const ResolvedHeartbeatVisibility DEFAULT_VISIBILITY{
    /*show_ok=*/false,
    /*show_alerts=*/true,
    /*use_indicator=*/true,
};

// ── resolve_heartbeat_visibility ─────────────────────────────────────────────
// Aligned with openclaw: resolveHeartbeatVisibility
//
// For webchat: uses channel_defaults only (no per-channel or per-account config).
// For other channels: per_account > per_channel > channel_defaults > global defaults.

ResolvedHeartbeatVisibility resolve_heartbeat_visibility(
    const std::string& channel,
    const std::optional<std::string>& /*account_id*/,
    const std::optional<HeartbeatVisibilityConfig>& channel_defaults,
    const std::optional<HeartbeatVisibilityConfig>& per_channel,
    const std::optional<HeartbeatVisibilityConfig>& per_account
) {
    // Aligned with openclaw: webchat uses channel defaults only
    if (channel == "webchat") {
        return {
            channel_defaults.has_value() && channel_defaults->show_ok.has_value()
                ? *channel_defaults->show_ok : DEFAULT_VISIBILITY.show_ok,
            channel_defaults.has_value() && channel_defaults->show_alerts.has_value()
                ? *channel_defaults->show_alerts : DEFAULT_VISIBILITY.show_alerts,
            channel_defaults.has_value() && channel_defaults->use_indicator.has_value()
                ? *channel_defaults->use_indicator : DEFAULT_VISIBILITY.use_indicator,
        };
    }

    // Aligned with openclaw: per_account > per_channel > channel_defaults > global defaults
    auto resolve = [&](
        std::optional<bool> HeartbeatVisibilityConfig::* field,
        bool default_val
    ) -> bool {
        if (per_account.has_value() && (per_account.value().*field).has_value())
            return *(per_account.value().*field);
        if (per_channel.has_value() && (per_channel.value().*field).has_value())
            return *(per_channel.value().*field);
        if (channel_defaults.has_value() && (channel_defaults.value().*field).has_value())
            return *(channel_defaults.value().*field);
        return default_val;
    };

    return {
        resolve(&HeartbeatVisibilityConfig::show_ok,       DEFAULT_VISIBILITY.show_ok),
        resolve(&HeartbeatVisibilityConfig::show_alerts,    DEFAULT_VISIBILITY.show_alerts),
        resolve(&HeartbeatVisibilityConfig::use_indicator,  DEFAULT_VISIBILITY.use_indicator),
    };
}

} // namespace infra
} // namespace hunterclaw
