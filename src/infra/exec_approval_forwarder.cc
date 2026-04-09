// 🎯 对齐 OpenClaw: src/infra/exec-approval-forwarder.ts
// 实现 exec approval 转发功能
// 调用 security/safe_regex 的 compile_safe_regex + test_regex_with_bounded_input

#include <string>
#include <vector>
#include <regex>
#include <optional>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace infra {

using json = nlohmann::json;

// Forward declarations from security module
namespace security {
    std::optional<std::regex> compile_safe_regex(
        const std::string& source,
        std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript
    );
    bool test_regex_with_bounded_input(
        const std::regex& regex,
        const std::string& input,
        int max_window = 2048
    );
}

// ============================================================================
// ExecApprovalForwardTarget - 对齐 OpenClaw ExecApprovalForwardTarget
// ============================================================================

struct ExecApprovalForwardTarget {
    std::string channel;          // "telegram" | channel name
    std::string account_id;
    std::optional<std::string> chat_id;
};

struct ExecApprovalForwardingConfig {
    bool enabled = false;
    std::string mode = "all";     // "all" | "selected"
    std::vector<std::string> agent_filter;
    std::vector<std::string> session_filter;
    std::vector<ExecApprovalForwardTarget> targets;
};

// ============================================================================
// match_session_filter - 对齐 OpenClaw: matchSessionFilter
// 🎯 调用 compile_safe_regex + test_regex_with_bounded_input
// ============================================================================

static bool match_session_filter(
    const std::string& session_key,
    const std::vector<std::string>& patterns
) {
    // 🎯 对齐 OpenClaw: patterns.some((pattern) => { ... })
    for (const auto& pattern : patterns) {
        // 🎯 对齐 OpenClaw: if (sessionKey.includes(pattern)) return true
        if (session_key.find(pattern) != std::string::npos) {
            return true;
        }
        // 🎯 对齐 OpenClaw: const regex = compileSafeRegex(pattern)
        auto regex = security::compile_safe_regex(pattern);
        // 🎯 对齐 OpenClaw: return regex ? testRegexWithBoundedInput(regex, sessionKey) : false
        if (regex.has_value()) {
            if (security::test_regex_with_bounded_input(*regex, session_key)) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// should_forward - 对齐 OpenClaw: shouldForward
// ============================================================================

struct ExecApprovalRequest {
    std::string command;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
};

static bool should_forward(
    const ExecApprovalForwardingConfig& config,
    const ExecApprovalRequest& request
) {
    // 🎯 对齐 OpenClaw: if (config?.enabled === false) return false
    if (!config.enabled) return false;

    // 🎯 对齐 OpenClaw: agentFilter check
    if (!config.agent_filter.empty() && request.agent_id.has_value()) {
        bool found = false;
        for (const auto& filter : config.agent_filter) {
            if (filter == *request.agent_id) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    // 🎯 对齐 OpenClaw: sessionFilter check (uses matchSessionFilter)
    if (!config.session_filter.empty() && request.session_key.has_value()) {
        if (!match_session_filter(*request.session_key, config.session_filter)) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// forward_exec_approval - 对齐 OpenClaw: forwardExecApproval
// ============================================================================

void forward_exec_approval(
    const ExecApprovalForwardingConfig& config,
    const ExecApprovalRequest& request,
    const std::string& display_text
) {
    if (!should_forward(config, request)) {
        return;
    }

    for (const auto& target : config.targets) {
        if (auto log = spdlog::get("default")) log->info("[exec-approval-forwarder] forwarding to channel={} account={}",
                    target.channel, target.account_id);
        // 🎯 对齐 OpenClaw: deliver via channel (telegram/discord/etc)
        // The actual delivery is handled by the channel system
        // This is the decision + routing layer
    }
}

}  // namespace infra
}  // namespace hunterclaw
