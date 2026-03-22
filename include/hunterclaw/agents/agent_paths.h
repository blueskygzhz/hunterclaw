#ifndef HUNTERCLAW_AGENTS_AGENT_PATHS_H
#define HUNTERCLAW_AGENTS_AGENT_PATHS_H

#include <string>
#include <optional>
#include <cstdlib>
#include <filesystem>

namespace hunterclaw {
namespace agents {

// Default agent ID (aligned with OpenClaw: DEFAULT_AGENT_ID)
inline const std::string DEFAULT_AGENT_ID_STR = "main";

/**
 * Resolve the state directory for HunterClaw.
 * Aligned with OpenClaw: resolveStateDir()
 *
 * Checks environment variables:
 * - HUNTERCLAW_STATE_DIR
 * - OPENCLAW_STATE_DIR (compatibility)
 * Falls back to ~/.hunterclaw
 */
inline std::string resolve_state_dir() {
    const char* override_env = std::getenv("HUNTERCLAW_STATE_DIR");
    if (override_env && *override_env) return std::string(override_env);

    const char* openclaw_env = std::getenv("OPENCLAW_STATE_DIR");
    if (openclaw_env && *openclaw_env) return std::string(openclaw_env);

    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.hunterclaw";
    }
    return ".hunterclaw";
}

/**
 * Resolve the HunterClaw agent directory.
 * Aligned with OpenClaw: resolveOpenClawAgentDir()
 *
 * Checks environment variables:
 * - HUNTERCLAW_AGENT_DIR
 * - OPENCLAW_AGENT_DIR (compatibility)
 * - PI_CODING_AGENT_DIR (compatibility)
 * Falls back to <state_dir>/agents/main/agent
 */
inline std::string resolve_hunterclaw_agent_dir() {
    const char* override_env = std::getenv("HUNTERCLAW_AGENT_DIR");
    if (override_env && *override_env) return std::string(override_env);

    const char* openclaw_env = std::getenv("OPENCLAW_AGENT_DIR");
    if (openclaw_env && *openclaw_env) return std::string(openclaw_env);

    const char* pi_env = std::getenv("PI_CODING_AGENT_DIR");
    if (pi_env && *pi_env) return std::string(pi_env);

    return resolve_state_dir() + "/agents/" + DEFAULT_AGENT_ID_STR + "/agent";
}

/**
 * Ensure the HunterClaw agent environment variables are set.
 * Aligned with OpenClaw: ensureOpenClawAgentEnv()
 *
 * Sets HUNTERCLAW_AGENT_DIR and OPENCLAW_AGENT_DIR if not already set.
 * Returns the resolved agent directory.
 */
inline std::string ensure_hunterclaw_agent_env() {
    std::string dir = resolve_hunterclaw_agent_dir();

    if (!std::getenv("HUNTERCLAW_AGENT_DIR")) {
        setenv("HUNTERCLAW_AGENT_DIR", dir.c_str(), 0);
    }
    if (!std::getenv("OPENCLAW_AGENT_DIR")) {
        setenv("OPENCLAW_AGENT_DIR", dir.c_str(), 0);
    }
    if (!std::getenv("PI_CODING_AGENT_DIR")) {
        setenv("PI_CODING_AGENT_DIR", dir.c_str(), 0);
    }

    return dir;
}

} // namespace agents
} // namespace hunterclaw

#endif // HUNTERCLAW_AGENTS_AGENT_PATHS_H
