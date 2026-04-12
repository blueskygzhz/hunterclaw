// subagent_capabilities.cc
// Aligned with openclaw:
//   src/agents/subagent-capabilities.ts
//
// Provides:
//   - SubagentSessionRole: main / orchestrator / leaf
//   - SubagentControlScope: children / none
//   - resolve_subagent_role_for_depth
//   - resolve_subagent_control_scope_for_role
//   - resolve_subagent_capabilities
//   - resolve_stored_subagent_capabilities

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/agents/agent_paths.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/json.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_capabilities");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_capabilities");
    return logger;
}

// 🎯 对齐 OpenClaw: DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH (config/agent-limits.ts)
static constexpr int DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH = 5;

// ============================================================================
// Helper: normalize string to lowercase trimmed
// ============================================================================

std::string normalize_optional_lowercase(const std::string& s) {
    if (s.empty()) return "";
    std::string result = s;
    auto start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = result.find_last_not_of(" \t\n\r");
    result = result.substr(start, end - start + 1);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

// 🎯 对齐 OpenClaw: normalizeSubagentSessionKey
std::string normalize_subagent_session_key_cap(const std::string& key) {
    return normalize_optional_lowercase(key);
}

// 🎯 对齐 OpenClaw: isSubagentSessionKey
bool is_subagent_session_key(const std::string& key) {
    // Session key containing "sub:" indicates a subagent
    return key.find("sub:") != std::string::npos;
}

// ============================================================================
// 🎯 对齐 OpenClaw: normalizeSubagentRole(value)
// ============================================================================

std::string normalize_subagent_role(const std::string& value) {
    std::string trimmed = normalize_optional_lowercase(value);
    if (trimmed == "main" || trimmed == "orchestrator" || trimmed == "leaf") {
        return trimmed;
    }
    return "";
}

// ============================================================================
// 🎯 对齐 OpenClaw: normalizeSubagentControlScope(value)
// ============================================================================

std::string normalize_subagent_control_scope(const std::string& value) {
    std::string trimmed = normalize_optional_lowercase(value);
    if (trimmed == "children" || trimmed == "none") {
        return trimmed;
    }
    return "";
}

// ============================================================================
// SessionCapabilityEntry — aligned with OpenClaw type
// ============================================================================

struct SessionCapabilityEntry {
    std::string session_id;
    int spawn_depth = -1;
    std::string subagent_role;
    std::string subagent_control_scope;
};

// ============================================================================
// 🎯 对齐 OpenClaw: readSessionStore(storePath)
// ============================================================================

using CapabilityStore = std::map<std::string, SessionCapabilityEntry>;

CapabilityStore read_capability_store(const std::string& store_path) {
    CapabilityStore store;
    try {
        if (!std::filesystem::exists(store_path)) return store;
        std::ifstream ifs(store_path);
        if (!ifs.good()) return store;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());

        auto parsed = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return store;

        for (auto& [key, value] : parsed.items()) {
            if (!value.is_object()) continue;
            SessionCapabilityEntry entry;
            if (value.contains("sessionId") && value["sessionId"].is_string()) {
                entry.session_id = value["sessionId"].get<std::string>();
            }
            if (value.contains("spawnDepth") && value["spawnDepth"].is_number_integer()) {
                entry.spawn_depth = value["spawnDepth"].get<int>();
            }
            if (value.contains("subagentRole") && value["subagentRole"].is_string()) {
                entry.subagent_role = value["subagentRole"].get<std::string>();
            }
            if (value.contains("subagentControlScope") && value["subagentControlScope"].is_string()) {
                entry.subagent_control_scope = value["subagentControlScope"].get<std::string>();
            }
            store[key] = entry;
        }
    } catch (const std::exception& e) {
        log()->debug("read_capability_store: error reading {}: {}", store_path, e.what());
    }
    return store;
}

// ============================================================================
// 🎯 对齐 OpenClaw: findEntryBySessionId(store, sessionId)
// ============================================================================

const SessionCapabilityEntry* find_capability_by_session_id(
    const CapabilityStore& store,
    const std::string& session_id
) {
    std::string normalized = normalize_subagent_session_key_cap(session_id);
    if (normalized.empty()) return nullptr;

    for (const auto& [key, entry] : store) {
        std::string candidate = normalize_subagent_session_key_cap(entry.session_id);
        if (!candidate.empty() && candidate == normalized) {
            return &entry;
        }
    }
    return nullptr;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSessionCapabilityEntry({ sessionKey, cfg, store })
// ============================================================================

const SessionCapabilityEntry* resolve_session_capability_entry(
    const std::string& session_key,
    const core::Config* cfg,
    const CapabilityStore* explicit_store
) {
    static thread_local CapabilityStore cached_store;
    static thread_local std::string cached_store_path;

    if (explicit_store) {
        auto it = explicit_store->find(session_key);
        if (it != explicit_store->end()) return &it->second;
        return find_capability_by_session_id(*explicit_store, session_key);
    }

    if (!cfg) return nullptr;

    auto parsed = AgentScopeManager::parse_agent_session_key(session_key);
    if (!parsed.has_value() || parsed->agent_id.empty()) return nullptr;

    std::string state_dir = resolve_state_dir();
    std::string store_path = state_dir + "/agents/" + parsed->agent_id + "/sessions.json";

    // Simple cache (invalidated on path change)
    if (store_path != cached_store_path) {
        cached_store = read_capability_store(store_path);
        cached_store_path = store_path;
    }

    auto it = cached_store.find(session_key);
    if (it != cached_store.end()) return &it->second;
    return find_capability_by_session_id(cached_store, session_key);
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentRoleForDepth({ depth, maxSpawnDepth })
// ============================================================================

std::string resolve_subagent_role_for_depth(int depth, int max_spawn_depth) {
    int d = (depth >= 0) ? depth : 0;
    int max_d = (max_spawn_depth >= 1) ? max_spawn_depth : DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH;

    if (d <= 0) return "main";
    return (d < max_d) ? "orchestrator" : "leaf";
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentControlScopeForRole(role)
// ============================================================================

std::string resolve_subagent_control_scope_for_role(const std::string& role) {
    return (role == "leaf") ? "none" : "children";
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveSubagentCapabilities({ depth, maxSpawnDepth })
// ============================================================================

SubagentCapabilities resolve_subagent_capabilities(int depth, int max_spawn_depth) {
    int d = (depth >= 0) ? depth : 0;
    std::string role = resolve_subagent_role_for_depth(d, max_spawn_depth);
    std::string control_scope = resolve_subagent_control_scope_for_role(role);

    SubagentCapabilities caps;
    caps.depth = d;
    caps.role = role;
    caps.control_scope = control_scope;
    caps.can_spawn = (role == "main" || role == "orchestrator");
    caps.can_control_children = (control_scope == "children");
    return caps;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveStoredSubagentCapabilities(sessionKey, opts)
// ============================================================================

SubagentCapabilities resolve_stored_subagent_capabilities(
    const std::string& session_key,
    const core::Config* cfg
) {
    std::string normalized = normalize_subagent_session_key_cap(session_key);
    int max_spawn_depth = DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH;

    // Try to get max_spawn_depth from config
    if (cfg) {
        // Would read from cfg->agents.defaults.subagents.maxSpawnDepth
        // For now use default
    }

    int depth = get_subagent_depth_from_session_store(normalized, cfg);

    // If not a subagent session key, return basic capabilities
    if (normalized.empty() || !is_subagent_session_key(normalized)) {
        return resolve_subagent_capabilities(depth, max_spawn_depth);
    }

    // Try to read stored role/scope from session store
    const SessionCapabilityEntry* entry = resolve_session_capability_entry(
        normalized, cfg, nullptr);

    SubagentCapabilities fallback = resolve_subagent_capabilities(depth, max_spawn_depth);

    if (!entry) return fallback;

    std::string stored_role = normalize_subagent_role(entry->subagent_role);
    std::string stored_scope = normalize_subagent_control_scope(entry->subagent_control_scope);

    std::string role = stored_role.empty() ? fallback.role : stored_role;
    std::string control_scope = stored_scope.empty()
        ? resolve_subagent_control_scope_for_role(role) : stored_scope;

    SubagentCapabilities caps;
    caps.depth = depth;
    caps.role = role;
    caps.control_scope = control_scope;
    caps.can_spawn = (role == "main" || role == "orchestrator");
    caps.can_control_children = (control_scope == "children");
    return caps;
}

} // namespace agents
} // namespace hunterclaw
