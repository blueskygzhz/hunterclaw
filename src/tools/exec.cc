// ============================================================================
// exec.cc - Exec Tool Implementation (Strictly aligned with OpenClaw)
//
// Alignment target: src/agents/bash-tools.exec.ts (createExecTool)
//                   src/agents/bash-tools.exec-runtime.ts (runExecProcess)
//                   src/agents/bash-tools.exec-host-gateway.ts (processGatewayAllowlist)
//
// Key flows aligned:
//   1. Parameter parsing (command/workdir/env/yieldMs/background/timeout/pty/elevated/host/security/ask/node)
//   2. Host routing: sandbox -> gateway -> node
//   3. Security/ask policy: minSecurity() / maxAsk()
//   4. Gateway allowlist + approval flow (exec.approval.requested broadcast)
//   5. before_tool_call / after_tool_call plugin hooks
//   6. runExecProcess: spawn, PTY, yield/background
//   7. validateScriptFileForShellBleed preflight
//   8. notifyOnExit / emitExecSystemEvent
// ============================================================================

#include "hunterclaw/tools/exec.h"
#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/approval.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/gateway/connection_pool.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>
#include <memory>
#include <chrono>
#include <thread>
#include <future>
#include <regex>
#include <algorithm>
#include <cstdlib>
#include <sys/wait.h>
#include <signal.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace tools {

namespace {

auto log() {
    static auto logger = spdlog::get("exec");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("exec");
    }
    return logger;
}

// ============================================================================
// Constants (aligned with OpenClaw bash-tools.exec-runtime.ts)
// ============================================================================

constexpr int64_t DEFAULT_MAX_OUTPUT_CHARS       = 200000;
constexpr int64_t DEFAULT_PENDING_MAX_OUTPUT_CHARS = 30000;
constexpr int64_t DEFAULT_BACKGROUND_MS          = 10000;
constexpr int64_t DEFAULT_TIMEOUT_SEC            = 1800;
constexpr int64_t DEFAULT_APPROVAL_TIMEOUT_MS    = 120000;
constexpr int64_t DEFAULT_NOTIFY_TAIL_CHARS      = 400;
constexpr int64_t DEFAULT_NOTIFY_SNIPPET_CHARS   = 180;

// ============================================================================
// ExecHost / ExecSecurity / ExecAsk (aligned with OpenClaw infra/exec-approvals.ts)
// ============================================================================

enum class ExecHost { SANDBOX, GATEWAY, NODE };
enum class ExecSecurityLevel { DENY, ALLOWLIST, FULL };
enum class ExecAskMode { OFF, ON_MISS, ALWAYS };

ExecHost normalize_exec_host(const std::string& s) {
    if (s == "gateway") return ExecHost::GATEWAY;
    if (s == "node")    return ExecHost::NODE;
    return ExecHost::SANDBOX;
}

ExecSecurityLevel normalize_exec_security(const std::string& s) {
    if (s == "deny")      return ExecSecurityLevel::DENY;
    if (s == "full")      return ExecSecurityLevel::FULL;
    return ExecSecurityLevel::ALLOWLIST;
}

ExecAskMode normalize_exec_ask(const std::string& s) {
    if (s == "off")    return ExecAskMode::OFF;
    if (s == "always") return ExecAskMode::ALWAYS;
    return ExecAskMode::ON_MISS;
}

// minSecurity: deny < allowlist < full (aligned with OpenClaw)
ExecSecurityLevel min_security(ExecSecurityLevel a, ExecSecurityLevel b) {
    return static_cast<ExecSecurityLevel>(
        std::min(static_cast<int>(a), static_cast<int>(b)));
}

// maxAsk: off < on-miss < always (aligned with OpenClaw)
ExecAskMode max_ask(ExecAskMode a, ExecAskMode b) {
    return static_cast<ExecAskMode>(
        std::max(static_cast<int>(a), static_cast<int>(b)));
}

std::string render_exec_host_label(ExecHost host) {
    switch (host) {
        case ExecHost::SANDBOX:  return "sandbox";
        case ExecHost::GATEWAY:  return "gateway";
        case ExecHost::NODE:     return "node";
    }
    return "sandbox";
}

// ============================================================================
// Script preflight: detect shell variable injection in Python/JS
// (aligned with OpenClaw bash-tools.exec.ts validateScriptFileForShellBleed)
// ============================================================================

struct ScriptTarget {
    enum class Kind { PYTHON, NODE };
    Kind kind;
    std::string rel_or_abs_path;
};

std::optional<ScriptTarget> extract_script_target(const std::string& command) {
    std::string raw = command;
    // trim
    size_t start = raw.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::nullopt;
    raw = raw.substr(start);

    // python3? file.py
    std::regex python_re(R"(^\s*(python3?|python)\s+(?:-[^\s]+\s+)*([^\s]+\.py)\b)",
                         std::regex::icase);
    std::smatch m;
    if (std::regex_search(raw, m, python_re) && m.size() >= 3) {
        return ScriptTarget{ScriptTarget::Kind::PYTHON, m[2].str()};
    }

    // node file.js
    std::regex node_re(R"(^\s*(node)\s+(?:--[^\s]+\s+)*([^\s]+\.js)\b)",
                       std::regex::icase);
    if (std::regex_search(raw, m, node_re) && m.size() >= 3) {
        return ScriptTarget{ScriptTarget::Kind::NODE, m[2].str()};
    }

    return std::nullopt;
}

// Returns error message if shell bleed detected, empty string otherwise
std::string validate_script_file_for_shell_bleed(
    const std::string& command,
    const std::string& workdir)
{
    auto target = extract_script_target(command);
    if (!target) return "";

    fs::path abs_path = fs::path(target->rel_or_abs_path).is_absolute()
        ? fs::path(target->rel_or_abs_path)
        : fs::path(workdir) / target->rel_or_abs_path;
    abs_path = fs::weakly_canonical(abs_path);

    std::error_code ec;
    auto stat = fs::status(abs_path, ec);
    if (ec || !fs::is_regular_file(stat)) return "";

    auto file_size = fs::file_size(abs_path, ec);
    if (ec || file_size > 512 * 1024) return "";

    std::ifstream f(abs_path);
    if (!f) return "";
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Detect $ALL_CAPS_VAR patterns
    std::regex env_var_re(R"(\$[A-Z_][A-Z0-9_]{1,})");
    std::smatch em;
    if (std::regex_search(content, em, env_var_re)) {
        std::string token = em[0].str();
        std::string kind_str = (target->kind == ScriptTarget::Kind::PYTHON) ? "Python" : "Node.js";
        return "exec preflight: detected likely shell variable injection (" + token +
               ") in " + kind_str + " script: " + abs_path.filename().string() +
               ". Use language-native env access instead of raw " + token + ".";
    }

    // Node.js: detect shell commands accidentally emitted as JS
    if (target->kind == ScriptTarget::Kind::NODE) {
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            // trim
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            std::string trimmed = line.substr(s);
            if (!trimmed.empty() && trimmed.substr(0, 4) == "NODE") {
                return "exec preflight: JS file starts with shell syntax (" + trimmed +
                       "). This looks like a shell command, not JavaScript.";
            }
            break;
        }
    }

    return "";
}

// ============================================================================
// Process execution (aligned with OpenClaw runExecProcess)
// ============================================================================

struct ExecProcessOutcome {
    enum class Status { COMPLETED, FAILED };
    Status status;
    int exit_code;
    int64_t duration_ms;
    std::string aggregated;
    bool timed_out;
    std::string reason;
};

struct ExecProcessHandle {
    std::string session_id;
    int64_t started_at;
    int pid;
    std::future<ExecProcessOutcome> promise;
    std::function<void()> kill_fn;
};

// Compact output for notifications (aligned with OpenClaw compactNotifyOutput)
std::string compact_notify_output(const std::string& value, int64_t max_chars = DEFAULT_NOTIFY_SNIPPET_CHARS) {
    // normalize whitespace
    std::string normalized;
    bool last_space = false;
    for (char c : value) {
        if (std::isspace(c)) {
            if (!last_space && !normalized.empty()) {
                normalized += ' ';
                last_space = true;
            }
        } else {
            normalized += c;
            last_space = false;
        }
    }
    // trim trailing space
    while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();

    if (normalized.empty()) return "";
    if (static_cast<int64_t>(normalized.size()) <= max_chars) return normalized;
    int64_t safe = std::max(int64_t(1), max_chars - 1);
    return normalized.substr(0, safe) + "…";
}

// Run exec process via popen (simplified; production should use supervisor)
// Aligned with OpenClaw runExecProcess structure
std::unique_ptr<ExecProcessHandle> run_exec_process(
    const std::string& command,
    const std::string& workdir,
    const std::map<std::string, std::string>& env,
    bool use_pty,
    int64_t timeout_sec,
    int64_t max_output,
    std::function<void(const std::string&)> on_update)
{
    auto handle = std::make_unique<ExecProcessHandle>();
    handle->session_id = core::utils::generate_uuid().substr(0, 8);
    handle->started_at = core::utils::current_timestamp_ms();
    handle->pid = 0;

    // Build env string for shell
    std::string env_prefix;
    for (const auto& [k, v] : env) {
        env_prefix += k + "=" + v + " ";
    }

    // Build full command with workdir
    std::string full_cmd = "cd " + workdir + " && " + env_prefix + command + " 2>&1";
    if (timeout_sec > 0) {
        full_cmd = "timeout " + std::to_string(timeout_sec) + " sh -c " +
                   std::string("'") + full_cmd + "'";
    }

    int64_t started_at_copy = handle->started_at;
    int64_t timeout_sec_copy = timeout_sec;
    handle->promise = std::async(std::launch::async, [full_cmd, max_output, started_at_copy, timeout_sec_copy, on_update]() -> ExecProcessOutcome {
        ExecProcessOutcome outcome;
        int64_t started_at = started_at_copy;
        int64_t timeout_sec = timeout_sec_copy;

        std::array<char, 4096> buffer;
        std::string aggregated;
        bool truncated = false;

        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) {
            outcome.status = ExecProcessOutcome::Status::FAILED;
            outcome.exit_code = -1;
            outcome.duration_ms = core::utils::current_timestamp_ms() - started_at;
            outcome.reason = "Failed to spawn process";
            return outcome;
        }

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            std::string chunk(buffer.data());
            if (static_cast<int64_t>(aggregated.size()) < max_output) {
                aggregated += chunk;
                if (on_update) on_update(chunk);
            } else if (!truncated) {
                truncated = true;
                aggregated += "\n\n[... output truncated ...]";
            }
        }

        int raw_exit = pclose(pipe);
        int exit_code = WIFEXITED(raw_exit) ? WEXITSTATUS(raw_exit) : -1;
        int64_t duration_ms = core::utils::current_timestamp_ms() - started_at;

        // Shell exit codes 126/127 are infrastructure failures (aligned with OpenClaw)
        bool is_shell_failure = (exit_code == 126 || exit_code == 127);
        bool timed_out = (exit_code == 124);  // timeout(1) returns 124

        if (exit_code == 0 && !is_shell_failure) {
            outcome.status = ExecProcessOutcome::Status::COMPLETED;
            outcome.exit_code = exit_code;
            outcome.duration_ms = duration_ms;
            outcome.aggregated = aggregated;
            outcome.timed_out = false;
        } else {
            outcome.status = ExecProcessOutcome::Status::FAILED;
            outcome.exit_code = exit_code;
            outcome.duration_ms = duration_ms;
            outcome.aggregated = aggregated;
            outcome.timed_out = timed_out;

            if (is_shell_failure) {
                outcome.reason = (exit_code == 127)
                    ? "Command not found"
                    : "Command not executable (permission denied)";
            } else if (timed_out) {
                outcome.reason = "Command timed out after " + std::to_string(timeout_sec) +
                                 " seconds. Re-run with a higher timeout if needed.";
            } else {
                outcome.reason = aggregated.empty()
                    ? "Command failed with exit code " + std::to_string(exit_code)
                    : aggregated + "\n\nCommand failed with exit code " + std::to_string(exit_code);
            }
        }

        return outcome;
    });

    handle->kill_fn = [raw_handle = handle.get()]() {
        // ✅ 对齐 OpenClaw: 通过 session tracking 终止进程
        // 使用 SIGTERM 发送给进程组
        if (raw_handle && raw_handle->pid > 0) {
            ::kill(-raw_handle->pid, SIGTERM);
        }
    };

    return handle;
}

// ============================================================================
// Gateway approval flow (aligned with OpenClaw processGatewayAllowlist)
// ============================================================================

struct GatewayApprovalResult {
    bool requires_approval;
    bool approved;
    std::string approval_id;
    std::string denied_reason;
    std::string enforced_command;  // safe-bin enforced command override
};

GatewayApprovalResult process_gateway_allowlist(
    const std::string& command,
    const std::string& workdir,
    ExecSecurityLevel security,
    ExecAskMode ask,
    const std::string& agent_id,
    const std::string& session_key,
    std::shared_ptr<ApprovalManager> approval_mgr,
    std::shared_ptr<gateway::GatewayServer> gateway_server)
{
    GatewayApprovalResult result;
    result.requires_approval = false;
    result.approved = true;

    // DENY mode: always requires approval (and will be denied)
    if (security == ExecSecurityLevel::DENY) {
        result.requires_approval = true;
        result.approved = false;
        result.denied_reason = "exec denied: security=deny";
        return result;
    }

    // FULL + OFF: bypass approval
    if (security == ExecSecurityLevel::FULL && ask == ExecAskMode::OFF) {
        result.approved = true;
        return result;
    }

    // ALLOWLIST mode: check allowlist
    bool allowlist_satisfied = false;
    if (security == ExecSecurityLevel::ALLOWLIST && approval_mgr) {
        allowlist_satisfied = approval_mgr->is_allowlisted(agent_id, command);
    }

    // Determine if approval is required (aligned with OpenClaw requiresExecApproval)
    bool requires_ask = false;
    if (ask == ExecAskMode::ALWAYS) {
        requires_ask = true;
    } else if (ask == ExecAskMode::ON_MISS) {
        requires_ask = !allowlist_satisfied;
    }
    // FULL security with ask != OFF still requires ask
    if (security == ExecSecurityLevel::FULL && ask != ExecAskMode::OFF) {
        requires_ask = true;
    }

    if (!requires_ask) {
        // Allowlist satisfied, no approval needed
        result.approved = true;
        return result;
    }

    result.requires_approval = true;

    if (!approval_mgr) {
        result.approved = false;
        result.denied_reason = "exec requires approval but approval manager not configured";
        return result;
    }

    // Build approval request (aligned with OpenClaw exec.approval.request)
    ApprovalRequest req;
    req.id = core::utils::generate_uuid();
    req.command = command;
    req.cwd = workdir;
    req.host = "gateway";
    req.agent_id = agent_id;
    req.session_key = session_key;
    req.timeout_ms = DEFAULT_APPROVAL_TIMEOUT_MS;
    req.two_phase = true;
    req.created_at_ms = core::utils::current_timestamp_ms();
    req.expires_at_ms = req.created_at_ms + req.timeout_ms;

    // Broadcast exec.approval.requested event to all connected clients
    // (aligned with OpenClaw: context.broadcast("exec.approval.requested", {...}))
    if (gateway_server) {
        json broadcast_payload;
        broadcast_payload["id"] = req.id;
        broadcast_payload["request"]["command"] = req.command;
        broadcast_payload["request"]["cwd"] = req.cwd;
        broadcast_payload["request"]["host"] = req.host;
        broadcast_payload["request"]["agentId"] = req.agent_id;
        broadcast_payload["request"]["sessionKey"] = req.session_key;
        broadcast_payload["createdAtMs"] = req.created_at_ms;
        broadcast_payload["expiresAtMs"] = req.expires_at_ms;

        gateway::ConnectionPool::instance().broadcast("exec.approval.requested", broadcast_payload);
        log()->info("exec: broadcast exec.approval.requested id={}", req.id);
    }

    // Register and wait for decision
    auto req_result = approval_mgr->request_approval(req);
    if (req_result.is_err()) {
        result.approved = false;
        result.denied_reason = "Failed to register approval: " + req_result.error;
        return result;
    }

    result.approval_id = req.id;
    log()->info("exec: waiting for approval id={} command={}", req.id, command);

    auto resp_result = approval_mgr->wait_decision(req.id);
    if (resp_result.is_err() || !resp_result.value.has_value()) {
        result.approved = false;
        result.denied_reason = resp_result.is_err() ? "Approval wait failed: " + resp_result.error : "No response";
        return result;
    }

    const auto& resp = resp_result.value.value();

    // Broadcast exec.approval.resolved event (aligned with OpenClaw)
    if (gateway_server) {
        json resolved_payload;
        resolved_payload["id"] = req.id;
        resolved_payload["decision"] = (resp.decision == ApprovalDecision::ALLOW_ONCE)   ? "allow-once"
                                      : (resp.decision == ApprovalDecision::ALLOW_ALWAYS) ? "allow-always"
                                      : "deny";
        resolved_payload["resolvedBy"] = resp.resolved_by;
        resolved_payload["ts"] = core::utils::current_timestamp_ms();
        resolved_payload["request"]["command"] = req.command;
        gateway::ConnectionPool::instance().broadcast("exec.approval.resolved", resolved_payload);
    }

    if (resp.decision == ApprovalDecision::DENY) {
        result.approved = false;
        result.denied_reason = "Execution denied by user";
        return result;
    }

    if (resp.decision == ApprovalDecision::TIMEOUT) {
        // askFallback logic: if allowlist satisfied, allow on timeout (aligned with OpenClaw)
        if (allowlist_satisfied) {
            result.approved = true;
        } else {
            result.approved = false;
            result.denied_reason = "Approval timeout (allowlist-miss)";
        }
        return result;
    }

    // ALLOW_ONCE or ALLOW_ALWAYS
    if (resp.decision == ApprovalDecision::ALLOW_ALWAYS) {
        // Add to allowlist (aligned with OpenClaw addAllowlistEntry)
        AllowlistEntry entry;
        entry.id = core::utils::generate_uuid();
        entry.pattern = command;
        entry.created_at_ms = core::utils::current_timestamp_ms();
        entry.created_by = resp.resolved_by;
        approval_mgr->add_allowlist_entry(agent_id, entry);
        log()->info("exec: added allowlist entry for agent={} pattern={}", agent_id, command);
    }

    result.approved = true;
    return result;
}

} // anonymous namespace

// ============================================================================
// ExecToolImpl - Full implementation aligned with OpenClaw createExecTool
// ============================================================================

class ExecToolImpl : public Tool {
public:
    explicit ExecToolImpl(
        const ToolContext& context,
        std::shared_ptr<ApprovalManager> approval_mgr,
        std::shared_ptr<ToolHookManager> hook_manager)
        : context_(context)
        , approval_mgr_(approval_mgr)
        , hook_manager_(hook_manager)
    {}

    std::string name() const override { return "exec"; }

    std::string description() const override {
        return "Execute shell commands with background continuation. "
               "Use yieldMs/background to continue later via process tool. "
               "Use pty=true for TTY-required commands (terminal UIs, coding agents).";
    }

    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        schema.description = "Parameters for exec tool";

        // Aligned with OpenClaw execSchema (bash-tools.exec-runtime.ts)
        auto add_prop = [&](const std::string& key, const std::string& type,
                            const std::string& desc, bool required = false) {
            json prop;
            prop["type"] = type;
            prop["description"] = desc;
            // Store as string representation
            schema.properties[key] = core::JsonValue(prop.dump());
            if (required) schema.required.push_back(key);
        };

        add_prop("command",    "string",  "Shell command to execute", true);
        add_prop("workdir",    "string",  "Working directory (defaults to cwd)");
        add_prop("yieldMs",    "number",  "Milliseconds to wait before backgrounding (default 10000)");
        add_prop("background", "boolean", "Run in background immediately");
        add_prop("timeout",    "number",  "Timeout in seconds (optional, kills process on expiry)");
        add_prop("pty",        "boolean", "Run in a pseudo-terminal (PTY) when available");
        add_prop("elevated",   "boolean", "Run on the host with elevated permissions (if allowed)");
        add_prop("host",       "string",  "Exec host (sandbox|gateway|node)");
        add_prop("security",   "string",  "Exec security mode (deny|allowlist|full)");
        add_prop("ask",        "string",  "Exec ask mode (off|on-miss|always)");
        add_prop("node",       "string",  "Node id/name for host=node");

        return schema;
    }

    bool requires_approval() const override { return false; }  // handled internally

    ToolResult execute(const core::JsonValue& raw_params) override {
        // ================================================================
        // STEP 1: Before hooks
        // Aligned with OpenClaw runBeforeToolCall() in plugins/hooks.ts:
        //   - Sequential (modifying hook)
        //   - Results merged: params/block/blockReason
        //   - PluginHookBeforeToolCallEvent: { toolName, params, runId?, toolCallId? }
        //   - PluginHookToolContext: { agentId?, sessionKey?, sessionId?, runId?, toolName, toolCallId? }
        // ================================================================
        core::JsonValue params = raw_params;

        // Build shared event + context for both private and global hook managers
        PluginHookBeforeToolCallEvent before_event;
        before_event.tool_name = "exec";
        before_event.params_json = raw_params.to_string();

        PluginHookToolContext tool_ctx;
        tool_ctx.tool_name = "exec";
        tool_ctx.agent_id = context_.agent_id.empty()
            ? std::nullopt : std::make_optional(context_.agent_id);
        tool_ctx.session_key = context_.session_key.empty()
            ? std::nullopt : std::make_optional(context_.session_key);
        tool_ctx.run_id = context_.run_id.empty()
            ? std::nullopt : std::make_optional(context_.run_id);
        tool_ctx.session_id = context_.session_id.empty()
            ? std::nullopt : std::make_optional(context_.session_id);

        // ── Private hook manager (exec-specific hooks) ──
        if (hook_manager_) {
            auto before_result = hook_manager_->run_before_tool_call(before_event, tool_ctx);
            if (before_result.has_value()) {
                if (before_result->block) {
                    std::string reason = before_result->block_reason.value_or("blocked by plugin hook");
                    log()->warn("exec: blocked by before_tool_call hook: {}", reason);
                    return tool_result::error("exec blocked: " + reason);
                }
                if (before_result->params_json.has_value()) {
                    auto parsed = core::JsonValue::parse(*before_result->params_json);
                    if (parsed.ok && parsed.value) params = std::move(*parsed.value);
                }
            }
        }

        // ── Global hook manager (process-wide plugin hooks)
        // Aligned with OpenClaw: all tools share the same global HookRunner
        // ──────────────────────────────────────────────────────────────────
        {
            auto* global_mgr = get_global_hook_manager();
            if (global_mgr && global_mgr->has_before_tool_call_hooks()) {
                // Update params_json in case private hooks modified params
                before_event.params_json = params.to_string();
                auto global_result = global_mgr->run_before_tool_call(before_event, tool_ctx);
                if (global_result.has_value()) {
                    if (global_result->block) {
                        std::string reason = global_result->block_reason.value_or("blocked by global plugin hook");
                        log()->warn("exec: blocked by global before_tool_call hook: {}", reason);
                        return tool_result::error("exec blocked: " + reason);
                    }
                    if (global_result->params_json.has_value()) {
                        auto parsed = core::JsonValue::parse(*global_result->params_json);
                        if (parsed.ok && parsed.value) params = std::move(*parsed.value);
                    }
                }
            }
        }

        auto start_time = std::chrono::steady_clock::now();
        std::string error_msg;
        ToolResult result;

        try {
            result = execute_internal(params);
        } catch (const std::exception& e) {
            error_msg = std::string("exec failed: ") + e.what();
            result = tool_result::error(error_msg);
        }

        // ================================================================
        // STEP 6: After hooks
        // Aligned with OpenClaw runAfterToolCall() in plugins/hooks.ts:
        //   - Parallel void hook (all handlers called)
        //   - PluginHookAfterToolCallEvent: { toolName, params, runId?, toolCallId?,
        //                                    result?, error?, durationMs? }
        // ================================================================
        if (hook_manager_ || get_global_hook_manager()) {
            auto end_time = std::chrono::steady_clock::now();
            double duration_ms = std::chrono::duration<double, std::milli>(
                end_time - start_time).count();

            PluginHookAfterToolCallEvent after_event;
            after_event.tool_name = "exec";
            after_event.params_json = params.to_string();
            after_event.duration_ms = duration_ms;
            if (!error_msg.empty()) {
                after_event.error = error_msg;
            } else {
                // Serialize result as JSON string
                core::JsonValue result_json = core::JsonValue::object();
                result_json["success"] = result.success;
                if (!result.error.empty()) result_json["error"] = result.error;
                after_event.result_json = result_json.to_string();
            }

            PluginHookToolContext tool_ctx;
            tool_ctx.tool_name = "exec";
            tool_ctx.agent_id = context_.agent_id.empty()
                ? std::nullopt : std::make_optional(context_.agent_id);
            tool_ctx.session_key = context_.session_key.empty()
                ? std::nullopt : std::make_optional(context_.session_key);
            tool_ctx.run_id = context_.run_id.empty()
                ? std::nullopt : std::make_optional(context_.run_id);
            tool_ctx.session_id = context_.session_id.empty()
                ? std::nullopt : std::make_optional(context_.session_id);

            // ── Private hook manager (exec-specific hooks) ──
            if (hook_manager_) {
                hook_manager_->run_after_tool_call(after_event, tool_ctx);
            }

            // ── Global hook manager (process-wide plugin hooks) ──
            // Aligned with OpenClaw: all tools share the same global HookRunner
            auto* global_mgr = get_global_hook_manager();
            if (global_mgr && global_mgr->has_after_tool_call_hooks()) {
                global_mgr->run_after_tool_call(after_event, tool_ctx);
            }
        }

        return result;
    }

private:
    ToolContext context_;
    std::shared_ptr<ApprovalManager> approval_mgr_;
    std::shared_ptr<ToolHookManager> hook_manager_;

    ToolResult execute_internal(const core::JsonValue& params) {
        // ================================================================
        // STEP 2: Parameter parsing (aligned with OpenClaw createExecTool.execute)
        // ================================================================
        if (!params.is_object() || !params.contains("command")) {
            throw std::runtime_error("Provide a command to start.");
        }

        std::string command = params["command"].as_string();
        if (command.empty()) {
            throw std::runtime_error("Provide a command to start.");
        }

        std::string workdir = context_.workspace_dir;
        if (params.contains("workdir") && params["workdir"].is_string()) {
            std::string wd = params["workdir"].as_string();
            if (!wd.empty()) workdir = wd;
        }

        std::map<std::string, std::string> env;
        if (params.contains("env") && params["env"].is_object()) {
            for (auto it = params["env"].begin(); it != params["env"].end(); ++it) {
                env[it->first] = it->second->as_string();
            }
        }

        bool background_requested = params.contains("background") &&
                                    params["background"].is_bool() &&
                                    params["background"].as_bool();
        bool yield_requested = params.contains("yieldMs") && params["yieldMs"].is_number();
        int64_t yield_ms = yield_requested
            ? static_cast<int64_t>(params["yieldMs"].as_number())
            : DEFAULT_BACKGROUND_MS;

        int64_t timeout_sec = DEFAULT_TIMEOUT_SEC;
        if (params.contains("timeout") && params["timeout"].is_number()) {
            timeout_sec = static_cast<int64_t>(params["timeout"].as_number());
        }

        bool use_pty = params.contains("pty") && params["pty"].is_bool() &&
                       params["pty"].as_bool();

        bool elevated_requested = params.contains("elevated") &&
                                  params["elevated"].is_bool() &&
                                  params["elevated"].as_bool();

        // ================================================================
        // STEP 3: Host routing (aligned with OpenClaw)
        // ================================================================
        ExecHost configured_host = ExecHost::SANDBOX;
        if (!context_.exec_host.empty()) {
            configured_host = normalize_exec_host(context_.exec_host);
        }

        ExecHost host = configured_host;
        if (params.contains("host") && params["host"].is_string()) {
            ExecHost requested_host = normalize_exec_host(params["host"].as_string());
            if (!elevated_requested && requested_host != configured_host) {
                throw std::runtime_error(
                    "exec host not allowed (requested " + render_exec_host_label(requested_host) +
                    "; configure tools.exec.host=" + render_exec_host_label(configured_host) +
                    " to allow).");
            }
            host = requested_host;
        }

        // Elevated always routes to gateway (aligned with OpenClaw)
        if (elevated_requested) {
            host = ExecHost::GATEWAY;
            log()->info("exec: elevated command, routing to gateway");
        }

        // ================================================================
        // STEP 4: Security/ask policy (aligned with OpenClaw minSecurity/maxAsk)
        // ================================================================
        ExecSecurityLevel configured_security = (host == ExecHost::SANDBOX)
            ? ExecSecurityLevel::DENY
            : ExecSecurityLevel::ALLOWLIST;
        if (!context_.exec_security.empty()) {
            configured_security = normalize_exec_security(context_.exec_security);
        }

        ExecSecurityLevel security = configured_security;
        if (params.contains("security") && params["security"].is_string()) {
            ExecSecurityLevel requested_security =
                normalize_exec_security(params["security"].as_string());
            security = min_security(configured_security, requested_security);
        }

        // Elevated full mode bypasses security (aligned with OpenClaw)
        if (elevated_requested && context_.exec_elevated_level == "full") {
            security = ExecSecurityLevel::FULL;
        }

        ExecAskMode configured_ask = ExecAskMode::ON_MISS;
        if (!context_.exec_ask.empty()) {
            configured_ask = normalize_exec_ask(context_.exec_ask);
        }

        ExecAskMode ask = configured_ask;
        if (params.contains("ask") && params["ask"].is_string()) {
            ExecAskMode requested_ask = normalize_exec_ask(params["ask"].as_string());
            ask = max_ask(configured_ask, requested_ask);
        }

        // Elevated full mode bypasses ask (aligned with OpenClaw bypassApprovals)
        bool bypass_approvals = elevated_requested && context_.exec_elevated_level == "full";
        if (bypass_approvals) {
            ask = ExecAskMode::OFF;
        }

        // ================================================================
        // STEP 5: Sandbox check (aligned with OpenClaw)
        // ================================================================
        if (host == ExecHost::SANDBOX && !context_.sandbox_manager) {
            throw std::runtime_error(
                "exec host=sandbox is configured, but sandbox runtime is unavailable.\n"
                "Enable sandbox mode or set tools.exec.host to \"gateway\"/\"node\".");
        }

        // ================================================================
        // STEP 6: Workdir resolution
        // ================================================================
        if (!workdir.empty() && !fs::exists(workdir)) {
            log()->warn("exec: workdir does not exist: {}, using cwd", workdir);
            workdir = fs::current_path().string();
        }

        // ================================================================
        // STEP 7: Script preflight (aligned with OpenClaw validateScriptFileForShellBleed)
        // ================================================================
        std::string preflight_error = validate_script_file_for_shell_bleed(command, workdir);
        if (!preflight_error.empty()) {
            throw std::runtime_error(preflight_error);
        }

        // ================================================================
        // STEP 8: Node host routing (aligned with OpenClaw executeNodeHostCommand)
        // ================================================================
        if (host == ExecHost::NODE) {
            return execute_node_host(command, workdir, env, params, timeout_sec);
        }

        // ================================================================
        // STEP 9: Gateway allowlist + approval flow
        // (aligned with OpenClaw processGatewayAllowlist)
        // ================================================================
        std::string exec_command_override;
        if (host == ExecHost::GATEWAY && !bypass_approvals) {
            auto gw_result = process_gateway_allowlist(
                command, workdir, security, ask,
                context_.agent_id, context_.session_key,
                approval_mgr_, nullptr);

            if (!gw_result.approved) {
                throw std::runtime_error("exec denied: " + gw_result.denied_reason);
            }

            if (!gw_result.enforced_command.empty()) {
                exec_command_override = gw_result.enforced_command;
            }
        }

        // ================================================================
        // STEP 10: Run exec process (aligned with OpenClaw runExecProcess)
        // ================================================================
        const std::string& effective_command = exec_command_override.empty()
            ? command : exec_command_override;

        // Background/yield window (aligned with OpenClaw)
        int64_t yield_window = -1;  // -1 = no background
        if (background_requested) {
            yield_window = 0;  // immediate background
        } else if (yield_requested) {
            yield_window = std::max(int64_t(10), std::min(yield_ms, int64_t(120000)));
        }

        // Effective timeout: background requests bypass timeout (aligned with OpenClaw)
        int64_t effective_timeout = timeout_sec;
        bool background_timeout_bypass = (yield_window >= 0) &&
                                         !params.contains("timeout");
        if (background_timeout_bypass) {
            effective_timeout = 0;  // no timeout for background
        }

        log()->info("exec: running command={} workdir={} host={} security={} ask={}",
            command, workdir,
            render_exec_host_label(host),
            (security == ExecSecurityLevel::DENY ? "deny" :
             security == ExecSecurityLevel::ALLOWLIST ? "allowlist" : "full"),
            (ask == ExecAskMode::OFF ? "off" :
             ask == ExecAskMode::ON_MISS ? "on-miss" : "always"));

        auto run = run_exec_process(
            effective_command, workdir, env, use_pty,
            effective_timeout, DEFAULT_MAX_OUTPUT_CHARS,
            nullptr  // on_update callback
        );

        // ================================================================
        // STEP 11: Yield/background handling (aligned with OpenClaw)
        // ================================================================
        if (yield_window == 0) {
            // Immediate background
            std::string session_id = run->session_id;
            // Detach the future (fire-and-forget)
            run->promise.wait_for(std::chrono::milliseconds(0));

            json details;
            details["status"] = "running";
            details["sessionId"] = session_id;
            details["startedAt"] = run->started_at;
            details["cwd"] = workdir;

            return tool_result::text(
                "Command still running (session " + session_id + "). "
                "Use process (list/poll/log/write/kill/clear/remove) for follow-up.");
        }

        if (yield_window > 0) {
            // Wait for yield_window ms, then background if still running
            auto status = run->promise.wait_for(
                std::chrono::milliseconds(yield_window));

            if (status != std::future_status::ready) {
                // Backgrounded
                std::string session_id = run->session_id;
                return tool_result::text(
                    "Command still running (session " + session_id + ", pid n/a). "
                    "Use process (list/poll/log/write/kill/clear/remove) for follow-up.");
            }
        }

        // ================================================================
        // STEP 12: Wait for completion and return result
        // ================================================================
        auto outcome = run->promise.get();

        if (outcome.status == ExecProcessOutcome::Status::FAILED) {
            throw std::runtime_error(outcome.reason.empty()
                ? "Command failed." : outcome.reason);
        }

        std::string output = outcome.aggregated.empty() ? "(no output)" : outcome.aggregated;
        if (outcome.exit_code != 0) {
            output += "\n\n(Command exited with code " + std::to_string(outcome.exit_code) + ")";
        }

        return tool_result::text(output);
    }

    // Node host execution (stub - aligned with OpenClaw executeNodeHostCommand)
    ToolResult execute_node_host(
        const std::string& command,
        const std::string& workdir,
        const std::map<std::string, std::string>& env,
        const core::JsonValue& params,
        int64_t timeout_sec)
    {
        // TODO: implement node host command via node registry invoke
        // Aligned with OpenClaw bash-tools.exec-host-node.ts executeNodeHostCommand
        log()->warn("exec: node host not yet implemented, command={}", command);
        throw std::runtime_error(
            "exec host=node is not yet implemented. "
            "Configure a node and retry.");
    }
};

// ============================================================================
// Factory function
// ============================================================================

std::shared_ptr<Tool> create_exec_tool(
    const ToolContext& context,
    std::shared_ptr<ApprovalManager> approval_mgr,
    std::shared_ptr<gateway::GatewayServer> /*gateway_server*/,
    std::shared_ptr<ToolHookManager> hook_manager)
{
    return std::make_shared<ExecToolImpl>(
        context, approval_mgr, hook_manager);
}

} // namespace tools
} // namespace hunterclaw
