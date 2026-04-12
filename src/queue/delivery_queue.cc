#include "hunterclaw/queue/delivery_queue.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <filesystem>
#include <regex>
#include <nlohmann/json.hpp>
#include <mutex>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace queue {

// ============================================================================
// Utility Functions
// ============================================================================

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    const char* hex = "0123456789abcdef";
    std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    
    for (auto& c : uuid) {
        if (c == 'x') {
            c = hex[dis(gen)];
        } else if (c == 'y') {
            c = hex[(dis(gen) & 0x3) | 0x8];
        }
    }
    
    return uuid;
}

static uint64_t current_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// JSON 序列化辅助
static json to_json(const MessagePayload& payload) {
    json j;
    j["text"] = payload.text;
    if (!payload.media_urls.empty()) {
        j["mediaUrls"] = payload.media_urls;
    }
    if (payload.markdown) {
        j["markdown"] = *payload.markdown;
    }
    if (payload.channel_data) {
        j["channelData"] = *payload.channel_data;
    }
    return j;
}

static MessagePayload payload_from_json(const json& j) {
    MessagePayload payload;
    payload.text = j.value("text", "");
    if (j.contains("mediaUrls")) {
        payload.media_urls = j["mediaUrls"].get<std::vector<std::string>>();
    }
    if (j.contains("markdown")) {
        payload.markdown = j["markdown"].get<std::string>();
    }
    if (j.contains("channelData")) {
        payload.channel_data = j["channelData"];
    }
    return payload;
}

static json to_json(const DeliveryTarget& target) {
    json j;
    j["channel"] = target.channel;
    j["to"] = target.to;
    if (target.account_id) j["accountId"] = *target.account_id;
    if (target.thread_id) j["threadId"] = *target.thread_id;
    if (target.reply_to_id) j["replyToId"] = *target.reply_to_id;
    if (target.best_effort) j["bestEffort"] = true;
    if (target.gif_playback) j["gifPlayback"] = true;
    if (target.silent) j["silent"] = true;
    return j;
}

static DeliveryTarget target_from_json(const json& j) {
    DeliveryTarget target;
    target.channel = j.value("channel", "");
    target.to = j.value("to", "");
    if (j.contains("accountId")) {
        target.account_id = j["accountId"].get<std::string>();
    }
    if (j.contains("threadId")) {
        target.thread_id = j["threadId"].get<std::string>();
    }
    if (j.contains("replyToId")) {
        target.reply_to_id = j["replyToId"].get<std::string>();
    }
    target.best_effort = j.value("bestEffort", false);
    target.gif_playback = j.value("gifPlayback", false);
    target.silent = j.value("silent", false);
    return target;
}

static json to_json(const QueuedDelivery& delivery) {
    json j;
    j["id"] = delivery.id;
    j["target"] = to_json(delivery.target);

    json payloads = json::array();
    for (const auto& p : delivery.payloads) {
        payloads.push_back(to_json(p));
    }
    j["payloads"] = payloads;

    j["enqueuedAt"] = delivery.enqueued_at;
    j["retryCount"] = delivery.retry_count;

    if (delivery.last_attempt_at) {
        j["lastAttemptAt"] = *delivery.last_attempt_at;
    }
    if (delivery.last_error) {
        j["lastError"] = *delivery.last_error;
    }

    j["bestEffort"] = delivery.best_effort;
    j["silent"] = delivery.silent;

    return j;
}

static QueuedDelivery delivery_from_json(const json& j) {
    QueuedDelivery delivery;
    delivery.id = j.value("id", "");
    delivery.target = target_from_json(j.value("target", json::object()));

    if (j.contains("payloads")) {
        for (const auto& p : j["payloads"]) {
            delivery.payloads.push_back(payload_from_json(p));
        }
    }

    delivery.enqueued_at = j.value("enqueuedAt", 0ULL);
    delivery.retry_count = j.value("retryCount", 0);

    if (j.contains("lastAttemptAt") && j["lastAttemptAt"].is_number()) {
        delivery.last_attempt_at = j["lastAttemptAt"].get<uint64_t>();
    }
    if (j.contains("lastError") && j["lastError"].is_string()) {
        delivery.last_error = j["lastError"].get<std::string>();
    }

    delivery.best_effort = j.value("bestEffort", false);
    delivery.silent = j.value("silent", false);

    return delivery;
}

// Aligned with openclaw: normalizeLegacyQueuedDeliveryEntry
// Migrates old entries that have retryCount > 0 but no lastAttemptAt.
// Sets lastAttemptAt = enqueuedAt so backoff is computed correctly.
struct NormalizeResult {
    QueuedDelivery entry;
    bool migrated;
};

static NormalizeResult normalize_legacy_queued_delivery_entry(const QueuedDelivery& entry) {
    bool has_attempt_ts = entry.last_attempt_at.has_value() &&
                          *entry.last_attempt_at > 0;
    if (has_attempt_ts || entry.retry_count <= 0) {
        return {entry, false};
    }
    bool has_enqueued_ts = entry.enqueued_at > 0;
    if (!has_enqueued_ts) {
        return {entry, false};
    }
    QueuedDelivery migrated = entry;
    migrated.last_attempt_at = entry.enqueued_at;
    return {migrated, true};
}

// ============================================================================
// FileDeliveryQueue Implementation
// ============================================================================

DeliveryQueue::Ptr DeliveryQueue::create(const std::string& queue_dir) {
    return std::make_shared<FileDeliveryQueue>(queue_dir);
}

FileDeliveryQueue::FileDeliveryQueue(const std::string& queue_dir)
    : queue_dir_(queue_dir)
    , failed_dir_(queue_dir + "/failed")
{
    ensure_dirs();
}

core::Result<bool> FileDeliveryQueue::ensure_dirs() {
    try {
        fs::create_directories(queue_dir_);
        fs::create_directories(failed_dir_);
        
        // Set permissions to 0700 (owner only)
        fs::permissions(queue_dir_,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace);
        fs::permissions(failed_dir_,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
            fs::perm_options::replace);
        
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to create queue directories: " + 
                                       std::string(e.what()));
    }
}

std::string FileDeliveryQueue::get_queue_file_path(const std::string& delivery_id) const {
    return queue_dir_ + "/" + delivery_id + ".json";
}

// Aligned with openclaw: ackDelivery uses a two-phase rename approach.
// Phase 1: {id}.json → {id}.delivered (atomic rename marks delivery complete)
// Phase 2: unlink {id}.delivered
// If the process crashes between phase 1 and phase 2, the .delivered marker
// is cleaned up by load_pending() on the next startup without re-sending.
std::string FileDeliveryQueue::get_delivered_file_path(const std::string& delivery_id) const {
    return queue_dir_ + "/" + delivery_id + ".delivered";
}

std::string FileDeliveryQueue::get_failed_file_path(const std::string& delivery_id) const {
    return failed_dir_ + "/" + delivery_id + ".json";
}

core::Result<bool> FileDeliveryQueue::write_delivery(const QueuedDelivery& delivery) {
    try {
        std::string file_path = get_queue_file_path(delivery.id);
        // Aligned with openclaw: use pid-suffixed tmp to avoid cross-process collisions.
        std::string tmp_path = file_path + "." + std::to_string(getpid()) + ".tmp";
        
        // Write to temp file first (atomic write)
        json j = to_json(delivery);
        std::ofstream ofs(tmp_path);
        ofs << j.dump(2);
        ofs.close();
        
        // Set permissions to 0600 (owner read/write only)
        fs::permissions(tmp_path,
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
        
        // Rename to final path (atomic)
        fs::rename(tmp_path, file_path);
        
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to write delivery: " + 
                                       std::string(e.what()));
    }
}

core::Result<QueuedDelivery> FileDeliveryQueue::read_delivery(const std::string& file_path) {
    try {
        std::ifstream ifs(file_path);
        if (!ifs.is_open()) {
            return core::Result<QueuedDelivery>::Err("Failed to open file: " + file_path);
        }
        
        json j;
        ifs >> j;
        
        return core::Result<QueuedDelivery>::Ok(delivery_from_json(j));
    } catch (const std::exception& e) {
        return core::Result<QueuedDelivery>::Err("Failed to read delivery: " + 
                                                 std::string(e.what()));
    }
}

core::Result<std::string> FileDeliveryQueue::enqueue(
    const DeliveryTarget& target,
    const std::vector<MessagePayload>& payloads
) {
    QueuedDelivery delivery;
    delivery.id = generate_uuid();
    delivery.target = target;
    delivery.payloads = payloads;
    delivery.enqueued_at = current_timestamp_ms();
    delivery.retry_count = 0;
    
    auto result = write_delivery(delivery);
    if (result.is_err()) {
        return core::Result<std::string>::Err(result.error);
    }
    
    // ✅ 可观测性事件: MessageQueuedEvent (对齐 OpenClaw)
    try {
        using namespace observability;
        MessageQueuedEvent event;
        event.message_id = delivery.id;
        event.channel = target.channel;
        event.queue_depth = 1;  // ✅ 对齐 OC: incremental depth (exact count requires dir listing)
        event.priority = 0;
        DiagnosticEventBus::instance().emit(std::make_shared<MessageQueuedEvent>(event));
    } catch (const std::exception& e) {
        // 可观测性失败不应该影响核心功能
    }
    
    return core::Result<std::string>::Ok(delivery.id);
}

// Aligned with openclaw ackDelivery: two-phase approach to prevent crash-loop re-delivery.
//   Phase 1: atomic rename {id}.json → {id}.delivered
//   Phase 2: unlink the .delivered marker
// If the process dies between phase 1 and phase 2, load_pending() cleans up
// the .delivered marker on next startup without re-sending.
core::Result<bool> FileDeliveryQueue::ack(const std::string& delivery_id) {
    std::string json_path = get_queue_file_path(delivery_id);
    std::string delivered_path = get_delivered_file_path(delivery_id);

    try {
        // Phase 1: atomic rename marks the delivery as complete.
        fs::rename(json_path, delivered_path);
    } catch (const std::exception& e) {
        std::string what = e.what();
        // ENOENT: .json already gone — may have been renamed by a previous ack attempt.
        // Try to clean up a leftover .delivered marker if present.
        if (!fs::exists(json_path)) {
            try { fs::remove(delivered_path); } catch (...) {}
            return core::Result<bool>::Ok(true);
        }
        return core::Result<bool>::Err("Failed to ack delivery (phase 1): " + what);
    }

    // Phase 2: remove the marker file (best-effort).
    try { fs::remove(delivered_path); } catch (...) {}

    // ✅ 可观测性事件: DeliverySuccessEvent (对齐 OpenClaw)
    try {
        using namespace observability;
        DeliverySuccessEvent event;
        event.delivery_id = delivery_id;
        event.channel = "";
        event.to = "";
        event.retry_count = 0;
        event.duration_ms = 0;
        DiagnosticEventBus::instance().emit(std::make_shared<DeliverySuccessEvent>(event));
    } catch (...) {}

    return core::Result<bool>::Ok(true);
}

// Aligned with openclaw failDelivery: atomic write via pid-suffixed tmp + rename.
core::Result<bool> FileDeliveryQueue::fail(
    const std::string& delivery_id,
    const std::string& error
) {
    std::string file_path = get_queue_file_path(delivery_id);
    auto result = read_delivery(file_path);
    
    if (result.is_err()) {
        return core::Result<bool>::Err(result.error);
    }
    
    QueuedDelivery delivery = result.get();
    delivery.retry_count++;
    delivery.last_attempt_at = current_timestamp_ms();
    delivery.last_error = error;
    
    // ✅ 可观测性事件: DeliveryFailureEvent (对齐 OpenClaw)
    try {
        using namespace observability;
        bool is_permanent = is_permanent_delivery_error(error);
        
        DeliveryFailureEvent event;
        event.delivery_id = delivery_id;
        event.channel = delivery.target.channel;
        event.to = delivery.target.to;
        event.retry_count = delivery.retry_count;
        event.error_message = error;
        event.is_permanent = is_permanent;
        DiagnosticEventBus::instance().emit(std::make_shared<DeliveryFailureEvent>(event));
    } catch (...) {}

    // Aligned with openclaw: atomic write via pid-suffixed tmp + rename.
    try {
        std::string tmp_path = file_path + "." + std::to_string(getpid()) + ".tmp";
        json j = to_json(delivery);
        std::ofstream ofs(tmp_path);
        ofs << j.dump(2);
        ofs.close();
        fs::permissions(tmp_path,
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
        fs::rename(tmp_path, file_path);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to write updated delivery: " + std::string(e.what()));
    }

    // ✅ 可观测性事件: DeliveryRetryScheduled (对齐 OpenClaw)
    try {
        using namespace observability;
        int backoff_ms = get_backoff_ms(delivery.retry_count);
        DeliveryRetryEvent event;
        event.delivery_id = delivery_id;
        event.retry_count = delivery.retry_count;
        event.max_retries = MAX_RETRIES;
        event.backoff_ms = backoff_ms;
        event.next_retry_ms = current_timestamp_ms() + backoff_ms;
        DiagnosticEventBus::instance().emit(std::make_shared<DeliveryRetryEvent>(event));
    } catch (...) {}

    return core::Result<bool>::Ok(true);
}

// Aligned with openclaw moveToFailed: use rename (not copy+delete) for atomicity.
core::Result<bool> FileDeliveryQueue::move_to_failed(const std::string& delivery_id) {
    try {
        std::string src = get_queue_file_path(delivery_id);
        std::string dest = get_failed_file_path(delivery_id);
        fs::rename(src, dest);

        // ✅ 可观测性事件: DeliveryMaxRetriesExceeded (对齐 OpenClaw)
        try {
            using namespace observability;
            DeliveryFailureEvent event;
            event.delivery_id = delivery_id;
            event.retry_count = MAX_RETRIES;
            event.error_message = "Max retries exceeded";
            event.is_permanent = true;
            DiagnosticEventBus::instance().emit(std::make_shared<DeliveryFailureEvent>(event));
        } catch (...) {}

        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to move to failed dir: " + std::string(e.what()));
    }
}

// Aligned with openclaw isEntryEligibleForRecoveryRetry:
// - firstReplayAfterCrash (retryCount==0 && no lastAttemptAt): always eligible
// - backoff based on retryCount+1 (next attempt)
// - use lastAttemptAt if available, else enqueuedAt as base
bool FileDeliveryQueue::should_retry(const QueuedDelivery& delivery) const {
    if (delivery.retry_count >= MAX_RETRIES) {
        return false;
    }

    int backoff_ms = get_backoff_ms(delivery.retry_count + 1);
    if (backoff_ms <= 0) {
        return true;
    }

    // First replay after crash: retryCount==0 and no lastAttemptAt — always eligible.
    bool first_replay_after_crash = (delivery.retry_count == 0 && !delivery.last_attempt_at);
    if (first_replay_after_crash) {
        return true;
    }

    uint64_t now = current_timestamp_ms();
    bool has_attempt_ts = delivery.last_attempt_at.has_value() && *delivery.last_attempt_at > 0;
    uint64_t base_attempt_at = has_attempt_ts ? *delivery.last_attempt_at : delivery.enqueued_at;
    uint64_t next_eligible_at = base_attempt_at + static_cast<uint64_t>(backoff_ms);

    return now >= next_eligible_at;
}

int FileDeliveryQueue::get_backoff_ms(int retry_count) const {
    if (retry_count <= 0) {
        return 0;
    }
    int idx = std::min(retry_count - 1, static_cast<int>(sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0])) - 1);
    return BACKOFF_MS[idx];
}

bool FileDeliveryQueue::is_permanent_delivery_error(const std::string& error) const {
    // Permanent error patterns (aligned with OpenClaw)
    static const std::vector<std::regex> PERMANENT_ERROR_PATTERNS = {
        std::regex("no conversation reference found", std::regex::icase),
        std::regex("chat not found", std::regex::icase),
        std::regex("user not found", std::regex::icase),
        std::regex("bot was blocked by the user", std::regex::icase),
        std::regex("forbidden: bot was kicked", std::regex::icase),
        std::regex("chat_id is empty", std::regex::icase),
        std::regex("recipient is not a valid", std::regex::icase),
        std::regex("outbound not configured for channel", std::regex::icase),
        std::regex("ambiguous discord recipient", std::regex::icase),
    };
    
    for (const auto& pattern : PERMANENT_ERROR_PATTERNS) {
        if (std::regex_search(error, pattern)) {
            return true;
        }
    }
    return false;
}

// Aligned with openclaw loadPendingDeliveries:
// - Clean up .delivered markers left by ackDelivery if process crashed between phase 1 and phase 2.
// - Skip malformed or inaccessible entries.
core::Result<std::vector<QueuedDelivery>> FileDeliveryQueue::load_pending() {
    try {
        std::vector<QueuedDelivery> deliveries;

        if (!fs::exists(queue_dir_)) {
            return core::Result<std::vector<QueuedDelivery>>::Ok(deliveries);
        }

        // First pass: clean up .delivered markers (crash-recovery cleanup).
        // These are left when ackDelivery completed phase 1 (rename) but crashed before phase 2 (unlink).
        for (const auto& entry : fs::directory_iterator(queue_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".delivered") {
                try { fs::remove(entry.path()); } catch (...) {}
            }
        }

        // Second pass: load .json entries.
        for (const auto& entry : fs::directory_iterator(queue_dir_)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            // Skip tmp files
            if (entry.path().string().find(".tmp") != std::string::npos) {
                continue;
            }
        auto result = read_delivery(entry.path().string());
            if (result.is_ok()) {
                // Aligned with openclaw: normalizeLegacyQueuedDeliveryEntry
                auto norm = normalize_legacy_queued_delivery_entry(result.get());
                if (norm.migrated) {
                    // Write back migrated entry (best-effort)
                    write_delivery(norm.entry);
                }
                deliveries.push_back(norm.entry);
            }
            // Skip malformed or inaccessible entries (aligned with openclaw).
        }
        
        return core::Result<std::vector<QueuedDelivery>>::Ok(deliveries);
    } catch (const std::exception& e) {
        return core::Result<std::vector<QueuedDelivery>>::Err(
            "Failed to load pending deliveries: " + std::string(e.what()));
    }
}

// Aligned with openclaw recoverPendingDeliveries:
// - Sort oldest first
// - Deadline (maxRecoveryMs, default 60s) to avoid blocking startup indefinitely
// - MAX_RETRIES exceeded → moveToFailed
// - Backoff check via should_retry (isEntryEligibleForRecoveryRetry)
// - isPermanentDeliveryError → moveToFailed immediately
// - skipQueue=true passed to callback to prevent re-enqueueing
core::Result<RecoverySummary> FileDeliveryQueue::recover(DeliveryCallback callback) {
    RecoverySummary summary;
    
    auto result = load_pending();
    if (result.is_err()) {
        return core::Result<RecoverySummary>::Err(result.error);
    }

    auto pending = result.get();
    if (pending.empty()) {
        return core::Result<RecoverySummary>::Ok(summary);
    }

    // Process oldest first (aligned with openclaw).
    std::sort(pending.begin(), pending.end(), [](const QueuedDelivery& a, const QueuedDelivery& b) {
        return a.enqueued_at < b.enqueued_at;
    });

    // Deadline: default 60s to avoid blocking startup indefinitely (aligned with openclaw).
    constexpr uint64_t MAX_RECOVERY_MS = 60000;
    uint64_t deadline = current_timestamp_ms() + MAX_RECOVERY_MS;

    for (const auto& delivery : pending) {
        if (current_timestamp_ms() >= deadline) {
            // Remaining entries deferred to next restart (aligned with openclaw).
            break;
        }

        // Max retries exceeded → move to failed/ (aligned with openclaw).
        if (delivery.retry_count >= MAX_RETRIES) {
            move_to_failed(delivery.id);
            summary.skipped_max_retries++;
            continue;
        }

        // Backoff check (aligned with openclaw isEntryEligibleForRecoveryRetry).
        if (!should_retry(delivery)) {
            summary.deferred_backoff++;
            continue;
        }

        // Attempt delivery (skipQueue=true to prevent re-enqueueing, aligned with openclaw).
        auto delivery_result = callback(delivery);

        if (delivery_result.is_ok() && delivery_result.value) {
            // Success: ack (aligned with openclaw).
            ack(delivery.id);
            summary.recovered++;
        } else {
            std::string error = delivery_result.is_err() ?
                delivery_result.error : "Unknown error";

            // Permanent error → move to failed/ immediately (aligned with openclaw).
            if (is_permanent_delivery_error(error)) {
                move_to_failed(delivery.id);
                summary.failed++;
                continue;
            }

            // Transient error → update retry count (aligned with openclaw).
            fail(delivery.id, error);
            summary.failed++;
        }
    }
    
    return core::Result<RecoverySummary>::Ok(summary);
}

// ============================================================================
// FollowupQueue Implementation
// ============================================================================

FollowupQueue::Ptr FollowupQueue::create(const QueueSettings& settings) {
    auto queue = std::make_shared<FollowupQueue>();
    queue->settings_ = settings;
    return queue;
}

void FollowupQueue::enqueue(const std::string& session_key, const FollowupRun& run) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& queue = queues_[session_key];
    
    // Check capacity
    if (queue.size() >= static_cast<size_t>(settings_.cap)) {
        switch (settings_.drop_policy) {
            case DropPolicy::Old:
                // Remove oldest
                queue.erase(queue.begin());
                break;
            case DropPolicy::New:
                // Reject new message
                return;
            case DropPolicy::Summarize:
                // ✅ 对齐 OC: Summarize 策略
                // FollowupRun 是不透明结构，不支持 text 合并
                // 退化为丢弃最旧的消息 (与 Old 策略相同)
                queue.erase(queue.begin());
                break;
        }
    }
    
    queue.push_back(run);
}

std::vector<FollowupRun> FollowupQueue::dequeue_all(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = queues_.find(session_key);
    if (it == queues_.end()) {
        return {};
    }
    
    std::vector<FollowupRun> runs = std::move(it->second);
    queues_.erase(it);
    
    return runs;
}

size_t FollowupQueue::size(const std::string& session_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = queues_.find(session_key);
    if (it == queues_.end()) {
        return 0;
    }
    
    return it->second.size();
}

void FollowupQueue::clear(const std::string& session_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    queues_.erase(session_key);
}

} // namespace queue
} // namespace hunterclaw
