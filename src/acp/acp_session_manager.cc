// ============================================================================
// acp_session_manager.cc
// Aligned with openclaw: src/acp/control-plane/manager.core.ts (1291 lines)
// ============================================================================

#include "hunterclaw/acp/acp_session_manager.h"
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
        auto resolution = resolve_session(input.session_key);
        if (resolution.kind != AcpSessionResolution::Kind::Ready) {
            if (resolution.error) throw *resolution.error;
            throw AcpRuntimeError(AcpErrorCode::ACP_SESSION_INIT_FAILED,
                "Session is not ACP-enabled: " + input.session_key);
        }

        auto ensured = ensure_runtime_handle(input.session_key, *resolution.meta);
        apply_runtime_controls(input.session_key, *ensured.runtime, ensured.handle, ensured.meta);

        int64_t turn_started_at = now_ms();
        std::string actor_key = normalize_actor_key(input.session_key);

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

        AcpRuntimeError* stream_error = nullptr;
        try {
            AcpRuntimeTurnInput turn_input;
            turn_input.handle = ensured.handle;
            turn_input.text = input.text;
            turn_input.attachments = input.attachments;
            turn_input.mode = input.mode;
            turn_input.request_id = input.request_id;

            ensured.runtime->run_turn(turn_input, [&](const AcpRuntimeEvent& event) {
                if (active_turn->cancelled.load()) return;
                if (input.cancel_signal && input.cancel_signal->load()) {
                    active_turn->cancelled.store(true);
                    return;
                }

                if (event.type == AcpRuntimeEvent::Type::Error) {
                    stream_error = new AcpRuntimeError(
                        event.code ? error_code_from_string(*event.code) : AcpErrorCode::ACP_TURN_FAILED,
                        event.message.value_or("ACP turn failed before completion.")
                    );
                }
                if (input.on_event) {
                    input.on_event(event);
                }
            });

            if (stream_error) {
                AcpRuntimeError err = *stream_error;
                delete stream_error;
                throw err;
            }

            record_turn_completion(turn_started_at, false);
            set_session_state(input.session_key, "idle", std::nullopt, true);

        } catch (const AcpRuntimeError& e) {
            delete stream_error;
            record_turn_completion(turn_started_at, true);
            set_session_state(input.session_key, "error", std::string(e.what()));
            // Cleanup active turn
            {
                std::lock_guard<std::mutex> lock(active_turns_mutex_);
                auto it = active_turns_.find(actor_key);
                if (it != active_turns_.end() && it->second == active_turn) {
                    active_turns_.erase(it);
                }
            }
            throw;
        } catch (...) {
            delete stream_error;
            record_turn_completion(turn_started_at, true);
            set_session_state(input.session_key, "error", "ACP turn failed.");
            {
                std::lock_guard<std::mutex> lock(active_turns_mutex_);
                active_turns_.erase(actor_key);
            }
            throw AcpRuntimeError(AcpErrorCode::ACP_TURN_FAILED, "ACP turn failed before completion.");
        }

        // Cleanup active turn
        {
            std::lock_guard<std::mutex> lock(active_turns_mutex_);
            auto it = active_turns_.find(actor_key);
            if (it != active_turns_.end() && it->second == active_turn) {
                active_turns_.erase(it);
            }
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
            runtime_cache_.clear(normalize_actor_key(input.session_key));
        }
    });
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
