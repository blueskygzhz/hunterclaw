#pragma once

// 严格对齐 OpenClaw: channels/thread-bindings-policy.ts (270行)
// + thread-bindings-messages.ts (114行) + thread-binding-id.ts (17行)
// Thread binding lifecycle policy engine

#include "hunterclaw/core/types.h"
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw {
namespace channels {

// ============================================================================
// ThreadBindingSpawnKind (对齐 OC: ThreadBindingSpawnKind)
// ============================================================================

enum class ThreadBindingSpawnKind { SUBAGENT, ACP };

// ============================================================================
// ThreadBindingSpawnPolicy (对齐 OC: ThreadBindingSpawnPolicy)
// ============================================================================

struct ThreadBindingSpawnPolicy {
    std::string channel;
    std::string account_id;
    bool enabled = true;
    bool spawn_enabled = true;
};

// ============================================================================
// ThreadBindingLifecycleRecord (对齐 OC)
// ============================================================================

struct ThreadBindingLifecycleRecord {
    int64_t bound_at = 0;
    int64_t last_activity_at = 0;
    std::optional<int64_t> idle_timeout_ms;
    std::optional<int64_t> max_age_ms;
};

// ============================================================================
// ThreadBindingLifecycleResult (对齐 OC)
// ============================================================================

struct ThreadBindingLifecycleResult {
    std::optional<int64_t> expires_at;
    std::optional<std::string> reason;  // "idle-expired" | "max-age-expired"
};

// ============================================================================
// Policy functions (对齐 OC)
// ============================================================================

/// 默认空闲超时: 24h (对齐 OC: DEFAULT_THREAD_BINDING_IDLE_HOURS = 24)
constexpr int64_t DEFAULT_THREAD_BINDING_IDLE_MS = 24 * 60 * 60 * 1000;
constexpr int64_t DEFAULT_THREAD_BINDING_MAX_AGE_MS = 0;

/// Resolve idle timeout ms from channel/session config (对齐 OC: resolveThreadBindingIdleTimeoutMs)
int64_t resolve_thread_binding_idle_timeout_ms(
    const std::optional<double>& channel_idle_hours,
    const std::optional<double>& session_idle_hours);

/// Resolve max age ms from channel/session config (对齐 OC: resolveThreadBindingMaxAgeMs)
int64_t resolve_thread_binding_max_age_ms(
    const std::optional<double>& channel_max_age_hours,
    const std::optional<double>& session_max_age_hours);

/// Resolve thread binding lifecycle (对齐 OC: resolveThreadBindingLifecycle)
ThreadBindingLifecycleResult resolve_thread_binding_lifecycle(
    const ThreadBindingLifecycleRecord& record,
    int64_t default_idle_timeout_ms,
    int64_t default_max_age_ms);

/// Check if thread bindings are enabled (对齐 OC: resolveThreadBindingsEnabled)
bool resolve_thread_bindings_enabled(
    const std::optional<bool>& channel_enabled,
    const std::optional<bool>& session_enabled);

/// Resolve spawn policy (对齐 OC: resolveThreadBindingSpawnPolicy)
ThreadBindingSpawnPolicy resolve_thread_binding_spawn_policy(
    const core::Config& cfg,
    const std::string& channel,
    const std::string& account_id,
    ThreadBindingSpawnKind kind);

/// Resolve placement: "current" or "child" (对齐 OC: resolveThreadBindingPlacementForCurrentContext)
std::string resolve_thread_binding_placement_for_current_context(
    const std::string& channel,
    const std::optional<std::string>& thread_id);

// ============================================================================
// Thread binding messages (对齐 OC: thread-bindings-messages.ts)
// ============================================================================

/// Format duration label (对齐 OC: formatThreadBindingDurationLabel)
std::string format_thread_binding_duration_label(int64_t duration_ms);

/// Resolve thread name (对齐 OC: resolveThreadBindingThreadName)
std::string resolve_thread_binding_thread_name(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& label);

/// Resolve intro text (对齐 OC: resolveThreadBindingIntroText)
std::string resolve_thread_binding_intro_text(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& label,
    std::optional<int64_t> idle_timeout_ms = std::nullopt,
    std::optional<int64_t> max_age_ms = std::nullopt,
    const std::optional<std::string>& session_cwd = std::nullopt);

/// Resolve farewell text (对齐 OC: resolveThreadBindingFarewellText)
std::string resolve_thread_binding_farewell_text(
    const std::optional<std::string>& reason,
    const std::optional<std::string>& farewell_text,
    int64_t idle_timeout_ms,
    int64_t max_age_ms);

// ============================================================================
// Thread binding ID (对齐 OC: thread-binding-id.ts)
// ============================================================================

/// Resolve conversation id from binding id (对齐 OC: resolveThreadBindingConversationIdFromBindingId)
std::optional<std::string> resolve_thread_binding_conversation_id_from_binding_id(
    const std::string& account_id,
    const std::optional<std::string>& binding_id);

// ============================================================================
// Error formatting (对齐 OC)
// ============================================================================

std::string format_thread_binding_disabled_error(
    const std::string& channel,
    const std::string& account_id,
    ThreadBindingSpawnKind kind);

std::string format_thread_binding_spawn_disabled_error(
    const std::string& channel,
    const std::string& account_id,
    ThreadBindingSpawnKind kind);

} // namespace channels
} // namespace hunterclaw
