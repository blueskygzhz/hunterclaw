#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

// 严格对齐 OpenClaw src/auto-reply/reply/reply-run-registry.ts (528行)
// ReplyOperation 状态机 + ReplyRunRegistry 全局单例

namespace hunterclaw {
namespace react {

// ============================================================================
// ReplyOperationPhase (对齐 reply-run-registry.ts:22-29)
// ============================================================================
enum class ReplyOperationPhase {
    Queued,                // "queued"
    PreflightCompacting,   // "preflight_compacting"
    MemoryFlushing,        // "memory_flushing"
    Running,               // "running"
    Completed,             // "completed"
    Failed,                // "failed"
    Aborted                // "aborted"
};

// ============================================================================
// ReplyOperationFailureCode (对齐 reply-run-registry.ts:31-36)
// ============================================================================
enum class ReplyOperationFailureCode {
    GatewayDraining,
    CommandLaneCleared,
    AbortedByUser,
    SessionCorruptionReset,
    RunFailed
};

// ============================================================================
// ReplyOperationAbortCode (对齐 reply-run-registry.ts:38)
// ============================================================================
enum class ReplyOperationAbortCode {
    AbortedByUser,
    AbortedForRestart
};

// ============================================================================
// ReplyBackendHandle (对齐 reply-run-registry.ts:10-20)
// ============================================================================
struct ReplyBackendHandle {
    std::string kind;  // "embedded" | "cli"
    std::function<void(const std::string& reason)> cancel;
    std::function<bool()> is_streaming;
    std::function<bool()> is_compacting;  // optional
    std::function<void(const std::string& text)> queue_message;  // optional
};

// ============================================================================
// ReplyOperation (对齐 reply-run-registry.ts:45-60)
// ============================================================================
class ReplyOperation {
public:
    ReplyOperation(const std::string& key, const std::string& session_id, bool reset_triggered);
    ~ReplyOperation();

    const std::string& key() const { return key_; }
    const std::string& session_id() const;
    ReplyOperationPhase phase() const;
    bool is_aborted() const;

    // 对齐: setPhase
    void set_phase(ReplyOperationPhase next);

    // 对齐: updateSessionId
    void update_session_id(const std::string& next_session_id);

    // 对齐: attachBackend / detachBackend
    void attach_backend(std::shared_ptr<ReplyBackendHandle> handle);
    void detach_backend(std::shared_ptr<ReplyBackendHandle> handle);

    // 对齐: complete / fail / abortByUser / abortForRestart
    void complete();
    void fail(ReplyOperationFailureCode code);
    void abort_by_user();
    void abort_for_restart();

private:
    std::string key_;
    std::string session_id_;
    bool reset_triggered_ = false;
    ReplyOperationPhase phase_ = ReplyOperationPhase::Queued;
    std::atomic<bool> aborted_{false};
    bool completed_ = false;
    std::shared_ptr<ReplyBackendHandle> backend_;
    mutable std::mutex mu_;
};

// ============================================================================
// ReplyRunRegistry (对齐 reply-run-registry.ts:62-75 + 370-442)
// ============================================================================
class ReplyRunRegistry {
public:
    static ReplyRunRegistry& instance();

    // 对齐: begin → createReplyOperation
    std::shared_ptr<ReplyOperation> begin(
        const std::string& session_key,
        const std::string& session_id,
        bool reset_triggered = false
    );

    // 对齐: get
    std::shared_ptr<ReplyOperation> get(const std::string& session_key);

    // 对齐: isActive
    bool is_active(const std::string& session_key);

    // 对齐: isStreaming
    bool is_streaming(const std::string& session_key);

    // 对齐: abort
    bool abort(const std::string& session_key);

    // 对齐: waitForIdle (blocking with timeout)
    bool wait_for_idle(const std::string& session_key, int timeout_ms = 15000);

    // 对齐: resolveSessionId
    std::optional<std::string> resolve_session_id(const std::string& session_key);

    // 对齐: getActiveReplyRunCount
    size_t get_active_count();

    // 对齐: abortActiveReplyRuns
    bool abort_active_runs(bool compacting_only = false);

    // Internal: called by ReplyOperation on completion
    void notify_completed(const std::string& session_key);

    // Testing
    void reset();

private:
    ReplyRunRegistry() = default;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<ReplyOperation>> active_runs_;
    std::map<std::string, std::string> session_ids_;  // key → session_id
    std::condition_variable idle_cv_;
};

}  // namespace react
}  // namespace hunterclaw
