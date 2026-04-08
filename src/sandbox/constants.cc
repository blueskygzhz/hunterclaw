// 🎯 严格对齐 OpenClaw: src/agents/sandbox/constants.ts (56 lines)
// 全模块默认值与常量集中定义

#include <string>
#include <vector>

namespace hunterclaw {
namespace sandbox {

// ============================================================================
// Container defaults - 对齐 OpenClaw constants.ts
// ============================================================================

const std::string DEFAULT_SANDBOX_IMAGE = "hunterclaw-sandbox:bookworm-slim";
const std::string DEFAULT_SANDBOX_CONTAINER_PREFIX = "hunterclaw-sbx-";
const std::string DEFAULT_SANDBOX_WORKDIR = "/workspace";
const int DEFAULT_SANDBOX_IDLE_HOURS = 24;
const int DEFAULT_SANDBOX_MAX_AGE_DAYS = 7;

// 🎯 对齐 OpenClaw: DEFAULT_TOOL_ALLOW
const std::vector<std::string> DEFAULT_TOOL_ALLOW = {
    "exec", "process", "read", "write", "edit", "apply_patch",
    "image", "sessions_list", "sessions_history", "sessions_send",
    "sessions_spawn", "sessions_yield", "subagents", "session_status",
};

// 🎯 对齐 OpenClaw: DEFAULT_TOOL_DENY
const std::vector<std::string> DEFAULT_TOOL_DENY = {
    "browser", "canvas", "nodes", "cron", "gateway",
    // Channel IDs (aligned with OpenClaw CHANNEL_IDS)
    "telegram", "whatsapp", "discord", "slack", "signal",
    "imessage", "irc", "googlechat", "msteams", "mattermost",
    "matrix", "nostr", "line", "zalouser", "feishu",
};

// ============================================================================
// Browser sandbox defaults
// ============================================================================

const std::string DEFAULT_SANDBOX_BROWSER_IMAGE = "hunterclaw-sandbox-browser:bookworm-slim";
const std::string DEFAULT_SANDBOX_COMMON_IMAGE = "hunterclaw-sandbox-common:bookworm-slim";
const std::string SANDBOX_BROWSER_SECURITY_HASH_EPOCH = "2026-02-28-no-sandbox-env";

const std::string DEFAULT_SANDBOX_BROWSER_PREFIX = "hunterclaw-sbx-browser-";
const std::string DEFAULT_SANDBOX_BROWSER_NETWORK = "hunterclaw-sandbox-browser";
const int DEFAULT_SANDBOX_BROWSER_CDP_PORT = 9222;
const int DEFAULT_SANDBOX_BROWSER_VNC_PORT = 5900;
const int DEFAULT_SANDBOX_BROWSER_NOVNC_PORT = 6080;
const int DEFAULT_SANDBOX_BROWSER_AUTOSTART_TIMEOUT_MS = 12000;

// 🎯 对齐 OpenClaw: SANDBOX_AGENT_WORKSPACE_MOUNT
const std::string SANDBOX_AGENT_WORKSPACE_MOUNT = "/agent";

// 🎯 对齐 OpenClaw: workspace root default
const std::string DEFAULT_SANDBOX_WORKSPACE_ROOT = "~/.hunterclaw/state/sandboxes";

}  // namespace sandbox
}  // namespace hunterclaw
