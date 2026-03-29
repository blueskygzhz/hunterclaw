#pragma once

// ============================================================================
// heartbeat_visibility.h
// Aligned with openclaw: src/infra/heartbeat-visibility.ts
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace infra {

// ── ResolvedHeartbeatVisibility ──────────────────────────────────────────────
// Aligned with openclaw: ResolvedHeartbeatVisibility

struct ResolvedHeartbeatVisibility {
    bool show_ok       = false;  // Silent by default
    bool show_alerts   = true;   // Show content messages
    bool use_indicator = true;   // Emit indicator events
};

// ── HeartbeatVisibilityConfig ────────────────────────────────────────────────
// Aligned with openclaw: ChannelHeartbeatVisibilityConfig

struct HeartbeatVisibilityConfig {
    std::optional<bool> show_ok;
    std::optional<bool> show_alerts;
    std::optional<bool> use_indicator;
};

// ── resolve_heartbeat_visibility ─────────────────────────────────────────────
// Aligned with openclaw: resolveHeartbeatVisibility
//
// Precedence: per_account > per_channel > channel_defaults > global defaults
// channel: "webchat" | "telegram" | "signal" | "none" | ...

ResolvedHeartbeatVisibility resolve_heartbeat_visibility(
    const std::string& channel,
    const std::optional<std::string>& account_id,
    const std::optional<HeartbeatVisibilityConfig>& channel_defaults,
    const std::optional<HeartbeatVisibilityConfig>& per_channel,
    const std::optional<HeartbeatVisibilityConfig>& per_account
);

} // namespace infra
} // namespace hunterclaw
