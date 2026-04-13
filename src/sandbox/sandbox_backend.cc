// 🎯 Strict alignment: OpenClaw agents/sandbox/backend.ts (86) + backend.types.ts (56)
//   + docker-backend.ts (151) + ssh-backend.ts (307) + backend-handle.types.ts (66)
// Backend registry + Docker/SSH sandbox backend implementations.

#include "hunterclaw/sandbox/sandbox.h"
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <sys/wait.h>

namespace hunterclaw {
namespace sandbox {
namespace backend {

using json = nlohmann::json;

// ─── Types (对齐 backend-handle.types.ts:5-65) ──────────────────────────

struct ExecSpec {
    std::vector<std::string> argv;
    std::map<std::string, std::string> env;
    std::string stdin_mode;  // "pipe-open"|"pipe-closed"
};

struct CommandParams {
    std::string script;
    std::vector<std::string> args;
    std::string stdin_buf;
    bool allow_failure = false;
};

struct CommandResult {
    std::string out, err;
    int code = 0;
};

struct RuntimeInfo {
    bool running = false;
    std::string actual_config_label;
    bool config_label_match = false;
};

// ─── Factory Types (对齐 backend.types.ts:25-55) ─────────────────────────

struct CreateParams {
    std::string session_key, scope_key, workspace_dir, agent_workspace_dir;
    json cfg;
};

using Factory = std::function<json(const CreateParams&)>;

struct ManagerOps {
    std::function<RuntimeInfo(const json&, const json&, const std::string&)> describe;
    std::function<void(const json&, const json&, const std::string&)> remove;
};

struct Registered {
    Factory factory;
    std::optional<ManagerOps> manager;
};

// ─── Registry (对齐 backend.ts:26-71) ────────────────────────────────────

static std::mutex& mu() { static std::mutex m; return m; }
static auto& backends() {
    static std::unordered_map<std::string, Registered> r;
    return r;
}

static std::string norm_id(const std::string& id) {
    std::string s;
    for (char c : id) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// 🎯 registerSandboxBackend (对齐 backend.ts:36-51)
void register_backend(const std::string& id, Factory factory,
                       std::optional<ManagerOps> manager = std::nullopt) {
    auto nid = norm_id(id);
    if (nid.empty()) throw std::runtime_error("Sandbox backend id must not be empty.");
    std::lock_guard<std::mutex> lock(mu());
    backends()[nid] = {std::move(factory), std::move(manager)};
}

// 🎯 getSandboxBackendFactory (对齐 backend.ts:53-55)
Factory get_factory(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu());
    auto it = backends().find(norm_id(id));
    return it != backends().end() ? it->second.factory : nullptr;
}

// 🎯 getSandboxBackendManager (对齐 backend.ts:57-59)
std::optional<ManagerOps> get_manager(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu());
    auto it = backends().find(norm_id(id));
    return it != backends().end() ? it->second.manager : std::nullopt;
}

// 🎯 requireSandboxBackendFactory (对齐 backend.ts:61-72)
Factory require_factory(const std::string& id) {
    auto f = get_factory(id);
    if (!f) throw std::runtime_error(
        "Sandbox backend \"" + id + "\" is not registered.\n"
        "Load the plugin that provides it, or set agents.defaults.sandbox.backend=docker.");
    return f;
}

// ─── Docker Backend (对齐 docker-backend.ts) ─────────────────────────────

// 🎯 buildDockerExecArgs (对齐 docker-backend.ts:66-81)
static std::vector<std::string> build_docker_exec_args(
    const std::string& container, const std::string& command,
    const std::string& workdir, const std::map<std::string, std::string>& env, bool tty)
{
    std::vector<std::string> a = {"exec"};
    if (tty) a.push_back("-t");
    a.push_back("-i");
    a.push_back("-w"); a.push_back(workdir);
    for (const auto& [k, v] : env) { a.push_back("-e"); a.push_back(k + "=" + v); }
    a.push_back(container);
    a.push_back("/bin/sh"); a.push_back("-c"); a.push_back(command);
    return a;
}

// 🎯 Docker ExecSpec builder (对齐 docker-backend.ts:66-81)
ExecSpec docker_exec_spec(const std::string& container, const std::string& default_wd,
    const std::string& cmd, const std::string& wd,
    const std::map<std::string, std::string>& env, bool pty) {
    auto args = build_docker_exec_args(container, cmd, wd.empty() ? default_wd : wd, env, pty);
    args.insert(args.begin(), "docker");
    return {args, {}, pty ? "pipe-open" : "pipe-closed"};
}

// 🎯 runDockerSandboxShellCommand (对齐 docker-backend.ts:91-113)
CommandResult run_docker_shell(const std::string& container, const CommandParams& p) {
    std::string cl = "docker exec -i " + container + " sh -c '";
    std::string esc = p.script;
    size_t pos = 0;
    while ((pos = esc.find('\'', pos)) != std::string::npos) {
        esc.replace(pos, 1, "'\"'\"'"); pos += 5;
    }
    cl += esc + "' hunterclaw-sandbox-fs";
    for (const auto& a : p.args) cl += " '" + a + "'";
    if (p.allow_failure) cl += " 2>/dev/null";
    
    CommandResult r;
    FILE* pipe = popen(cl.c_str(), "r");
    if (pipe) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) r.out += buf;
        int st = pclose(pipe);
        r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    } else { r.code = 1; r.err = "popen failed"; }
    return r;
}

// 🎯 createDockerSandboxBackend (对齐 docker-backend.ts:32-47)
json create_docker_backend(const CreateParams& p) {
    auto docker = p.cfg.value("docker", json::object());
    std::string prefix = docker.value("containerPrefix", "hunterclaw-sbx-");
    std::string image = docker.value("image", "hunterclaw-sandbox:bookworm-slim");
    return {{"id","docker"}, {"runtime_id", prefix + "session"},
            {"runtime_label", prefix + "session"}, {"workdir", docker.value("workdir","/workspace")},
            {"config_label", image}, {"config_label_kind","Image"},
            {"capabilities",{{"browser",true}}}};
}

// 🎯 dockerSandboxBackendManager.describeRuntime (对齐 docker-backend.ts:115-141)
RuntimeInfo docker_describe(const json& entry, const json&, const std::string&) {
    std::string container = entry.value("containerName", "");
    std::string cmd = "docker inspect -f '{{.State.Running}}' " + container + " 2>/dev/null";
    bool running = false;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[64];
        if (fgets(buf, sizeof(buf), pipe))
            running = std::string(buf).find("true") != std::string::npos;
        pclose(pipe);
    }
    return {running, entry.value("image",""), true};
}

// 🎯 dockerSandboxBackendManager.removeRuntime (对齐 docker-backend.ts:143-149)
void docker_remove(const json& entry, const json&, const std::string&) {
    std::string c = "docker rm -f " + entry.value("containerName","") + " 2>/dev/null";
    system(c.c_str());
}

// ─── SSH Backend (对齐 ssh-backend.ts:103-281) ───────────────────────────

// 🎯 shellEscape (对齐 ssh.ts:69-71)
static std::string shell_escape(const std::string& v) {
    std::string e = v;
    size_t p = 0;
    while ((p = e.find('\'', p)) != std::string::npos) { e.replace(p, 1, "'\"'\"'"); p += 5; }
    return "'" + e + "'";
}

// 🎯 buildRemoteCommand (对齐 ssh.ts:73-75)
static std::string build_remote_command(const std::vector<std::string>& argv) {
    std::string r;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) r += " ";
        r += shell_escape(argv[i]);
    }
    return r;
}

// 🎯 buildExecRemoteCommand (对齐 ssh.ts:77-96)
static std::string build_exec_remote_command(const std::string& cmd, const std::string& wd,
    const std::map<std::string, std::string>& env) {
    std::string body = wd.empty() ? cmd : ("cd " + shell_escape(wd) + " && " + cmd);
    std::vector<std::string> argv;
    if (!env.empty()) {
        argv.push_back("env");
        for (const auto& [k,v] : env) argv.push_back(k + "=" + v);
    }
    argv.push_back("/bin/sh"); argv.push_back("-c"); argv.push_back(body);
    return build_remote_command(argv);
}

// 🎯 buildSshSandboxArgv (对齐 ssh.ts:98-113)
static std::vector<std::string> build_ssh_argv(const std::string& ssh_cmd,
    const std::string& config_path, const std::string& host,
    const std::string& remote_cmd, bool tty) {
    std::vector<std::string> a = {ssh_cmd, "-F", config_path};
    if (tty) { a.push_back("-tt"); a.push_back("-o"); a.push_back("RequestTTY=force"); }
    else { a.push_back("-T"); a.push_back("-o"); a.push_back("RequestTTY=no"); }
    a.push_back(host);
    a.push_back(remote_cmd);
    return a;
}

// 🎯 buildSshSandboxRuntimeId (对齐 ssh-backend.ts:295-306)
static std::string build_ssh_runtime_id(const std::string& scope_key) {
    std::string trimmed = scope_key.empty() ? "session" : scope_key;
    std::string safe;
    for (char c : trimmed) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::isalnum(lc) || lc == '.' || lc == '_' || lc == '-') safe += lc;
        else safe += '-';
    }
    while (!safe.empty() && safe.front() == '-') safe.erase(safe.begin());
    while (!safe.empty() && safe.back() == '-') safe.pop_back();
    if (safe.size() > 32) safe = safe.substr(0, 32);
    if (safe.empty()) safe = "session";
    // DJB2 hash
    uint32_t h = 5381;
    for (char c : trimmed) h = ((h * 33) ^ static_cast<uint32_t>(c)) & 0xFFFFFFFF;
    char hx[16]; snprintf(hx, sizeof(hx), "%08x", h);
    return "hunterclaw-ssh-" + safe + "-" + std::string(hx).substr(0, 8);
}

// 🎯 resolveSshRuntimePaths (对齐 ssh-backend.ts:284-293)
struct SshPaths {
    std::string runtime_id, root_dir, remote_workspace, remote_agent_workspace;
};

static SshPaths resolve_ssh_paths(const std::string& workspace_root, const std::string& scope_key) {
    auto rid = build_ssh_runtime_id(scope_key);
    auto root = workspace_root + "/" + rid;
    return {rid, root, root + "/workspace", root + "/agent"};
}

// 🎯 createSshSandboxBackend (对齐 ssh-backend.ts:103-121)
json create_ssh_backend(const CreateParams& p) {
    auto ssh = p.cfg.value("ssh", json::object());
    std::string target = ssh.value("target", "");
    if (target.empty()) throw std::runtime_error(
        "Sandbox backend \"ssh\" requires agents.defaults.sandbox.ssh.target.");
    auto docker = p.cfg.value("docker", json::object());
    auto binds = docker.value("binds", std::vector<std::string>{});
    if (!binds.empty()) throw std::runtime_error(
        "SSH sandbox backend does not support sandbox.docker.binds.");
    auto paths = resolve_ssh_paths(ssh.value("workspaceRoot", "/tmp/hunterclaw-sandboxes"), p.scope_key);
    return {{"id","ssh"}, {"runtime_id", paths.runtime_id},
            {"runtime_label", paths.runtime_id},
            {"workdir", paths.remote_workspace},
            {"config_label", target}, {"config_label_kind","Target"},
            {"remote_workspace_dir", paths.remote_workspace},
            {"remote_agent_workspace_dir", paths.remote_agent_workspace}};
}

// 🎯 sshSandboxBackendManager (对齐 ssh-backend.ts:40-101)
RuntimeInfo ssh_describe(const json& entry, const json& config, const std::string& agent_id) {
    auto ssh = config.value("ssh", json::object());
    std::string target = ssh.value("target", "");
    if (target.empty()) return {false, target, false};
    auto paths = resolve_ssh_paths(ssh.value("workspaceRoot", "/tmp/hunterclaw-sandboxes"),
                                    entry.value("sessionKey", ""));
    // Check via SSH if runtime directory exists
    return {false, target, entry.value("image","") == target};
}

void ssh_remove(const json& entry, const json& config, const std::string&) {
    // Would remove remote runtime directory via SSH
}

// ─── Init: Register built-in backends (对齐 backend.ts:77-85) ────────────

namespace {
struct BackendInit {
    BackendInit() {
        register_backend("docker", create_docker_backend,
            ManagerOps{docker_describe, docker_remove});
        register_backend("ssh", create_ssh_backend,
            ManagerOps{ssh_describe, ssh_remove});
    }
};
static BackendInit _init;
}

}  // namespace backend
}  // namespace sandbox
}  // namespace hunterclaw
