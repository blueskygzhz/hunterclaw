#include "hunterclaw/agents/agent_scope.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <unistd.h>
#include <pwd.h>

namespace hunterclaw {
namespace agents {

namespace fs = std::filesystem;

// Helper function to trim whitespace
static void trim(std::string& s) {
    // Trim left
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    // Trim right
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// Helper to iterate JsonValue array through nlohmann::json
static std::vector<nlohmann::json> json_array_items(const core::JsonValue& jv) {
    std::vector<nlohmann::json> items;
    if (jv.is_array()) {
        auto j = jv.to_json();
        for (const auto& item : j) {
            items.push_back(item);
        }
    }
    return items;
}

// Helper to convert JsonValue to int
static int json_as_int(const core::JsonValue& jv) {
    return static_cast<int>(jv.as_number());
}

// ============================================================================
// Constants
// ============================================================================

constexpr const char* DEFAULT_AGENT_ID = "main";

// ============================================================================
// AgentScopeManager Implementation
// ============================================================================

AgentScopeManager::AgentScopeManager(const core::Config& config)
    : config_(config) {
}

// ========================================================================
// Basic Agent Resolution
// ========================================================================

std::vector<AgentEntry> AgentScopeManager::list_agent_entries() const {
    std::vector<AgentEntry> entries;
    
    // Load from config.agents
    if (config_.agents_json.is_object() && config_.agents_json.contains("list") &&
        config_.agents_json["list"].is_array()) {
        
        for (const auto& entry_json_item : json_array_items(config_.agents_json["list"])) {
            core::JsonValue entry_json(entry_json_item);
            if (!entry_json.is_object()) continue;
            
            AgentEntry entry = load_agent_entry_from_json(entry_json);
            entries.push_back(entry);
        }
    }
    
    return entries;
}

std::vector<std::string> AgentScopeManager::list_agent_ids() const {
    auto entries = list_agent_entries();
    if (entries.empty()) {
        return {DEFAULT_AGENT_ID};
    }
    
    std::unordered_set<std::string> seen;
    std::vector<std::string> ids;
    
    for (const auto& entry : entries) {
        std::string id = normalize_agent_id(entry.id);
        if (seen.count(id)) {
            continue;
        }
        seen.insert(id);
        ids.push_back(id);
    }
    
    return ids.empty() ? std::vector<std::string>{DEFAULT_AGENT_ID} : ids;
}

std::string AgentScopeManager::resolve_default_agent_id() const {
    auto entries = list_agent_entries();
    if (entries.empty()) {
        return DEFAULT_AGENT_ID;
    }
    
    // Find entries marked default
    std::vector<AgentEntry> defaults;
    for (const auto& entry : entries) {
        if (entry.is_default) {
            defaults.push_back(entry);
        }
    }
    
    // Warn if multiple defaults
    if (defaults.size() > 1 && !default_agent_warned_) {
        default_agent_warned_ = true;
        spdlog::get("default")->warn("[AgentScope] Multiple agents marked default=true; using the first entry as default.");
    }
    
    // Return first default or first entry
    const auto& chosen = !defaults.empty() ? defaults[0] : entries[0];
    std::string id = chosen.id;
    trim(id);
    
    return id.empty() ? DEFAULT_AGENT_ID : normalize_agent_id(id);
}

std::optional<AgentEntry> AgentScopeManager::resolve_agent_entry(
    const std::string& agent_id
) const {
    std::string id = normalize_agent_id(agent_id);
    auto entries = list_agent_entries();
    
    for (const auto& entry : entries) {
        if (normalize_agent_id(entry.id) == id) {
            return entry;
        }
    }
    
    return std::nullopt;
}

std::optional<ResolvedAgentConfig> AgentScopeManager::resolve_agent_config(
    const std::string& agent_id
) const {
    std::string id = normalize_agent_id(agent_id);
    auto entry = resolve_agent_entry(id);
    
    if (!entry) {
        return std::nullopt;
    }
    
    ResolvedAgentConfig config;
    config.id = id;
    config.name = entry->name;
    config.workspace = entry->workspace;
    config.agent_dir = entry->agent_dir;
    config.model_primary = entry->model_primary;
    config.model_fallbacks = entry->model_fallbacks;
    config.skills_filter = entry->skills;
    config.memory_search = entry->features.memory_search;
    config.human_delay_ms = entry->features.human_delay_ms;
    config.heartbeat_enabled = entry->features.heartbeat_enabled;
    config.identity_name = entry->identity.identity_name;
    config.group_chat_enabled = entry->identity.group_chat_enabled;
    config.max_spawn_depth = entry->subagents.max_spawn_depth;
    config.allow_spawn = entry->subagents.allow_spawn;
    config.sandbox_enabled = entry->sandbox.enabled;
    config.sandbox_mode = entry->sandbox.mode;
    config.exec_enabled = entry->tools.exec_enabled;
    config.disabled_tools = entry->tools.disabled_tools.value_or(std::vector<std::string>{});
    
    return config;
}

// ========================================================================
// Session Agent Resolution
// ========================================================================

SessionAgentIds AgentScopeManager::resolve_session_agent_ids(
    const std::string& session_key,
    const std::optional<std::string>& explicit_agent_id
) const {
    std::string default_agent_id = resolve_default_agent_id();
    
    // Handle explicit agent ID
    if (explicit_agent_id && !explicit_agent_id->empty()) {
        std::string id = *explicit_agent_id;
        trim(id);
        std::transform(id.begin(), id.end(), id.begin(), ::tolower);
        
        if (!id.empty()) {
            return {default_agent_id, normalize_agent_id(id)};
        }
    }
    
    // Parse session key
    std::string normalized_session_key = session_key;
    trim(normalized_session_key);
    std::transform(normalized_session_key.begin(), normalized_session_key.end(),
                   normalized_session_key.begin(), ::tolower);
    
    auto parsed = parse_agent_session_key(normalized_session_key);
    
    std::string session_agent_id = default_agent_id;
    if (parsed && !parsed->agent_id.empty()) {
        session_agent_id = normalize_agent_id(parsed->agent_id);
    }
    
    return {default_agent_id, session_agent_id};
}

std::string AgentScopeManager::resolve_session_agent_id(
    const std::string& session_key
) const {
    return resolve_session_agent_ids(session_key, std::nullopt).session_agent_id;
}

std::string AgentScopeManager::resolve_agent_id_from_session_key(
    const std::string& session_key
) const {
    auto parsed = parse_agent_session_key(session_key);
    if (parsed && !parsed->agent_id.empty()) {
        return normalize_agent_id(parsed->agent_id);
    }
    return resolve_default_agent_id();
}

// ========================================================================
// Model Configuration
// ========================================================================

std::optional<std::string> AgentScopeManager::resolve_agent_explicit_model_primary(
    const std::string& agent_id
) const {
    auto config = resolve_agent_config(agent_id);
    if (!config) {
        return std::nullopt;
    }
    return config->model_primary;
}

std::optional<std::string> AgentScopeManager::resolve_agent_effective_model_primary(
    const std::string& agent_id
) const {
    // Try agent-specific config first
    auto explicit_model = resolve_agent_explicit_model_primary(agent_id);
    if (explicit_model) {
        return explicit_model;
    }
    
    // Fall back to defaults.model
    if (config_.agents_json.is_object() && config_.agents_json.contains("defaults")) {
        const auto& defaults = config_.agents_json["defaults"];
        if (defaults.is_object() && defaults.contains("model")) {
            const auto& model = defaults["model"];
            if (model.is_string()) {
                return model.as_string();
            } else if (model.is_object() && model.contains("primary")) {
                if (model["primary"].is_string()) {
                    return model["primary"].as_string();
                }
            }
        }
    }
    
    return std::nullopt;
}

std::optional<std::vector<std::string>> AgentScopeManager::resolve_agent_model_fallbacks_override(
    const std::string& agent_id
) const {
    auto config = resolve_agent_config(agent_id);
    if (!config || config->model_fallbacks.empty()) {
        return std::nullopt;
    }
    return config->model_fallbacks;
}

std::optional<std::vector<std::string>> AgentScopeManager::resolve_effective_model_fallbacks(
    const std::string& agent_id,
    bool has_session_model_override
) const {
    auto agent_fallbacks = resolve_agent_model_fallbacks_override(agent_id);
    
    if (!has_session_model_override) {
        return agent_fallbacks;
    }
    
    // If session has model override, check defaults
    if (!agent_fallbacks) {
        // Load default fallbacks from config
        if (config_.agents_json.is_object() && config_.agents_json.contains("defaults")) {
            const auto& defaults = config_.agents_json["defaults"];
            if (defaults.is_object() && defaults.contains("model")) {
                const auto& model = defaults["model"];
                if (model.is_object() && model.contains("fallbacks") && model["fallbacks"].is_array()) {
                    std::vector<std::string> fallbacks;
                    for (const auto& fb_item : json_array_items(model["fallbacks"])) {
                        core::JsonValue fb(fb_item);
                        if (fb.is_string()) {
                            fallbacks.push_back(fb.as_string());
                        }
                    }
                    return fallbacks.empty() ? std::nullopt : std::optional(fallbacks);
                }
            }
        }
    }
    
    return agent_fallbacks;
}

bool AgentScopeManager::has_configured_model_fallbacks(
    const std::string& agent_id,
    const std::string& session_key
) const {
    auto fallbacks = resolve_agent_model_fallbacks_override(agent_id);
    if (fallbacks && !fallbacks->empty()) {
        return true;
    }
    
    // Check defaults
    if (config_.agents_json.is_object() && config_.agents_json.contains("defaults")) {
        const auto& defaults = config_.agents_json["defaults"];
        if (defaults.is_object() && defaults.contains("model")) {
            const auto& model = defaults["model"];
            if (model.is_object() && model.contains("fallbacks") && model["fallbacks"].is_array()) {
                return !json_array_items(model["fallbacks"]).empty();
            }
        }
    }
    
    return false;
}

// ========================================================================
// Skills Configuration
// ========================================================================

std::vector<std::string> AgentScopeManager::resolve_agent_skills_filter(
    const std::string& agent_id
) const {
    auto config = resolve_agent_config(agent_id);
    if (!config || config->skills_filter.empty()) {
        return {"*"};  // Default: all skills allowed
    }
    return config->skills_filter;
}

bool AgentScopeManager::is_skill_enabled_for_agent(
    const std::string& agent_id,
    const std::string& skill_id
) const {
    auto filter = resolve_agent_skills_filter(agent_id);
    return skill_matches_filter(skill_id, filter);
}

bool AgentScopeManager::skill_matches_filter(
    const std::string& skill_id,
    const std::vector<std::string>& filter
) const {
    if (filter.empty()) {
        return true;  // Empty filter = all allowed
    }
    
    for (const auto& pattern : filter) {
        if (pattern == "*") {
            return true;
        }
        if (pattern == skill_id) {
            return true;
        }
        // Simple wildcard matching (skill-* matches skill-foo, skill-bar)
        if (pattern.back() == '*') {
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            if (skill_id.find(prefix) == 0) {
                return true;
            }
        }
    }
    
    return false;
}

// ========================================================================
// Workspace & Directory Resolution
// ========================================================================

std::string AgentScopeManager::resolve_agent_workspace_dir(
    const std::string& agent_id
) const {
    std::string id = normalize_agent_id(agent_id);
    auto config = resolve_agent_config(id);
    
    // Use configured workspace if available
    if (config && !config->workspace.empty()) {
        std::string ws = config->workspace;
        trim(ws);
        return strip_null_bytes(expand_user_path(ws));
    }
    
    // Check if this is the default agent
    std::string default_agent_id = resolve_default_agent_id();
    if (id == default_agent_id) {
        // Try defaults.workspace
        if (config_.agents_json.is_object() && config_.agents_json.contains("defaults")) {
            const auto& defaults = config_.agents_json["defaults"];
            if (defaults.is_object() && defaults.contains("workspace")) {
                if (defaults["workspace"].is_string()) {
                    std::string fallback = defaults["workspace"].as_string();
                    trim(fallback);
                    if (!fallback.empty()) {
                        return strip_null_bytes(expand_user_path(fallback));
                    }
                }
            }
        }
        
        // Use default workspace
        return strip_null_bytes(resolve_default_agent_workspace_dir());
    }
    
    // Non-default agent: use state-based workspace
    const char* state_dir_env = std::getenv("HUNTERCLAW_STATE_DIR");
    std::string state_dir = state_dir_env ? state_dir_env : expand_user_path("~/.hunterclaw");
    
    return strip_null_bytes(state_dir + "/workspace-" + id);
}

std::string AgentScopeManager::resolve_agent_dir(const std::string& agent_id) const {
    std::string id = normalize_agent_id(agent_id);
    auto config = resolve_agent_config(id);
    
    // Use configured agent_dir if available
    if (config && !config->agent_dir.empty()) {
        std::string dir = config->agent_dir;
        trim(dir);
        return expand_user_path(dir);
    }
    
    // Default: state_dir/agents/<agent_id>/agent
    const char* state_dir_env = std::getenv("HUNTERCLAW_STATE_DIR");
    std::string root = state_dir_env ? state_dir_env : expand_user_path("~/.hunterclaw");
    
    return root + "/agents/" + id + "/agent";
}

std::vector<std::string> AgentScopeManager::resolve_agent_ids_by_workspace_path(
    const std::string& workspace_path
) const {
    std::string normalized_workspace = normalize_path_for_comparison(workspace_path);
    auto ids = list_agent_ids();
    
    struct Match {
        std::string id;
        std::string workspace_dir;
        int order;
    };
    std::vector<Match> matches;
    
    for (size_t index = 0; index < ids.size(); ++index) {
        const auto& id = ids[index];
        std::string workspace_dir = normalize_path_for_comparison(resolve_agent_workspace_dir(id));
        
        if (!is_path_within_root(normalized_workspace, workspace_dir)) {
            continue;
        }
        
        matches.push_back({id, workspace_dir, static_cast<int>(index)});
    }
    
    // Sort by workspace length (longer = more specific) and then order
    std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
        int workspace_length_delta = static_cast<int>(right.workspace_dir.length()) - 
                                      static_cast<int>(left.workspace_dir.length());
        if (workspace_length_delta != 0) {
            return workspace_length_delta > 0;
        }
        return left.order < right.order;
    });
    
    std::vector<std::string> result;
    for (const auto& match : matches) {
        result.push_back(match.id);
    }
    
    return result;
}

std::optional<std::string> AgentScopeManager::resolve_agent_id_by_workspace_path(
    const std::string& workspace_path
) const {
    auto ids = resolve_agent_ids_by_workspace_path(workspace_path);
    if (ids.empty()) {
        return std::nullopt;
    }
    return ids[0];
}

// ========================================================================
// Utility Methods
// ========================================================================

std::string AgentScopeManager::normalize_agent_id(const std::string& agent_id) {
    std::string id = agent_id;
    trim(id);
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    return id.empty() ? DEFAULT_AGENT_ID : id;
}

std::optional<AgentScopeManager::ParsedSessionKey> AgentScopeManager::parse_agent_session_key(
    const std::string& session_key
) {
    std::string key = session_key;
    trim(key);
    
    if (key.empty()) {
        return std::nullopt;
    }
    
    // Format: "agentId:sessionName" or just "sessionName"
    size_t colon_pos = key.find(':');
    if (colon_pos == std::string::npos) {
        // No agent ID, just session name
        return ParsedSessionKey{"", key};
    }
    
    std::string agent_id = key.substr(0, colon_pos);
    std::string session_name = key.substr(colon_pos + 1);
    trim(agent_id);
    trim(session_name);
    
    return ParsedSessionKey{agent_id, session_name};
}

std::string AgentScopeManager::strip_null_bytes(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    for (char c : path) {
        if (c != '\0') {
            result += c;
        }
    }
    return result;
}

std::string AgentScopeManager::normalize_path_for_comparison(const std::string& path) {
    try {
        // Inline expand_user_path logic
        std::string clean_path = strip_null_bytes(path);
        if (!clean_path.empty() && clean_path[0] == '~') {
            const char* home = std::getenv("HOME");
            if (!home) {
                struct passwd* pw = getpwuid(getuid());
                home = pw ? pw->pw_dir : nullptr;
            }
            if (home) {
                clean_path = std::string(home) + clean_path.substr(1);
            }
        }
        
        fs::path p = clean_path;
        fs::path resolved = fs::absolute(p);
        
        // Try to get canonical path (resolves symlinks)
        if (fs::exists(resolved)) {
            resolved = fs::canonical(resolved);
        }
        
        std::string result = resolved.string();
        
#ifdef _WIN32
        // Windows: case-insensitive
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
#endif
        
        return result;
    } catch (const std::exception& e) {
        spdlog::get("default")->warn("[AgentScope] Failed to normalize path '{}': {}", path, e.what());
        return path;
    }
}

bool AgentScopeManager::is_path_within_root(
    const std::string& candidate_path,
    const std::string& root_path
) {
    try {
        fs::path candidate = fs::path(candidate_path);
        fs::path root = fs::path(root_path);
        
        fs::path relative = fs::relative(candidate, root);
        std::string relative_str = relative.string();
        
        // Check if relative path goes outside root (../)
        return relative_str.empty() || 
               (relative_str.find("..") != 0 && !fs::path(relative_str).is_absolute());
    } catch (const std::exception& e) {
        auto logger = spdlog::get("default");
        if (logger) {
            logger->warn("[AgentScope] Failed to check path containment: {}", e.what());
        }
        return false;
    }
}

// ========================================================================
// Config Management
// ========================================================================

void AgentScopeManager::update_config(const core::Config& config) {
    config_ = config;
    default_agent_warned_ = false;  // Reset warning flag
}

// ========================================================================
// Private Helpers
// ========================================================================

AgentEntry AgentScopeManager::load_agent_entry_from_json(
    const core::JsonValue& json
) const {
    AgentEntry entry;
    
    if (json.contains("id") && json["id"].is_string()) {
        entry.id = json["id"].as_string();
    }
    
    if (json.contains("name") && json["name"].is_string()) {
        entry.name = json["name"].as_string();
    }
    
    if (json.contains("default") && json["default"].is_bool()) {
        entry.is_default = json["default"].as_bool();
    }
    
    if (json.contains("workspace") && json["workspace"].is_string()) {
        entry.workspace = json["workspace"].as_string();
    }
    
    if (json.contains("agentDir") && json["agentDir"].is_string()) {
        entry.agent_dir = json["agentDir"].as_string();
    }
    
    // Model configuration
    if (json.contains("model")) {
        const auto& model = json["model"];
        if (model.is_string()) {
            entry.model_primary = model.as_string();
        } else if (model.is_object()) {
            if (model.contains("primary") && model["primary"].is_string()) {
                entry.model_primary = model["primary"].as_string();
            }
            if (model.contains("fallbacks") && model["fallbacks"].is_array()) {
                for (const auto& fb_item : json_array_items(model["fallbacks"])) {
                    core::JsonValue fb(fb_item);
                    if (fb.is_string()) {
                        entry.model_fallbacks.push_back(fb.as_string());
                    }
                }
            }
        }
    }
    
    // Skills
    if (json.contains("skills") && json["skills"].is_array()) {
        for (const auto& skill_item : json_array_items(json["skills"])) {
            core::JsonValue skill(skill_item);
            if (skill.is_string()) {
                entry.skills.push_back(skill.as_string());
            }
        }
    }
    
    // Features
    if (json.contains("memorySearch") && json["memorySearch"].is_bool()) {
        entry.features.memory_search = json["memorySearch"].as_bool();
    }
    if (json.contains("humanDelay") && json["humanDelay"].is_number()) {
        entry.features.human_delay_ms = json_as_int(json["humanDelay"]);
    }
    if (json.contains("heartbeat")) {
        const auto& hb = json["heartbeat"];
        if (hb.is_object() && hb.contains("enabled") && hb["enabled"].is_bool()) {
            entry.features.heartbeat_enabled = hb["enabled"].as_bool();
        }
    }
    
    return entry;
}

std::string AgentScopeManager::resolve_default_agent_workspace_dir() const {
    // Check HUNTERCLAW_WORKSPACE env var
    const char* ws_env = std::getenv("HUNTERCLAW_WORKSPACE");
    if (ws_env) {
        return expand_user_path(ws_env);
    }
    
    // Default: ~/hunterclaw-workspace
    return expand_user_path("~/hunterclaw-workspace");
}

std::string AgentScopeManager::expand_user_path(const std::string& path) const {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : nullptr;
    }
    
    if (!home) {
        return path;
    }
    
    return std::string(home) + path.substr(1);
}

// ============================================================================
// Global Helper Functions
// ============================================================================

std::string resolve_fallback_agent_id(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
) {
    if (agent_id && !agent_id->empty()) {
        std::string id = *agent_id;
        trim(id);
        return id.empty() ? DEFAULT_AGENT_ID : AgentScopeManager::normalize_agent_id(id);
    }
    
    if (session_key) {
        auto parsed = AgentScopeManager::parse_agent_session_key(*session_key);
        if (parsed && !parsed->agent_id.empty()) {
            return AgentScopeManager::normalize_agent_id(parsed->agent_id);
        }
    }
    
    return DEFAULT_AGENT_ID;
}

std::optional<std::vector<std::string>> resolve_run_model_fallbacks_override(
    const core::Config& config,
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key
) {
    std::string resolved_agent_id = resolve_fallback_agent_id(agent_id, session_key);
    
    AgentScopeManager manager(config);
    return manager.resolve_agent_model_fallbacks_override(resolved_agent_id);
}

}  // namespace agents
}  // namespace hunterclaw
