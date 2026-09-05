// ============================================================================
// acp_session_manager.cc
// Aligned with openclaw: src/acp/control-plane/manager.core.ts (1291 lines)
// ============================================================================

#include "hunterclaw/acp/acp_session_manager.h"
#include "hunterclaw/acp/acp_background_task.h"
#include "hunterclaw/acp/acp_turn_timeout.h"
#include "hunterclaw/acp/acp_backend_failover.h"
#include "hunterclaw/acp/acp_active_turns.h"
#include <algorithm>

namespace hunterclaw {
namespace acp {

// ── Singleton ────────────────────────────────────────────────────────────────

static std::shared_ptr<AcpSessionManager> g_acp_session_manager;
static std::mutex g_acp_mgr_mutex;

AcpSessionManager& AcpSessionManager::instance() {
    std::lock_guard<std::mutex> lock(g_acp_mgr_mutex);
    if (!g_acp_session_manager) {
        throw std::runtime_error("AcpSessionManager not initialized");
    }
    return *g_acp_session_manager;
}

void AcpSessionManager::set_instance(std::shared_ptr<AcpSessionManager> mgr) {
    std::lock_guard<std::mutex> lock(g_acp_mgr_mutex);
    g_acp_session_manager = std::move(mgr);
}

AcpSessionManager& get_acp_session_manager() {
    return AcpSessionManager::instance();
}

// ── resolveSession ───────────────────────────────────────────────────────────
// Aligned with openclaw: resolveSession (manager.core.ts:89-119)

AcpSessionResolution AcpSessionManager::resolve_session(const std::string& session_key) {
    AcpSessionResolution result;
    result.session_key = session_key;

    auto meta = read_meta_(session_key);
    if (meta) {
        result.kind = AcpSessionResolution::Kind::Ready;
        result.meta = *meta;
        return result;
    }

    // Check if this looks like an ACP session key (agent:xxx:acp:xxx)
    std::string lower = session_key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.find(":acp:") != std::string::npos || lower.substr(0, 4) == "acp:") {
        result.kind = AcpSessionResolution::Kind::Stale;
        result.error = AcpRuntimeError(
            AcpErrorCode::ACP_SESSION_INIT_FAILED,
            "ACP metadata is missing for " + session_key +
            ". Recreate this ACP session with /acp spawn and rebind the thread."
        );
        return result;
    }

    result.kind = AcpSessionResolution::Kind::None;
    return result;
}

// ── initializeSession ────────────────────────────────────────────────────────
// Aligned with openclaw: initializeSession (manager.core.ts:211-320)

AcpSessionMeta AcpSessionManager::initialize_session(
    const std::string& session_key,
    const std::string& agent,
    AcpRuntimeSessionMode mode,
    const std::optional<std::string>& cwd,
    const std::optional<std::string>& backend_id,
    const std::optional<std::string>& resume_session_id
) {
    if (session_key.empty()) {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "ACP session key is required.");
    }

    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> AcpSessionMeta {
        auto backend = AcpRuntimeRegistry::instance().require_backend(backend_id.value_or(""));
        auto runtime = backend.runtime;

        AcpRuntimeEnsureInput ensure_input;
        ensure_input.session_key = session_key;
        ensure_input.agent = agent;
        ensure_input.mode = mode;
        if (resume_session_id) ensure_input.resume_session_id = *resume_session_id;
        if (cwd) ensure_input.cwd = *cwd;

        auto handle = with_acp_runtime_error_boundary(
            [&]() { return runtime->ensure_session(ensure_input); },
            AcpErrorCode::ACP_SESSION_INIT_FAILED,
            "Could not initialize ACP session runtime."
        );

        std::string effective_cwd;
        if (handle.cwd) effective_cwd = *handle.cwd;
        else if (cwd) effective_cwd = *cwd;

        AcpSessionRuntimeOptions initial_opts;
        if (!effective_cwd.empty()) initial_opts.cwd = effective_cwd;

        AcpSessionMeta meta;
        meta.backend = handle.backend.empty() ? backend.id : handle.backend;
        meta.agent = agent;
        meta.runtime_session_name = handle.runtime_session_name;
        meta.mode = mode;
        meta.runtime_options = normalize_runtime_options(initial_opts);
        if (!effective_cwd.empty()) meta.cwd = effective_cwd;
        meta.state = "idle";
        meta.last_activity_at = now_ms();

        // Identity from ensure
        AcpSessionMeta::Identity identity;
        identity.state = "pending";
        identity.source = "ensure";
        identity.last_updated_at = now_ms();
        if (handle.acpx_record_id) identity.acpx_record_id = handle.acpx_record_id;
        if (handle.backend_session_id) identity.acpx_session_id = handle.backend_session_id;
        if (handle.agent_session_id) identity.agent_session_id = handle.agent_session_id;
        if (identity.acpx_session_id || identity.agent_session_id) {
            identity.state = "resolved";
        }
        meta.identity = identity;

        // Persist
        try {
            write_meta_(session_key, meta);
        } catch (...) {
            try { runtime->close(handle, "init-meta-failed"); } catch (...) {}
            throw;
        }

        // Cache
        CachedRuntimeState cached;
        cached.runtime = runtime;
        cached.handle = handle;
        cached.backend = meta.backend;
        cached.agent = agent;
        cached.mode = mode;
        if (!effective_cwd.empty()) cached.cwd = effective_cwd;
        runtime_cache_.set(normalize_actor_key(session_key), cached);

        return meta;
    });
}

// ── runTurn ──────────────────────────────────────────────────────────────────
// Aligned with openclaw: runTurn (manager.core.ts:596-739)

void AcpSessionManager::run_turn(const AcpRunTurnInput& input) {
    if (input.session_key.empty()) {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "ACP session key is required.");
    }

    actor_queue_.run(normalize_actor_key(input.session_key), [&]() {
        int64_t turn_started_at = now_ms();

        // ── Background task mirror ───────────────────────────────────────────
        // Aligned with openclaw: manager.turn-runner.ts:77-89
        // ACP 子会话不进 SubagentRegistry（OC 原注释："ACP children bypass the
        // subagent registry"），父侧只能靠这份 detached task 镜像感知子会话结果。
        // 仅 Prompt 模式镜像（OC: input.mode === "prompt"）。
        std::optional<AcpBackgroundTaskContext> task_context;
        if (input.mode == AcpRuntimePromptMode::Prompt) {
            task_context = resolve_background_task_context(
                input.session_key, input.request_id, input.text);
        }
        if (task_context) {
            create_background_task_record(*task_context, turn_started_at);
        }
        std::string task_progress_summary;

        auto initial_resolution = resolve_session(input.session_key);
        if (initial_resolution.kind != AcpSessionResolution::Kind::Ready) {
            // 对齐 OC recordBackendFailure：ready 校验失败也要写终态，
            // 否则 task 永远停在 running，父侧一直等不到结果。
            if (task_context) {
                MarkBackgroundTaskTerminalParams terminal;
                terminal.status = tasks::TaskStatus::FAILED;
                terminal.ended_at = now_ms();
                terminal.error = initial_resolution.error
                    ? std::string(initial_resolution.error->what())
                    : ("Session is not ACP-enabled: " + input.session_key);
                terminal.progress_summary = std::nullopt;
                mark_background_task_terminal(task_context->run_id, terminal);
            }
            if (initial_resolution.error) throw *initial_resolution.error;
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + input.session_key);
        }

        std::string actor_key = normalize_actor_key(input.session_key);

        // ── Backend candidate plan ───────────────────────────────────────────
        // 对齐 OC: resolveBackendCandidatePlan (manager.turn-runner.ts:109-113)
        // 未配置 fallbacks 时候选只有一项（可能是空串＝registry 自选默认），
        // 循环退化为单次执行，与对齐前行为一致。
        const auto backend_plan = resolve_backend_candidate_plan(
            configured_backend_, initial_resolution.meta->backend, fallback_backends_);
        const auto& candidate_backends = backend_plan.candidate_backends;

        // 对齐 OC: backendAttempts (manager.turn-runner.ts:114)
        std::vector<BackendAttempt> backend_attempts;

        // 对齐 OC: markAcpTurnActive (manager.turn-runner.ts:160-167)
        // 存活标记覆盖整个任务而非单次 attempt：ready 校验通过后标一次，
        // 所有出口（含重试与清理阶段的意外失败）清一次。
        // 用 RAII 而非 finally —— C++ 异常路径必须靠析构保证摘除。
        struct ActiveTurnMarkerGuard {
            std::string key;
            bool armed = false;
            ~ActiveTurnMarkerGuard() {
                if (armed) {
                    try { clear_acp_turn_active(key); } catch (...) {}
                }
            }
        } marker_guard;
        if (task_context) {
            mark_acp_turn_active(input.session_key);
            marker_guard.key = input.session_key;
            marker_guard.armed = true;
        }

        // 对齐 OC recordBackendFailure：把失败统一写进 task 终态。
        // ensure/controls/turn 三个阶段的异常都要经过它，否则 task 泄漏在 running。
        // 多 backend 都失败时聚合成一条错误，便于诊断到底试了哪些。
        auto fail_background_task = [&](const std::string& error_text,
                                        const std::optional<std::string>& detail_code) {
            if (!task_context) return;
            AcpRuntimeError probe(AcpErrorCode::ACP_TURN_FAILED, error_text, detail_code);
            MarkBackgroundTaskTerminalParams terminal;
            terminal.status = resolve_background_task_failure_status(probe);
            terminal.ended_at = now_ms();
            terminal.error = error_text;
            terminal.progress_summary =
                task_progress_summary.empty() ? std::nullopt
                                              : std::optional<std::string>(task_progress_summary);
            terminal.terminal_summary = std::nullopt;
            mark_background_task_terminal(task_context->run_id, terminal);
        };

        // 对齐 OC recordBackendFailure 的错误聚合部分
        auto aggregate_backend_error = [&](const AcpRuntimeError& error) -> AcpRuntimeError {
            if (backend_attempts.size() <= 1) {
                return error;
            }
            std::string failed;
            for (size_t i = 0; i < backend_attempts.size(); ++i) {
                if (i > 0) failed += " | ";
                failed += backend_attempts[i].backend + ": " + backend_attempts[i].error;
            }
            return AcpRuntimeError(error.code,
                                   "All ACP backends failed (" +
                                       std::to_string(backend_attempts.size()) + "): " + failed,
                                   error.detail_code);
        };

        // 对齐 OC: for (backendIndex) { for (attempt) } (manager.turn-runner.ts:169-455)
        // 外层换 backend，内层用新 handle 重试一次。completed 为真即整体成功退出。
        bool completed = false;

        for (size_t backend_idx = 0; backend_idx < candidate_backends.size() && !completed;
             ++backend_idx) {
        const std::string& current_backend = candidate_backends[backend_idx];

        if (backend_idx > 0) {
            // 对齐 OC: runtimeHandles.close({ reason: "backend-failover" })
            // 必须先关掉上一个 backend 的 handle，否则旧连接泄漏。
            try {
                auto* stale = runtime_cache_.peek(actor_key);
                if (stale && stale->runtime) {
                    stale->runtime->close(stale->handle, "backend-failover");
                }
            } catch (...) {
                // 关闭失败不阻断切换 —— 目标是尽快换到可用 backend
            }
            runtime_cache_.clear(actor_key);
            log()->info("acp-manager: switching backend for {} from {} to {}",
                        input.session_key,
                        backend_plan.describe_backend_candidate(
                            candidate_backends[backend_idx - 1]),
                        backend_plan.describe_backend_candidate(current_backend));
        }

        for (int attempt = 0; attempt < 2 && !completed; ++attempt) {
        // 对齐 OC：仅首个 backend 的首次尝试复用 initialResolution，
        // 其余都要重查（resume 状态可能刚被清）。
        AcpSessionResolution resolution;
        if (backend_idx == 0 && attempt == 0) {
            resolution = initial_resolution;
        } else {
            resolution = resolve_session(input.session_key);
            if (resolution.kind != AcpSessionResolution::Kind::Ready) {
                AcpRuntimeError err =
                    resolution.error
                        ? *resolution.error
                        : AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                                          "Session is not ACP-enabled: " + input.session_key);
                const auto aggregated = aggregate_backend_error(err);
                record_turn_completion(turn_started_at, true);
                fail_background_task(aggregated.what(), aggregated.detail_code);
                set_session_state(input.session_key, "error", std::string(aggregated.what()));
                throw aggregated;
            }
        }

        // 对齐 OC: metaWithBackend —— 空 backend 表示让 registry 自选默认
        AcpSessionMeta meta_with_backend = *resolution.meta;
        if (!current_backend.empty()) {
            meta_with_backend.backend = current_backend;
        }

        // 对齐 OC: sawTurnOutput / retryFreshHandle
        bool saw_turn_output = false;
        bool retry_fresh_handle = false;
        bool active_turn_started = false;

        AcpSessionManager::EnsuredRuntime ensured;
        bool has_ensured = false;
        try {
            ensured = ensure_runtime_handle(input.session_key, meta_with_backend);
            has_ensured = true;
            apply_runtime_controls(input.session_key, *ensured.runtime, ensured.handle, ensured.meta);
        } catch (const std::exception& e) {
            // 对齐 OC toAcpRuntimeError：turn 尚未启动 → ACP_SESSION_INIT_FAILED
            const auto* typed = dynamic_cast<const AcpRuntimeError*>(&e);
            AcpRuntimeError acp_error =
                typed ? *typed
                      : AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, e.what());

            // 对齐 OC: prepareFreshManagerRuntimeHandleRetry —— ensure 阶段也可重试
            const auto retry_decision = prepare_fresh_runtime_handle_retry(
                attempt, acp_error, /*saw_turn_output=*/false, has_ensured,
                meta_with_backend.mode == AcpRuntimeSessionMode::Persistent);
            if (retry_decision.should_retry) {
                bool ok = true;
                if (retry_decision.needs_clear_resume_state) {
                    ok = clear_persisted_resume_state(input.session_key);
                }
                if (ok) {
                    runtime_cache_.clear(actor_key);
                    log()->info("acp-manager: retrying {} with a fresh runtime handle after "
                                "early init failure: {}",
                                input.session_key, acp_error.what());
                    continue;
                }
            }

            BackendAttempt failed_attempt;
            failed_attempt.backend = backend_plan.describe_backend_candidate(current_backend);
            failed_attempt.error = acp_error.what();
            failed_attempt.code = acp_error.code;
            failed_attempt.saw_output = false;
            backend_attempts.push_back(failed_attempt);

            if (is_failover_worthy_backend_error(failed_attempt) &&
                should_attempt_backend_failover(backend_idx, candidate_backends)) {
                break;  // 换下一个 backend
            }
            const auto aggregated = aggregate_backend_error(acp_error);
            fail_background_task(aggregated.what(), aggregated.detail_code);
            set_session_state(input.session_key, "error", std::string(aggregated.what()));
            throw aggregated;
        }

        // setSessionState("running")
        set_session_state(input.session_key, "running", std::nullopt, true);

        // Register active turn
        auto active_turn = std::make_shared<ActiveTurnState>();
        active_turn->runtime = ensured.runtime;
        active_turn->handle = ensured.handle;
        {
            std::lock_guard<std::mutex> lock(active_turns_mutex_);
            active_turns_[actor_key] = active_turn;
        }
        active_turn_started = true;
        (void)active_turn_started;

        // 对齐 OC：用 optional 而非裸指针。原实现连续两个 Error 事件会让第一个
        // new 出的对象泄漏，且三处 delete 易漏。
        std::optional<AcpRuntimeError> stream_error;

        // 对齐 OC: const eventGate = { open: true } (manager.turn-runner.ts:252)
        // 超时后置 false，抑制迟到事件。C++ 差异：turn 线程杀不掉，超时后它仍会
        // 继续投事件，必须 shared_ptr + atomic 才能既不 UAF 也不 data race。
        auto event_gate = std::make_shared<AcpTurnEventGate>();

        // 对齐 OC: skipPostTurnCleanup —— 超时路径已在 cleanupTimedOutTurn 里
        // 做过 cancel/close，turn 尾部不可再 close 一次（会 double-close）。
        bool skip_post_turn_cleanup = false;

        try {
            AcpRuntimeTurnInput turn_input;
            turn_input.handle = ensured.handle;
            turn_input.text = input.text;
            turn_input.attachments = input.attachments;
            turn_input.mode = input.mode;
            turn_input.request_id = input.request_id;

            // 对齐 OC: consumeAcpTurnStream —— 统计 sawOutput 与 terminalStatus
            auto run_turn_body = [&]() -> AcpTurnStreamOutcome {
                AcpTurnStreamOutcome outcome;
                ensured.runtime->run_turn(turn_input, [&](const AcpRuntimeEvent& event) {
                    // 对齐 OC: if (!eventGate.open) continue
                    if (!event_gate->open.load()) return;
                    if (active_turn->cancelled.load()) return;
                    if (input.cancel_signal && input.cancel_signal->load()) {
                        active_turn->cancelled.store(true);
                        return;
                    }

                    AcpRuntimeEvent forwarded = event;

                    if (event.type == AcpRuntimeEvent::Type::Done) {
                        // 对齐 OC: 旧适配器可能只给 stopReason 不给 status
                        outcome.terminal_status = resolve_acp_turn_terminal_status(event);
                        forwarded.status = outcome.terminal_status;
                    } else if (event.type == AcpRuntimeEvent::Type::Error) {
                        // 保留首个错误：后续错误多为级联噪音，首个最接近根因
                        if (!stream_error) {
                            stream_error.emplace(
                                event.code ? error_code_from_string(*event.code)
                                           : AcpErrorCode::ACP_TURN_FAILED,
                                event.message.value_or("ACP turn failed before completion."),
                                event.detail_code);
                        }
                    } else if (event.type == AcpRuntimeEvent::Type::TextDelta ||
                               event.type == AcpRuntimeEvent::Type::ToolCall) {
                        // 对齐 OC onOutputEvent (manager.turn-runner.ts:268-283)
                        // sawTurnOutput 一旦为真就禁止 failover / fresh-handle 重试，
                        // 否则用户会看到同一问题被回答两遍。
                        outcome.saw_output = true;
                        saw_turn_output = true;
                        if (event.type == AcpRuntimeEvent::Type::TextDelta &&
                            (!event.stream || *event.stream != "thought") &&
                            !event.text.empty()) {
                            task_progress_summary = append_background_task_progress_summary(
                                task_progress_summary, event.text);
                        }
                        if (task_context) {
                            mark_background_task_running(
                                task_context->run_id,
                                task_progress_summary.empty()
                                    ? std::nullopt
                                    : std::optional<std::string>(task_progress_summary));
                        }
                    }

                    if (input.on_event) {
                        input.on_event(forwarded);
                    }
                });

                // 对齐 OC: if (eventGate.open && streamError) throw streamError
                if (event_gate->open.load() && stream_error) {
                    throw *stream_error;
                }
                return outcome;
            };

            // 对齐 OC: resolveTurnTimeoutMs + ACP_TURN_TIMEOUT_GRACE_MS
            const auto runtime_options = normalize_runtime_options(ensured.meta.runtime_options);
            const int64_t turn_timeout_ms = resolve_turn_timeout_ms(runtime_options);
            const AcpRuntimeSessionMode session_mode = ensured.meta.mode;

            auto await_result = await_turn_with_timeout(
                input.session_key,
                turn_timeout_ms + ACP_TURN_TIMEOUT_GRACE_MS,
                turn_timeout_ms,
                run_turn_body);

            if (await_result.kind == AwaitTurnResult::Kind::TimedOut) {
                // 对齐 OC onTimeout (manager.turn-runner.ts:296-313)
                event_gate->open.store(false);
                skip_post_turn_cleanup = true;
                active_turn->cancelled.store(true);
                const std::string cache_key = normalize_actor_key(input.session_key);
                const AcpRuntimeHandle timed_out_handle = ensured.handle;
                cleanup_timed_out_turn(
                    input.session_key, ensured.runtime, ensured.handle, session_mode,
                    [this, cache_key, timed_out_handle]() {
                        // 对齐 OC: clearIfHandleMatches —— 只清仍指向这个 handle 的缓存，
                        // 否则会误删后续 turn 新建的 handle。
                        auto* cached = runtime_cache_.peek(cache_key);
                        if (cached && cached->handle.runtime_session_name ==
                                          timed_out_handle.runtime_session_name) {
                            runtime_cache_.clear(cache_key);
                        }
                    });
                throw make_turn_timeout_error(turn_timeout_ms);
            }

            if (await_result.kind == AwaitTurnResult::Kind::Failed) {
                throw *await_result.error;
            }

            // 对齐 OC (manager.turn-runner.ts:315-320)：没有 done 事件即视为失败。
            // 这条闸门很关键 —— 缺了它，runtime 静默断流会被当成成功。
            if (!await_result.outcome.terminal_status) {
                throw AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED,
                                      "ACP turn ended without a terminal done event.");
            }

            const bool turn_cancelled = *await_result.outcome.terminal_status == "cancelled";

            record_turn_completion(turn_started_at, false);

            // 对齐 OC (manager.turn-runner.ts:324-344)：成功终态 + blocked 推断
            if (task_context) {
                const auto terminal_result =
                    resolve_background_task_terminal_result(task_progress_summary);
                MarkBackgroundTaskTerminalParams terminal;
                // 对齐 OC：终态取自 done 事件的 terminalStatus，而非本地 cancelled 标志
                terminal.status = turn_cancelled ? tasks::TaskStatus::CANCELLED
                                                 : tasks::TaskStatus::SUCCEEDED;
                terminal.ended_at = now_ms();
                terminal.progress_summary =
                    task_progress_summary.empty()
                        ? std::nullopt
                        : std::optional<std::string>(task_progress_summary);
                terminal.terminal_summary = terminal_result.terminal_summary;
                terminal.terminal_outcome = terminal_result.terminal_outcome;
                mark_background_task_terminal(task_context->run_id, terminal);
            }

            set_session_state(input.session_key, "idle", std::nullopt, true);
            completed = true;

        } catch (const std::exception& e) {
            event_gate->open.store(false);

            // 对齐 OC toAcpRuntimeError：turn 已启动 → ACP_TURN_FAILED
            const auto* typed = dynamic_cast<const AcpRuntimeError*>(&e);
            AcpRuntimeError acp_error =
                typed ? *typed : AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED, e.what());

            // Cleanup active turn（先摘除，重试时会重新注册）
            {
                std::lock_guard<std::mutex> lock(active_turns_mutex_);
                auto it = active_turns_.find(actor_key);
                if (it != active_turns_.end() && it->second == active_turn) {
                    active_turns_.erase(it);
                }
            }

            // 对齐 OC: prepareFreshManagerRuntimeHandleRetry (manager.turn-runner.ts:363-386)
            // 仅 attempt==0 且无可见输出时生效；needs_clear 时先把 identity 退回 pending。
            const auto retry_decision = prepare_fresh_runtime_handle_retry(
                attempt, acp_error, saw_turn_output, has_ensured,
                ensured.meta.mode == AcpRuntimeSessionMode::Persistent);
            if (retry_decision.should_retry) {
                retry_fresh_handle = true;
                if (retry_decision.needs_clear_resume_state &&
                    !clear_persisted_resume_state(input.session_key)) {
                    retry_fresh_handle = false;
                }
                if (retry_fresh_handle) {
                    runtime_cache_.clear(actor_key);
                    log()->info("acp-manager: retrying {} with a fresh runtime handle after "
                                "early turn failure: {}",
                                input.session_key, acp_error.what());
                    continue;  // 同一 backend 的 attempt=1
                }
            }
            (void)retry_fresh_handle;

            // 对齐 OC: backendAttempts.push (manager.turn-runner.ts:404-410)
            BackendAttempt failed_attempt;
            failed_attempt.backend = backend_plan.describe_backend_candidate(current_backend);
            failed_attempt.error = acp_error.what();
            failed_attempt.code = acp_error.code;
            failed_attempt.saw_output = saw_turn_output;
            backend_attempts.push_back(failed_attempt);

            if (is_failover_worthy_backend_error(failed_attempt) &&
                should_attempt_backend_failover(backend_idx, candidate_backends)) {
                break;  // 换下一个 backend（不写终态，留给最终失败时聚合）
            }

            // 对齐 OC recordBackendFailure：所有候选耗尽，写终态并抛聚合错误
            record_turn_completion(turn_started_at, true);
            const auto aggregated = aggregate_backend_error(acp_error);
            fail_background_task(aggregated.what(), aggregated.detail_code);
            set_session_state(input.session_key, "error", std::string(aggregated.what()));
            throw aggregated;
        }

        // Cleanup active turn
        {
            std::lock_guard<std::mutex> lock(active_turns_mutex_);
            auto it = active_turns_.find(actor_key);
            if (it != active_turns_.end() && it->second == active_turn) {
                active_turns_.erase(it);
            }
        }

        // 对齐 OC: skipPostTurnCleanup —— 超时路径已做过 cancel/close
        if (skip_post_turn_cleanup) {
            break;
        }

        // Reconcile identity (persistent mode only)
        if (ensured.meta.mode != AcpRuntimeSessionMode::Oneshot) {
            reconcile_identifiers(input.session_key, *ensured.runtime, ensured.handle, ensured.meta);
        }

        // Oneshot: close runtime after turn
        if (ensured.meta.mode == AcpRuntimeSessionMode::Oneshot) {
            try {
                ensured.runtime->close(ensured.handle, "oneshot-complete");
            } catch (...) {
                log()->debug("acp-manager: oneshot close failed for {}", input.session_key);
            }
            runtime_cache_.clear(actor_key);
        }
        }  // attempt loop
        }  // backend loop
    });
}

// ── resolveBackgroundTaskContext ─────────────────────────────────────────────
// Aligned with openclaw: resolveBackgroundTaskContext (manager.background-task.ts:99-128)

// ── clearPersistedRuntimeResumeState ─────────────────────────────────────────
// Aligned with openclaw: clearPersistedRuntimeResumeState
// (manager.runtime-resume-state.ts:101-149)
//
// 把 identity 退回 pending 并抹掉 acpxSessionId / agentSessionId，
// 但**保留 acpxRecordId** —— 记录本身还在，只是 backend 侧的可 resume 会话没了；
// 保留它能让下次 ensure 继续复用同一条 acpx 记录。

bool AcpSessionManager::clear_persisted_resume_state(const std::string& session_key) {
    if (!write_meta_) {
        return false;
    }
    auto current = read_meta_ ? read_meta_(session_key) : std::nullopt;
    if (!current) {
        // 对齐 OC: mutate 收到 entry==null 或 current==null 时返回 null → updated 为假
        return false;
    }

    // 对齐 OC: 没有任何 resume 标识时原样返回（视为已清理成功）
    const bool has_resume_ids =
        current->identity.has_value() &&
        (current->identity->acpx_session_id.has_value() ||
         current->identity->agent_session_id.has_value());
    if (!has_resume_ids) {
        return true;
    }

    const int64_t now = now_ms();
    AcpSessionMeta next = *current;
    AcpSessionMeta::Identity next_identity;
    next_identity.state = "pending";
    // 只带回 acpxRecordId，其余两个 id 刻意留空
    next_identity.acpx_record_id = current->identity->acpx_record_id;
    next_identity.source = current->identity->source;
    next_identity.last_updated_at = now;
    next.identity = next_identity;
    next.last_activity_at = now;

    try {
        write_meta_(session_key, next);
    } catch (...) {
        log()->warn("acp-manager: unable to clear persisted runtime resume state for {}",
                    session_key);
        return false;
    }
    return true;
}

std::optional<AcpBackgroundTaskContext> AcpSessionManager::resolve_background_task_context(
    const std::string& session_key,
    const std::string& request_id,
    const std::string& text)
{
    // 未注入血缘加载器 → 无法确定 requester，整体关闭镜像
    // （对齐 OC：拿不到 requesterSessionKey 就 return null）
    if (!session_lineage_loader_) {
        return std::nullopt;
    }
    if (request_id.empty()) {
        // run_id 是 task 的主键，缺了写不进 registry
        return std::nullopt;
    }

    std::optional<AcpSessionLineage> child_entry;
    try {
        child_entry = session_lineage_loader_(session_key);
    } catch (...) {
        return std::nullopt;
    }
    if (!child_entry) {
        return std::nullopt;
    }

    // 对齐 OC: normalizeText(entry?.spawnedBy) ?? normalizeText(entry?.parentSessionKey)
    auto pick = [](const std::optional<std::string>& v) -> std::optional<std::string> {
        if (!v) return std::nullopt;
        std::string s = *v;
        size_t b = 0;
        while (b < s.size() && static_cast<unsigned char>(s[b]) <= 0x20) ++b;
        size_t e = s.size();
        while (e > b && static_cast<unsigned char>(s[e - 1]) <= 0x20) --e;
        if (e <= b) return std::nullopt;
        return s.substr(b, e - b);
    };

    auto requester = pick(child_entry->spawned_by);
    if (!requester) {
        requester = pick(child_entry->parent_session_key);
    }
    if (!requester) {
        // 非 spawn 出来的会话（如用户直接绑定的 ACP 会话）没有 requester，
        // 不做镜像 —— 对齐 OC。
        return std::nullopt;
    }

    std::optional<AcpSessionLineage> parent_entry;
    try {
        parent_entry = session_lineage_loader_(*requester);
    } catch (...) {
        parent_entry = std::nullopt;
    }

    AcpBackgroundTaskContext ctx;
    ctx.requester_session_key = *requester;
    // 对齐 OC: deliveryContextFromSession(parentEntry) ?? deliveryContextFromSession(childEntry)
    // 父会话的投递上下文优先 —— 结果要回到发起者所在频道，而非子会话的。
    const AcpSessionLineage* origin =
        (parent_entry && parent_entry->last_channel) ? &*parent_entry
        : (child_entry->last_channel ? &*child_entry : nullptr);
    if (origin) {
        ctx.requester_channel = origin->last_channel;
        ctx.requester_to = origin->last_to;
        ctx.requester_account_id = origin->last_account_id;
        ctx.requester_thread_id = origin->last_thread_id;
    }
    ctx.child_session_key = session_key;
    ctx.run_id = request_id;
    ctx.label = pick(child_entry->label);
    ctx.task = summarize_background_task_text(text);
    return ctx;
}

// ── cancelSession ────────────────────────────────────────────────────────────
// Aligned with openclaw: cancelSession (manager.core.ts:742-812)

void AcpSessionManager::cancel_session(const std::string& session_key, const std::string& reason) {
    if (session_key.empty()) {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "ACP session key is required.");
    }

    std::string actor_key = normalize_actor_key(session_key);

    // Check if there's an active turn — abort it directly (no actor queue needed)
    {
        std::lock_guard<std::mutex> lock(active_turns_mutex_);
        auto it = active_turns_.find(actor_key);
        if (it != active_turns_.end()) {
            it->second->cancelled.store(true);
            try {
                it->second->runtime->cancel(it->second->handle, reason);
            } catch (...) {}
            return;
        }
    }

    // No active turn — go through actor queue
    actor_queue_.run(actor_key, [&]() {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready) {
            if (resolution.error) throw *resolution.error;
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + session_key);
        }

        auto ensured = ensure_runtime_handle(session_key, *resolution.meta);
        try {
            with_acp_runtime_error_boundary(
                [&]() { ensured.runtime->cancel(ensured.handle, reason); },
                AcpErrorCode::ACP_TURN_FAILED,
                "ACP cancel failed before completion."
            );
            set_session_state(session_key, "idle", std::nullopt, true);
        } catch (const AcpRuntimeError& e) {
            set_session_state(session_key, "error", std::string(e.what()));
            throw;
        }
    });
}

// ── closeSession ─────────────────────────────────────────────────────────────
// Aligned with openclaw: closeSession (manager.core.ts:814-900)

AcpSessionManager::CloseSessionResult AcpSessionManager::close_session(
    const std::string& session_key,
    const std::string& reason,
    bool clear_meta,
    bool allow_backend_unavailable,
    bool require_acp_session
) {
    if (session_key.empty()) {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED, "ACP session key is required.");
    }

    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> CloseSessionResult {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready) {
            if (require_acp_session) {
                if (resolution.error) throw *resolution.error;
                throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                    "Session is not ACP-enabled: " + session_key);
            }
            return {false, std::nullopt, false};
        }

        CloseSessionResult result;
        try {
            auto ensured = ensure_runtime_handle(session_key, *resolution.meta);
            with_acp_runtime_error_boundary(
                [&]() { ensured.runtime->close(ensured.handle, reason); },
                AcpErrorCode::ACP_TURN_FAILED,
                "ACP close failed before completion."
            );
            result.runtime_closed = true;
            runtime_cache_.clear(normalize_actor_key(session_key));
        } catch (const AcpRuntimeError& e) {
            if (allow_backend_unavailable &&
                (e.code == AcpErrorCode::ACP_BACKEND_MISSING ||
                 e.code == AcpErrorCode::ACP_BACKEND_UNAVAILABLE)) {
                runtime_cache_.clear(normalize_actor_key(session_key));
                result.runtime_notice = e.what();
            } else {
                throw;
            }
        }

        if (clear_meta) {
            write_meta_(session_key, std::nullopt);
            result.meta_cleared = true;
        }

        return result;
    });
}

// ── setSessionRuntimeMode ────────────────────────────────────────────────────

AcpSessionRuntimeOptions AcpSessionManager::set_session_runtime_mode(
    const std::string& session_key,
    const std::string& runtime_mode
) {
    auto validated = validate_runtime_mode(runtime_mode);
    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> AcpSessionRuntimeOptions {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready || !resolution.meta) {
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + session_key);
        }
        auto ensured = ensure_runtime_handle(session_key, *resolution.meta);
        if (!ensured.runtime->has_set_mode()) {
            throw AcpRuntimeError(AcpErrorCode::ACP_BACKEND_UNSUPPORTED_CONTROL,
                "ACP backend \"" + ensured.meta.backend + "\" does not support session/set_mode.");
        }
        with_acp_runtime_error_boundary(
            [&]() { ensured.runtime->set_mode(ensured.handle, validated); },
            AcpErrorCode::ACP_TURN_FAILED,
            "Could not update ACP runtime mode."
        );
        AcpSessionRuntimeOptions patch;
        patch.runtime_mode = validated;
        auto next = merge_runtime_options(ensured.meta.runtime_options, patch);
        auto updated_meta = ensured.meta;
        updated_meta.runtime_options = next;
        updated_meta.last_activity_at = now_ms();
        write_meta_(session_key, updated_meta);
        return next;
    });
}

// ── setSessionConfigOption ───────────────────────────────────────────────────

AcpSessionRuntimeOptions AcpSessionManager::set_session_config_option(
    const std::string& session_key,
    const std::string& key,
    const std::string& value
) {
    auto validated_key = validate_config_option_key(key);
    auto validated_value = validate_config_option_value(value);

    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> AcpSessionRuntimeOptions {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready || !resolution.meta) {
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + session_key);
        }
        auto ensured = ensure_runtime_handle(session_key, *resolution.meta);
        if (!ensured.runtime->has_set_config_option()) {
            throw AcpRuntimeError(AcpErrorCode::ACP_BACKEND_UNSUPPORTED_CONTROL,
                "ACP backend \"" + ensured.meta.backend + "\" does not support session/set_config_option.");
        }
        with_acp_runtime_error_boundary(
            [&]() { ensured.runtime->set_config_option(ensured.handle, validated_key, validated_value); },
            AcpErrorCode::ACP_TURN_FAILED,
            "Could not update ACP runtime config option."
        );

        // Infer patch from key/value
        AcpSessionRuntimeOptions patch;
        std::string lower_key = validated_key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if (lower_key == "model") {
            patch.model = validated_value;
        } else if (lower_key == "approval_policy" || lower_key == "permission_profile" || lower_key == "permissions") {
            patch.permission_profile = validated_value;
        } else if (lower_key == "timeout" || lower_key == "timeout_seconds") {
            try { patch.timeout_seconds = std::stoi(validated_value); } catch (...) {}
        } else if (lower_key == "cwd") {
            patch.cwd = validated_value;
        } else {
            patch.backend_extras[validated_key] = validated_value;
        }

        auto next = merge_runtime_options(ensured.meta.runtime_options, patch);
        auto updated_meta = ensured.meta;
        updated_meta.runtime_options = next;
        updated_meta.last_activity_at = now_ms();
        write_meta_(session_key, updated_meta);
        return next;
    });
}

// ── updateSessionRuntimeOptions ──────────────────────────────────────────────

AcpSessionRuntimeOptions AcpSessionManager::update_session_runtime_options(
    const std::string& session_key,
    const AcpSessionRuntimeOptions& patch
) {
    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> AcpSessionRuntimeOptions {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready || !resolution.meta) {
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + session_key);
        }
        auto next = merge_runtime_options(resolution.meta->runtime_options, patch);
        auto updated_meta = *resolution.meta;
        updated_meta.runtime_options = next;
        updated_meta.last_activity_at = now_ms();
        write_meta_(session_key, updated_meta);
        return next;
    });
}

// ── resetSessionRuntimeOptions ───────────────────────────────────────────────

AcpSessionRuntimeOptions AcpSessionManager::reset_session_runtime_options(
    const std::string& session_key
) {
    return actor_queue_.run(normalize_actor_key(session_key), [&]() -> AcpSessionRuntimeOptions {
        auto resolution = resolve_session(session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready || !resolution.meta) {
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + session_key);
        }
        auto ensured = ensure_runtime_handle(session_key, *resolution.meta);
        with_acp_runtime_error_boundary(
            [&]() { ensured.runtime->close(ensured.handle, "reset-runtime-options"); },
            AcpErrorCode::ACP_TURN_FAILED,
            "Could not reset ACP runtime options."
        );
        runtime_cache_.clear(normalize_actor_key(session_key));
        auto updated_meta = *resolution.meta;
        updated_meta.runtime_options = {};
        updated_meta.last_activity_at = now_ms();
        write_meta_(session_key, updated_meta);
        return {};
    });
}

// ── ensureRuntimeHandle (private) ────────────────────────────────────────────
// Aligned with openclaw: ensureRuntimeHandle (manager.core.ts:902-1021)

AcpSessionManager::EnsuredRuntime AcpSessionManager::ensure_runtime_handle(
    const std::string& session_key,
    const AcpSessionMeta& meta
) {
    std::string actor_key = normalize_actor_key(session_key);
    std::string agent = meta.agent.empty() ? "main" : meta.agent;
    auto mode = meta.mode;
    std::string effective_cwd;
    if (meta.runtime_options.cwd) effective_cwd = *meta.runtime_options.cwd;
    else if (meta.cwd) effective_cwd = *meta.cwd;

    std::string configured_backend = meta.backend;

    // Check cache
    auto* cached = runtime_cache_.get(actor_key);
    if (cached) {
        bool backend_match = configured_backend.empty() || cached->backend == configured_backend;
        bool agent_match = cached->agent == agent;
        bool mode_match = cached->mode == mode;
        std::string cached_cwd = cached->cwd.value_or("");
        bool cwd_match = cached_cwd == effective_cwd;
        if (backend_match && agent_match && mode_match && cwd_match) {
            return {cached->runtime, cached->handle, meta};
        }
        runtime_cache_.clear(actor_key);
    }

    // Create new runtime handle
    auto backend = AcpRuntimeRegistry::instance().require_backend(
        configured_backend.empty() ? "" : configured_backend
    );
    auto runtime = backend.runtime;

    AcpRuntimeEnsureInput ensure_input;
    ensure_input.session_key = session_key;
    ensure_input.agent = agent;
    ensure_input.mode = mode;
    if (!effective_cwd.empty()) ensure_input.cwd = effective_cwd;

    auto handle = with_acp_runtime_error_boundary(
        [&]() { return runtime->ensure_session(ensure_input); },
        AcpErrorCode::ACP_SESSION_INIT_FAILED,
        "Could not initialize ACP session runtime."
    );

    // Update cwd from handle if available
    if (handle.cwd && !handle.cwd->empty()) {
        effective_cwd = *handle.cwd;
    }

    // Update meta with new backend/runtime info
    AcpSessionMeta next_meta = meta;
    next_meta.backend = handle.backend.empty() ? backend.id : handle.backend;
    next_meta.runtime_session_name = handle.runtime_session_name;
    next_meta.last_activity_at = now_ms();
    if (!effective_cwd.empty()) {
        next_meta.cwd = effective_cwd;
        next_meta.runtime_options.cwd = effective_cwd;
    }

    // Persist if changed
    if (next_meta.backend != meta.backend ||
        next_meta.runtime_session_name != meta.runtime_session_name) {
        write_meta_(session_key, next_meta);
    }

    // Cache
    CachedRuntimeState cache_state;
    cache_state.runtime = runtime;
    cache_state.handle = handle;
    cache_state.backend = next_meta.backend;
    cache_state.agent = agent;
    cache_state.mode = mode;
    if (!effective_cwd.empty()) cache_state.cwd = effective_cwd;
    runtime_cache_.set(actor_key, cache_state);

    return {runtime, handle, next_meta};
}

// ── applyRuntimeControls (private) ───────────────────────────────────────────
// Aligned with openclaw: applyManagerRuntimeControls (manager.runtime-controls.ts)

void AcpSessionManager::apply_runtime_controls(
    const std::string& session_key,
    AcpRuntime& runtime,
    const AcpRuntimeHandle& handle,
    const AcpSessionMeta& meta
) {
    auto opts = normalize_runtime_options(meta.runtime_options);
    auto signature = build_runtime_control_signature(opts);

    std::string actor_key = normalize_actor_key(session_key);
    auto* cached = runtime_cache_.peek(actor_key);
    if (cached && cached->applied_control_signature == signature) {
        return;  // Already applied
    }

    // Apply runtime mode
    auto runtime_mode = opts.runtime_mode;
    if (runtime_mode && !runtime_mode->empty() && runtime.has_set_mode()) {
        with_acp_runtime_error_boundary(
            [&]() { runtime.set_mode(handle, *runtime_mode); },
            AcpErrorCode::ACP_TURN_FAILED,
            "Could not apply ACP runtime options before turn execution."
        );
    }

    // Apply config options
    auto pairs = build_runtime_config_option_pairs(opts);
    if (!pairs.empty() && runtime.has_set_config_option()) {
        for (auto& [key, value] : pairs) {
            with_acp_runtime_error_boundary(
                [&]() { runtime.set_config_option(handle, key, value); },
                AcpErrorCode::ACP_TURN_FAILED,
                "Could not apply ACP runtime options before turn execution."
            );
        }
    }

    // Update cache signature
    if (cached) {
        cached->applied_control_signature = signature;
    }
}

// ── setSessionState (private) ────────────────────────────────────────────────
// Aligned with openclaw: setSessionState (manager.core.ts:1169-1207)

void AcpSessionManager::set_session_state(
    const std::string& session_key,
    const std::string& state,
    const std::optional<std::string>& last_error,
    bool clear_last_error
) {
    auto meta = read_meta_(session_key);
    if (!meta) return;

    meta->state = state;
    meta->last_activity_at = now_ms();

    if (last_error && !last_error->empty()) {
        meta->last_error = *last_error;
    } else if (clear_last_error) {
        meta->last_error = std::nullopt;
    }

    write_meta_(session_key, *meta);
}

// ── reconcile_identifiers (private) ──────────────────────────────────────────
// Aligned with openclaw: reconcileRuntimeSessionIdentifiers (manager.identity-reconcile.ts)

void AcpSessionManager::reconcile_identifiers(
    const std::string& session_key,
    AcpRuntime& runtime,
    AcpRuntimeHandle& handle,
    AcpSessionMeta& meta
) {
    if (!runtime.has_get_status()) return;

    AcpRuntimeStatus status;
    try {
        status = runtime.get_status(handle);
    } catch (...) {
        log()->debug("acp-manager: failed to refresh status for {}", session_key);
        return;
    }

    // Merge identity from status
    if (!meta.identity) {
        AcpSessionMeta::Identity identity;
        identity.state = "pending";
        identity.source = "status";
        identity.last_updated_at = now_ms();
        meta.identity = identity;
    }

    bool changed = false;
    if (status.acpx_record_id && (!meta.identity->acpx_record_id ||
        meta.identity->state != "resolved")) {
        meta.identity->acpx_record_id = status.acpx_record_id;
        changed = true;
    }
    if (status.backend_session_id && (!meta.identity->acpx_session_id ||
        meta.identity->state != "resolved")) {
        meta.identity->acpx_session_id = status.backend_session_id;
        changed = true;
    }
    if (status.agent_session_id && (!meta.identity->agent_session_id ||
        meta.identity->state != "resolved")) {
        meta.identity->agent_session_id = status.agent_session_id;
        changed = true;
    }

    if (meta.identity->acpx_session_id || meta.identity->agent_session_id) {
        if (meta.identity->state != "resolved") {
            meta.identity->state = "resolved";
            changed = true;
        }
    }

    if (changed) {
        meta.identity->source = "status";
        meta.identity->last_updated_at = now_ms();

        // Update handle
        if (meta.identity->acpx_session_id) {
            handle.backend_session_id = meta.identity->acpx_session_id;
        }
        if (meta.identity->agent_session_id) {
            handle.agent_session_id = meta.identity->agent_session_id;
        }

        // Update cache
        auto* cached = runtime_cache_.peek(normalize_actor_key(session_key));
        if (cached) {
            cached->handle = handle;
        }

        // Persist
        write_meta_(session_key, meta);
    }
}

// ── record_turn_completion ───────────────────────────────────────────────────

void AcpSessionManager::record_turn_completion(int64_t started_at, bool failed) {
    int64_t duration = std::max<int64_t>(0, now_ms() - started_at);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    turn_stats_.total_ms += duration;
    turn_stats_.max_ms = std::max(turn_stats_.max_ms, duration);
    if (failed) {
        turn_stats_.failed++;
    } else {
        turn_stats_.completed++;
    }
}

// ── evict_idle_handles ───────────────────────────────────────────────────────

void AcpSessionManager::evict_idle_handles(int64_t idle_ttl_ms) {
    if (idle_ttl_ms <= 0 || runtime_cache_.size() == 0) return;

    auto candidates = runtime_cache_.collect_idle_candidates(idle_ttl_ms);
    for (auto& candidate : candidates) {
        actor_queue_.run(candidate.actor_key, [&]() {
            {
                std::lock_guard<std::mutex> lock(active_turns_mutex_);
                if (active_turns_.count(candidate.actor_key)) return;
            }
            auto* cached = runtime_cache_.peek(candidate.actor_key);
            if (!cached) return;
            auto runtime = cached->runtime;
            auto handle = cached->handle;
            runtime_cache_.clear(candidate.actor_key);
            evicted_count_++;
            try {
                runtime->close(handle, "idle-evicted");
            } catch (...) {
                log()->debug("acp-manager: idle eviction close failed");
            }
        });
    }
}

// ── enforce_concurrent_limit ─────────────────────────────────────────────────

void AcpSessionManager::enforce_concurrent_limit(const std::string& session_key, int max_concurrent) {
    if (max_concurrent <= 0) return;
    std::string actor_key = normalize_actor_key(session_key);
    if (runtime_cache_.has(actor_key)) return;
    int active = runtime_cache_.size();
    if (active >= max_concurrent) {
        throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
            "ACP max concurrent sessions reached (" + std::to_string(active) +
            "/" + std::to_string(max_concurrent) + ").");
    }
}

} // namespace acp
} // namespace hunterclaw
