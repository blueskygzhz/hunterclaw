// ============================================================================
// delivery_queue_storage.cc
// 严格对齐 OpenClaw: infra/outbound/delivery-queue-storage.ts (269行)
//
// 文件系统 JSON 持久化投递队列:
//   - enqueueDelivery: 写入 {stateDir}/delivery-queue/{id}.json
//   - ackDelivery: 删除 JSON 文件（可选写 .delivered sentinel）
//   - failDelivery: 更新 retryCount/lastError/lastAttemptAt
//   - moveToFailed: 移到 failed/ 子目录
//   - loadPendingDelivery: 读取单个队列条目
//   - loadPendingDeliveries: 扫描目录加载全部 pending 条目
//   - ensureQueueDir: 创建队列目录
// ============================================================================

#include "hunterclaw/outbound/deliver.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <random>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace outbound {

namespace {

static const std::string QUEUE_DIRNAME = "delivery-queue";
static const std::string FAILED_DIRNAME = "failed";

auto log() {
    static auto logger = []() {
        auto l = spdlog::get("outbound/queue-storage");
        if (!l) l = spdlog::stdout_logger_mt("outbound/queue-storage");
        return l;
    }();
    return logger;
}

// 对齐 OC: generateSecureUuid (简化版 — UUID v4)
static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) uuid += '-';
        else if (i == 14) uuid += '4';
        else if (i == 19) uuid += hex[(dis(gen) & 0x3) | 0x8];
        else uuid += hex[dis(gen)];
    }
    return uuid;
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string resolve_state_dir() {
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) + "/.hunterclaw" : ".hunterclaw";
    return base;
}

} // namespace

// ============================================================================
// QueuedDelivery — JSON 持久化结构
// ============================================================================

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

// ============================================================================
// Path resolution (对齐 delivery-queue-storage.ts:41-62)
// ============================================================================

std::string resolve_queue_dir(const std::string& state_dir) {
    std::string base = state_dir.empty() ? resolve_state_dir() : state_dir;
    return (fs::path(base) / QUEUE_DIRNAME).string();
}

static std::string resolve_failed_dir(const std::string& state_dir) {
    return (fs::path(resolve_queue_dir(state_dir)) / FAILED_DIRNAME).string();
}

struct QueueEntryPaths {
    std::string json_path;
    std::string delivered_path;
};

static QueueEntryPaths resolve_queue_entry_paths(
    const std::string& id, const std::string& state_dir
) {
    auto queue_dir = resolve_queue_dir(state_dir);
    return {
        (fs::path(queue_dir) / (id + ".json")).string(),
        (fs::path(queue_dir) / (id + ".delivered")).string()
    };
}

// ============================================================================
// ensureQueueDir (对齐 delivery-queue-storage.ts:113-115)
// ============================================================================

void ensure_queue_dir(const std::string& state_dir) {
    auto dir = resolve_queue_dir(state_dir);
    fs::create_directories(dir);
    auto failed_dir = resolve_failed_dir(state_dir);
    fs::create_directories(failed_dir);
}

// ============================================================================
// Serialize/Deserialize helpers
// ============================================================================

static json payload_to_json(const queue::MessagePayload& p) {
    json j;
    j["text"] = p.text;
    if (!p.media_url.empty()) j["mediaUrl"] = p.media_url;
    if (!p.media_urls.empty()) j["mediaUrls"] = p.media_urls;
    if (p.reply_to_id.has_value() && !p.reply_to_id->empty()) j["replyToId"] = *p.reply_to_id;
    if (p.silent) j["silent"] = true;
    if (p.channel_data.has_value()) j["channelData"] = *p.channel_data;
    return j;
}

static queue::MessagePayload payload_from_json(const json& j) {
    queue::MessagePayload p;
    p.text = j.value("text", "");
    if (j.contains("mediaUrl")) p.media_url = j["mediaUrl"].get<std::string>();
    if (j.contains("mediaUrls") && j["mediaUrls"].is_array()) {
        for (const auto& u : j["mediaUrls"]) {
            if (u.is_string()) p.media_urls.push_back(u.get<std::string>());
        }
    }
    if (j.contains("replyToId") && j["replyToId"].is_string())
        p.reply_to_id = j["replyToId"].get<std::string>();
    if (j.contains("silent")) p.silent = j["silent"].get<bool>();
    if (j.contains("channelData") && j["channelData"].is_object()) {
        p.channel_data = j["channelData"];
    }
    return p;
}

static json entry_to_json(const QueuedDelivery& e) {
    json j;
    j["id"] = e.id;
    j["channel"] = e.channel;
    j["to"] = e.to;
    if (e.account_id.has_value() && !e.account_id->empty()) j["accountId"] = *e.account_id;
    j["payloads"] = json::array();
    for (const auto& p : e.payloads) j["payloads"].push_back(payload_to_json(p));
    if (e.thread_id.has_value() && !e.thread_id->empty()) j["threadId"] = *e.thread_id;
    if (e.reply_to_id.has_value() && !e.reply_to_id->empty()) j["replyToId"] = *e.reply_to_id;
    j["bestEffort"] = e.best_effort;
    j["gifPlayback"] = e.gif_playback;
    j["forceDocument"] = e.force_document;
    j["silent"] = e.silent;
    j["enqueuedAt"] = e.enqueued_at;
    j["retryCount"] = e.retry_count;
    if (e.last_attempt_at > 0) j["lastAttemptAt"] = e.last_attempt_at;
    if (!e.last_error.empty()) j["lastError"] = e.last_error;
    if (e.mirror) {
        json m;
        m["sessionKey"] = e.mirror->session_key;
        if (!e.mirror->agent_id.empty()) m["agentId"] = e.mirror->agent_id;
        if (!e.mirror->text.empty()) m["text"] = e.mirror->text;
        if (!e.mirror->media_urls.empty()) m["mediaUrls"] = e.mirror->media_urls;
        if (e.mirror->idempotency_key) m["idempotencyKey"] = *e.mirror->idempotency_key;
        if (e.mirror->is_group) m["isGroup"] = true;
        if (e.mirror->group_id) m["groupId"] = *e.mirror->group_id;
        j["mirror"] = m;
    }
    return j;
}

static QueuedDelivery entry_from_json(const json& j) {
    QueuedDelivery e;
    e.id = j.value("id", "");
    e.channel = j.value("channel", "");
    e.to = j.value("to", "");
    if (j.contains("accountId") && j["accountId"].is_string())
        e.account_id = j["accountId"].get<std::string>();
    if (j.contains("payloads") && j["payloads"].is_array()) {
        for (const auto& pj : j["payloads"]) e.payloads.push_back(payload_from_json(pj));
    }
    if (j.contains("threadId") && j["threadId"].is_string())
        e.thread_id = j["threadId"].get<std::string>();
    if (j.contains("replyToId") && j["replyToId"].is_string())
        e.reply_to_id = j["replyToId"].get<std::string>();
    e.best_effort = j.value("bestEffort", false);
    e.gif_playback = j.value("gifPlayback", false);
    e.force_document = j.value("forceDocument", false);
    e.silent = j.value("silent", false);
    e.enqueued_at = j.value("enqueuedAt", int64_t(0));
    e.retry_count = j.value("retryCount", 0);
    e.last_attempt_at = j.value("lastAttemptAt", int64_t(0));
    e.last_error = j.value("lastError", "");
    // Normalize legacy entries without lastAttemptAt
    if (e.last_attempt_at <= 0 && e.retry_count > 0) {
        e.last_attempt_at = e.enqueued_at > 0 ? e.enqueued_at : now_ms();
    }
    if (j.contains("mirror") && j["mirror"].is_object()) {
        DeliveryMirror m;
        m.session_key = j["mirror"].value("sessionKey", "");
        m.agent_id = j["mirror"].value("agentId", "");
        m.text = j["mirror"].value("text", "");
        if (j["mirror"].contains("mediaUrls") && j["mirror"]["mediaUrls"].is_array()) {
            for (const auto& u : j["mirror"]["mediaUrls"])
                if (u.is_string()) m.media_urls.push_back(u.get<std::string>());
        }
        if (j["mirror"].contains("idempotencyKey"))
            m.idempotency_key = j["mirror"]["idempotencyKey"].get<std::string>();
        m.is_group = j["mirror"].value("isGroup", false);
        if (j["mirror"].contains("groupId"))
            m.group_id = j["mirror"]["groupId"].get<std::string>();
        e.mirror = m;
    }
    return e;
}

// ============================================================================
// Atomic file write (对齐 delivery-queue-storage.ts:78-85)
// ============================================================================

static bool write_queue_entry(const std::string& path, const QueuedDelivery& entry) {
    try {
        auto tmp = path + "." + std::to_string(getpid()) + ".tmp";
        {
            std::ofstream f(tmp);
            if (!f.is_open()) return false;
            f << entry_to_json(entry).dump(2);
        }
        fs::rename(tmp, path);
        return true;
    } catch (...) { return false; }
}

static std::optional<QueuedDelivery> read_queue_entry(const std::string& path) {
    try {
        std::ifstream f(path);
        if (!f.is_open()) return std::nullopt;
        json j = json::parse(f);
        return entry_from_json(j);
    } catch (...) { return std::nullopt; }
}

// ============================================================================
// enqueueDelivery (对齐 delivery-queue-storage.ts:117-143)
// ============================================================================

std::string enqueue_delivery(
    const std::string& state_dir,
    const std::string& channel,
    const std::string& to,
    const std::vector<queue::MessagePayload>& payloads,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& thread_id,
    const std::optional<std::string>& reply_to_id,
    bool best_effort,
    bool gif_playback,
    bool silent,
    const std::optional<DeliveryMirror>& mirror
) {
    ensure_queue_dir(state_dir);
    QueuedDelivery entry;
    entry.id = generate_uuid();
    entry.channel = channel;
    entry.to = to;
    entry.payloads = payloads;
    entry.account_id = account_id;
    entry.thread_id = thread_id;
    entry.reply_to_id = reply_to_id;
    entry.best_effort = best_effort;
    entry.gif_playback = gif_playback;
    entry.silent = silent;
    entry.mirror = mirror;
    entry.enqueued_at = now_ms();
    entry.retry_count = 0;

    auto paths = resolve_queue_entry_paths(entry.id, state_dir);
    if (!write_queue_entry(paths.json_path, entry)) {
        log()->warn("enqueue_delivery: failed to write queue entry {}", entry.id);
        return "";
    }
    return entry.id;
}

// ============================================================================
// ackDelivery (对齐 delivery-queue-storage.ts:145-165)
// ============================================================================

void ack_delivery(const std::string& state_dir, const std::string& id) {
    if (id.empty()) return;
    auto paths = resolve_queue_entry_paths(id, state_dir);
    try { fs::remove(paths.json_path); } catch (...) {}
    // Best-effort: write .delivered sentinel (for debugging)
    try {
        std::ofstream f(paths.delivered_path);
        f << now_ms();
    } catch (...) {}
}

// ============================================================================
// failDelivery (对齐 delivery-queue-storage.ts:167-201)
// ============================================================================

void fail_delivery(const std::string& state_dir, const std::string& id,
                   const std::string& error) {
    if (id.empty()) return;
    auto paths = resolve_queue_entry_paths(id, state_dir);
    auto entry = read_queue_entry(paths.json_path);
    if (!entry) return;

    entry->retry_count++;
    entry->last_error = error;
    entry->last_attempt_at = now_ms();
    write_queue_entry(paths.json_path, *entry);
}

// ============================================================================
// moveToFailed (对齐 delivery-queue-storage.ts:203-220)
// ============================================================================

void move_to_failed(const std::string& state_dir, const std::string& id) {
    if (id.empty()) return;
    auto paths = resolve_queue_entry_paths(id, state_dir);
    auto failed_dir = resolve_failed_dir(state_dir);
    fs::create_directories(failed_dir);
    auto dest = (fs::path(failed_dir) / (id + ".json")).string();
    try { fs::rename(paths.json_path, dest); } catch (...) {
        try { fs::remove(paths.json_path); } catch (...) {}
    }
}

// ============================================================================
// loadPendingDelivery (对齐 delivery-queue-storage.ts:222-232)
// ============================================================================

std::optional<QueuedDelivery> load_pending_delivery(
    const std::string& state_dir, const std::string& id
) {
    auto paths = resolve_queue_entry_paths(id, state_dir);
    return read_queue_entry(paths.json_path);
}

// ============================================================================
// loadPendingDeliveries (对齐 delivery-queue-storage.ts:234-269)
// ============================================================================

std::vector<QueuedDelivery> load_pending_deliveries(const std::string& state_dir) {
    std::vector<QueuedDelivery> result;
    auto queue_dir = resolve_queue_dir(state_dir);
    if (!fs::exists(queue_dir)) return result;

    try {
        for (const auto& entry : fs::directory_iterator(queue_dir)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().string();
            // Skip .delivered sentinels and non-json files
            if (name.size() < 6 || name.substr(name.size() - 5) != ".json") continue;
            // Skip failed/ subdirectory entries
            if (name.find(".tmp") != std::string::npos) continue;

            auto loaded = read_queue_entry(entry.path().string());
            if (loaded) {
                result.push_back(*loaded);
            }
        }
    } catch (...) {}

    // Sort by enqueuedAt ascending (FIFO)
    std::sort(result.begin(), result.end(),
        [](const QueuedDelivery& a, const QueuedDelivery& b) {
            return a.enqueued_at < b.enqueued_at;
        });
    return result;
}

} // namespace outbound
} // namespace hunterclaw
