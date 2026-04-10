#include "hunterclaw/react/reply_operation.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace react {

namespace {
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) logger = spdlog::stdout_logger_mt("react");
    return logger;
}
}

// ============================================================================
// ReplyOperation implementation
// ============================================================================

ReplyOperation::ReplyOperation(const std::string& key, const std::string& session_id, bool reset_triggered)
    : key_(key), session_id_(session_id), reset_triggered_(reset_triggered) {}

ReplyOperation::~ReplyOperation() = default;

const std::string& ReplyOperation::session_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return session_id_;
}

ReplyOperationPhase ReplyOperation::phase() const {
    std::lock_guard<std::mutex> lock(mu_);
    return phase_;
}

bool ReplyOperation::is_aborted() const {
    return aborted_.load();
}

void ReplyOperation::set_phase(ReplyOperationPhase next) {
    std::lock_guard<std::mutex> lock(mu_);
    if (completed_) return;
    phase_ = next;
}

void ReplyOperation::update_session_id(const std::string& next_session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (completed_ || next_session_id.empty()) return;
    session_id_ = next_session_id;
}

void ReplyOperation::attach_backend(std::shared_ptr<ReplyBackendHandle> handle) {
    std::lock_guard<std::mutex> lock(mu_);
    if (completed_ && handle && handle->cancel) {
        handle->cancel("superseded");
        return;
    }
    backend_ = std::move(handle);
    if (aborted_.load() && backend_ && backend_->cancel) {
        backend_->cancel("superseded");
    }
}

void ReplyOperation::detach_backend(std::shared_ptr<ReplyBackendHandle> handle) {
    std::lock_guard<std::mutex> lock(mu_);
    if (backend_ == handle) {
        backend_.reset();
    }
}

void ReplyOperation::complete() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!completed_) {
            completed_ = true;
            phase_ = ReplyOperationPhase::Completed;
        }
    }
    ReplyRunRegistry::instance().notify_completed(key_);
}

void ReplyOperation::fail(ReplyOperationFailureCode code) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!completed_) {
            completed_ = true;
            phase_ = ReplyOperationPhase::Failed;
        }
    }
    ReplyRunRegistry::instance().notify_completed(key_);
}

void ReplyOperation::abort_by_user() {
    aborted_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        phase_ = ReplyOperationPhase::Aborted;
        if (backend_ && backend_->cancel) {
            backend_->cancel("user_abort");
        }
        if (phase_ == ReplyOperationPhase::Queued) {
            completed_ = true;
        }
    }
    if (completed_) {
        ReplyRunRegistry::instance().notify_completed(key_);
    }
}

void ReplyOperation::abort_for_restart() {
    aborted_.store(true);
    {
        std::lock_guard<std::mutex> lock(mu_);
        phase_ = ReplyOperationPhase::Aborted;
        if (backend_ && backend_->cancel) {
            backend_->cancel("restart");
        }
        if (phase_ == ReplyOperationPhase::Queued) {
            completed_ = true;
        }
    }
    if (completed_) {
        ReplyRunRegistry::instance().notify_completed(key_);
    }
}

// ============================================================================
// ReplyRunRegistry implementation
// ============================================================================

ReplyRunRegistry& ReplyRunRegistry::instance() {
    static ReplyRunRegistry reg;
    return reg;
}

std::shared_ptr<ReplyOperation> ReplyRunRegistry::begin(
    const std::string& session_key,
    const std::string& session_id,
    bool reset_triggered
) {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_runs_.count(session_key)) {
        log()->warn("ReplyRunRegistry: already active for '{}'", session_key);
        return nullptr;
    }
    auto op = std::make_shared<ReplyOperation>(session_key, session_id, reset_triggered);
    active_runs_[session_key] = op;
    session_ids_[session_key] = session_id;
    return op;
}

std::shared_ptr<ReplyOperation> ReplyRunRegistry::get(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = active_runs_.find(session_key);
    if (it == active_runs_.end()) return nullptr;
    return it->second;
}

bool ReplyRunRegistry::is_active(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    return active_runs_.count(session_key) > 0;
}

bool ReplyRunRegistry::is_streaming(const std::string& session_key) {
    auto op = get(session_key);
    if (!op || op->phase() != ReplyOperationPhase::Running) return false;
    // Would check backend->is_streaming() but backend is private
    return true;  // If running, assume streaming
}

bool ReplyRunRegistry::abort(const std::string& session_key) {
    auto op = get(session_key);
    if (!op) return false;
    op->abort_by_user();
    return true;
}

bool ReplyRunRegistry::wait_for_idle(const std::string& session_key, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!active_runs_.count(session_key)) return true;

    auto pred = [this, &session_key]() {
        return active_runs_.count(session_key) == 0;
    };

    if (timeout_ms <= 0) {
        idle_cv_.wait(lock, pred);
        return true;
    }
    return idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
}

std::optional<std::string> ReplyRunRegistry::resolve_session_id(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = session_ids_.find(session_key);
    if (it == session_ids_.end()) return std::nullopt;
    return it->second;
}

size_t ReplyRunRegistry::get_active_count() {
    std::lock_guard<std::mutex> lock(mu_);
    return active_runs_.size();
}

bool ReplyRunRegistry::abort_active_runs(bool compacting_only) {
    std::lock_guard<std::mutex> lock(mu_);
    bool aborted = false;
    for (auto& [key, op] : active_runs_) {
        if (compacting_only) {
            auto phase = op->phase();
            if (phase != ReplyOperationPhase::PreflightCompacting &&
                phase != ReplyOperationPhase::MemoryFlushing) {
                continue;
            }
        }
        op->abort_for_restart();
        aborted = true;
    }
    return aborted;
}

void ReplyRunRegistry::notify_completed(const std::string& session_key) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        active_runs_.erase(session_key);
        session_ids_.erase(session_key);
    }
    idle_cv_.notify_all();
}

void ReplyRunRegistry::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    active_runs_.clear();
    session_ids_.clear();
    idle_cv_.notify_all();
}

}  // namespace react
}  // namespace hunterclaw
