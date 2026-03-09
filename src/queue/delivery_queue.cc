#include "hunterclaw/queue/delivery_queue.h"
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

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
    return payload;
}

static json to_json(const DeliveryTarget& target) {
    json j;
    j["channel"] = target.channel;
    j["to"] = target.to;
    if (target.account_id) j["accountId"] = *target.account_id;
    if (target.thread_id) j["threadId"] = *target.thread_id;
    if (target.reply_to_id) j["replyToId"] = *target.reply_to_id;
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
    delivery.target = target_from_json(j["target"]);
    
    for (const auto& p : j["payloads"]) {
        delivery.payloads.push_back(payload_from_json(p));
    }
    
    delivery.enqueued_at = j.value("enqueuedAt", 0ULL);
    delivery.retry_count = j.value("retryCount", 0);
    
    if (j.contains("lastAttemptAt")) {
        delivery.last_attempt_at = j["lastAttemptAt"].get<uint64_t>();
    }
    if (j.contains("lastError")) {
        delivery.last_error = j["lastError"].get<std::string>();
    }
    
    delivery.best_effort = j.value("bestEffort", false);
    delivery.silent = j.value("silent", false);
    
    return delivery;
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

std::string FileDeliveryQueue::get_failed_file_path(const std::string& delivery_id) const {
    return failed_dir_ + "/" + delivery_id + ".json";
}

core::Result<bool> FileDeliveryQueue::write_delivery(const QueuedDelivery& delivery) {
    try {
        std::string file_path = get_queue_file_path(delivery.id);
        std::string tmp_path = file_path + ".tmp";
        
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
    
    return core::Result<std::string>::Ok(delivery.id);
}

core::Result<bool> FileDeliveryQueue::ack(const std::string& delivery_id) {
    try {
        std::string file_path = get_queue_file_path(delivery_id);
        
        if (fs::exists(file_path)) {
            fs::remove(file_path);
        }
        
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err("Failed to ack delivery: " + 
                                       std::string(e.what()));
    }
}

core::Result<bool> FileDeliveryQueue::fail(
    const std::string& delivery_id,
    const std::string& error
) {
    std::string file_path = get_queue_file_path(delivery_id);
    auto result = read_delivery(file_path);
    
    if (result.is_err()) {
        return core::Result<bool>::Err(result.error);
    }
    
    QueuedDelivery delivery = result.value;
    delivery.retry_count++;
    delivery.last_attempt_at = current_timestamp_ms();
    delivery.last_error = error;
    
    // Check if max retries exceeded
    if (delivery.retry_count >= MAX_RETRIES) {
        // Move to failed directory
        try {
            std::string failed_path = get_failed_file_path(delivery_id);
            json j = to_json(delivery);
            std::ofstream ofs(failed_path);
            ofs << j.dump(2);
            ofs.close();
            
            fs::remove(file_path);
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err("Failed to move to failed dir: " + 
                                           std::string(e.what()));
        }
    }
    
    // Update queue entry with new retry count
    return write_delivery(delivery);
}

bool FileDeliveryQueue::should_retry(const QueuedDelivery& delivery) const {
    if (delivery.retry_count >= MAX_RETRIES) {
        return false;
    }
    
    if (!delivery.last_attempt_at) {
        return true;
    }
    
    // Check if backoff period has elapsed
    int backoff_ms = get_backoff_ms(delivery.retry_count);
    uint64_t now = current_timestamp_ms();
    uint64_t elapsed = now - *delivery.last_attempt_at;
    
    return elapsed >= static_cast<uint64_t>(backoff_ms);
}

int FileDeliveryQueue::get_backoff_ms(int retry_count) const {
    if (retry_count <= 0 || retry_count > 4) {
        return BACKOFF_MS[3]; // Use max backoff
    }
    return BACKOFF_MS[retry_count - 1];
}

core::Result<std::vector<QueuedDelivery>> FileDeliveryQueue::load_pending() {
    try {
        std::vector<QueuedDelivery> deliveries;
        
        for (const auto& entry : fs::directory_iterator(queue_dir_)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                auto result = read_delivery(entry.path().string());
                if (result.is_ok()) {
                    deliveries.push_back(result.value);
                }
            }
        }
        
        return core::Result<std::vector<QueuedDelivery>>::Ok(deliveries);
    } catch (const std::exception& e) {
        return core::Result<std::vector<QueuedDelivery>>::Err(
            "Failed to load pending deliveries: " + std::string(e.what()));
    }
}

core::Result<RecoverySummary> FileDeliveryQueue::recover(DeliveryCallback callback) {
    RecoverySummary summary;
    
    auto result = load_pending();
    if (result.is_err()) {
        return core::Result<RecoverySummary>::Err(result.error);
    }
    
    for (const auto& delivery : result.value) {
        // Check if should retry
        if (!should_retry(delivery)) {
            if (delivery.retry_count >= MAX_RETRIES) {
                summary.skipped_max_retries++;
            } else {
                summary.deferred_backoff++;
            }
            continue;
        }
        
        // Attempt delivery
        auto delivery_result = callback(delivery);
        
        if (delivery_result.is_ok() && delivery_result.value) {
            // Success - ack
            ack(delivery.id);
            summary.recovered++;
        } else {
            // Failed - update retry count
            std::string error = delivery_result.is_err() ? 
                delivery_result.error : "Unknown error";
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
                // TODO: Generate summary (complex, skip for now)
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
