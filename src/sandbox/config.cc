// 🎯 严格对齐 OpenClaw: src/agents/sandbox/config.ts (217 lines)
// 将全局配置与 agent 级配置合并解析为最终的 SandboxConfig

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace sandbox {

using json = nlohmann::json;

// Forward declarations from constants.cc
extern const std::string DEFAULT_SANDBOX_IMAGE;
extern const std::string DEFAULT_SANDBOX_CONTAINER_PREFIX;
extern const std::string DEFAULT_SANDBOX_WORKDIR;
extern const int DEFAULT_SANDBOX_IDLE_HOURS;
extern const int DEFAULT_SANDBOX_MAX_AGE_DAYS;
extern const std::string DEFAULT_SANDBOX_WORKSPACE_ROOT;
extern const std::string DEFAULT_SANDBOX_BROWSER_IMAGE;
extern const std::string DEFAULT_SANDBOX_BROWSER_PREFIX;
extern const std::string DEFAULT_SANDBOX_BROWSER_NETWORK;
extern const int DEFAULT_SANDBOX_BROWSER_CDP_PORT;
extern const int DEFAULT_SANDBOX_BROWSER_VNC_PORT;
extern const int DEFAULT_SANDBOX_BROWSER_NOVNC_PORT;
extern const int DEFAULT_SANDBOX_BROWSER_AUTOSTART_TIMEOUT_MS;

// ============================================================================
// SandboxScope resolution - 对齐 OpenClaw resolveSandboxScope
// ============================================================================

std::string resolve_sandbox_scope(const std::string& scope, bool per_session) {
    if (!scope.empty()) return scope;
    if (per_session) return "session";
    return "agent";
}

// ============================================================================
// Docker config merge - 对齐 OpenClaw resolveSandboxDockerConfig
// ============================================================================

json resolve_sandbox_docker_config(
    const std::string& scope,
    const json& global_docker,
    const json& agent_docker
) {
    const json& agent = (scope == "shared") ? json::object() : agent_docker;
    const json& global = global_docker;

    json result;
    // 🎯 对齐 OpenClaw: agent > global > default 优先级
    result["image"] = agent.value("image", global.value("image", DEFAULT_SANDBOX_IMAGE));
    result["containerPrefix"] = agent.value("containerPrefix",
        global.value("containerPrefix", DEFAULT_SANDBOX_CONTAINER_PREFIX));
    result["workdir"] = agent.value("workdir", global.value("workdir", DEFAULT_SANDBOX_WORKDIR));
    result["readOnlyRoot"] = agent.value("readOnlyRoot", global.value("readOnlyRoot", true));
    result["network"] = agent.value("network", global.value("network", "none"));
    result["capDrop"] = agent.contains("capDrop") ? agent["capDrop"] :
        (global.contains("capDrop") ? global["capDrop"] : json::array({"ALL"}));

    // 🎯 对齐 OpenClaw: tmpfs merge
    if (agent.contains("tmpfs")) result["tmpfs"] = agent["tmpfs"];
    else if (global.contains("tmpfs")) result["tmpfs"] = global["tmpfs"];
    else result["tmpfs"] = json::array({"/tmp", "/var/tmp", "/run"});

    // 🎯 对齐 OpenClaw: env merge (agent overrides global)
    json env = global.value("env", json::object({{"LANG", "C.UTF-8"}}));
    if (agent.contains("env")) {
        for (auto& [k, v] : agent["env"].items()) env[k] = v;
    }
    result["env"] = env;

    // Optional fields: agent > global
    auto opt_str = [&](const std::string& key) {
        if (agent.contains(key)) result[key] = agent[key];
        else if (global.contains(key)) result[key] = global[key];
    };
    opt_str("user"); opt_str("setupCommand"); opt_str("seccompProfile");
    opt_str("apparmorProfile"); opt_str("memory"); opt_str("memorySwap");

    auto opt_int = [&](const std::string& key) {
        if (agent.contains(key)) result[key] = agent[key];
        else if (global.contains(key)) result[key] = global[key];
    };
    opt_int("pidsLimit"); opt_int("cpus");

    // 🎯 对齐 OpenClaw: binds concat
    json binds = json::array();
    if (global.contains("binds") && global["binds"].is_array())
        for (const auto& b : global["binds"]) binds.push_back(b);
    if (agent.contains("binds") && agent["binds"].is_array())
        for (const auto& b : agent["binds"]) binds.push_back(b);
    if (!binds.empty()) result["binds"] = binds;

    // 🎯 对齐 OpenClaw: dangerous boolean keys
    for (const auto& key : {"dangerouslyAllowReservedContainerTargets",
                            "dangerouslyAllowExternalBindSources",
                            "dangerouslyAllowContainerNamespaceJoin"}) {
        if (agent.contains(key)) result[key] = agent[key];
        else if (global.contains(key)) result[key] = global[key];
    }

    return result;
}

// ============================================================================
// Browser config merge - 对齐 OpenClaw resolveSandboxBrowserConfig
// ============================================================================

json resolve_sandbox_browser_config(
    const std::string& scope,
    const json& global_browser,
    const json& agent_browser
) {
    const json& agent = (scope == "shared") ? json::object() : agent_browser;
    const json& global = global_browser;

    json result;
    result["enabled"] = agent.value("enabled", global.value("enabled", false));
    result["image"] = agent.value("image", global.value("image", DEFAULT_SANDBOX_BROWSER_IMAGE));
    result["containerPrefix"] = agent.value("containerPrefix",
        global.value("containerPrefix", DEFAULT_SANDBOX_BROWSER_PREFIX));
    result["network"] = agent.value("network",
        global.value("network", DEFAULT_SANDBOX_BROWSER_NETWORK));
    result["cdpPort"] = agent.value("cdpPort",
        global.value("cdpPort", DEFAULT_SANDBOX_BROWSER_CDP_PORT));
    result["vncPort"] = agent.value("vncPort",
        global.value("vncPort", DEFAULT_SANDBOX_BROWSER_VNC_PORT));
    result["noVncPort"] = agent.value("noVncPort",
        global.value("noVncPort", DEFAULT_SANDBOX_BROWSER_NOVNC_PORT));
    result["headless"] = agent.value("headless", global.value("headless", false));
    result["enableNoVnc"] = agent.value("enableNoVnc", global.value("enableNoVnc", true));
    result["allowHostControl"] = agent.value("allowHostControl",
        global.value("allowHostControl", false));
    result["autoStart"] = agent.value("autoStart", global.value("autoStart", true));
    result["autoStartTimeoutMs"] = agent.value("autoStartTimeoutMs",
        global.value("autoStartTimeoutMs", DEFAULT_SANDBOX_BROWSER_AUTOSTART_TIMEOUT_MS));

    return result;
}

// ============================================================================
// Prune config merge - 对齐 OpenClaw resolveSandboxPruneConfig
// ============================================================================

json resolve_sandbox_prune_config(
    const std::string& scope,
    const json& global_prune,
    const json& agent_prune
) {
    const json& agent = (scope == "shared") ? json::object() : agent_prune;
    const json& global = global_prune;

    json result;
    result["idleHours"] = agent.value("idleHours",
        global.value("idleHours", DEFAULT_SANDBOX_IDLE_HOURS));
    result["maxAgeDays"] = agent.value("maxAgeDays",
        global.value("maxAgeDays", DEFAULT_SANDBOX_MAX_AGE_DAYS));
    return result;
}

// ============================================================================
// Master config resolver - 对齐 OpenClaw resolveSandboxConfigForAgent
// ============================================================================

json resolve_sandbox_config_for_agent(const json& cfg, const std::string& agent_id) {
    json defaults_sandbox = json::object();
    if (cfg.contains("agents") && cfg["agents"].contains("defaults") &&
        cfg["agents"]["defaults"].contains("sandbox")) {
        defaults_sandbox = cfg["agents"]["defaults"]["sandbox"];
    }

    json agent_sandbox = json::object();
    if (!agent_id.empty() && cfg.contains("agents") && cfg["agents"].contains("list")) {
        for (const auto& agent : cfg["agents"]["list"]) {
            if (agent.value("id", "") == agent_id && agent.contains("sandbox")) {
                agent_sandbox = agent["sandbox"];
                break;
            }
        }
    }

    std::string scope = resolve_sandbox_scope(
        agent_sandbox.value("scope", defaults_sandbox.value("scope", "")),
        agent_sandbox.value("perSession", defaults_sandbox.value("perSession", false))
    );

    json result;
    result["mode"] = agent_sandbox.value("mode", defaults_sandbox.value("mode", "off"));
    result["scope"] = scope;
    result["workspaceAccess"] = agent_sandbox.value("workspaceAccess",
        defaults_sandbox.value("workspaceAccess", "none"));
    result["workspaceRoot"] = agent_sandbox.value("workspaceRoot",
        defaults_sandbox.value("workspaceRoot", DEFAULT_SANDBOX_WORKSPACE_ROOT));

    result["docker"] = resolve_sandbox_docker_config(scope,
        defaults_sandbox.value("docker", json::object()),
        agent_sandbox.value("docker", json::object()));
    result["browser"] = resolve_sandbox_browser_config(scope,
        defaults_sandbox.value("browser", json::object()),
        agent_sandbox.value("browser", json::object()));
    result["prune"] = resolve_sandbox_prune_config(scope,
        defaults_sandbox.value("prune", json::object()),
        agent_sandbox.value("prune", json::object()));

    return result;
}

}  // namespace sandbox
}  // namespace hunterclaw
