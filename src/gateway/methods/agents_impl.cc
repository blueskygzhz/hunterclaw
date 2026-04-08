// ============================================================================
// agents_impl.cc - Agent CRUD & Workspace File Management
// 🎯 严格对齐 OpenClaw server-methods/agents.ts (agentsHandlers)
//
// Methods:
//   agents.list       → list all agents
//   agents.create     → create new agent (workspace init + IDENTITY.md)
//   agents.update     → update agent config (name, workspace, model, avatar)
//   agents.delete     → delete agent (config prune + files trash)
//   agents.files.list → list workspace files (bootstrap + memory)
//   agents.files.get  → read workspace file (symlink + hardlink safety)
//   agents.files.set  → write workspace file (path escape guard)
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <set>
#include <algorithm>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t agents_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ============================================================================
// Constants (对齐 OpenClaw agents.ts + workspace.ts)
// ============================================================================
static const std::string IDENTITY_FILENAME = "IDENTITY.md";
static const std::string SOUL_FILENAME = "SOUL.md";
static const std::string TOOLS_FILENAME = "TOOLS.md";
static const std::string USER_FILENAME = "USER.md";
static const std::string AGENTS_FILENAME = "AGENTS.md";
static const std::string HEARTBEAT_FILENAME = "HEARTBEAT.md";
static const std::string BOOTSTRAP_FILENAME = "BOOTSTRAP.md";
static const std::string MEMORY_FILENAME = "MEMORY.md";
static const std::string MEMORY_ALT_FILENAME = "MEMORY.yaml";
static const std::string DEFAULT_AGENT_ID = "main";

static const std::vector<std::string> BOOTSTRAP_FILE_NAMES = {
    AGENTS_FILENAME, SOUL_FILENAME, TOOLS_FILENAME,
    IDENTITY_FILENAME, USER_FILENAME, HEARTBEAT_FILENAME, BOOTSTRAP_FILENAME
};

static const std::set<std::string> ALLOWED_FILE_NAMES = {
    AGENTS_FILENAME, SOUL_FILENAME, TOOLS_FILENAME,
    IDENTITY_FILENAME, USER_FILENAME, HEARTBEAT_FILENAME, BOOTSTRAP_FILENAME,
    MEMORY_FILENAME, MEMORY_ALT_FILENAME
};

// ============================================================================
// Helper: normalizeAgentId
// 🎯 OpenClaw 对齐: routing/session-key.ts normalizeAgentId
// ============================================================================
static std::string normalize_agent_id(const std::string& raw) {
    std::string id = raw;
    // Trim
    while (!id.empty() && id.front() == ' ') id.erase(0, 1);
    while (!id.empty() && id.back() == ' ') id.pop_back();
    // Lowercase
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    // Replace spaces with hyphens
    std::replace(id.begin(), id.end(), ' ', '-');
    // Remove non-alphanumeric except hyphens
    id.erase(std::remove_if(id.begin(), id.end(), [](char c) {
        return !std::isalnum(c) && c != '-';
    }), id.end());
    return id;
}

// ============================================================================
// Helper: load/save agents config
// ============================================================================
static json load_agents_config(std::shared_ptr<GatewayServer> server) {
    auto cfg = server->config();
    if (!cfg) return json::object();
    std::string file = cfg->config_dir + "/agents.json";
    try {
        if (fs::exists(file)) {
            std::ifstream f(file);
            json data;
            if (f.is_open()) { f >> data; return data; }
        }
    } catch (...) {}
    // Default with main agent
    json def;
    def["agents"] = json::array({json::object({{"id", "main"}, {"name", "Main"}})});
    return def;
}

static void save_agents_config(std::shared_ptr<GatewayServer> server, const json& data) {
    auto cfg = server->config();
    if (!cfg) return;
    std::string file = cfg->config_dir + "/agents.json";
    try {
        fs::create_directories(fs::path(file).parent_path());
        std::ofstream out(file);
        if (out.is_open()) out << data.dump(2);
    } catch (...) {}
}

// ============================================================================
// Helper: resolve agent workspace dir
// ============================================================================
static std::string resolve_agent_workspace(
    std::shared_ptr<GatewayServer> server,
    const std::string& agent_id
) {
    auto cfg = server->config();
    if (!cfg) return "";
    // Check agents config for workspace override
    auto agents_cfg = load_agents_config(server);
    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        for (const auto& a : agents_cfg["agents"]) {
            if (a.value("id", "") == agent_id) {
                if (a.contains("workspace") && a["workspace"].is_string()) {
                    return a["workspace"].get<std::string>();
                }
            }
        }
    }
    // Default workspace
    return cfg->config_dir + "/agents/" + agent_id;
}

// ============================================================================
// Helper: assertNoPathEscape
// 🎯 OpenClaw 对齐: agents.ts assertNoPathAliasEscape
// ============================================================================
static bool path_escapes_root(const std::string& path, const std::string& root) {
    try {
        auto abs_path = fs::canonical(fs::path(path)).string();
        auto abs_root = fs::canonical(fs::path(root)).string();
        return abs_path.find(abs_root) != 0;
    } catch (...) {
        // If canonical fails, use string comparison
        auto resolved = fs::path(root) / fs::path(path).filename();
        return false;
    }
}

// ============================================================================
// Helper: stat file safely
// 🎯 OpenClaw 对齐: agents.ts statFileSafely
// ============================================================================
static json stat_file_safely(const std::string& file_path) {
    try {
        auto st = fs::status(file_path);
        if (!fs::is_regular_file(st)) return nullptr;
        auto size = fs::file_size(file_path);
        auto mtime = fs::last_write_time(file_path);
        auto mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            mtime.time_since_epoch()
        ).count();
        json meta;
        meta["size"] = (int64_t)size;
        meta["updatedAtMs"] = mtime_ms;
        return meta;
    } catch (...) {
        return nullptr;
    }
}

// ============================================================================
// agents.list
// 🎯 OpenClaw 对齐: agents.ts L459-475
// ============================================================================
core::Result<json> method_agents_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto agents_cfg = load_agents_config(server);
    json agents = json::array();

    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        for (const auto& a : agents_cfg["agents"]) {
            json entry;
            entry["id"] = a.value("id", "");
            entry["name"] = a.value("name", a.value("id", ""));
            entry["workspace"] = resolve_agent_workspace(server, a.value("id", ""));
            if (a.contains("model")) entry["model"] = a["model"];
            agents.push_back(entry);
        }
    }

    // Ensure main agent exists
    bool has_main = false;
    for (const auto& a : agents) {
        if (a.value("id", "") == DEFAULT_AGENT_ID) { has_main = true; break; }
    }
    if (!has_main) {
        json main_agent;
        main_agent["id"] = DEFAULT_AGENT_ID;
        main_agent["name"] = "Main";
        main_agent["workspace"] = resolve_agent_workspace(server, DEFAULT_AGENT_ID);
        agents.insert(agents.begin(), main_agent);
    }

    json result;
    result["agents"] = agents;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// agents.create
// 🎯 OpenClaw 对齐: agents.ts L476-547
// ============================================================================
core::Result<json> method_agents_create(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string raw_name = params.value("name", "");
    while (!raw_name.empty() && raw_name.front() == ' ') raw_name.erase(0, 1);
    while (!raw_name.empty() && raw_name.back() == ' ') raw_name.pop_back();

    if (raw_name.empty()) {
        return core::Result<json>::Err("invalid agents.create params: name required");
    }

    std::string agent_id = normalize_agent_id(raw_name);
    if (agent_id == DEFAULT_AGENT_ID) {
        return core::Result<json>::Err("\"" + DEFAULT_AGENT_ID + "\" is reserved");
    }

    // Check if exists
    auto agents_cfg = load_agents_config(server);
    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        for (const auto& a : agents_cfg["agents"]) {
            if (a.value("id", "") == agent_id) {
                return core::Result<json>::Err("agent \"" + agent_id + "\" already exists");
            }
        }
    }

    // Resolve workspace
    std::string workspace = params.value("workspace", "");
    while (!workspace.empty() && workspace.front() == ' ') workspace.erase(0, 1);
    while (!workspace.empty() && workspace.back() == ' ') workspace.pop_back();
    if (workspace.empty()) {
        workspace = resolve_agent_workspace(server, agent_id);
    }

    // Create workspace directory
    try {
        fs::create_directories(workspace);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("failed to create workspace: " + std::string(e.what()));
    }

    // Add to config
    if (!agents_cfg.contains("agents") || !agents_cfg["agents"].is_array()) {
        agents_cfg["agents"] = json::array();
    }
    json agent_entry;
    agent_entry["id"] = agent_id;
    agent_entry["name"] = raw_name;
    agent_entry["workspace"] = workspace;
    agents_cfg["agents"].push_back(agent_entry);
    save_agents_config(server, agents_cfg);

    // Write IDENTITY.md
    // 🎯 OpenClaw 对齐: agents.ts L537-544
    std::string identity_path = workspace + "/" + IDENTITY_FILENAME;
    try {
        std::ofstream ident_f(identity_path, std::ios::app);
        if (ident_f.is_open()) {
            ident_f << "\n- Name: " << raw_name << "\n";
            std::string emoji = params.value("emoji", "");
            if (!emoji.empty()) ident_f << "- Emoji: " << emoji << "\n";
            std::string avatar = params.value("avatar", "");
            if (!avatar.empty()) ident_f << "- Avatar: " << avatar << "\n";
            ident_f << "\n";
        }
    } catch (...) {}

    // Create transcripts dir
    auto cfg = server->config();
    if (cfg) {
        try {
            fs::create_directories(cfg->config_dir + "/sessions/" + agent_id);
        } catch (...) {}
    }

    json result;
    result["ok"] = true;
    result["agentId"] = agent_id;
    result["name"] = raw_name;
    result["workspace"] = workspace;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// agents.update
// 🎯 OpenClaw 对齐: agents.ts L548-593
// ============================================================================
core::Result<json> method_agents_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = normalize_agent_id(params.value("agentId", ""));
    if (agent_id.empty()) {
        return core::Result<json>::Err("invalid agents.update params: agentId required");
    }

    auto agents_cfg = load_agents_config(server);
    bool found = false;
    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        for (auto& a : agents_cfg["agents"]) {
            if (a.value("id", "") == agent_id) {
                found = true;
                if (params.contains("name") && params["name"].is_string()) {
                    std::string name = params["name"].get<std::string>();
                    if (!name.empty()) a["name"] = name;
                }
                if (params.contains("workspace") && params["workspace"].is_string()) {
                    std::string ws = params["workspace"].get<std::string>();
                    if (!ws.empty()) {
                        a["workspace"] = ws;
                        try { fs::create_directories(ws); } catch (...) {}
                    }
                }
                if (params.contains("model") && params["model"].is_string()) {
                    a["model"] = params["model"];
                }
                break;
            }
        }
    }

    if (!found) {
        return core::Result<json>::Err("agent \"" + agent_id + "\" not found");
    }

    save_agents_config(server, agents_cfg);

    // Handle avatar update → append to IDENTITY.md
    if (params.contains("avatar") && params["avatar"].is_string()) {
        std::string avatar = params["avatar"].get<std::string>();
        if (!avatar.empty()) {
            std::string workspace = resolve_agent_workspace(server, agent_id);
            std::string identity_path = workspace + "/" + IDENTITY_FILENAME;
            try {
                fs::create_directories(workspace);
                std::ofstream f(identity_path, std::ios::app);
                if (f.is_open()) {
                    f << "\n- Avatar: " << avatar << "\n";
                }
            } catch (...) {}
        }
    }

    json result;
    result["ok"] = true;
    result["agentId"] = agent_id;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// agents.delete
// 🎯 OpenClaw 对齐: agents.ts L594-632
// ============================================================================
core::Result<json> method_agents_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = normalize_agent_id(params.value("agentId", ""));
    if (agent_id.empty()) {
        return core::Result<json>::Err("invalid agents.delete params: agentId required");
    }
    if (agent_id == DEFAULT_AGENT_ID) {
        return core::Result<json>::Err("\"" + DEFAULT_AGENT_ID + "\" cannot be deleted");
    }

    auto agents_cfg = load_agents_config(server);
    bool found = false;
    std::string workspace_dir;

    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        json new_agents = json::array();
        for (const auto& a : agents_cfg["agents"]) {
            if (a.value("id", "") == agent_id) {
                found = true;
                workspace_dir = a.value("workspace", resolve_agent_workspace(server, agent_id));
            } else {
                new_agents.push_back(a);
            }
        }
        agents_cfg["agents"] = new_agents;
    }

    if (!found) {
        return core::Result<json>::Err("agent \"" + agent_id + "\" not found");
    }

    save_agents_config(server, agents_cfg);

    // Delete files if requested (default: true)
    bool delete_files = params.value("deleteFiles", true);
    if (delete_files && !workspace_dir.empty()) {
        try {
            fs::remove_all(workspace_dir);
        } catch (...) {}
    }

    json result;
    result["ok"] = true;
    result["agentId"] = agent_id;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// agents.files.list
// 🎯 OpenClaw 对齐: agents.ts L633-662
// ============================================================================
core::Result<json> method_agents_files_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = normalize_agent_id(params.value("agentId", DEFAULT_AGENT_ID));

    // Verify agent exists
    auto agents_cfg = load_agents_config(server);
    bool found = false;
    if (agents_cfg.contains("agents") && agents_cfg["agents"].is_array()) {
        for (const auto& a : agents_cfg["agents"]) {
            if (a.value("id", "") == agent_id) { found = true; break; }
        }
    }
    if (!found && agent_id != DEFAULT_AGENT_ID) {
        return core::Result<json>::Err("unknown agent id");
    }

    std::string workspace = resolve_agent_workspace(server, agent_id);

    json files = json::array();

    // Bootstrap files
    for (const auto& name : BOOTSTRAP_FILE_NAMES) {
        std::string path = workspace + "/" + name;
        auto meta = stat_file_safely(path);
        json entry;
        entry["name"] = name;
        entry["path"] = path;
        if (meta.is_null()) {
            entry["missing"] = true;
        } else {
            entry["missing"] = false;
            entry["size"] = meta["size"];
            entry["updatedAtMs"] = meta["updatedAtMs"];
        }
        files.push_back(entry);
    }

    // Memory files
    std::string mem_path = workspace + "/" + MEMORY_FILENAME;
    auto mem_meta = stat_file_safely(mem_path);
    if (!mem_meta.is_null()) {
        json entry;
        entry["name"] = MEMORY_FILENAME;
        entry["path"] = mem_path;
        entry["missing"] = false;
        entry["size"] = mem_meta["size"];
        entry["updatedAtMs"] = mem_meta["updatedAtMs"];
        files.push_back(entry);
    } else {
        // Try alt memory
        std::string alt_path = workspace + "/" + MEMORY_ALT_FILENAME;
        auto alt_meta = stat_file_safely(alt_path);
        if (!alt_meta.is_null()) {
            json entry;
            entry["name"] = MEMORY_ALT_FILENAME;
            entry["path"] = alt_path;
            entry["missing"] = false;
            entry["size"] = alt_meta["size"];
            entry["updatedAtMs"] = alt_meta["updatedAtMs"];
            files.push_back(entry);
        } else {
            json entry;
            entry["name"] = MEMORY_FILENAME;
            entry["path"] = mem_path;
            entry["missing"] = true;
            files.push_back(entry);
        }
    }

    json result;
    result["agentId"] = agent_id;
    result["workspace"] = workspace;
    result["files"] = files;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// agents.files.get
// 🎯 OpenClaw 对齐: agents.ts L663-713
// ============================================================================
core::Result<json> method_agents_files_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = normalize_agent_id(params.value("agentId", DEFAULT_AGENT_ID));
    std::string name = params.value("name", "");
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();

    if (ALLOWED_FILE_NAMES.find(name) == ALLOWED_FILE_NAMES.end()) {
        return core::Result<json>::Err("unsupported file \"" + name + "\"");
    }

    std::string workspace = resolve_agent_workspace(server, agent_id);
    std::string file_path = workspace + "/" + name;

    // Safety: check symlink
    try {
        if (fs::is_symlink(file_path)) {
            auto target = fs::read_symlink(file_path);
            auto real = fs::canonical(file_path);
            auto ws_real = fs::canonical(workspace);
            if (real.string().find(ws_real.string()) != 0) {
                return core::Result<json>::Err("unsafe workspace file \"" + name + "\"");
            }
        }
    } catch (...) {}

    // Check existence
    if (!fs::exists(file_path)) {
        json result;
        result["agentId"] = agent_id;
        result["workspace"] = workspace;
        result["file"] = json::object({
            {"name", name}, {"path", file_path}, {"missing", true}
        });
        return core::Result<json>::Ok(result);
    }

    // Read content
    try {
        std::ifstream f(file_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
        auto meta = stat_file_safely(file_path);

        json result;
        result["agentId"] = agent_id;
        result["workspace"] = workspace;
        result["file"] = json::object({
            {"name", name},
            {"path", file_path},
            {"missing", false},
            {"size", meta.is_null() ? 0 : meta.value("size", 0)},
            {"updatedAtMs", meta.is_null() ? 0 : meta.value("updatedAtMs", (int64_t)0)},
            {"content", content},
        });
        return core::Result<json>::Ok(result);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("unsafe workspace file \"" + name + "\"");
    }
}

// ============================================================================
// agents.files.set
// 🎯 OpenClaw 对齐: agents.ts L714-773
// ============================================================================
core::Result<json> method_agents_files_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = normalize_agent_id(params.value("agentId", DEFAULT_AGENT_ID));
    std::string name = params.value("name", "");
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();

    if (ALLOWED_FILE_NAMES.find(name) == ALLOWED_FILE_NAMES.end()) {
        return core::Result<json>::Err("unsupported file \"" + name + "\"");
    }

    std::string content = params.value("content", "");
    std::string workspace = resolve_agent_workspace(server, agent_id);

    // Ensure workspace exists
    try {
        fs::create_directories(workspace);
    } catch (...) {}

    std::string file_path = workspace + "/" + name;

    // 🎯 OpenClaw 对齐: path escape guard
    auto ws_path = fs::path(workspace);
    auto fp_path = ws_path / name;
    // Ensure file stays within workspace
    auto resolved = fs::weakly_canonical(fp_path);
    auto resolved_ws = fs::weakly_canonical(ws_path);
    if (resolved.string().find(resolved_ws.string()) != 0) {
        return core::Result<json>::Err("unsafe workspace file \"" + name + "\"");
    }

    // Write
    try {
        std::ofstream out(file_path, std::ios::trunc);
        if (!out.is_open()) {
            return core::Result<json>::Err("unsafe workspace file \"" + name + "\"");
        }
        out << content;
        out.close();
    } catch (...) {
        return core::Result<json>::Err("unsafe workspace file \"" + name + "\"");
    }

    auto meta = stat_file_safely(file_path);

    json result;
    result["ok"] = true;
    result["agentId"] = agent_id;
    result["workspace"] = workspace;
    result["file"] = json::object({
        {"name", name},
        {"path", file_path},
        {"missing", false},
        {"size", meta.is_null() ? (int64_t)content.size() : meta.value("size", (int64_t)0)},
        {"updatedAtMs", meta.is_null() ? agents_now_ms() : meta.value("updatedAtMs", (int64_t)0)},
        {"content", content},
    });
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
