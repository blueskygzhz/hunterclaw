// subagent_depth.cc
// Aligned with openclaw:
//   src/agents/subagent-depth.ts    (getSubagentDepthFromSessionStore)
//
// Provides:
//   - normalize_spawn_depth
//   - read_session_store_depth
//   - build_key_candidates
//   - find_entry_by_session_id
//   - resolve_entry_for_session_key
//   - get_subagent_depth_from_session_store

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/agents/agent_paths.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/core/json.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <set>
#include <cctype>
#include <algorithm>

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("subagent_depth");
    if (!logger) logger = spdlog::stdout_logger_mt("subagent_depth");
    return logger;
}

// ============================================================================
// 🎯 对齐 OpenClaw: subagent-depth.ts — SessionDepthEntry
// ============================================================================

struct SessionDepthEntry {
    std::string session_id;
    int spawn_depth = -1;       // -1 means not set
    std::string spawned_by;
};

// ============================================================================
// 🎯 对齐 OpenClaw: normalizeSpawnDepth(value)
// ============================================================================

int normalize_spawn_depth(int value) {
    return (value >= 0) ? value : -1;
}

int normalize_spawn_depth_from_string(const std::string& s) {
    std::string trimmed = s;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return -1;
    auto end = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(start, end - start + 1);
    if (trimmed.empty()) return -1;

    try {
        int val = std::stoi(trimmed);
        return (val >= 0) ? val : -1;
    } catch (...) {
        return -1;
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: normalizeSubagentSessionKey(sessionKey)
// Lowercase trim (simple normalization for comparison)
// ============================================================================

std::string normalize_subagent_session_key(const std::string& key) {
    if (key.empty()) return "";
    std::string result = key;
    // trim
    auto start = result.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = result.find_last_not_of(" \t\n\r");
    result = result.substr(start, end - start + 1);
    // lowercase
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

// ============================================================================
// 🎯 对齐 OpenClaw: getSubagentDepth(raw)
// Parse depth from session key format: "agent:<id>:sub:<depth>:..."
// ============================================================================

int get_subagent_depth_from_key(const std::string& raw) {
    // Count "sub:" segments in session key
    int depth = 0;
    size_t pos = 0;
    while ((pos = raw.find("sub:", pos)) != std::string::npos) {
        depth++;
        pos += 4;
    }
    return depth;
}

// ============================================================================
// 🎯 对齐 OpenClaw: readSessionStore(storePath) → Record<string, SessionDepthEntry>
// ============================================================================

using SessionDepthStore = std::map<std::string, SessionDepthEntry>;

SessionDepthStore read_session_store_depth(const std::string& store_path) {
    SessionDepthStore store;
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
            SessionDepthEntry entry;
            if (value.contains("sessionId") && value["sessionId"].is_string()) {
                entry.session_id = value["sessionId"].get<std::string>();
            }
            if (value.contains("spawnDepth") && value["spawnDepth"].is_number_integer()) {
                entry.spawn_depth = value["spawnDepth"].get<int>();
            } else if (value.contains("spawnDepth") && value["spawnDepth"].is_string()) {
                entry.spawn_depth = normalize_spawn_depth_from_string(
                    value["spawnDepth"].get<std::string>());
            }
            if (value.contains("spawnedBy") && value["spawnedBy"].is_string()) {
                entry.spawned_by = value["spawnedBy"].get<std::string>();
            }
            store[key] = entry;
        }
    } catch (const std::exception& e) {
        // ignore missing/invalid stores — aligned with OpenClaw catch {}
        log()->debug("read_session_store_depth: error reading {}: {}", store_path, e.what());
    }
    return store;
}

// ============================================================================
// 🎯 对齐 OpenClaw: buildKeyCandidates(rawKey, cfg)
// ============================================================================

std::vector<std::string> build_key_candidates(
    const std::string& raw_key,
    const core::Config* cfg
) {
    if (!cfg) {
        return {raw_key};
    }
    if (raw_key == "global" || raw_key == "unknown") {
        return {raw_key};
    }
    // Check if already has agent: prefix
    if (raw_key.find("agent:") == 0) {
        return {raw_key};
    }
    // Try to parse as agent session key
    auto parsed = AgentScopeManager::parse_agent_session_key(raw_key);
    if (parsed.has_value()) {
        return {raw_key};
    }
    // Build prefixed candidate
    AgentScopeManager scope(*cfg);
    std::string default_agent_id = scope.resolve_default_agent_id();
    std::string prefixed = "agent:" + default_agent_id + ":" + raw_key;
    if (prefixed == raw_key) {
        return {raw_key};
    }
    return {raw_key, prefixed};
}

// ============================================================================
// 🎯 对齐 OpenClaw: findEntryBySessionId(store, sessionId)
// ============================================================================

const SessionDepthEntry* find_entry_by_session_id(
    const SessionDepthStore& store,
    const std::string& session_id
) {
    std::string normalized = normalize_subagent_session_key(session_id);
    if (normalized.empty()) return nullptr;

    for (const auto& [key, entry] : store) {
        std::string candidate = normalize_subagent_session_key(entry.session_id);
        if (!candidate.empty() && candidate == normalized) {
            return &entry;
        }
    }
    return nullptr;
}

// ============================================================================
// 🎯 对齐 OpenClaw: resolveEntryForSessionKey({ sessionKey, cfg, store, cache })
// ============================================================================

struct ResolveDepthParams {
    std::string session_key;
    const core::Config* cfg = nullptr;
    const SessionDepthStore* store = nullptr;
    std::map<std::string, SessionDepthStore>& cache;
};

const SessionDepthEntry* resolve_entry_for_session_key(ResolveDepthParams& params) {
    auto candidates = build_key_candidates(params.session_key, params.cfg);

    // If explicit store provided, search there
    if (params.store) {
        for (const auto& key : candidates) {
            auto it = params.store->find(key);
            if (it != params.store->end()) {
                return &it->second;
            }
        }
        return find_entry_by_session_id(*params.store, params.session_key);
    }

    if (!params.cfg) return nullptr;

    // Search across store files, using cache
    for (const auto& key : candidates) {
        auto parsed = AgentScopeManager::parse_agent_session_key(key);
        if (!parsed.has_value() || parsed->agent_id.empty()) continue;

        // Resolve store path for this agent
        std::string state_dir = resolve_state_dir();
        std::string store_path = state_dir + "/agents/" + parsed->agent_id + "/sessions.json";

        auto cache_it = params.cache.find(store_path);
        if (cache_it == params.cache.end()) {
            auto loaded = read_session_store_depth(store_path);
            params.cache[store_path] = std::move(loaded);
            cache_it = params.cache.find(store_path);
        }

        auto it = cache_it->second.find(key);
        if (it != cache_it->second.end()) {
            return &it->second;
        }
        auto by_id = find_entry_by_session_id(cache_it->second, params.session_key);
        if (by_id) return by_id;
    }

    return nullptr;
}

} // anonymous namespace

// ============================================================================
// 🎯 对齐 OpenClaw: getSubagentDepthFromSessionStore(sessionKey, opts)
// ============================================================================
//
// Resolves spawn depth for a session key by:
// 1. Checking session store for explicit spawnDepth
// 2. Walking spawnedBy chain recursively
// 3. Falling back to parsing depth from session key format

int get_subagent_depth_from_session_store(
    const std::string& session_key,
    const core::Config* cfg
) {
    std::string raw = session_key;
    // trim
    auto start = raw.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) raw = "";
    else {
        auto end = raw.find_last_not_of(" \t\n\r");
        raw = raw.substr(start, end - start + 1);
    }

    int fallback_depth = get_subagent_depth_from_key(raw);
    if (raw.empty()) return fallback_depth;

    // Cache for session stores (avoid re-reading files)
    std::map<std::string, SessionDepthStore> cache;
    // Visited set to prevent cycles
    std::set<std::string> visited;

    // Recursive depth resolver — aligned with OpenClaw depthFromStore(key)
    std::function<int(const std::string&)> depth_from_store;
    depth_from_store = [&](const std::string& key) -> int {
        std::string normalized = normalize_subagent_session_key(key);
        if (normalized.empty()) return -1;
        if (visited.count(normalized)) return -1;
        visited.insert(normalized);

        ResolveDepthParams params{normalized, cfg, nullptr, cache};
        const SessionDepthEntry* entry = resolve_entry_for_session_key(params);

        // Check stored depth
        if (entry && entry->spawn_depth >= 0) {
            return entry->spawn_depth;
        }

        // Walk spawnedBy chain
        if (entry && !entry->spawned_by.empty()) {
            std::string parent = normalize_subagent_session_key(entry->spawned_by);
            if (!parent.empty()) {
                int parent_depth = depth_from_store(parent);
                if (parent_depth >= 0) {
                    return parent_depth + 1;
                }
                // Fallback: parse parent key
                return get_subagent_depth_from_key(parent) + 1;
            }
        }

        return -1; // not found
    };

    int result = depth_from_store(raw);
    return (result >= 0) ? result : fallback_depth;
}

} // namespace agents
} // namespace hunterclaw
