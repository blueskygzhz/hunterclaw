#include "hunterclaw/react/queue_policy.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace react {

// ============================================================================
// 对齐 queue-policy.ts:5-21
// ============================================================================
ActiveRunQueueAction resolve_active_run_queue_action(
    bool is_active,
    bool is_heartbeat,
    bool should_followup,
    QueueMode queue_mode,
    bool reset_triggered
) {
    if (!is_active) {
        return ActiveRunQueueAction::RunNow;
    }
    if (is_heartbeat) {
        return ActiveRunQueueAction::Drop;
    }
    // 🎯 对齐 queue-policy.ts:21-23: resetTriggered 强制立即新 run（绕过 followup 排队）。
    if (reset_triggered) {
        return ActiveRunQueueAction::RunNow;
    }
    if (should_followup || queue_mode == QueueMode::Steer) {
        return ActiveRunQueueAction::EnqueueFollowup;
    }
    return ActiveRunQueueAction::RunNow;
}

// ============================================================================
// 对齐 agent-runner-auth-profile.ts:3-15
// ============================================================================
ScopedAuthProfile resolve_provider_scoped_auth_profile(
    const std::string& provider,
    const std::string& primary_provider,
    const std::optional<std::string>& auth_profile_id,
    const std::optional<std::string>& auth_profile_id_source
) {
    ScopedAuthProfile result;
    if (provider == primary_provider) {
        result.auth_profile_id = auth_profile_id;
        result.auth_profile_id_source = auth_profile_id.has_value()
            ? auth_profile_id_source : std::nullopt;
    }
    return result;
}

// ============================================================================
// 对齐 agent-runner-utils.ts:106-117
// ============================================================================
bool is_bun_fetch_socket_error(const std::string& message) {
    if (message.empty()) return false;
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("socket connection was closed unexpectedly") != std::string::npos;
}

std::string format_bun_fetch_socket_error(const std::string& message) {
    std::string trimmed = message;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) trimmed = trimmed.substr(start, end - start + 1);
    else trimmed = "Unknown error";

    return "⚠️ LLM connection failed. This could be due to server issues, network problems, "
           "or context length exceeded (e.g., with local LLMs like LM Studio). Original error:\n"
           "```\n" + trimmed + "\n```";
}

// ============================================================================
// 对齐 agent-runner-utils.ts:119-132
// ============================================================================
bool resolve_enforce_final_tag(
    const std::string& provider,
    const std::string& /*model*/,
    bool skip_provider_runtime_hints,
    bool enforce_final_tag
) {
    if (skip_provider_runtime_hints) return false;
    if (enforce_final_tag) return true;

    // Check if provider uses reasoning tags (e.g., DeepSeek, o1)
    static const std::vector<std::string> reasoning_tag_providers = {
        "deepseek", "deepinfra",
    };
    std::string lower_provider = provider;
    std::transform(lower_provider.begin(), lower_provider.end(),
                   lower_provider.begin(), ::tolower);
    for (const auto& p : reasoning_tag_providers) {
        if (lower_provider.find(p) != std::string::npos) return true;
    }
    return false;
}

} // namespace react
} // namespace hunterclaw
