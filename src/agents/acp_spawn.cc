// acp_spawn.cc
// Aligned with openclaw:
//   src/agents/acp-spawn.ts          (spawnAcpDirect)
//   src/agents/acp-spawn-parent-stream.ts (startAcpSpawnParentStreamRelay)

#include "hunterclaw/agents/acp_spawn.h"
#include "hunterclaw/acp/acp_session_manager.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/infra/agent_events.h"
#include "hunterclaw/infra/system_events.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include "hunterclaw/infra/session_binding_service.h"
#include "hunterclaw/tasks/detached_task_runtime.h"
#include "hunterclaw/core/types.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

namespace hunterclaw {
namespace agents {

namespace {

auto log() {
    static auto logger = spdlog::get("acp_spawn");
    if (!logger) logger = spdlog::stdout_logger_mt("acp_spawn");
    return logger;
}

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

static std::string compact_whitespace(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    bool last_space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space && !result.empty()) {
                result += ' ';
                last_space = true;
            }
        } else {
            result += c;
            last_space = false;
        }
    }
    // trim trailing space
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

static std::string truncate_str(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars <= 1) return s.substr(0, max_chars);
    // UTF-8 safe truncation (approximate)
    std::string result = s.substr(0, max_chars - 1);
    result += "\xe2\x80\xa6"; // UTF-8 ellipsis '…'
    return result;
}

} // namespace

// ============================================================================
// resolve_acp_spawn_runtime_policy_error
// Aligned with openclaw: resolveAcpSpawnRuntimePolicyError
// ============================================================================

std::optional<std::string> resolve_acp_spawn_runtime_policy_error(
    const AcpSpawnConfig& cfg,
    const std::optional<std::string>& requester_session_key,
    bool requester_sandboxed,
    const std::optional<std::string>& sandbox_mode
) {
    // Aligned with openclaw: sandboxed sessions cannot spawn ACP
    if (requester_sandboxed) {
        return "Sandboxed sessions cannot spawn ACP sessions because runtime=\"acp\" runs on the host. "
               "Use runtime=\"subagent\" from sandboxed sessions.";
    }
    // Aligned with openclaw: sandbox="require" is unsupported for ACP
    if (sandbox_mode.has_value() && *sandbox_mode == "require") {
        return "sessions_spawn sandbox=\"require\" is unsupported for runtime=\"acp\" because ACP sessions "
               "run outside the sandbox. Use runtime=\"subagent\" or sandbox=\"inherit\".";
    }
    return std::nullopt;
}

// ============================================================================
// start_acp_spawn_parent_stream_relay
// Aligned with openclaw: startAcpSpawnParentStreamRelay
// ============================================================================

AcpSpawnParentRelayHandle start_acp_spawn_parent_stream_relay(
    const std::string& run_id,
    const std::string& parent_session_key,
    const std::string& child_session_key,
    const std::string& agent_id,
    const std::optional<std::string>& log_path,
    int stream_flush_ms,
    int no_output_notice_ms,
    int no_output_poll_ms,
    int max_relay_lifetime_ms,
    bool emit_start_notice
) {
    if (run_id.empty() || parent_session_key.empty()) {
        return { [](){}, [](){} };
    }

    // Aligned with openclaw: STREAM_SNIPPET_MAX_CHARS = 220
    static constexpr size_t STREAM_SNIPPET_MAX_CHARS = 220;
    // Aligned with openclaw: STREAM_BUFFER_MAX_CHARS = 4000
    static constexpr size_t STREAM_BUFFER_MAX_CHARS = 4000;

    const std::string relay_label = truncate_str(compact_whitespace(agent_id), 40).empty()
        ? "ACP child" : truncate_str(compact_whitespace(agent_id), 40);
    const std::string context_prefix = "acp-spawn:" + run_id;

    // Shared relay state
    struct RelayState {
        std::atomic<bool> disposed{false};
        std::string pending_text;
        std::mutex text_mutex;
        std::atomic<int64_t> last_progress_at{0};
        std::atomic<bool> stall_notified{false};

        // Log file
        std::optional<std::string> log_path_opt;
        std::mutex log_mutex;
        std::string pending_log_lines;
    };
    auto state = std::make_shared<RelayState>();
    state->last_progress_at.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    if (log_path.has_value() && !log_path->empty()) {
        state->log_path_opt = *log_path;
    }

    // Helper: write log line (best-effort)
    auto write_log_line = [state](const json& entry) {
        if (!state->log_path_opt.has_value()) return;
        try {
            std::lock_guard<std::mutex> lock(state->log_mutex);
            state->pending_log_lines += entry.dump() + "\n";
            if (state->pending_log_lines.size() >= 16384) {
                // flush
                std::filesystem::create_directories(
                    std::filesystem::path(*state->log_path_opt).parent_path()
                );
                std::ofstream f(*state->log_path_opt, std::ios::app);
                f << state->pending_log_lines;
                state->pending_log_lines.clear();
            }
        } catch (...) {}
    };

    // Helper: flush log
    auto flush_log = [state]() {
        if (!state->log_path_opt.has_value()) return;
        try {
            std::lock_guard<std::mutex> lock(state->log_mutex);
            if (state->pending_log_lines.empty()) return;
            std::filesystem::create_directories(
                std::filesystem::path(*state->log_path_opt).parent_path()
            );
            std::ofstream f(*state->log_path_opt, std::ios::app);
            f << state->pending_log_lines;
            state->pending_log_lines.clear();
        } catch (...) {}
    };

    // Helper: wake parent heartbeat
    auto wake = [parent_session_key]() {
        // Aligned with openclaw: requestHeartbeatNow(scopedHeartbeatWakeOptions(parentSessionKey, {reason:"acp:spawn:stream"}))
        infra::request_heartbeat_now("acp:spawn:stream", std::nullopt, parent_session_key);
    };

    // Helper: emit system event to parent
    auto emit = [state, parent_session_key, context_prefix, wake, write_log_line](
        const std::string& text, const std::string& context_key
    ) {
        std::string cleaned = text;
        // trim
        size_t start = cleaned.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return;
        cleaned = cleaned.substr(start);
        size_t end = cleaned.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) cleaned = cleaned.substr(0, end + 1);
        if (cleaned.empty()) return;

        write_log_line({
            {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"kind", "system_event"},
            {"contextKey", context_key},
            {"text", cleaned}
        });

        // Aligned with openclaw: enqueueSystemEvent(cleaned, { sessionKey: parentSessionKey, contextKey })
        infra::enqueue_system_event(cleaned, parent_session_key, context_key);
        wake();
    };

    // Helper: flush pending text
    auto flush_pending = [state, relay_label, context_prefix, emit]() {
        std::string text_to_flush;
        {
            std::lock_guard<std::mutex> lock(state->text_mutex);
            text_to_flush = std::move(state->pending_text);
            state->pending_text.clear();
        }
        if (text_to_flush.empty()) return;
        std::string snippet = truncate_str(compact_whitespace(text_to_flush), STREAM_SNIPPET_MAX_CHARS);
        if (snippet.empty()) return;
        emit(relay_label + ": " + snippet, context_prefix + ":progress");
    };

    // Emit start notice if requested
    if (emit_start_notice) {
        emit(
            "Started " + relay_label + " session " + child_session_key +
            ". Streaming progress updates to parent session.",
            context_prefix + ":start"
        );
    }

    // Subscribe to agent events
    // Aligned with openclaw: onAgentEvent(event => { if (event.runId !== runId) return; ... })
    auto unsubscribe = infra::on_agent_event([
        state, run_id, relay_label, context_prefix,
        emit, flush_pending, write_log_line,
        no_output_notice_ms
    ](const infra::AgentEvent& event) {
        if (state->disposed.load()) return;
        if (event.run_id != run_id) return;

        if (event.stream == "assistant") {
            // Aligned with openclaw: delta accumulation + flush
            std::string delta = event.delta();
            if (delta.empty()) return;

            write_log_line({
                {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()},
                {"kind", "assistant_delta"},
                {"delta", delta}
            });

            // Reset stall notifier
            if (state->stall_notified.exchange(false)) {
                emit(relay_label + " resumed output.", context_prefix + ":resumed");
            }

            state->last_progress_at.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );

            {
                std::lock_guard<std::mutex> lock(state->text_mutex);
                state->pending_text += delta;
                if (state->pending_text.size() > STREAM_BUFFER_MAX_CHARS) {
                    state->pending_text = state->pending_text.substr(
                        state->pending_text.size() - STREAM_BUFFER_MAX_CHARS
                    );
                }
                // Flush immediately if buffer is large or contains double newline
                bool should_flush_now =
                    state->pending_text.size() >= STREAM_SNIPPET_MAX_CHARS ||
                    delta.find("\n\n") != std::string::npos;
                if (!should_flush_now) return;
            }
            flush_pending();
            return;
        }

        if (event.stream != "lifecycle") return;

        std::string phase = event.phase();
        write_log_line({
            {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()},
            {"kind", "lifecycle"},
            {"phase", phase.empty() ? "unknown" : phase},
            {"data", event.data}
        });

        if (phase == "end") {
            flush_pending();
            // Aligned with openclaw: emit "run completed in Xs"
            int64_t started_at = 0, ended_at = 0;
            if (event.data.contains("startedAt") && event.data["startedAt"].is_number())
                started_at = event.data["startedAt"].get<int64_t>();
            if (event.data.contains("endedAt") && event.data["endedAt"].is_number())
                ended_at = event.data["endedAt"].get<int64_t>();
            int64_t duration_ms = (started_at > 0 && ended_at >= started_at)
                ? (ended_at - started_at) : 0;
            if (duration_ms > 0) {
                int64_t secs = std::max((int64_t)1, duration_ms / 1000);
                emit(relay_label + " run completed in " + std::to_string(secs) + "s.",
                     context_prefix + ":done");
            } else {
                emit(relay_label + " run completed.", context_prefix + ":done");
            }
            state->disposed.store(true);
            return;
        }

        if (phase == "error") {
            flush_pending();
            std::string error_text;
            if (event.data.contains("error") && event.data["error"].is_string())
                error_text = event.data["error"].get<std::string>();
            // trim
            size_t s = error_text.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) error_text = error_text.substr(s);
            size_t e = error_text.find_last_not_of(" \t\r\n");
            if (e != std::string::npos) error_text = error_text.substr(0, e + 1);

            if (!error_text.empty()) {
                emit(relay_label + " run failed: " + error_text, context_prefix + ":error");
            } else {
                emit(relay_label + " run failed.", context_prefix + ":error");
            }
            state->disposed.store(true);
        }
    });

    // Stall watcher thread (aligned with openclaw: noOutputWatcherTimer)
    // Aligned with openclaw: every noOutputPollMs, check if no output for noOutputNoticeMs
    auto stall_thread = std::make_shared<std::thread>([
        state, relay_label, context_prefix, emit,
        no_output_notice_ms, no_output_poll_ms, max_relay_lifetime_ms
    ]() {
        int64_t relay_start = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        while (!state->disposed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(no_output_poll_ms));
            if (state->disposed.load()) break;

            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            // Aligned with openclaw: maxRelayLifetimeMs timeout
            if (max_relay_lifetime_ms > 0 && (now - relay_start) >= max_relay_lifetime_ms) {
                int64_t secs = std::max((int64_t)1, (int64_t)max_relay_lifetime_ms / 1000);
                emit(relay_label + " stream relay timed out after " +
                     std::to_string(secs) + "s without completion.",
                     context_prefix + ":timeout");
                state->disposed.store(true);
                break;
            }

            // Aligned with openclaw: noOutputNoticeMs stall detection
            if (no_output_notice_ms > 0 && !state->stall_notified.load()) {
                int64_t last = state->last_progress_at.load();
                if ((now - last) >= no_output_notice_ms) {
                    state->stall_notified.store(true);
                    int64_t secs = std::max((int64_t)1, (int64_t)no_output_notice_ms / 1000);
                    emit(relay_label + " has produced no output for " +
                         std::to_string(secs) + "s. It may be waiting for interactive input.",
                         context_prefix + ":stall");
                }
            }
        }
    });
    stall_thread->detach();

    auto dispose_fn = [state, unsubscribe, flush_log]() mutable {
        if (state->disposed.exchange(true)) return;
        unsubscribe();
        flush_log();
    };

    auto notify_started_fn = [state, relay_label, context_prefix, child_session_key, emit]() {
        if (state->disposed.load()) return;
        emit(
            "Started " + relay_label + " session " + child_session_key +
            ". Streaming progress updates to parent session.",
            context_prefix + ":start"
        );
    };

    return { dispose_fn, notify_started_fn };
}

// ============================================================================
// cleanup_failed_acp_spawn
// Aligned with openclaw: cleanupFailedAcpSpawn (control-plane/spawn.ts)
// ============================================================================

void cleanup_failed_acp_spawn(
    const std::string& session_key,
    bool should_delete_session,
    bool delete_transcript,
    std::shared_ptr<gateway::GatewayClient> gw,
    const AcpSpawnRuntimeCloseHandle* runtime_close_handle
) {
    // Step 1: Close the ACP runtime handle via AcpSessionManager
    // Aligned with openclaw: acpManager.closeSession({ reason: "spawn-failed", allowBackendUnavailable: true, requireAcpSession: false })
    try {
        acp::get_acp_session_manager().close_session(
            session_key,
            "spawn-failed",
            /*clear_meta=*/false,
            /*allow_backend_unavailable=*/true,
            /*require_acp_session=*/false
        );
    } catch (...) {
        log()->debug("acp-spawn: manager cleanup close failed for {}", session_key);
    }

    // Step 2: Unbind any thread binding (best-effort)
    // Aligned with openclaw: getSessionBindingService().unbind({ targetSessionKey, reason: "spawn-failed" })
    try {
        infra::get_session_binding_service().unbind(session_key, "spawn-failed");
    } catch (...) {
        log()->debug("acp-spawn: binding cleanup unbind failed for {}", session_key);
    }

    // Step 3: Delete the Gateway session record (best-effort)
    // Aligned with openclaw: callGateway("sessions.delete", { key, deleteTranscript, emitLifecycleHooks: false })
    if (!should_delete_session || !gw) return;
    try {
        gw->request("sessions.delete", json{
            {"key", session_key},
            {"deleteTranscript", delete_transcript},
            {"emitLifecycleHooks", false}
        });
    } catch (...) {
        // Best-effort cleanup only
    }
}

// ============================================================================
// initialize_acp_spawn_runtime (internal helper)
// Aligned with openclaw: initializeAcpSpawnRuntime (agents/acp-spawn.ts)
// ============================================================================

struct AcpSpawnInitializedRuntime {
    acp::AcpSessionMeta meta;
    AcpSpawnRuntimeCloseHandle close_handle;
};

static std::optional<AcpSpawnInitializedRuntime> initialize_acp_spawn_runtime(
    const std::string& session_key,
    const std::string& target_agent_id,
    acp::AcpRuntimeSessionMode runtime_mode,
    const std::optional<std::string>& cwd,
    const std::optional<std::string>& backend_id,
    const std::optional<std::string>& resume_session_id,
    std::string& out_error
) {
    try {
        // Aligned with openclaw: getAcpSessionManager().initializeSession({ sessionKey, agent, mode, resumeSessionId, cwd, backendId })
        auto meta = acp::get_acp_session_manager().initialize_session(
            session_key,
            target_agent_id,
            runtime_mode,
            cwd,
            backend_id,
            resume_session_id
        );

        AcpSpawnRuntimeCloseHandle close_handle;
        close_handle.session_key = session_key;
        close_handle.backend = meta.backend;
        close_handle.runtime_session_name = meta.runtime_session_name;

        return AcpSpawnInitializedRuntime{ std::move(meta), std::move(close_handle) };
    } catch (const acp::AcpRuntimeError& e) {
        out_error = e.what();
        return std::nullopt;
    } catch (const std::exception& e) {
        out_error = e.what();
        return std::nullopt;
    }
}

// ============================================================================
// spawn_acp_direct
// Aligned with openclaw: spawnAcpDirect
// ============================================================================

SpawnAcpResult spawn_acp_direct(
    const SpawnAcpParams& params,
    const SpawnAcpContext& ctx,
    std::shared_ptr<gateway::GatewayClient> gw,
    const AcpSpawnConfig& cfg
) {
    // ✅ 对齐 OC createAcpSpawnFailure: 统一的错误返回，带 error_code
    auto make_failure = [](const std::string& status,
                           const std::string& error_code,
                           const std::string& error,
                           std::optional<std::string> session_key = std::nullopt,
                           std::optional<std::string> mode = std::nullopt) -> SpawnAcpResult {
        SpawnAcpResult r;
        r.status = status;
        r.child_session_key = session_key;
        r.mode = mode;
        r.error = error;
        r.error_code = error_code;
        return r;
    };

    // ── Step 1: Policy checks ─────────────────────────────────────────────
    // Aligned with openclaw: isAcpEnabledByPolicy(cfg)
    // HunterClaw: cfg.enable_acp
    if (!cfg.enable_acp) {
        return make_failure("forbidden", "acp_disabled",
                            "ACP is disabled by policy (`acp.enabled=false`).");
    }

    const bool stream_to_parent_requested = params.stream_to.has_value() && *params.stream_to == "parent";
    const std::string parent_session_key = ctx.agent_session_key.value_or("");

    if (stream_to_parent_requested && parent_session_key.empty()) {
        return make_failure("error", "requester_session_required",
                            "sessions_spawn streamTo=\"parent\" requires an active requester session context.");
    }

    // Aligned with openclaw: resolveAcpSpawnRuntimePolicyError
    auto policy_error = resolve_acp_spawn_runtime_policy_error(
        cfg,
        ctx.agent_session_key,
        ctx.sandboxed,
        params.sandbox
    );
    if (policy_error.has_value()) {
        return make_failure("forbidden", "runtime_policy", *policy_error);
    }

    // ── Step 2: Resolve spawn mode ────────────────────────────────────────
    // Aligned with openclaw: resolveSpawnMode
    const bool request_thread_binding = params.thread;
    std::string spawn_mode;
    if (params.mode.has_value() && (*params.mode == "run" || *params.mode == "session")) {
        spawn_mode = *params.mode;
    } else {
        spawn_mode = request_thread_binding ? "session" : "run";
    }

    if (spawn_mode == "session" && !request_thread_binding) {
        return make_failure("error", "thread_required",
                            "mode=\"session\" requires thread=true so the ACP session can stay bound to a thread.");
    }

    // ── Step 3: Resolve target agent ID ──────────────────────────────────
    // Aligned with openclaw: resolveTargetAcpAgentId
    std::string target_agent_id;
    if (params.agent_id.has_value() && !params.agent_id->empty()) {
        target_agent_id = *params.agent_id;
    } else if (!cfg.acp_default_agent.empty()) {
        target_agent_id = cfg.acp_default_agent;
    } else {
        // Fallback: derive from requester session key
        // e.g. "agent:main:subagent:xxx" → "main"
        if (!parent_session_key.empty()) {
            // parse "agent:{agentId}:..."
            const std::string prefix = "agent:";
            if (parent_session_key.rfind(prefix, 0) == 0) {
                std::string rest = parent_session_key.substr(prefix.size());
                size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    target_agent_id = rest.substr(0, colon);
                } else {
                    target_agent_id = rest;
                }
            }
        }
        if (target_agent_id.empty()) {
            return make_failure("error", "target_agent_required",
                "ACP target agent is not configured. Pass `agentId` in `sessions_spawn` or set `acp.defaultAgent` in config.");
        }
    }

    // Aligned with openclaw: resolveAcpAgentPolicyError (allowedAgents whitelist)
    if (!cfg.acp_allowed_agents.empty()) {
        bool allowed = false;
        for (const auto& a : cfg.acp_allowed_agents) {
            if (a == target_agent_id) { allowed = true; break; }
        }
        if (!allowed) {
            return make_failure("forbidden", "agent_forbidden",
                "ACP agent \"" + target_agent_id + "\" is not in the allowed agents list.");
        }
    }

    // ── Step 4: Resolve requester internal key ────────────────────────────
    // Aligned with openclaw: resolveRequesterInternalSessionKey
    std::string requester_internal_key = parent_session_key;
    if (requester_internal_key.empty()) {
        requester_internal_key = "agent:" + target_agent_id + ":main";
    }

    // ── Step 5: implicitStreamToParent ────────────────────────────────────────
    // Aligned with openclaw: resolveAcpSpawnStreamPlan
    //
    // implicitStreamToParent = !streamToParentRequested
    //   && spawnMode == "run"
    //   && !requestThreadBinding
    //   && requester.isSubagentSession
    //   && !requester.hasActiveSubagentBinding
    //   && !requester.hasThreadContext
    //   && requester.heartbeatEnabled
    //   && requester.heartbeatRelayRouteUsable
    //
    // HunterClaw: 通过 infra::resolve_acp_spawn_stream_plan() 完成，
    // 该函数封装了 heartbeat config 检查逻辑（对齐 OC isHeartbeatEnabledForSessionAgent
    // + hasSessionLocalHeartbeatRelayRoute）。
    bool implicit_stream_to_parent = false;
    if (!stream_to_parent_requested && spawn_mode == "run" && !request_thread_binding
        && !parent_session_key.empty()) {
        // Aligned with openclaw: resolveAcpSpawnStreamPlan
        // Delegate to infra helper which checks heartbeat config
        implicit_stream_to_parent = infra::resolve_implicit_stream_to_parent(
            parent_session_key, cfg
        );
    }
    const bool effective_stream_to_parent = stream_to_parent_requested || implicit_stream_to_parent;
    // ── Step 5b: subagentEnvelopeState check ─────────────────────────────────
    // Aligned with openclaw: resolveAcpSubagentEnvelopeState
    //   → maxSpawnDepth / maxChildrenPerAgent / requireAgentId / allowAgents
    //   → errorCode: "subagent_policy"
    {
        std::string subagent_error;
        bool is_subagent_envelope = infra::is_subagent_envelope_session(
            parent_session_key, cfg
        );
        if (is_subagent_envelope && !parent_session_key.empty()) {
            subagent_error = infra::resolve_acp_subagent_envelope_error(
                parent_session_key,
                target_agent_id,
                params.agent_id.value_or(""),
                cfg
            );
        }
        if (!subagent_error.empty()) {
            return make_failure("forbidden", "subagent_policy", subagent_error);
        }
    }

    // ── Step 6: Generate child session key ───────────────────────────────────
    // Aligned with openclaw: `agent:${targetAgentId}:acp:${crypto.randomUUID()}`
    const std::string session_key = "agent:" + target_agent_id + ":acp:" + generate_uuid();
    // ── Step 6b: Resolve effective Gateway client ─────────────────────────
    // 🎯 对齐 OpenClaw: 真正的跨进程 ACP 路由
    //
    // OpenClaw 设计:
    //   callGateway("agent", params) 通过 WebSocket 连接发到另一个 Gateway 实例。
    //   targetAgentId 决定路由到哪个远端 Gateway（不同进程/机器）。
    //
    // HunterClaw 实现:
    //   - 如果 cfg.acp_gateway_url 非空: 创建真正的 WebSocket GatewayClient 连接到远端
    //   - 否则: 使用传入的 gw（同进程内 method_handler，向后兼容）
    //
    // 注意: 远端 GatewayClient 在 spawn 完成后会被 stop()，因为 ACP 是 fire-and-forget。
    // 父子通信通过 ParentStreamRelay（进程内事件总线）或 subagent announce（跨进程消息）完成。
    std::shared_ptr<gateway::GatewayClient> effective_gw = gw;
    std::shared_ptr<gateway::GatewayClient> remote_gw;  // owned by this spawn call

    if (!cfg.acp_gateway_url.empty()) {
        // 🎯 对齐 OC: 创建真正的 WebSocket GatewayClient 连接到远端 Gateway
        gateway::GatewayClientOptions gw_opts;
        gw_opts.url = cfg.acp_gateway_url;
        gw_opts.client_name = "hunterclaw-acp-spawn";
        gw_opts.client_version = "1.0.0";
        gw_opts.mode = "headless";
        gw_opts.role = "operator";
        gw_opts.scopes = {"admin"};
        if (!cfg.acp_gateway_token.empty()) {
            gw_opts.token = cfg.acp_gateway_token;
        }
        if (!cfg.acp_gateway_instance_id.empty()) {
            gw_opts.instance_id = cfg.acp_gateway_instance_id;
        }
        // 短超时的 tick watch（spawn 完成后立即 stop）
        gw_opts.tick_watch_min_interval_ms = 60000;

        auto gw_result = gateway::create_gateway_client(gw_opts);
        if (!gw_result.is_ok()) {
            return make_failure("error", "spawn_failed",
                "Failed to create remote gateway client: " + gw_result.error,
                session_key, spawn_mode);
        }
        remote_gw = gw_result.get();
        remote_gw->start();

        // Wait for connection (up to 10s)
        // 🎯 对齐 OC: GatewayClient 连接是异步的，需要等待 hello.ok
        constexpr int CONNECT_TIMEOUT_MS = 10000;
        constexpr int POLL_INTERVAL_MS = 50;
        int waited_ms = 0;
        while (!remote_gw->is_connected() && waited_ms < CONNECT_TIMEOUT_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            waited_ms += POLL_INTERVAL_MS;
        }
        if (!remote_gw->is_connected()) {
            remote_gw->stop();
            return make_failure("error", "spawn_failed",
                "Timeout connecting to remote gateway: " + cfg.acp_gateway_url,
                session_key, spawn_mode);
        }

        effective_gw = remote_gw;
        log()->info("spawn_acp_direct: using remote gateway {} for agent {}",
            cfg.acp_gateway_url, target_agent_id);
    }

    if (!effective_gw) {
        return make_failure("error", "spawn_failed", "Gateway not available", session_key, spawn_mode);
    }

    // ── Step 6b: Resolve runtime cwd (with filesystem validation) ───────────
    // Aligned with openclaw: resolveSpawnedWorkspaceInheritance + resolveRuntimeCwdForAcpSpawn
    //   → if cwd path does not exist (ENOENT), silently drop it (return undefined)
    //   → if other error, return errorCode: "cwd_resolution_failed"
    std::optional<std::string> runtime_cwd;
    {
        // resolveSpawnedWorkspaceInheritance: inherit cwd from parent session if not explicit
        std::optional<std::string> resolved_cwd = infra::resolve_spawned_workspace_inheritance(
            target_agent_id,
            parent_session_key,
            params.cwd,
            cfg
        );
        if (resolved_cwd.has_value() && !resolved_cwd->empty()) {
            // resolveRuntimeCwdForAcpSpawn: validate path exists
            // If explicit cwd was given, trust it directly.
            // If inherited, check existence; ENOENT → silently drop.
            if (params.cwd.has_value() && !params.cwd->empty()) {
                // Explicit cwd: use as-is (aligned with OC: explicitCwd present → return resolvedCwd)
                runtime_cwd = resolved_cwd;
            } else {
                // Inherited cwd: validate existence
                std::error_code ec;
                bool exists = std::filesystem::exists(*resolved_cwd, ec);
                if (ec) {
                    // Non-ENOENT error → cwd_resolution_failed
                    if (remote_gw) remote_gw->stop();
                    return make_failure("error", "cwd_resolution_failed",
                        "Failed to resolve cwd: " + ec.message());
                }
                if (exists) {
                    runtime_cwd = resolved_cwd;
                }
                // else: ENOENT → silently drop (aligned with OC: return undefined)
            }
        }
    }

    // ── Step 7: Create Gateway session (sessions.patch) ──────────────────
    // 🎯 对齐 OpenClaw: callGateway("sessions.patch", { key, spawnedBy, ...subagentEnvelopePatch, label })
    // 使用 effective_gw（可能是远端 WebSocket 客户端）
    bool session_created = false;
    {
        json patch = {
            {"key", session_key},
            {"spawnedBy", requester_internal_key}
        };
        if (params.label.has_value() && !params.label->empty()) {
            patch["label"] = *params.label;
        }
        // Aligned with openclaw: subagentEnvelopeState.childSessionPatch
        // (spawnDepth, subagentRole, subagentControlScope)
        auto child_patch = infra::resolve_subagent_child_session_patch(
            parent_session_key, cfg
        );
        if (child_patch.has_value()) {
            if (child_patch->spawn_depth > 0)
                patch["spawnDepth"] = child_patch->spawn_depth;
            if (!child_patch->subagent_role.empty())
                patch["subagentRole"] = child_patch->subagent_role;
            if (!child_patch->subagent_control_scope.empty())
                patch["subagentControlScope"] = child_patch->subagent_control_scope;
        }
        auto patch_result = effective_gw->request("sessions.patch", patch);
        if (!patch_result.is_ok()) {
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "spawn_failed",
                                patch_result.error, session_key, spawn_mode);
        }
        session_created = true;
    }

    // ── Step 7b: Initialize ACP runtime via AcpSessionManager ────────────
    // 🎯 对齐 OpenClaw: initializeAcpSpawnRuntime()
    //   → getAcpSessionManager().initializeSession({ sessionKey, agent, mode, cwd, backendId })
    //   → 持久化 session meta（acp 字段）到 session store
    //   → 缓存 runtime handle（供后续 runTurn 复用）
    //
    // 这是 HunterClaw 与 OpenClaw 对齐的关键步骤：
    //   - OpenClaw: agents/acp-spawn.ts → initializeAcpSpawnRuntime() → AcpSessionManager.initializeSession()
    //   - HunterClaw: 同样通过 acp::get_acp_session_manager().initialize_session() 完成
    //
    // 注意: AcpSessionManager 需要已通过 AcpSessionManager::set_instance() 初始化。
    // 如果未初始化（singleton 未设置），initialize_acp_spawn_runtime 会捕获异常并返回 nullopt。
    std::optional<AcpSpawnRuntimeCloseHandle> initialized_runtime_handle;
    {
        // Resolve runtime mode: "run" → oneshot, "session" → persistent
        acp::AcpRuntimeSessionMode runtime_mode =
            (spawn_mode == "session")
                ? acp::AcpRuntimeSessionMode::Persistent
                : acp::AcpRuntimeSessionMode::Oneshot;

        std::string init_error;
        auto initialized = initialize_acp_spawn_runtime(
            session_key,
            target_agent_id,
            runtime_mode,
            runtime_cwd,  // validated cwd (may be nullopt if path not found)
            std::nullopt,  // backend_id: use default from AcpSessionManager
            params.resume_session_id,
            init_error
        );

        if (!initialized.has_value()) {
            // Aligned with openclaw: cleanupFailedAcpSpawn on initializeAcpSpawnRuntime failure
            cleanup_failed_acp_spawn(session_key, session_created, true, effective_gw, nullptr);
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "spawn_failed", init_error, session_key, spawn_mode);
        }

        initialized_runtime_handle = std::move(initialized->close_handle);
    }

    // ── Step 8: Start parent stream relay (before dispatch) ──────────────
    // Aligned with openclaw: register relay BEFORE dispatch so fast lifecycle failures are not missed
    const std::string child_idem = generate_uuid();
    std::string child_run_id = child_idem;

    AcpSpawnParentRelayHandle parent_relay;
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        parent_relay = start_acp_spawn_parent_stream_relay(
            child_idem,
            parent_session_key,
            session_key,
            target_agent_id,
            std::nullopt,  // log_path (TODO: resolve from session store)
            2500,          // stream_flush_ms
            60000,         // no_output_notice_ms
            15000,         // no_output_poll_ms
            6 * 60 * 60 * 1000, // max_relay_lifetime_ms
            false          // emit_start_notice (will call notifyStarted after dispatch)
        );
    }

    // ── Step 9: Dispatch agent turn ───────────────────────────────────────
    // Aligned with openclaw: callGateway("agent", { message, sessionKey, idempotencyKey, deliver, label })
    try {
        json agent_params = {
            {"message",        params.task},
            {"sessionKey",     session_key},
            {"idempotencyKey", child_idem},
            {"deliver",        false}
        };
        if (params.label.has_value() && !params.label->empty()) {
            agent_params["label"] = *params.label;
        }
        // Aligned with openclaw: useInlineDelivery = hasDeliveryTarget && spawnMode=="session" && !effectiveStreamToParent
        // For thread-bound sessions, inject delivery context
        if (spawn_mode == "session" && !effective_stream_to_parent &&
            ctx.agent_channel.has_value() && !ctx.agent_channel->empty() &&
            ctx.agent_to.has_value() && !ctx.agent_to->empty()) {
            agent_params["deliver"] = true;
            agent_params["channel"] = *ctx.agent_channel;
            agent_params["to"] = *ctx.agent_to;
            if (ctx.agent_account_id.has_value()) agent_params["accountId"] = *ctx.agent_account_id;
            if (ctx.agent_thread_id.has_value()) agent_params["threadId"] = *ctx.agent_thread_id;
        }

        // 🎯 对齐 OpenClaw: callGateway("agent", params) — 通过 effective_gw 发到目标 Gateway
        // 如果 effective_gw 是远端 WebSocket 客户端，这里就是真正的跨进程 ACP 调用
        auto response = effective_gw->request<json>("agent", agent_params);
        if (!response.is_ok()) {
            if (parent_relay.dispose) parent_relay.dispose();
            // Aligned with openclaw: cleanupFailedAcpSpawn on dispatch failure
            cleanup_failed_acp_spawn(
                session_key, /*should_delete_session=*/true, /*delete_transcript=*/true,
                effective_gw, initialized_runtime_handle ? &*initialized_runtime_handle : nullptr
            );
            if (remote_gw) remote_gw->stop();
            return make_failure("error", "dispatch_failed",
                                response.error, session_key, spawn_mode);
        }
        const auto& resp = response.get();
        if (resp.contains("run_id") && resp["run_id"].is_string()) {
            std::string resp_run_id = resp["run_id"].get<std::string>();
            // trim
            size_t s = resp_run_id.find_first_not_of(" \t");
            if (s != std::string::npos) resp_run_id = resp_run_id.substr(s);
            if (!resp_run_id.empty()) child_run_id = resp_run_id;
        }
        // Also check camelCase runId
        if (child_run_id == child_idem && resp.contains("runId") && resp["runId"].is_string()) {
            std::string resp_run_id = resp["runId"].get<std::string>();
            size_t s = resp_run_id.find_first_not_of(" \t");
            if (s != std::string::npos) resp_run_id = resp_run_id.substr(s);
            if (!resp_run_id.empty()) child_run_id = resp_run_id;
        }
    } catch (const std::exception& e) {
        if (parent_relay.dispose) parent_relay.dispose();
        // Aligned with openclaw: cleanupFailedAcpSpawn on dispatch exception
        cleanup_failed_acp_spawn(
            session_key, /*should_delete_session=*/true, /*delete_transcript=*/true,
            effective_gw, initialized_runtime_handle ? &*initialized_runtime_handle : nullptr
        );
        if (remote_gw) remote_gw->stop();
        return make_failure("error", "dispatch_failed",
                            std::string(e.what()), session_key, spawn_mode);
    }

    // ── Step 10: Finalize relay ───────────────────────────────────────────
    // Aligned with openclaw: if childRunId !== childIdem, restart relay with correct runId
    if (effective_stream_to_parent && !parent_session_key.empty()) {
        if (child_run_id != child_idem) {
            // Restart relay with correct runId
            if (parent_relay.dispose) parent_relay.dispose();
            parent_relay = start_acp_spawn_parent_stream_relay(
                child_run_id,
                parent_session_key,
                session_key,
                target_agent_id,
                std::nullopt,
                2500, 60000, 15000, 6 * 60 * 60 * 1000, false
            );
        }
        // Aligned with openclaw: parentRelay.notifyStarted()
        if (parent_relay.notify_started) parent_relay.notify_started();
    }

    // ── Step 11: Register background task run ────────────────────────────────
    // Aligned with openclaw: createRunningTaskRun({ runtime: "acp", sourceId, ownerKey, ... })
    // Used for sessions_list display and task lifecycle tracking.
    try {
        tasks::create_running_task_run({
            /*runtime=*/"acp",
            /*source_id=*/child_run_id,
            /*owner_key=*/requester_internal_key,
            /*scope_kind=*/"session",
            /*child_session_key=*/session_key,
            /*run_id=*/child_run_id,
            /*label=*/params.label.value_or(""),
            /*task=*/params.task,
            /*delivery_status=*/requester_internal_key.empty() ? "parent_missing" : "pending",
            /*started_at=*/std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
        });
    } catch (...) {
        log()->warn("acp-spawn: failed to create background task for session {}, run {}",
            session_key, child_run_id);
    }

    // ── Step 12: Return result ────────────────────────────────────────────
    // 🎯 对齐 OC: fire-and-forget — spawn 完成后 stop 远端 gw（连接不再需要）
    // 父子通信通过 ParentStreamRelay（进程内事件总线）或 subagent announce（跨进程消息）完成
    if (remote_gw) {
        // 异步 stop，避免阻塞 spawn 返回
        auto gw_to_stop = remote_gw;
        std::thread([gw_to_stop]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gw_to_stop->stop();
        }).detach();
    }

    std::string note = (spawn_mode == "session")
        ? ACP_SPAWN_SESSION_ACCEPTED_NOTE
        : ACP_SPAWN_ACCEPTED_NOTE;

    SpawnAcpResult ok;
    ok.status = "accepted";
    ok.child_session_key = session_key;
    ok.run_id = child_run_id;
    ok.mode = spawn_mode;
    ok.stream_log_path = std::nullopt;
    ok.note = note;
    ok.error = std::nullopt;
    ok.error_code = std::nullopt;
    return ok;
}
} // namespace agents
} // namespace hunterclaw
