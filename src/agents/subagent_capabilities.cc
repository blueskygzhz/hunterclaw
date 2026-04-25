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
//   - resolve_subagent_capability_store
//   - is_subagent_envelope_session
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
#include <set>

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
// 🎯 对齐 OpenClaw: normalizeOptionalLowercaseString / normalizeOptionalString
// ============================================================================

std::string normalize_optional_string(const std::string& s) {
    if (s.empty()) return "";
    std::string result = s;
    auto start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = result.find_last_not_of(" \t\n\r");
    return result.substr(start, end - start + 1);
}

std::string normalize_optional_lowercase(const std::string& s) {
    std::string result = normalize_optional_string(s);
    if (result.empty()) return "";
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

// 🎯 对齐 OpenClaw: normalizeSubagentSessionKey
// normalizeSubagentSessionKey = normalizeOptionalString (subagent-session-key.ts)
std::string normalize_subagent_session_key_cap(const std::string& key) {
    return normalize_optional_string(key);
}

// ============================================================================
// 🎯 对齐 OpenClaw: isSubagentSessionKey (sessions/session-key-utils.ts)
// ============================================================================

bool is_subagent_session_key(const std::string& key) {
    std::string raw = normalize_optional_string(key);
    if (raw.empty()) return false;
    std::string lower = normalize_optional_lowercase(raw);
    if (lower.find("subagent:") == 0) return true;
    // Check rest after "agent:<id>:"
    auto parsed = AgentScopeManager::parse_agent_session_key(raw);
    if (!parsed.has_value()) return false;
    std::string rest = normalize_optional_lowercase(parsed->rest);
    return rest.find("subagent:") == 0;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isAcpSessionKey (sessions/session-key-utils.ts)
// ============================================================================

bool is_acp_session_key(const std::string& key) {
    std::string raw = normalize_optional_string(key);
    if (raw.empty()) return false;
    std::string lower = normalize_optional_lowercase(raw);
    if (lower.find("acp:") == 0) return true;
    auto parsed = AgentScopeManager::parse_agent_session_key(raw);
    if (!parsed.has_value()) return false;
    std::string rest = normalize_optional_lowercase(parsed->rest);
    return rest.find("acp:") == 0;
}

// ============================================================================
// 🎯 对齐 OpenClaw: isSameAgentSessionStore(leftSessionKey, rightSessionKey)
// ============================================================================

bool is_same_agent_session_store(const std::string& left_key, const std::string& right_key) {
    auto left_parsed = AgentScopeManager::parse_agent_session_key(left_key);
    auto right_parsed = AgentScopeManager::parse_agent_session_key(right_key);
    std::string left_agent_id = left_parsed.has_value()
        ? normalize_optional_lowercase(left_parsed->agent_id) : "";
    std::string right_agent_id = right_parsed.has_value()
        ? normalize_optional_lowercase(right_parsed->agent_id) : "";
    return !left_agent_id.empty() && left_agent_id == right_agent_id;
}

// ============================================================================
// 🎯 对齐 OpenClaw: shouldInspectStoredSubagentEnvelope(sessionKey)
// OC: isSubagentSessionKey(sessionKey) || isAcpSessionKey(sessionKey)
// ============================================================================

bool should_inspect_stored_subagent_envelope(const std::string& session_key) {
    return is_subagent_session_key(session_key) || is_acp_session_key(session_key);
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
// 🎯 对齐 OC: added spawnedBy field
// ============================================================================

struct SessionCapabilityEntry {
    std::string session_id;
    int spawn_depth = -1;
    std::string subagent_role;
    std::string subagent_control_scope;
    std::string spawned_by;  // 🎯 对齐 OC: spawnedBy field
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
            // 🎯 对齐 OC: read spawnedBy field
            if (value.contains("spawnedBy") && value["spawnedBy"].is_string()) {
                entry.spawned_by = value["spawnedBy"].get<std::string>();
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

    // Simple per-call store (no thread-local cache to avoid stale data)
    static thread_local CapabilityStore cached_store;
    static thread_local std::string cached_store_path;

    if (store_path != cached_store_path) {
        cached_store = read_capability_store(store_path);
        cached_store_path = store_path;
    }

    auto it = cached_store.find(session_key);
    if (it != cached_store.end()) return &it->second;
    return find_capability_by_session_id(cached_store, session_key);
}

// ============================================================================
// 🎯 对齐 OpenClaw: isStoredSubagentEnvelopeSession(params, visited)
// Recursive: walks spawnedBy chain to determine if session is a subagent envelope
// ============================================================================

bool is_stored_subagent_envelope_session(
    const std::string& session_key,
    const core::Config* cfg,
    const CapabilityStore* store,
    const SessionCapabilityEntry* entry_hint,
    std::set<std::string>& visited
) {
    std::string normalized = normalize_subagent_session_key_cap(session_key);
    if (normalized.empty() || visited.count(normalized)) return false;
    visited.insert(normalized);

    // Direct subagent session key → always true
    if (is_subagent_session_key(normalized)) return true;

    // Not ACP → cannot be subagent envelope
    if (!is_acp_session_key(normalized)) return false;

    // Resolve entry
    const SessionCapabilityEntry* entry = entry_hint
        ? entry_hint
        : resolve_session_capability_entry(normalized, cfg, store);

    // If entry has subagentRole or subagentControlScope → it's a subagent envelope
    if (entry) {
        if (!normalize_subagent_role(entry->subagent_role).empty()) return true;
        if (!normalize_subagent_control_scope(entry->subagent_control_scope).empty()) return true;
    }

    // Walk spawnedBy chain
    if (!entry || entry->spawned_by.empty()) return false;
    std::string spawned_by = normalize_subagent_session_key_cap(entry->spawned_by);
    if (spawned_by.empty()) return false;

    // 🎯 对齐 OC: isSameAgentSessionStore → reuse same store if same agent
    const CapabilityStore* parent_store = is_same_agent_session_store(normalized, spawned_by)
        ? store : nullptr;

    return is_stored_subagent_envelope_session(spawned_by, cfg, parent_store, nullptr, visited);
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
// 🎯 对齐 OpenClaw: resolveSubagentCapabilityStore(sessionKey, opts)
// Returns the capability store for a session key, loading from disk if needed.
// ============================================================================

std::optional<CapabilityStore> resolve_subagent_capability_store(
    const std::string& session_key,
    const core::Config* cfg,
    const CapabilityStore* explicit_store
) {
    std::string normalized = normalize_subagent_session_key_cap(session_key);
    if (normalized.empty()) {
        if (explicit_store) return *explicit_store;
        return std::nullopt;
    }
    if (explicit_store) return *explicit_store;
    if (!cfg || !should_inspect_stored_subagent_envelope(normalized)) return std::nullopt;

    auto parsed = AgentScopeManager::parse_agent_session_key(normalized);
    if (!parsed.has_value() || parsed->agent_id.empty()) return std::nullopt;

    std::string state_dir = resolve_state_dir();
    std::string store_path = state_dir + "/agents/" + parsed->agent_id + "/sessions.json";
    return read_capability_store(store_path);
}

// ============================================================================
// 🎯 对齐 OpenClaw: isSubagentEnvelopeSession(sessionKey, opts)
// ============================================================================

bool is_subagent_envelope_session(
    const std::string& session_key,
    const core::Config* cfg,
    const CapabilityStore* explicit_store,
    const SessionCapabilityEntry* entry_hint
) {
    std::string normalized = normalize_subagent_session_key_cap(session_key);
    if (normalized.empty()) return false;

    // Direct subagent session key → always true
    if (is_subagent_session_key(normalized)) return true;

    // Not ACP → cannot be subagent envelope
    if (!is_acp_session_key(normalized)) return false;

    // Resolve store
    auto store_opt = resolve_subagent_capability_store(normalized, cfg, explicit_store);
    const CapabilityStore* store = store_opt.has_value() ? &store_opt.value() : nullptr;

    std::set<std::string> visited;
    return is_stored_subagent_envelope_session(normalized, cfg, store, entry_hint, visited);
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
    // TODO: read from cfg->agents.defaults.subagents.maxSpawnDepth when available

    // Empty session key → depth 0
    if (normalized.empty()) {
        return resolve_subagent_capabilities(0, max_spawn_depth);
    }

    // 🎯 对齐 OC: if NOT shouldInspectStoredSubagentEnvelope → use depth from store directly
    if (!should_inspect_stored_subagent_envelope(normalized)) {
        int depth = get_subagent_depth_from_session_store(normalized, cfg);
        return resolve_subagent_capabilities(depth, max_spawn_depth);
    }

    // Resolve capability store
    auto store_opt = resolve_subagent_capability_store(normalized, cfg, nullptr);
    const CapabilityStore* store = store_opt.has_value() ? &store_opt.value() : nullptr;

    // Resolve entry
    const SessionCapabilityEntry* entry = resolve_session_capability_entry(normalized, cfg, store);

    // 🎯 对齐 OC: depthStore — only use store for depth if entry has explicit spawnDepth
    // If entry doesn't have spawnDepth, pass nullptr so depth falls back to key parsing
    const CapabilityStore* depth_store = nullptr;
    if (entry && entry->spawn_depth >= 0) {
        depth_store = store;
    }

    int depth = get_subagent_depth_from_session_store(normalized, cfg);
    // If we have a depth_store, try to get depth from it
    if (depth_store && entry && entry->spawn_depth >= 0) {
        depth = entry->spawn_depth;
    }

    // 🎯 对齐 OC: isSubagentEnvelopeSession check
    std::set<std::string> visited;
    bool is_envelope = is_stored_subagent_envelope_session(
        normalized, cfg, store, entry, visited);

    if (!is_envelope) {
        return resolve_subagent_capabilities(depth, max_spawn_depth);
    }

    // Use stored role/scope if available, fall back to computed
    std::string stored_role = entry ? normalize_subagent_role(entry->subagent_role) : "";
    std::string stored_scope = entry ? normalize_subagent_control_scope(entry->subagent_control_scope) : "";

    SubagentCapabilities fallback = resolve_subagent_capabilities(depth, max_spawn_depth);
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
