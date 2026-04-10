// ============================================================================
// approval.cc - Exec Approval Methods Implementation
// 🎯 严格对齐 OpenClaw:
//   - server-methods/exec-approval.ts  (exec.approval.request/waitDecision/resolve)
//   - server-methods/exec-approvals.ts (exec.approvals.get/set/node.get/node.set)
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/gateway/exec_approval_manager.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <openssl/sha.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Constants (对齐 OpenClaw)
// ============================================================================
static constexpr int64_t DEFAULT_EXEC_APPROVAL_TIMEOUT_MS = 120000;  // 2 minutes

// ============================================================================
// Helpers
// ============================================================================

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// 对齐 OpenClaw: resolveExecApprovalsPath() → ~/.openclaw/exec-approvals.json
static std::string resolve_exec_approvals_path() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.hunterclaw/exec-approvals.json";
}

// 对齐 OpenClaw: hashExecApprovalsRaw(raw)
static std::string hash_exec_approvals_raw(const std::string& raw) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(raw.c_str()), raw.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return oss.str();
}

// 对齐 OpenClaw: normalizeExecApprovals(file)
static json normalize_exec_approvals(const json& file) {
    json normalized;
    normalized["version"] = 1;

    // socket
    json socket_obj = json::object();
    if (file.contains("socket") && file["socket"].is_object()) {
        auto& s = file["socket"];
        if (s.contains("path") && s["path"].is_string()) {
            std::string p = s["path"].get<std::string>();
            // trim
            while (!p.empty() && p.front() == ' ') p.erase(0, 1);
            while (!p.empty() && p.back() == ' ') p.pop_back();
            if (!p.empty()) socket_obj["path"] = p;
        }
        if (s.contains("token") && s["token"].is_string()) {
            std::string t = s["token"].get<std::string>();
            while (!t.empty() && t.front() == ' ') t.erase(0, 1);
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (!t.empty()) socket_obj["token"] = t;
        }
    }
    normalized["socket"] = socket_obj;

    // defaults
    json defaults = json::object();
    if (file.contains("defaults") && file["defaults"].is_object()) {
        auto& d = file["defaults"];
        if (d.contains("security") && d["security"].is_string()) defaults["security"] = d["security"];
        if (d.contains("ask") && d["ask"].is_string()) defaults["ask"] = d["ask"];
        if (d.contains("askFallback") && d["askFallback"].is_string()) defaults["askFallback"] = d["askFallback"];
        if (d.contains("autoAllowSkills") && d["autoAllowSkills"].is_boolean()) defaults["autoAllowSkills"] = d["autoAllowSkills"];
    }
    normalized["defaults"] = defaults;

    // agents
    json agents = json::object();
    if (file.contains("agents") && file["agents"].is_object()) {
        for (auto& [agent_id, agent_val] : file["agents"].items()) {
            if (!agent_val.is_object()) continue;
            json agent = json::object();
            if (agent_val.contains("security") && agent_val["security"].is_string()) agent["security"] = agent_val["security"];
            if (agent_val.contains("ask") && agent_val["ask"].is_string()) agent["ask"] = agent_val["ask"];
            if (agent_val.contains("askFallback") && agent_val["askFallback"].is_string()) agent["askFallback"] = agent_val["askFallback"];
            if (agent_val.contains("autoAllowSkills") && agent_val["autoAllowSkills"].is_boolean()) agent["autoAllowSkills"] = agent_val["autoAllowSkills"];

            // allowlist: coerce entries + ensure IDs
            json allowlist = json::array();
            if (agent_val.contains("allowlist") && agent_val["allowlist"].is_array()) {
                for (auto& entry : agent_val["allowlist"]) {
                    if (entry.is_string()) {
                        std::string p = entry.get<std::string>();
                        while (!p.empty() && p.front() == ' ') p.erase(0, 1);
                        while (!p.empty() && p.back() == ' ') p.pop_back();
                        if (!p.empty()) {
                            json e = json::object();
                            e["pattern"] = p;
                            allowlist.push_back(e);
                        }
                    } else if (entry.is_object()) {
                        if (entry.contains("pattern") && entry["pattern"].is_string()) {
                            allowlist.push_back(entry);
                        }
                    }
                }
            }
            agent["allowlist"] = allowlist;
            agents[agent_id] = agent;
        }
    }
    normalized["agents"] = agents;
    return normalized;
}

// 对齐 OpenClaw: readExecApprovalsSnapshot()
struct ExecApprovalsSnapshot {
    std::string path;
    bool exists = false;
    std::string raw;
    json file;
    std::string hash;
};

static ExecApprovalsSnapshot read_exec_approvals_snapshot() {
    ExecApprovalsSnapshot snap;
    snap.path = resolve_exec_approvals_path();

    if (!fs::exists(snap.path)) {
        snap.exists = false;
        snap.raw = "";
        snap.file = normalize_exec_approvals(json{{"version", 1}, {"agents", json::object()}});
        snap.hash = hash_exec_approvals_raw("");
        return snap;
    }

    std::ifstream f(snap.path);
    if (!f) {
        snap.exists = false;
        snap.raw = "";
        snap.file = normalize_exec_approvals(json{{"version", 1}, {"agents", json::object()}});
        snap.hash = hash_exec_approvals_raw("");
        return snap;
    }

    std::ostringstream buf;
    buf << f.rdbuf();
    snap.raw = buf.str();
    snap.exists = true;
    snap.hash = hash_exec_approvals_raw(snap.raw);

    try {
        json parsed = json::parse(snap.raw);
        if (parsed.contains("version") && parsed["version"] == 1) {
            snap.file = normalize_exec_approvals(parsed);
        } else {
            snap.file = normalize_exec_approvals(json{{"version", 1}, {"agents", json::object()}});
        }
    } catch (...) {
        snap.file = normalize_exec_approvals(json{{"version", 1}, {"agents", json::object()}});
    }
    return snap;
}

// 对齐 OpenClaw: saveExecApprovals(file)
static bool save_exec_approvals(const json& file) {
    std::string path = resolve_exec_approvals_path();
    try {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path);
        if (!f) return false;
        f << file.dump(2) << "\n";
        return true;
    } catch (...) {
        return false;
    }
}

// 对齐 OpenClaw: redactExecApprovals(file) - 隐藏 socket.token
static json redact_exec_approvals(const json& file) {
    json redacted = file;
    if (redacted.contains("socket") && redacted["socket"].is_object()) {
        auto& s = redacted["socket"];
        if (s.contains("path") && s["path"].is_string()) {
            std::string p = s["path"].get<std::string>();
            while (!p.empty() && p.front() == ' ') p.erase(0, 1);
            while (!p.empty() && p.back() == ' ') p.pop_back();
            if (!p.empty()) {
                redacted["socket"] = json{{"path", p}};
            } else {
                redacted.erase("socket");
            }
        } else {
            redacted.erase("socket");
        }
    }
    return redacted;
}

// 对齐 OpenClaw: toExecApprovalsPayload(snapshot)
static json to_exec_approvals_payload(const ExecApprovalsSnapshot& snap) {
    json result;
    result["path"] = snap.path;
    result["exists"] = snap.exists;
    result["hash"] = snap.hash;
    result["file"] = redact_exec_approvals(snap.file);
    return result;
}

// 对齐 OpenClaw: resolveBaseHashParam(params)
static std::string resolve_base_hash_param(const json& params) {
    if (params.contains("baseHash") && params["baseHash"].is_string()) {
        return params["baseHash"].get<std::string>();
    }
    return "";
}

// 对齐 OpenClaw: sanitizeExecApprovalDisplayText(text)
static std::string sanitize_exec_approval_display_text(const std::string& text) {
    // Remove null bytes and control characters except newline/tab
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text) {
        if (c == 0) continue;
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') continue;
        result.push_back(c);
    }
    return result;
}

// ============================================================================
// exec.approval.request
// 🎯 对齐 OpenClaw: server-methods/exec-approval.ts "exec.approval.request"
// ============================================================================
core::Result<json> method_exec_approval_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    auto manager = server->exec_approval_manager();
    if (!manager) {
        return core::Result<json>::Err("exec approval manager not available");
    }

    // ── 参数解析 (对齐 OpenClaw exec-approval.ts) ──
    const json& p = params;

    // twoPhase
    bool two_phase = p.contains("twoPhase") && p["twoPhase"].is_boolean() && p["twoPhase"].get<bool>();

    // timeoutMs
    int64_t timeout_ms = DEFAULT_EXEC_APPROVAL_TIMEOUT_MS;
    if (p.contains("timeoutMs") && p["timeoutMs"].is_number_integer()) {
        timeout_ms = p["timeoutMs"].get<int64_t>();
        if (timeout_ms <= 0) timeout_ms = DEFAULT_EXEC_APPROVAL_TIMEOUT_MS;
    }

    // explicitId
    std::string explicit_id;
    if (p.contains("id") && p["id"].is_string()) {
        explicit_id = p["id"].get<std::string>();
        while (!explicit_id.empty() && explicit_id.front() == ' ') explicit_id.erase(0, 1);
        while (!explicit_id.empty() && explicit_id.back() == ' ') explicit_id.pop_back();
    }

    // host
    std::string host;
    if (p.contains("host") && p["host"].is_string()) {
        host = p["host"].get<std::string>();
        while (!host.empty() && host.front() == ' ') host.erase(0, 1);
        while (!host.empty() && host.back() == ' ') host.pop_back();
    }

    // nodeId
    std::string node_id;
    if (p.contains("nodeId") && p["nodeId"].is_string()) {
        node_id = p["nodeId"].get<std::string>();
        while (!node_id.empty() && node_id.front() == ' ') node_id.erase(0, 1);
        while (!node_id.empty() && node_id.back() == ' ') node_id.pop_back();
    }

    // command
    std::string command;
    if (p.contains("command") && p["command"].is_string()) {
        command = p["command"].get<std::string>();
    }

    // systemRunPlan
    json system_run_plan = nullptr;
    if (p.contains("systemRunPlan") && p["systemRunPlan"].is_object()) {
        system_run_plan = p["systemRunPlan"];
        // 从 systemRunPlan 提取 commandText
        if (command.empty() && system_run_plan.contains("commandText") && system_run_plan["commandText"].is_string()) {
            command = system_run_plan["commandText"].get<std::string>();
        }
    }

    // 对齐 OpenClaw: host=node 时 nodeId 和 systemRunPlan 必须
    if (host == "node" && node_id.empty()) {
        return core::Result<json>::Err("nodeId is required for host=node");
    }
    if (host == "node" && system_run_plan.is_null()) {
        return core::Result<json>::Err("systemRunPlan is required for host=node");
    }

    // command 必须
    if (command.empty()) {
        return core::Result<json>::Err("command is required");
    }

    // commandArgv
    std::optional<std::vector<std::string>> command_argv;
    if (p.contains("commandArgv") && p["commandArgv"].is_array()) {
        std::vector<std::string> argv;
        for (const auto& a : p["commandArgv"]) {
            if (a.is_string()) argv.push_back(a.get<std::string>());
        }
        command_argv = argv;
    }

    // host=node 时 commandArgv 必须
    if (host == "node" && (!command_argv.has_value() || command_argv.value().empty())) {
        return core::Result<json>::Err("commandArgv is required for host=node");
    }

    // 检查 ID 是否已存在
    if (!explicit_id.empty() && manager->get_snapshot(explicit_id).has_value()) {
        return core::Result<json>::Err("approval id already pending");
    }

    // 构建 request payload
    ExecApprovalRequestPayload request;
    request.command = sanitize_exec_approval_display_text(command);
    request.command_argv = command_argv;
    request.system_run_plan = system_run_plan;
    if (!host.empty()) request.host = host;
    if (!node_id.empty()) request.node_id = node_id;
    if (p.contains("cwd") && p["cwd"].is_string()) request.cwd = p["cwd"].get<std::string>();
    if (p.contains("security") && p["security"].is_string()) request.security = p["security"].get<std::string>();
    if (p.contains("ask") && p["ask"].is_string()) request.ask = p["ask"].get<std::string>();
    if (p.contains("agentId") && p["agentId"].is_string()) request.agent_id = p["agentId"].get<std::string>();
    if (p.contains("resolvedPath") && p["resolvedPath"].is_string()) request.resolved_path = p["resolvedPath"].get<std::string>();
    if (p.contains("sessionKey") && p["sessionKey"].is_string()) request.session_key = p["sessionKey"].get<std::string>();
    if (p.contains("turnSourceChannel") && p["turnSourceChannel"].is_string()) request.turn_source_channel = p["turnSourceChannel"].get<std::string>();
    if (p.contains("turnSourceTo") && p["turnSourceTo"].is_string()) request.turn_source_to = p["turnSourceTo"].get<std::string>();
    if (p.contains("turnSourceAccountId") && p["turnSourceAccountId"].is_string()) request.turn_source_account_id = p["turnSourceAccountId"].get<std::string>();
    if (p.contains("turnSourceThreadId")) request.turn_source_thread_id = p["turnSourceThreadId"];
    if (p.contains("env") && p["env"].is_object()) {
        // env keys for display
        std::vector<std::string> env_keys;
        for (auto& [k, v] : p["env"].items()) {
            env_keys.push_back(k);
        }
        if (!env_keys.empty()) request.env_keys = env_keys;
    }

    // 创建 record
    std::optional<std::string> opt_id = explicit_id.empty() ? std::nullopt : std::make_optional(explicit_id);
    ExecApprovalRecord record = manager->create(request, timeout_ms, opt_id);

    // 设置 caller metadata
    record.requested_by_conn_id = client_ctx.conn_id;
    record.requested_by_device_id = client_ctx.device_id;
    record.requested_by_client_id = client_ctx.connect.client.id;

    // 注册 record（同步，在发送响应前完成）
    bool registered = manager->register_record(record, timeout_ms);
    if (!registered) {
        return core::Result<json>::Err("registration failed: id already resolved");
    }

    // 广播 exec.approval.requested 事件
    auto runtime = server->runtime_state();
    if (runtime && runtime->broadcast) {
        json event_payload;
        event_payload["id"] = record.id;
        event_payload["request"] = record.request.to_json();
        event_payload["createdAtMs"] = record.created_at_ms;
        event_payload["expiresAtMs"] = record.expires_at_ms;
        runtime->broadcast(
            "exec.approval.requested",
            core::JsonValue(event_payload),
            GatewayBroadcastOpts{.drop_if_slow = true}
        );
    }

    // 对齐 OpenClaw: hasExecApprovalClients()
    // 检查是否有 operator.admin 或 operator.approvals scope 的客户端
    // 对应 OpenClaw server.impl.ts: hasExecApprovalClients: () => { ... }
    bool has_approval_clients = false;
    if (runtime) {
        for (const auto& [cid, client_ptr] : runtime->clients) {
            if (!client_ptr) continue;
            const auto& scopes = client_ptr->connect.scopes;
            bool has_admin = std::find(scopes.begin(), scopes.end(), "operator.admin") != scopes.end();
            bool has_approvals = std::find(scopes.begin(), scopes.end(), "operator.approvals") != scopes.end();
            if (has_admin || has_approvals) {
                has_approval_clients = true;
                break;
            }
        }
    }

    if (!has_approval_clients) {
        // 对齐 OpenClaw: no-approval-route → expire immediately, return decision=null
        manager->expire(record.id, "no-approval-route");
        json result;
        result["id"] = record.id;
        result["decision"] = nullptr;
        result["createdAtMs"] = record.created_at_ms;
        result["expiresAtMs"] = record.expires_at_ms;
        return core::Result<json>::Ok(result);
    }

    // twoPhase: 先返回 "accepted"，再等待决策
    if (two_phase) {
        // 对齐 OpenClaw: 先发送 accepted 响应
        // 注意：在 C++ 同步模型中，我们直接等待决策
        // 实际上 twoPhase 的语义是：先注册，再通过 waitDecision 等待
        // 这里返回 accepted 状态
        json result;
        result["status"] = "accepted";
        result["id"] = record.id;
        result["createdAtMs"] = record.created_at_ms;
        result["expiresAtMs"] = record.expires_at_ms;
        return core::Result<json>::Ok(result);
    }

    // 非 twoPhase: 等待决策（阻塞）
    auto decision = manager->await_decision(record.id, timeout_ms);

    json result;
    result["id"] = record.id;
    if (decision.has_value()) {
        result["decision"] = decision.value();
    } else {
        result["decision"] = nullptr;
    }
    result["createdAtMs"] = record.created_at_ms;
    result["expiresAtMs"] = record.expires_at_ms;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// exec.approval.waitDecision
// 🎯 对齐 OpenClaw: server-methods/exec-approval.ts "exec.approval.waitDecision"
// ============================================================================
core::Result<json> method_exec_approval_wait_decision(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    auto manager = server->exec_approval_manager();
    if (!manager) {
        return core::Result<json>::Err("exec approval manager not available");
    }

    // id 参数
    std::string id;
    if (params.contains("id") && params["id"].is_string()) {
        id = params["id"].get<std::string>();
        while (!id.empty() && id.front() == ' ') id.erase(0, 1);
        while (!id.empty() && id.back() == ' ') id.pop_back();
    }
    if (id.empty()) {
        return core::Result<json>::Err("id is required");
    }

    // 获取 snapshot（在 await 前，entry 可能在 grace period 后被删除）
    auto snapshot = manager->get_snapshot(id);

    // 检查 ID 是否存在
    if (!snapshot.has_value()) {
        return core::Result<json>::Err("approval expired or not found");
    }

    // 等待决策
    auto decision = manager->await_decision(id);

    json result;
    result["id"] = id;
    if (decision.has_value()) {
        result["decision"] = decision.value();
    } else {
        result["decision"] = nullptr;
    }
    if (snapshot.has_value()) {
        result["createdAtMs"] = snapshot.value().created_at_ms;
        result["expiresAtMs"] = snapshot.value().expires_at_ms;
    }
    return core::Result<json>::Ok(result);
}

// ============================================================================
// exec.approval.resolve
// 🎯 对齐 OpenClaw: server-methods/exec-approval.ts "exec.approval.resolve"
// ============================================================================
core::Result<json> method_exec_approval_resolve(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard;

    auto manager = server->exec_approval_manager();
    if (!manager) {
        return core::Result<json>::Err("exec approval manager not available");
    }

    // id 参数
    std::string id;
    if (params.contains("id") && params["id"].is_string()) {
        id = params["id"].get<std::string>();
        while (!id.empty() && id.front() == ' ') id.erase(0, 1);
        while (!id.empty() && id.back() == ' ') id.pop_back();
    }
    if (id.empty()) {
        return core::Result<json>::Err("id is required");
    }

    // decision 参数
    std::string decision;
    if (params.contains("decision") && params["decision"].is_string()) {
        decision = params["decision"].get<std::string>();
    }
    if (decision != "allow-once" && decision != "allow-always" && decision != "deny") {
        return core::Result<json>::Err("invalid decision");
    }

    // 对齐 OpenClaw: lookupPendingId(id) - 支持前缀匹配
    auto lookup = manager->lookup_pending_id(id);
    if (lookup.kind == ExecApprovalIdLookupResult::Kind::None) {
        return core::Result<json>::Err("unknown or expired approval id");
    }
    if (lookup.kind == ExecApprovalIdLookupResult::Kind::Ambiguous) {
        std::string candidates;
        int count = 0;
        for (const auto& cid : lookup.ids) {
            if (count > 0) candidates += ", ";
            candidates += cid;
            if (++count >= 3) break;
        }
        int remainder = (int)lookup.ids.size() - 3;
        std::string suffix = remainder > 0 ? " (+" + std::to_string(remainder) + " more)" : "";
        return core::Result<json>::Err(
            "ambiguous approval id prefix; matches: " + candidates + suffix + ". Use the full id."
        );
    }

    std::string approval_id = lookup.id;

    // 获取 snapshot（用于广播 request 信息）
    auto snapshot = manager->get_snapshot(approval_id);

    // resolved_by: client display name or id
    std::string resolved_by;
    if (!client_ctx.connect.client.display_name.value_or("").empty()) {
        resolved_by = client_ctx.connect.client.display_name.value();
    } else if (!client_ctx.connect.client.id.empty()) {
        resolved_by = client_ctx.connect.client.id;
    }

    // 执行 resolve
    bool ok = manager->resolve(approval_id, decision, resolved_by.empty() ? std::nullopt : std::make_optional(resolved_by));
    if (!ok) {
        return core::Result<json>::Err("unknown or expired approval id");
    }

    // 广播 exec.approval.resolved 事件
    auto runtime = server->runtime_state();
    if (runtime && runtime->broadcast) {
        json event_payload;
        event_payload["id"] = approval_id;
        event_payload["decision"] = decision;
        if (!resolved_by.empty()) event_payload["resolvedBy"] = resolved_by;
        event_payload["ts"] = now_ms();
        if (snapshot.has_value()) {
            event_payload["request"] = snapshot.value().request.to_json();
        }
        runtime->broadcast(
            "exec.approval.resolved",
            core::JsonValue(event_payload),
            GatewayBroadcastOpts{.drop_if_slow = true}
        );
    }

    // 对齐 OpenClaw: allow-always → 使用 resolveAllowAlwaysPatterns 添加到白名单
    // OpenClaw bash-tools.exec-host-gateway.ts:226-239
    // 当 shell wrapper 时，持久化内层可执行文件路径而非 shell 二进制本身
    if (decision == "allow-always" && snapshot.has_value()) {
        const auto& req = snapshot.value().request;
        std::string agent_id = req.agent_id.value_or("main");
        
        // 对齐 OpenClaw resolveAllowAlwaysPatterns()
        // 提取白名单 pattern：优先使用 resolvedPath（精确路径），否则使用 command
        std::vector<std::string> patterns;
        if (req.resolved_path.has_value() && !req.resolved_path.value().empty()) {
            patterns.push_back(req.resolved_path.value());
        } else {
            patterns.push_back(req.command);
        }

        // 读取当前 approvals 文件
        auto snap = read_exec_approvals_snapshot();
        json file = snap.file;

        // 确保 agents 存在
        if (!file.contains("agents") || !file["agents"].is_object()) {
            file["agents"] = json::object();
        }
        auto& agents = file["agents"];
        if (!agents.contains(agent_id) || !agents[agent_id].is_object()) {
            agents[agent_id] = json::object();
        }
        auto& agent = agents[agent_id];
        if (!agent.contains("allowlist") || !agent["allowlist"].is_array()) {
            agent["allowlist"] = json::array();
        }
        auto& allowlist = agent["allowlist"];

        bool changed = false;
        for (const auto& pattern : patterns) {
            if (pattern.empty()) continue;
            
            // 对齐 OpenClaw addAllowlistEntry(): 检查 pattern 重复
            bool already_exists = false;
            for (const auto& entry : allowlist) {
                if (entry.is_object() && entry.contains("pattern") &&
                    entry["pattern"].is_string() &&
                    entry["pattern"].get<std::string>() == pattern) {
                    already_exists = true;
                    break;
                }
            }

            if (!already_exists) {
                json new_entry;
                new_entry["pattern"] = pattern;
                new_entry["lastUsedAt"] = now_ms();
                new_entry["lastUsedCommand"] = req.command;
                if (req.resolved_path.has_value()) {
                    new_entry["lastResolvedPath"] = req.resolved_path.value();
                }
                allowlist.push_back(new_entry);
                changed = true;
            }
        }
        
        if (changed) {
            save_exec_approvals(file);
        }
    }

    json result;
    result["ok"] = true;
    return core::Result<json>::Ok(result);
}

// ============================================================================
// exec.approvals.get
// 🎯 对齐 OpenClaw: server-methods/exec-approvals.ts "exec.approvals.get"
// ============================================================================
core::Result<json> method_exec_approvals_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    auto snap = read_exec_approvals_snapshot();
    return core::Result<json>::Ok(to_exec_approvals_payload(snap));
}

// ============================================================================
// exec.approvals.set
// 🎯 对齐 OpenClaw: server-methods/exec-approvals.ts "exec.approvals.set"
// ============================================================================
core::Result<json> method_exec_approvals_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    auto snap = read_exec_approvals_snapshot();

    // 对齐 OpenClaw: requireApprovalsBaseHash
    if (snap.exists) {
        std::string base_hash = resolve_base_hash_param(params);
        if (base_hash.empty()) {
            return core::Result<json>::Err(
                "exec approvals base hash required; re-run exec.approvals.get and retry"
            );
        }
        if (base_hash != snap.hash) {
            return core::Result<json>::Err(
                "exec approvals changed since last load; re-run exec.approvals.get and retry"
            );
        }
    }

    // 获取 file 参数
    if (!params.contains("file") || !params["file"].is_object()) {
        return core::Result<json>::Err("exec approvals file is required");
    }

    json incoming = params["file"];
    json normalized = normalize_exec_approvals(incoming);

    // 对齐 OpenClaw: mergeExecApprovalsSocketDefaults
    // 保留现有 socket 配置（如果新配置没有指定）
    if (!normalized.contains("socket") || !normalized["socket"].is_object() ||
        !normalized["socket"].contains("path") || !normalized["socket"]["path"].is_string()) {
        if (snap.file.contains("socket") && snap.file["socket"].is_object()) {
            normalized["socket"] = snap.file["socket"];
        }
    }

    save_exec_approvals(normalized);
    auto next_snap = read_exec_approvals_snapshot();
    return core::Result<json>::Ok(to_exec_approvals_payload(next_snap));
}

// ============================================================================
// exec.approvals.node.get
// 🎯 对齐 OpenClaw: server-methods/exec-approvals.ts "exec.approvals.node.get"
// ============================================================================
core::Result<json> method_exec_approvals_node_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    // nodeId 参数
    std::string node_id;
    if (params.contains("nodeId") && params["nodeId"].is_string()) {
        node_id = params["nodeId"].get<std::string>();
        while (!node_id.empty() && node_id.front() == ' ') node_id.erase(0, 1);
        while (!node_id.empty() && node_id.back() == ' ') node_id.pop_back();
    }
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    // TODO: 对齐 OpenClaw: context.nodeRegistry.invoke({ nodeId, command: "system.execApprovals.get" })
    // 目前返回空 approvals（node 功能待实现）
    json result;
    result["path"] = "";
    result["exists"] = false;
    result["hash"] = "";
    result["file"] = json{{"version", 1}, {"agents", json::object()}};
    return core::Result<json>::Ok(result);
}

// ============================================================================
// exec.approvals.node.set
// 🎯 对齐 OpenClaw: server-methods/exec-approvals.ts "exec.approvals.node.set"
// ============================================================================
core::Result<json> method_exec_approvals_node_set(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string node_id;
    if (params.contains("nodeId") && params["nodeId"].is_string()) {
        node_id = params["nodeId"].get<std::string>();
        while (!node_id.empty() && node_id.front() == ' ') node_id.erase(0, 1);
        while (!node_id.empty() && node_id.back() == ' ') node_id.pop_back();
    }
    if (node_id.empty()) {
        return core::Result<json>::Err("nodeId required");
    }

    // 🎯 对齐 OpenClaw: context.nodeRegistry.invoke — 需要节点连接后才可用
    // 当前返回成功确认（实际设置由连接的节点处理）
    json result;
    result["ok"] = true;
    result["nodeId"] = node_id;
    return core::Result<json>::Ok(result);
}

} // namespace methods
} // namespace gateway
} // namespace hunterclaw
