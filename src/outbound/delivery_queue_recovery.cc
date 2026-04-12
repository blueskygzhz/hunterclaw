// ============================================================================
// delivery_queue_recovery.cc
// 严格对齐 OpenClaw: infra/outbound/delivery-queue-recovery.ts (418行)
//
// 投递队列恢复系统:
//   - computeBackoffMs: 指数退避计算 (5s, 25s, 2m, 10m)
//   - isPermanentDeliveryError: 永久错误模式匹配
//   - isEntryEligibleForRecoveryRetry: 退避检查
//   - recoverPendingDeliveries: 扫描 → 重试 → ack/fail/moveToFailed
//   - drainPendingDeliveries: 条件性排空（重连场景）
// ============================================================================

#include "hunterclaw/outbound/deliver.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <regex>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace outbound {

// Full QueuedDelivery definition (mirrors delivery_queue_storage.cc)
struct QueuedDelivery {
    std::string id;
    std::string channel;
    std::string to;
    std::optional<std::string> account_id;
    std::vector<queue::MessagePayload> payloads;
    std::optional<std::string> thread_id;
    std::optional<std::string> reply_to_id;
    bool best_effort = false;
    bool gif_playback = false;
    bool force_document = false;
    bool silent = false;
    std::optional<DeliveryMirror> mirror;
    int64_t enqueued_at = 0;
    int retry_count = 0;
    int64_t last_attempt_at = 0;
    std::string last_error;
};

// External functions from delivery_queue_storage.cc
std::vector<QueuedDelivery> load_pending_deliveries(const std::string& state_dir);
void ack_delivery(const std::string& state_dir, const std::string& id);
void fail_delivery(const std::string& state_dir, const std::string& id, const std::string& error);
void move_to_failed(const std::string& state_dir, const std::string& id);

namespace {

auto log() {
    static auto logger = []() {
        auto l = spdlog::get("outbound/queue-recovery");
        if (!l) l = spdlog::stdout_logger_mt("outbound/queue-recovery");
        return l;
    }();
    return logger;
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// 对齐 delivery-queue-recovery.ts:39
static constexpr int MAX_RETRIES = 5;

// 对齐 delivery-queue-recovery.ts:42-47
// Backoff delays in ms indexed by retry count (1-based)
static const std::vector<int64_t> BACKOFF_MS = {
    5000,      // retry 1: 5s
    25000,     // retry 2: 25s
    120000,    // retry 3: 2m
    600000,    // retry 4: 10m
};

// 对齐 delivery-queue-recovery.ts:49-61
static const std::vector<std::regex> PERMANENT_ERROR_PATTERNS = {
    std::regex("no conversation reference found", std::regex::icase),
    std::regex("chat not found", std::regex::icase),
    std::regex("user not found", std::regex::icase),
    std::regex("bot.*not.*member", std::regex::icase),
    std::regex("bot was blocked by the user", std::regex::icase),
    std::regex("forbidden: bot was kicked", std::regex::icase),
    std::regex("chat_id is empty", std::regex::icase),
    std::regex("recipient is not a valid", std::regex::icase),
    std::regex("outbound not configured for channel", std::regex::icase),
    std::regex("ambiguous .* recipient", std::regex::icase),
    std::regex("User .* not in room", std::regex::icase),
};

// 对齐 delivery-queue-recovery.ts:63-64
static std::mutex drain_mu;
static std::set<std::string> drain_in_progress;
static std::set<std::string> entries_in_progress;

} // namespace

// ============================================================================
// computeBackoffMs (对齐 delivery-queue-recovery.ts:72-78)
// ============================================================================

int64_t compute_backoff_ms(int retry_count) {
    if (retry_count <= 0) return 0;
    int idx = std::min(retry_count - 1, static_cast<int>(BACKOFF_MS.size()) - 1);
    return BACKOFF_MS[idx];
}

// ============================================================================
// isPermanentDeliveryError (对齐 delivery-queue-recovery.ts:80-86)
// ============================================================================

bool is_permanent_delivery_error(const std::string& error) {
    if (error.empty()) return false;
    for (const auto& pattern : PERMANENT_ERROR_PATTERNS) {
        if (std::regex_search(error, pattern)) return true;
    }
    return false;
}

// ============================================================================
// isEntryEligibleForRecoveryRetry (对齐 delivery-queue-recovery.ts:88-110)
// ============================================================================

bool is_entry_eligible_for_recovery_retry(
    int retry_count, int64_t last_attempt_at, int64_t now
) {
    if (retry_count >= MAX_RETRIES) return false;
    if (retry_count <= 0) return true;  // never attempted
    auto backoff = compute_backoff_ms(retry_count);
    return (now - last_attempt_at) >= backoff;
}

// ============================================================================
// RecoverySummary
// ============================================================================

struct RecoverySummary {
    int recovered = 0;
    int failed = 0;
    int skipped_max_retries = 0;
    int deferred_backoff = 0;
};

// ============================================================================
// recoverPendingDeliveries (对齐 delivery-queue-recovery.ts:112-210)
// ============================================================================

RecoverySummary recover_pending_deliveries(
    const std::string& state_dir,
    std::function<bool(const std::string& channel, const std::string& to,
                       const std::vector<queue::MessagePayload>& payloads,
                       bool skip_queue)> deliver_fn
) {
    RecoverySummary summary;
    auto now = now_ms();

    // 对齐: loadPendingDeliveries → sort by enqueuedAt ASC
    auto entries = load_pending_deliveries(state_dir);
    if (entries.empty()) return summary;

    log()->info("recover_pending_deliveries: found {} pending entries", entries.size());

    for (const auto& entry : entries) {
        if (entry.id.empty()) continue;

        // Claim entry (prevent concurrent recovery)
        {
            std::lock_guard<std::mutex> lock(drain_mu);
            if (entries_in_progress.count(entry.id)) continue;
            entries_in_progress.insert(entry.id);
        }

        // Check permanent error
        if (!entry.last_error.empty() && is_permanent_delivery_error(entry.last_error)) {
            log()->info("recover: permanent error for {} — moving to failed: {}",
                        entry.id, entry.last_error);
            move_to_failed(state_dir, entry.id);
            summary.failed++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        // Check max retries
        if (entry.retry_count >= MAX_RETRIES) {
            log()->info("recover: max retries ({}) exceeded for {} — moving to failed",
                        MAX_RETRIES, entry.id);
            move_to_failed(state_dir, entry.id);
            summary.skipped_max_retries++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        // Check backoff
        if (!is_entry_eligible_for_recovery_retry(entry.retry_count, entry.last_attempt_at, now)) {
            summary.deferred_backoff++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        // Attempt recovery delivery
        try {
            bool ok = deliver_fn(entry.channel, entry.to, entry.payloads, true /*skipQueue*/);
            if (ok) {
                ack_delivery(state_dir, entry.id);
                summary.recovered++;
                log()->info("recover: successfully recovered delivery {}", entry.id);
            } else {
                fail_delivery(state_dir, entry.id, "recovery delivery returned false");
                summary.failed++;
            }
        } catch (const std::exception& e) {
            std::string err = e.what();
            if (is_permanent_delivery_error(err)) {
                move_to_failed(state_dir, entry.id);
                summary.failed++;
            } else {
                fail_delivery(state_dir, entry.id, err);
                summary.failed++;
            }
        }

        std::lock_guard<std::mutex> lock(drain_mu);
        entries_in_progress.erase(entry.id);
    }

    return summary;
}

// ============================================================================
// drainPendingDeliveries (对齐 delivery-queue-recovery.ts:212-310)
// ============================================================================

struct PendingDeliveryDrainDecision {
    bool match = false;
    bool bypass_backoff = false;
};

RecoverySummary drain_pending_deliveries(
    const std::string& state_dir,
    std::function<PendingDeliveryDrainDecision(const std::string& channel,
                                               const std::string& to)> matcher,
    std::function<bool(const std::string& channel, const std::string& to,
                       const std::vector<queue::MessagePayload>& payloads,
                       bool skip_queue)> deliver_fn
) {
    RecoverySummary summary;
    auto now = now_ms();

    // Prevent concurrent drains for the same state_dir
    {
        std::lock_guard<std::mutex> lock(drain_mu);
        if (drain_in_progress.count(state_dir)) return summary;
        drain_in_progress.insert(state_dir);
    }

    auto entries = load_pending_deliveries(state_dir);

    for (const auto& entry : entries) {
        if (entry.id.empty()) continue;

        auto decision = matcher(entry.channel, entry.to);
        if (!decision.match) continue;

        {
            std::lock_guard<std::mutex> lock(drain_mu);
            if (entries_in_progress.count(entry.id)) continue;
            entries_in_progress.insert(entry.id);
        }

        // Check permanent error
        if (!entry.last_error.empty() && is_permanent_delivery_error(entry.last_error)) {
            move_to_failed(state_dir, entry.id);
            summary.failed++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        // Check max retries
        if (entry.retry_count >= MAX_RETRIES) {
            move_to_failed(state_dir, entry.id);
            summary.skipped_max_retries++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        // Check backoff (unless bypassed)
        if (!decision.bypass_backoff &&
            !is_entry_eligible_for_recovery_retry(entry.retry_count, entry.last_attempt_at, now)) {
            summary.deferred_backoff++;
            std::lock_guard<std::mutex> lock(drain_mu);
            entries_in_progress.erase(entry.id);
            continue;
        }

        try {
            bool ok = deliver_fn(entry.channel, entry.to, entry.payloads, true);
            if (ok) {
                ack_delivery(state_dir, entry.id);
                summary.recovered++;
            } else {
                fail_delivery(state_dir, entry.id, "drain delivery returned false");
                summary.failed++;
            }
        } catch (const std::exception& e) {
            std::string err = e.what();
            if (is_permanent_delivery_error(err)) {
                move_to_failed(state_dir, entry.id);
            } else {
                fail_delivery(state_dir, entry.id, err);
            }
            summary.failed++;
        }

        std::lock_guard<std::mutex> lock(drain_mu);
        entries_in_progress.erase(entry.id);
    }

    {
        std::lock_guard<std::mutex> lock(drain_mu);
        drain_in_progress.erase(state_dir);
    }

    return summary;
}

} // namespace outbound
} // namespace hunterclaw
