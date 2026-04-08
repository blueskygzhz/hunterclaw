// 🎯 严格对齐 OpenClaw: src/agents/sandbox/runtime-status.ts (139 lines)
// 运行时状态判定：根据 mode 和 session key 判断当前会话是否沙箱化

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace sandbox {

using json = nlohmann::json;

// Forward declarations
json resolve_sandbox_config_for_agent(const json& cfg, const std::string& agent_id);

// ============================================================================
// SandboxRuntimeStatus - 对齐 OpenClaw resolveSandboxRuntimeStatus return type
// ============================================================================

struct SandboxRuntimeStatus {
    std::string agent_id;
    std::string session_key;
    std::string main_session_key;
    std::string mode;  // "off" | "non-main" | "all"
    bool sandboxed = false;
};

// 🎯 对齐 OpenClaw: shouldSandboxSession
static bool should_sandbox_session(
    const std::string& mode,
    const std::string& session_key,
    const std::string& main_session_key
) {
    if (mode == "off") return false;
    if (mode == "all") return true;
    // mode == "non-main"
    std::string sk = session_key;
    std::string mk = main_session_key;
    // Trim
    while (!sk.empty() && sk.front() == ' ') sk.erase(sk.begin());
    while (!sk.empty() && sk.back() == ' ') sk.pop_back();
    while (!mk.empty() && mk.front() == ' ') mk.erase(mk.begin());
    while (!mk.empty() && mk.back() == ' ') mk.pop_back();
    return sk != mk;
}

// 🎯 对齐 OpenClaw: resolveSandboxRuntimeStatus
SandboxRuntimeStatus resolve_sandbox_runtime_status(
    const json& cfg,
    const std::string& session_key
) {
    SandboxRuntimeStatus status;
    status.session_key = session_key;

    // Extract agent ID from session key
    auto colon = session_key.find(':');
    status.agent_id = (colon != std::string::npos) ? session_key.substr(0, colon) : "main";

    // Resolve main session key
    status.main_session_key = "agent:" + status.agent_id + ":main";

    // Get sandbox config for this agent
    auto sandbox_cfg = resolve_sandbox_config_for_agent(cfg, status.agent_id);
    status.mode = sandbox_cfg.value("mode", "off");

    status.sandboxed = !session_key.empty() && should_sandbox_session(
        status.mode, session_key, status.main_session_key
    );

    return status;
}

// ============================================================================
// formatSandboxToolPolicyBlockedMessage
// 对齐 OpenClaw: formatSandboxToolPolicyBlockedMessage
// ============================================================================

std::optional<std::string> format_sandbox_tool_policy_blocked_message(
    const json& cfg,
    const std::string& session_key,
    const std::string& tool_name
) {
    std::string tool = tool_name;
    // Trim + lowercase
    while (!tool.empty() && tool.front() == ' ') tool.erase(tool.begin());
    while (!tool.empty() && tool.back() == ' ') tool.pop_back();
    for (auto& c : tool) c = std::tolower(c);
    if (tool.empty()) return std::nullopt;

    auto runtime = resolve_sandbox_runtime_status(cfg, session_key);
    if (!runtime.sandboxed) return std::nullopt;

    // Build message
    std::string msg;
    msg += "Tool \"" + tool + "\" blocked by sandbox tool policy (mode=" + runtime.mode + ").\n";
    msg += "Session: " + (runtime.session_key.empty() ? "(unknown)" : runtime.session_key) + "\n";
    msg += "Fix:\n";
    msg += "- agents.defaults.sandbox.mode=off (disable sandbox)\n";
    if (runtime.mode == "non-main") {
        msg += "- Use main session key (direct): " + runtime.main_session_key + "\n";
    }
    msg += "- See: hunterclaw sandbox explain --session " + runtime.session_key + "\n";

    return msg;
}

}  // namespace sandbox
}  // namespace hunterclaw
