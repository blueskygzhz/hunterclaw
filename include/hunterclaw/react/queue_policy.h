#pragma once
// ============================================================================
// queue_policy.h — 严格对齐 OpenClaw queue-policy.ts (22行)
//                  + agent-runner-auth-profile.ts (25行)
//                  + agent-runner-utils.ts 部分工具函数
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 queue-policy.ts:3: ActiveRunQueueAction
// ============================================================================
enum class ActiveRunQueueAction {
    RunNow,            // "run-now"
    EnqueueFollowup,   // "enqueue-followup"
    Drop,              // "drop"
};

// ============================================================================
// 对齐 queue-policy.ts:5-21: resolveActiveRunQueueAction
// ============================================================================
enum class QueueMode {
    Default,   // Normal queue behavior
    Steer,     // Steer mode — active runs get enqueued
};

ActiveRunQueueAction resolve_active_run_queue_action(
    bool is_active,
    bool is_heartbeat,
    bool should_followup,
    QueueMode queue_mode = QueueMode::Default
);

// ============================================================================
// 对齐 agent-runner-auth-profile.ts:3-15: resolveProviderScopedAuthProfile
// ============================================================================
struct ScopedAuthProfile {
    std::optional<std::string> auth_profile_id;
    std::optional<std::string> auth_profile_id_source;
};

ScopedAuthProfile resolve_provider_scoped_auth_profile(
    const std::string& provider,
    const std::string& primary_provider,
    const std::optional<std::string>& auth_profile_id = std::nullopt,
    const std::optional<std::string>& auth_profile_id_source = std::nullopt
);

// ============================================================================
// 对齐 agent-runner-utils.ts:106-117: isBunFetchSocketError + format
// ============================================================================
bool is_bun_fetch_socket_error(const std::string& message);
std::string format_bun_fetch_socket_error(const std::string& message);

// ============================================================================
// 对齐 agent-runner-utils.ts:119-132: resolveEnforceFinalTag
// ============================================================================
bool resolve_enforce_final_tag(
    const std::string& provider,
    const std::string& model = "",
    bool skip_provider_runtime_hints = false,
    bool enforce_final_tag = false
);

} // namespace react
} // namespace hunterclaw
