#pragma once

// ============================================================================
// session_manager.h  (ACP control-plane)
// Aligned with openclaw: src/acp/control-plane/manager.core.ts (1291 lines)
//
// Global singleton ACP session manager. Manages the full lifecycle:
//   resolveSession / initializeSession / runTurn / cancelSession / closeSession
// ============================================================================

#include "hunterclaw/acp/runtime_types.h"
#include "hunterclaw/acp/runtime_errors.h"
#include "hunterclaw/acp/runtime_registry.h"
#include "hunterclaw/acp/runtime_cache.h"
#include "hunterclaw/acp/runtime_options.h"
#include "hunterclaw/acp/session_actor_queue.h"
#include "hunterclaw/acp/acp_background_task.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <optional>
#include <functional>
#include <chrono>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace acp {

// ── Session ACP Meta (control-plane view) ────────────────────────────────────
// Aligned with openclaw: SessionAcpMeta (sessions/types.ts)
// Note: This is the ACP control-plane's own view, separate from
// sessions::SessionAcpMeta in session_manager.h (session storage layer).
struct AcpSessionMeta {
    std::string backend;
    std::string agent;
    std::string runtime_session_name;
    std::string state;  // "idle" | "running" | "error"
    AcpRuntimeSessionMode mode = AcpRuntimeSessionMode::Persistent;
    AcpSessionRuntimeOptions runtime_options;
    std::optional<std::string> cwd;
    int64_t last_activity_at = 0;
    std::optional<std::string> last_error;

    // Identity (pending/resolved)
    struct Identity {
        std::string state;  // "pending" | "resolved"
        std::optional<std::string> acpx_record_id;
        std::optional<std::string> acpx_session_id;
        std::optional<std::string> agent_session_id;
        std::string source;  // "ensure" | "status" | "event"
        int64_t last_updated_at = 0;
    };
    std::optional<Identity> identity;
};

// ── Session resolution (three-state) ─────────────────────────────────────────
// Aligned with openclaw: AcpSessionResolution
struct AcpSessionResolution {
    enum class Kind { None, Stale, Ready };
    Kind kind = Kind::None;
    std::string session_key;
    std::optional<AcpSessionMeta> meta;      // only when kind == Ready
    std::optional<AcpRuntimeError> error;    // only when kind == Stale
};

// ── Run turn input ───────────────────────────────────────────────────────────
// Aligned with openclaw: AcpRunTurnInput
struct AcpRunTurnInput {
    std::string session_key;
    std::string text;
    std::vector<AcpRuntimeTurnAttachment> attachments;
    AcpRuntimePromptMode mode = AcpRuntimePromptMode::Prompt;
    std::string request_id;
    AcpRuntimeEventCallback on_event;
    std::atomic<bool>* cancel_signal = nullptr;
};

// ── Active turn state ────────────────────────────────────────────────────────
// Aligned with openclaw: ActiveTurnState
struct ActiveTurnState {
    std::shared_ptr<AcpRuntime> runtime;
    AcpRuntimeHandle handle;
    std::atomic<bool> cancelled{false};
};

// ── Session meta read/write callbacks ────────────────────────────────────────
// These decouple AcpSessionManager from the storage layer.
using ReadSessionMetaFn = std::function<std::optional<AcpSessionMeta>(const std::string& session_key)>;
using WriteSessionMetaFn = std::function<void(const std::string& session_key, const std::optional<AcpSessionMeta>& meta)>;

// ── Session lineage lookup ───────────────────────────────────────────────────
// Aligned with openclaw: AcpSessionManagerDeps.loadSessionEntry —— 只取
// background-task 镜像需要的血缘/投递字段，避免 acp 模块反向依赖 sessions 模块。
struct AcpSessionLineage {
    std::optional<std::string> spawned_by;          // OC: entry.spawnedBy
    std::optional<std::string> parent_session_key;  // OC: entry.parentSessionKey
    std::optional<std::string> label;              // OC: entry.label
    // OC: deliveryContextFromSession(entry)
    std::optional<std::string> last_channel;
    std::optional<std::string> last_to;
    std::optional<std::string> last_account_id;
    std::optional<std::string> last_thread_id;
};
using LoadSessionLineageFn =
    std::function<std::optional<AcpSessionLineage>(const std::string& session_key)>;

// ── AcpSessionManager ────────────────────────────────────────────────────────
// Aligned with openclaw: class AcpSessionManager (manager.core.ts)
class AcpSessionManager {
public:
    AcpSessionManager(ReadSessionMetaFn read_meta, WriteSessionMetaFn write_meta)
        : read_meta_(std::move(read_meta))
        , write_meta_(std::move(write_meta))
    {}

    // ── Singleton access ─────────────────────────────────────────────────
    static AcpSessionManager& instance();
    static void set_instance(std::shared_ptr<AcpSessionManager> mgr);

    // ── resolveSession ───────────────────────────────────────────────────
    // Aligned with openclaw: resolveSession (manager.core.ts:89-119)
    AcpSessionResolution resolve_session(const std::string& session_key);

    // ── initializeSession ────────────────────────────────────────────────
    // Aligned with openclaw: initializeSession (manager.core.ts:211-320)
    AcpSessionMeta initialize_session(
        const std::string& session_key,
        const std::string& agent,
        AcpRuntimeSessionMode mode,
        const std::optional<std::string>& cwd = std::nullopt,
        const std::optional<std::string>& backend_id = std::nullopt,
        const std::optional<std::string>& resume_session_id = std::nullopt
    );

    // ── runTurn ──────────────────────────────────────────────────────────
    // Aligned with openclaw: runTurn (manager.core.ts:596-739)
    void run_turn(const AcpRunTurnInput& input);

    // ── cancelSession ────────────────────────────────────────────────────
    // Aligned with openclaw: cancelSession (manager.core.ts:742-812)
    void cancel_session(const std::string& session_key, const std::string& reason = "");

    // ── closeSession ─────────────────────────────────────────────────────
    // Aligned with openclaw: closeSession (manager.core.ts:814-900)
    struct CloseSessionResult {
        bool runtime_closed = false;
        std::optional<std::string> runtime_notice;
        bool meta_cleared = false;
    };
    CloseSessionResult close_session(
        const std::string& session_key,
        const std::string& reason,
        bool clear_meta = false,
        bool allow_backend_unavailable = false,
        bool require_acp_session = true
    );

    // ── setSessionRuntimeMode ────────────────────────────────────────────
    // Aligned with openclaw: setSessionRuntimeMode (manager.core.ts:398-450)
    AcpSessionRuntimeOptions set_session_runtime_mode(
        const std::string& session_key,
        const std::string& runtime_mode
    );

    // ── setSessionConfigOption ───────────────────────────────────────────
    // Aligned with openclaw: setSessionConfigOption (manager.core.ts:452-524)
    AcpSessionRuntimeOptions set_session_config_option(
        const std::string& session_key,
        const std::string& key,
        const std::string& value
    );

    // ── updateSessionRuntimeOptions ──────────────────────────────────────
    AcpSessionRuntimeOptions update_session_runtime_options(
        const std::string& session_key,
        const AcpSessionRuntimeOptions& patch
    );

    // ── resetSessionRuntimeOptions ───────────────────────────────────────
    AcpSessionRuntimeOptions reset_session_runtime_options(const std::string& session_key);

    // ── Session lineage injection ────────────────────────────────────────
    // Aligned with openclaw: AcpSessionManagerDeps.loadSessionEntry
    // 用于把子会话 turn 镜像成父侧可见的 detached task（background task）。
    // 未注入时 background task 镜像整体关闭（对齐 OC：拿不到 requester 就
    // resolveBackgroundTaskContext 返回 null，不建 task）。
    void set_session_lineage_loader(LoadSessionLineageFn loader) {
        session_lineage_loader_ = std::move(loader);
    }

    // ── Backend failover configuration ───────────────────────────────────
    // Aligned with openclaw: cfg.acp.backend / cfg.acp.fallbacks
    // （manager.turn-runner.ts:110-112 传给 resolveBackendCandidatePlan）
    // 未配置时候选只有一项空串，即"registry 自选默认 backend"，行为与
    // 对齐前完全一致 —— 故此项为纯增量，不改变既有部署。
    void set_backend_failover_config(const std::string& configured_backend,
                                     const std::vector<std::string>& fallback_backends) {
        configured_backend_ = configured_backend;
        fallback_backends_ = fallback_backends;
    }

private:
    ReadSessionMetaFn read_meta_;
    WriteSessionMetaFn write_meta_;
    LoadSessionLineageFn session_lineage_loader_;
    std::string configured_backend_;
    std::vector<std::string> fallback_backends_;

    // Clears persisted resume identifiers so the next ensure rebuilds the backend session.
    // Aligned with openclaw: clearPersistedRuntimeResumeState
    // 返回 false 表示无法清理（调用方须放弃 fresh-handle 重试）。
    bool clear_persisted_resume_state(const std::string& session_key);

    // Resolves the requester task context for a spawned child ACP session.
    // Aligned with openclaw: resolveBackgroundTaskContext
    std::optional<AcpBackgroundTaskContext> resolve_background_task_context(
        const std::string& session_key,
        const std::string& request_id,
        const std::string& text);


    SessionActorQueue actor_queue_;
    RuntimeCache runtime_cache_;
    std::map<std::string, std::shared_ptr<ActiveTurnState>> active_turns_;
    std::mutex active_turns_mutex_;

    // Stats
    struct TurnStats {
        int completed = 0;
        int failed = 0;
        int64_t total_ms = 0;
        int64_t max_ms = 0;
    } turn_stats_;
    std::mutex stats_mutex_;
    int evicted_count_ = 0;

    // ── Private helpers ──────────────────────────────────────────────────

    static std::string normalize_actor_key(const std::string& session_key) {
        std::string s = session_key;
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        s = s.substr(start, end - start + 1);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    auto log() {
        static auto logger = spdlog::get("acp_manager");
        if (!logger) logger = spdlog::get("");
        return logger;
    }

    // Aligned with openclaw: ensureRuntimeHandle (manager.core.ts:902-1021)
    struct EnsuredRuntime {
        std::shared_ptr<AcpRuntime> runtime;
        AcpRuntimeHandle handle;
        AcpSessionMeta meta;
    };
    EnsuredRuntime ensure_runtime_handle(const std::string& session_key, const AcpSessionMeta& meta);

    // Aligned with openclaw: applyRuntimeControls (manager.runtime-controls.ts)
    void apply_runtime_controls(
        const std::string& session_key,
        AcpRuntime& runtime,
        const AcpRuntimeHandle& handle,
        const AcpSessionMeta& meta
    );

    // Aligned with openclaw: setSessionState (manager.core.ts:1169-1207)
    void set_session_state(const std::string& session_key,
                           const std::string& state,
                           const std::optional<std::string>& last_error = std::nullopt,
                           bool clear_last_error = false);

    // Aligned with openclaw: reconcileRuntimeSessionIdentifiers
    void reconcile_identifiers(
        const std::string& session_key,
        AcpRuntime& runtime,
        AcpRuntimeHandle& handle,
        AcpSessionMeta& meta
    );

    void record_turn_completion(int64_t started_at, bool failed = false);

    void evict_idle_handles(int64_t idle_ttl_ms = 0);

    void enforce_concurrent_limit(const std::string& session_key, int max_concurrent = 0);
};

// ── Global singleton helpers ─────────────────────────────────────────────────
AcpSessionManager& get_acp_session_manager();

} // namespace acp
} // namespace hunterclaw
