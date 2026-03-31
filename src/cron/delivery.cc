// Aligned with openclaw: src/cron/isolated-agent/delivery-dispatch.ts
//
// DeliveryService::deliver_announce:
//   对齐 OpenClaw dispatchCronDelivery → deliverViaDirect → deliverOutboundPayloads
//   通过 ChannelPluginRegistry 找到对应 channel 的 ChannelOutboundAdapter 并调用 send_text
//
// DeliveryService::deliver_webhook:
//   对齐 OpenClaw CronDelivery.mode="webhook" → HTTP POST

#include "hunterclaw/cron/delivery.h"
#include "hunterclaw/outbound/deliver.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/infra/http_client.h"
#include <spdlog/spdlog.h>
#include <regex>
#include <sstream>
#include <chrono>
#include <mutex>
#include <map>
#include <thread>

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = []() {
            auto l = spdlog::get("cron_delivery");
            if (!l) l = spdlog::stdout_logger_mt("cron_delivery");
            return l;
        }();
        return logger;
    }

    // ── normalizeDeliveryTarget ──────────────────────────────────────────────
    // 对齐 OpenClaw delivery-dispatch.ts: normalizeDeliveryTarget(channel, to)
    // feishu/lark: strip "user:" / "chat:" prefix
    std::string normalize_delivery_target(const std::string& channel, const std::string& to) {
        std::string ch = channel;
        std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);
        std::string to_trimmed = to;
        while (!to_trimmed.empty() && to_trimmed.front() == ' ') to_trimmed.erase(0, 1);
        while (!to_trimmed.empty() && to_trimmed.back() == ' ') to_trimmed.pop_back();

        if (ch == "feishu" || ch == "lark") {
            std::string lower = to_trimmed;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.rfind("user:", 0) == 0) return to_trimmed.substr(5);
            if (lower.rfind("chat:", 0) == 0) return to_trimmed.substr(5);
        }
        return to_trimmed;
    }

    // ── 错误分类 (对齐 OpenClaw PERMANENT/TRANSIENT patterns) ─────────────────
    // 对齐 OpenClaw: PERMANENT_DIRECT_CRON_DELIVERY_ERROR_PATTERNS
    bool is_permanent_delivery_error(const std::string& msg) {
        static const std::vector<std::regex> patterns = {
            std::regex(R"(unsupported channel)", std::regex::icase),
            std::regex(R"(unknown channel)", std::regex::icase),
            std::regex(R"(chat not found)", std::regex::icase),
            std::regex(R"(user not found)", std::regex::icase),
            std::regex(R"(bot was blocked by the user)", std::regex::icase),
            std::regex(R"(forbidden: bot was kicked)", std::regex::icase),
            std::regex(R"(recipient is not a valid)", std::regex::icase),
            std::regex(R"(outbound not configured for channel)", std::regex::icase),
        };
        for (const auto& re : patterns) {
            if (std::regex_search(msg, re)) return true;
        }
        return false;
    }

    // 对齐 OpenClaw: TRANSIENT_DIRECT_CRON_DELIVERY_ERROR_PATTERNS
    bool is_transient_delivery_error(const std::string& msg) {
        if (is_permanent_delivery_error(msg)) return false;
        static const std::vector<std::regex> patterns = {
            std::regex(R"(\berrorcode=unavailable\b)", std::regex::icase),
            std::regex(R"(\bstatus\s*[:=]\s*"?unavailable\b)", std::regex::icase),
            std::regex(R"(\bUNAVAILABLE\b)"),
            std::regex(R"(no active .* listener)", std::regex::icase),
            std::regex(R"(gateway not connected)", std::regex::icase),
            std::regex(R"(gateway closed \(1006)", std::regex::icase),
            std::regex(R"(gateway timeout)", std::regex::icase),
            std::regex(R"(\b(econnreset|econnrefused|etimedout|enotfound|ehostunreach|network error)\b)", std::regex::icase),
        };
        for (const auto& re : patterns) {
            if (std::regex_search(msg, re)) return true;
        }
        return false;
    }

    // ── 幂等性缓存 (对齐 OpenClaw COMPLETED_DIRECT_CRON_DELIVERIES) ───────────
    // 对齐 OpenClaw: 24h TTL, max 2000 entries
    struct CompletedDeliveryEntry {
        int64_t ts_ms;
        bool delivered;
    };

    std::mutex g_delivery_cache_mutex;
    std::map<std::string, CompletedDeliveryEntry> g_completed_deliveries;
    static constexpr int64_t DELIVERY_CACHE_TTL_MS = 24LL * 60 * 60 * 1000;
    static constexpr size_t DELIVERY_CACHE_MAX_ENTRIES = 2000;

    int64_t now_ms_impl() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    void prune_delivery_cache(int64_t now) {
        // TTL prune
        for (auto it = g_completed_deliveries.begin(); it != g_completed_deliveries.end(); ) {
            if (now - it->second.ts_ms >= DELIVERY_CACHE_TTL_MS) {
                it = g_completed_deliveries.erase(it);
            } else {
                ++it;
            }
        }
        // Size prune: remove oldest entries
        while (g_completed_deliveries.size() > DELIVERY_CACHE_MAX_ENTRIES) {
            int64_t oldest_ts = std::numeric_limits<int64_t>::max();
            auto oldest_it = g_completed_deliveries.begin();
            for (auto it = g_completed_deliveries.begin(); it != g_completed_deliveries.end(); ++it) {
                if (it->second.ts_ms < oldest_ts) {
                    oldest_ts = it->second.ts_ms;
                    oldest_it = it;
                }
            }
            g_completed_deliveries.erase(oldest_it);
        }
    }

    bool get_completed_delivery(const std::string& key) {
        std::lock_guard<std::mutex> lock(g_delivery_cache_mutex);
        prune_delivery_cache(now_ms_impl());
        auto it = g_completed_deliveries.find(key);
        if (it == g_completed_deliveries.end()) return false;
        return it->second.delivered;
    }

    void remember_completed_delivery(const std::string& key, bool delivered) {
        if (!delivered) return;  // 只缓存成功的 delivery
        std::lock_guard<std::mutex> lock(g_delivery_cache_mutex);
        g_completed_deliveries[key] = {now_ms_impl(), delivered};
        prune_delivery_cache(now_ms_impl());
    }

    // ── 构建幂等性 key (对齐 OpenClaw buildDirectCronDeliveryIdempotencyKey) ──
    std::string build_delivery_idempotency_key(
        const std::string& job_id,
        const std::string& run_session_id,
        const std::string& channel,
        const std::string& to
    ) {
        return job_id + ":" + run_session_id + ":" + channel + ":" + to;
    }
}

// ============================================================================
// DeliveryService Implementation
// ============================================================================

DeliveryService::DeliveryService(Dependencies deps)
    : deps_(std::move(deps)) {
}

DeliveryService::~DeliveryService() = default;

core::Result<bool> DeliveryService::deliver(
    const Delivery& delivery,
    const Job& job,
    const std::string& result_text
) {
    if (delivery.mode == DeliveryMode::None) {
        log()->debug("[cron:{}] delivery mode=none, skipping", job.id);
        return core::Result<bool>::Ok(true);
    }

    if (delivery.mode == DeliveryMode::Announce) {
        return deliver_announce(delivery, job, result_text);
    } else if (delivery.mode == DeliveryMode::Webhook) {
        return deliver_webhook(delivery, job, result_text);
    }

    return core::Result<bool>::Err("Unknown delivery mode");
}

// ============================================================================
// deliver_announce
// 对齐 OpenClaw: dispatchCronDelivery → deliverViaDirect → deliverOutboundPayloads
//
// 关键对齐点:
//   1. normalizeDeliveryTarget (feishu/lark strip prefix)
//   2. skipQueue=true (isolated cron direct delivery 不走 write-ahead queue)
//   3. 幂等性缓存 (COMPLETED_DIRECT_CRON_DELIVERIES)
//   4. 重试机制 (retryTransientDirectCronDelivery: 5s/10s/20s)
//   5. 错误分类 (永久错误不重试, 瞬时错误重试)
// ============================================================================

core::Result<bool> DeliveryService::deliver_announce(
    const Delivery& delivery,
    const Job& job,
    const std::string& result_text
) {
    // ── 参数校验 ──────────────────────────────────────────────────────────────
    if (!delivery.channel.has_value() || delivery.channel->empty()) {
        return core::Result<bool>::Err("Missing 'channel' for Announce delivery");
    }
    if (!delivery.to.has_value() || delivery.to->empty()) {
        return core::Result<bool>::Err("Missing 'to' for Announce delivery");
    }

    const std::string& channel_id = *delivery.channel;
    const std::string& raw_to = *delivery.to;

    // ── normalizeDeliveryTarget (对齐 OpenClaw) ───────────────────────────────
    std::string to = normalize_delivery_target(channel_id, raw_to);
    std::string account_id = delivery.account_id.value_or("");

    log()->info("[cron:{}] Announce delivery: channel={} to={} account={}",
                job.id, channel_id, to, account_id);

    // ── 检查 channel outbound adapter 是否存在 ────────────────────────────────
    // 对齐 OpenClaw: "Outbound not configured for channel" (永久错误)
    auto plugin_opt = channels::ChannelPluginRegistry::instance().get_plugin(channel_id);
    if (!plugin_opt.has_value() || !plugin_opt->outbound.has_value()) {
        std::string err = "Outbound not configured for channel: " + channel_id;
        log()->error("[cron:{}] {}", job.id, err);
        return core::Result<bool>::Err(err);
    }

    // ── 构建 payload (对齐 OpenClaw deliveryPayloads) ─────────────────────────
    // 对齐 OpenClaw: payloadsForDelivery = deliveryPayloads.length > 0
    //   ? deliveryPayloads : synthesizedText ? [{ text: synthesizedText }] : []
    std::string message_text = result_text.empty()
        ? format_announce_message(job, result_text)
        : result_text;

    if (message_text.empty()) {
        // 对齐 OpenClaw: if (payloadsForDelivery.length === 0) return null
        log()->debug("[cron:{}] Announce: empty payload, skipping", job.id);
        return core::Result<bool>::Ok(false);
    }

    queue::MessagePayload payload;
    payload.text = message_text;

    // ── 幂等性检查 (对齐 OpenClaw getCompletedDirectCronDelivery) ─────────────
    // 使用 job.id + run_session_id + channel + to 作为幂等性 key
    // 注意: run_session_id 在 C++ 中用 job.id + last_run_at_ms 代替
    std::string run_session_id = job.id + ":" + std::to_string(
        job.state.last_run_at_ms.value_or(0)
    );
    std::string idempotency_key = build_delivery_idempotency_key(
        job.id, run_session_id, channel_id, to
    );

    if (get_completed_delivery(idempotency_key)) {
        // 对齐 OpenClaw: cached entries are only recorded after a successful delivery
        log()->info("[cron:{}] Announce: idempotent skip (already delivered)", job.id);
        return core::Result<bool>::Ok(true);
    }

    // ── 构建 DeliverOutboundPayloadsParams ────────────────────────────────────
    // 对齐 OpenClaw: deliverOutboundPayloads({ cfg, channel, to, accountId,
    //   threadId, payloads, session, identity, bestEffort, skipQueue: true })
    outbound::DeliverOutboundPayloadsParams params;
    params.channel = channel_id;
    params.to = to;
    if (!account_id.empty()) params.account_id = account_id;
    params.payloads = {payload};
    params.best_effort = false;
    // 对齐 OpenClaw: skipQueue=true for isolated cron direct delivery
    // "Keep all attempts out of the write-ahead delivery queue so a
    //  late-successful first send cannot leave behind a failed queue entry
    //  that replays on the next restart." (openclaw #40545)
    params.skip_queue = true;
    params.delivery_queue = deps_.delivery_queue;

    // ── 重试机制 (对齐 OpenClaw retryTransientDirectCronDelivery) ─────────────
    // 对齐 OpenClaw: retry delays = [5000, 10000, 20000] ms
    static const std::vector<int64_t> RETRY_DELAYS_MS = {5000, 10000, 20000};

    bool delivered = false;
    std::string last_error;

    for (int attempt = 0; attempt <= static_cast<int>(RETRY_DELAYS_MS.size()); ++attempt) {
        try {
            auto result = outbound::deliver_outbound_payloads(params);

            if (result.is_err()) {
                last_error = result.error;
                // 对齐 OpenClaw: 永久错误不重试
                if (is_permanent_delivery_error(last_error)) {
                    log()->error("[cron:{}] Announce permanent error: {}", job.id, last_error);
                    return core::Result<bool>::Err(last_error);
                }
                // 对齐 OpenClaw: 瞬时错误重试
                if (is_transient_delivery_error(last_error) && attempt < static_cast<int>(RETRY_DELAYS_MS.size())) {
                    int64_t delay = RETRY_DELAYS_MS[attempt];
                    log()->warn("[cron:{}] transient direct announce delivery failure, retrying {}/{} in {}s: {}",
                                job.id, attempt + 2, RETRY_DELAYS_MS.size() + 1,
                                delay / 1000, last_error);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    continue;
                }
                // 非瞬时错误或已达最大重试次数
                log()->error("[cron:{}] Announce delivery failed: {}", job.id, last_error);
                return core::Result<bool>::Err(last_error);
            }

            const auto& delivery_results = result.get();
            delivered = !delivery_results.empty();

            if (delivered) {
                // 对齐 OpenClaw: rememberCompletedDirectCronDelivery
                remember_completed_delivery(idempotency_key, true);
                log()->info("[cron:{}] Announce delivered successfully via channel={} to={}",
                            job.id, channel_id, to);
            } else {
                log()->warn("[cron:{}] Announce: no delivery results (empty payloads?)", job.id);
            }

            return core::Result<bool>::Ok(delivered);

        } catch (const std::exception& e) {
            last_error = e.what();
            if (is_permanent_delivery_error(last_error)) {
                log()->error("[cron:{}] Announce permanent error: {}", job.id, last_error);
                return core::Result<bool>::Err(last_error);
            }
            if (is_transient_delivery_error(last_error) && attempt < static_cast<int>(RETRY_DELAYS_MS.size())) {
                int64_t delay = RETRY_DELAYS_MS[attempt];
                log()->warn("[cron:{}] transient direct announce delivery failure, retrying {}/{} in {}s: {}",
                            job.id, attempt + 2, RETRY_DELAYS_MS.size() + 1,
                            delay / 1000, last_error);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                continue;
            }
            log()->error("[cron:{}] Announce delivery exception: {}", job.id, last_error);
            return core::Result<bool>::Err(last_error);
        }
    }

    return core::Result<bool>::Err("Announce delivery failed after retries: " + last_error);
}

// ============================================================================
// deliver_webhook
// 对齐 OpenClaw: CronDelivery.mode="webhook" → HTTP POST
// ============================================================================

core::Result<bool> DeliveryService::deliver_webhook(
    const Delivery& delivery,
    const Job& job,
    const std::string& result_text
) {
    if (!delivery.webhook_url.has_value() || delivery.webhook_url->empty()) {
        return core::Result<bool>::Err("Missing 'webhook_url' for Webhook delivery");
    }

    std::string url = *delivery.webhook_url;

    log()->info("[cron:{}] Webhook delivery: url={}", job.id, url);

    // SSRF Protection (对齐 OpenClaw)
    if (!is_safe_webhook_url(url)) {
        return core::Result<bool>::Err("Webhook URL blocked by SSRF protection: " + url);
    }

    // Construct webhook payload
    nlohmann::json webhook_payload;
    webhook_payload["event"] = "cron.job.completed";
    webhook_payload["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    nlohmann::json job_json;
    job_json["id"] = job.id;
    job_json["name"] = job.name;
    job_json["scheduleKind"] = schedule_kind_to_string(job.schedule.kind);
    job_json["enabled"] = job.enabled;
    if (job.state.last_run_at_ms.has_value()) {
        job_json["lastRunAtMs"] = *job.state.last_run_at_ms;
    }
    if (job.state.last_run_status.has_value()) {
        job_json["lastRunStatus"] = *job.state.last_run_status;
    }
    if (job.state.next_run_at_ms.has_value()) {
        job_json["nextRunAtMs"] = *job.state.next_run_at_ms;
    }
    webhook_payload["job"] = job_json;

    nlohmann::json result_json;
    result_json["text"] = result_text;
    result_json["success"] = (job.state.last_run_status.value_or("") == "ok");
    if (job.state.last_run_error.has_value()) {
        result_json["error"] = *job.state.last_run_error;
    }
    webhook_payload["result"] = result_json;

    try {
        auto response = send_webhook_post(url, webhook_payload);

        if (response.is_err()) {
            log()->error("[cron:{}] Webhook delivery failed: {}", job.id, response.error);
            return core::Result<bool>::Err(response.error);
        }

        log()->info("[cron:{}] Webhook delivered successfully to {}", job.id, url);
        return core::Result<bool>::Ok(true);

    } catch (const std::exception& e) {
        log()->error("[cron:{}] Webhook exception: {}", job.id, e.what());
        return core::Result<bool>::Err(std::string("Webhook delivery failed: ") + e.what());
    }
}

// ============================================================================
// SSRF Protection (对齐 OpenClaw)
// ============================================================================

bool DeliveryService::is_safe_webhook_url(const std::string& url) {
    static const std::vector<std::regex> blocked_patterns = {
        std::regex(R"(^https?://localhost)", std::regex::icase),
        std::regex(R"(^https?://127\.)", std::regex::icase),
        std::regex(R"(^https?://192\.168\.)", std::regex::icase),
        std::regex(R"(^https?://10\.)", std::regex::icase),
        std::regex(R"(^https?://172\.(1[6-9]|2[0-9]|3[0-1])\.)", std::regex::icase),
        std::regex(R"(^https?://169\.254\.)", std::regex::icase),
        std::regex(R"(^https?://0\.)", std::regex::icase),
        std::regex(R"(^https?://\[::1\])", std::regex::icase),
        std::regex(R"(^https?://\[fe80:)", std::regex::icase),
    };

    for (const auto& pattern : blocked_patterns) {
        if (std::regex_search(url, pattern)) {
            log()->warn("Blocked SSRF attempt: {}", url);
            return false;
        }
    }

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        log()->warn("Blocked non-HTTP webhook: {}", url);
        return false;
    }

    return true;
}

// ============================================================================
// HTTP Client (使用 libcurl)
// ============================================================================

core::Result<std::string> DeliveryService::send_webhook_post(
    const std::string& url,
    const nlohmann::json& payload
) {
    // 使用 infra::HttpClient 替代 libcurl
    auto http = infra::HttpClient::create();

    infra::HttpRequestOptions opts;
    opts.headers["Content-Type"] = "application/json";
    opts.headers["User-Agent"] = "HunterClaw-Cron/1.0";
    opts.body = payload.dump();
    opts.timeout_ms = 10000;  // 10s

    auto result = http->post(url, opts);
    if (!result.is_ok()) {
        return core::Result<std::string>::Err(result.error);
    }

    const auto& resp = result.get();
    if (!resp.ok) {
        return core::Result<std::string>::Err(
            "HTTP " + std::to_string(resp.status_code) + ": " + resp.body
        );
    }

    return core::Result<std::string>::Ok(resp.body);
}

// ============================================================================
// Helpers
// ============================================================================

std::string DeliveryService::format_announce_message(
    const Job& job,
    const std::string& result_text
) {
    // 对齐 OpenClaw: cron direct delivery 直接发送 synthesizedText
    // 不做额外格式化包装，保持与 agent 输出一致
    if (!result_text.empty()) {
        return result_text;
    }

    // Fallback: 如果没有 result_text，构建简单摘要
    std::ostringstream oss;
    oss << "Cron job \"" << job.name << "\" completed";
    if (job.state.last_run_status.has_value()) {
        oss << " [" << *job.state.last_run_status << "]";
    }
    if (job.state.last_run_error.has_value()) {
        oss << "\nError: " << *job.state.last_run_error;
    }
    return oss.str();
}

std::string DeliveryService::schedule_kind_to_string(ScheduleKind kind) {
    switch (kind) {
        case ScheduleKind::At:    return "at";
        case ScheduleKind::Every: return "every";
        case ScheduleKind::Cron:  return "cron";
        default:                  return "unknown";
    }
}

} // namespace cron
} // namespace hunterclaw
