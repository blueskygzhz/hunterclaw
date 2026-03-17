#include "hunterclaw/cron/delivery.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <regex>

namespace hunterclaw {
namespace cron {

namespace {
    auto log() {
        static auto logger = spdlog::get("cron_delivery");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("cron_delivery");
        }
        return logger;
    }
    
    // Curl write callback
    size_t curl_write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
        size_t total_size = size * nmemb;
        output->append(static_cast<char*>(contents), total_size);
        return total_size;
    }
}

// ============================================================================
// DeliveryService Implementation
// ============================================================================

DeliveryService::DeliveryService(Dependencies deps)
    : deps_(std::move(deps)) {
    if (!deps_.channel_manager) {
        log()->warn("ChannelManager not provided, Announce delivery will not work");
    }
}

DeliveryService::~DeliveryService() = default;

core::Result<bool> DeliveryService::deliver(
    const Delivery& delivery,
    const Job& job,
    const std::string& result_text
) {
    if (delivery.mode == DeliveryMode::None) {
        log()->debug("Delivery mode is None, skipping");
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
// Announce Delivery (对齐 OpenClaw)
// ============================================================================

core::Result<bool> DeliveryService::deliver_announce(
    const Delivery& delivery,
    const Job& job,
    const std::string& result_text
) {
    if (!deps_.channel_manager) {
        return core::Result<bool>::Err("ChannelManager not available");
    }
    
    if (!delivery.channel.has_value() || delivery.channel->empty()) {
        return core::Result<bool>::Err("Missing 'channel' for Announce delivery");
    }
    
    if (!delivery.to.has_value() || delivery.to->empty()) {
        return core::Result<bool>::Err("Missing 'to' for Announce delivery");
    }
    
    std::string channel_id = *delivery.channel;
    std::string to = *delivery.to;
    std::string account_id = delivery.account_id.value_or("default");
    
    log()->info("Deliver Announce: channel={}, to={}, account={}", 
                channel_id, to, account_id);
    
    // Parse 'to' field: "channel:C123" or "user:U456"
    std::regex to_regex(R"((channel|user):(.+))");
    std::smatch match;
    
    if (!std::regex_match(to, match, to_regex)) {
        return core::Result<bool>::Err("Invalid 'to' format, expected 'channel:ID' or 'user:ID'");
    }
    
    std::string to_type = match[1].str();  // "channel" or "user"
    std::string to_id = match[2].str();    // Channel ID or User ID
    
    // Construct message payload
    nlohmann::json message_payload;
    message_payload["text"] = format_announce_message(job, result_text);
    message_payload["type"] = "cron_result";
    message_payload["job_id"] = job.id;
    message_payload["job_name"] = job.name;
    
    if (to_type == "channel") {
        message_payload["channel_id"] = to_id;
    } else if (to_type == "user") {
        message_payload["user_id"] = to_id;
    }
    
    // Send through channel
    // TODO: Implement channel message sending when Channel interface is ready
    try {
        log()->info("Would send message to {} via channel {} (not yet implemented)", to, channel_id);
        // For now, just log the delivery attempt
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        log()->error("Failed to send Announce: {}", e.what());
        return core::Result<bool>::Err(std::string("Announce delivery failed: ") + e.what());
    }
}

// ============================================================================
// Webhook Delivery (对齐 OpenClaw)
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
    
    log()->info("Deliver Webhook: url={}", url);
    
    // SSRF Protection (对齐 OpenClaw)
    if (!is_safe_webhook_url(url)) {
        return core::Result<bool>::Err("Webhook URL blocked by SSRF protection: " + url);
    }
    
    // Construct webhook payload
    nlohmann::json webhook_payload;
    webhook_payload["event"] = "cron.job.completed";
    webhook_payload["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
    
    webhook_payload["job"] = {
        {"id", job.id},
        {"name", job.name},
        {"schedule_kind", schedule_kind_to_string(job.schedule.kind)},
        {"enabled", job.enabled}
    };
    
    if (job.state.last_run_at_ms.has_value()) {
        webhook_payload["job"]["last_run_at_ms"] = *job.state.last_run_at_ms;
    }
    
    if (job.state.last_run_status.has_value()) {
        webhook_payload["job"]["last_run_status"] = *job.state.last_run_status;
    }
    
    if (job.state.next_run_at_ms.has_value()) {
        webhook_payload["job"]["next_run_at_ms"] = *job.state.next_run_at_ms;
    }
    
    webhook_payload["result"] = {
        {"text", result_text},
        {"success", job.state.last_run_status == "ok"}
    };
    
    if (job.state.last_run_error.has_value()) {
        webhook_payload["result"]["error"] = *job.state.last_run_error;
    }
    
    // Send HTTP POST request
    try {
        auto response = send_webhook_post(url, webhook_payload);
        
        if (!response.is_ok()) {
            log()->error("Webhook delivery failed: {}", response.error);
            return core::Result<bool>::Err(response.error);
        }
        
        log()->info("Webhook delivered successfully to {}", url);
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        log()->error("Webhook exception: {}", e.what());
        return core::Result<bool>::Err(std::string("Webhook delivery failed: ") + e.what());
    }
}

// ============================================================================
// SSRF Protection (对齐 OpenClaw)
// ============================================================================

bool DeliveryService::is_safe_webhook_url(const std::string& url) {
    // Block private IP ranges
    std::vector<std::regex> blocked_patterns = {
        std::regex(R"(^https?://localhost)", std::regex::icase),
        std::regex(R"(^https?://127\.)", std::regex::icase),
        std::regex(R"(^https?://192\.168\.)", std::regex::icase),
        std::regex(R"(^https?://10\.)", std::regex::icase),
        std::regex(R"(^https?://172\.(1[6-9]|2[0-9]|3[0-1])\.)", std::regex::icase),
        std::regex(R"(^https?://169\.254\.)", std::regex::icase),  // Link-local
        std::regex(R"(^https?://0\.)", std::regex::icase),
        std::regex(R"(^https?://\[::1\])", std::regex::icase),     // IPv6 localhost
        std::regex(R"(^https?://\[fe80:)", std::regex::icase),     // IPv6 link-local
    };
    
    for (const auto& pattern : blocked_patterns) {
        if (std::regex_search(url, pattern)) {
            log()->warn("Blocked SSRF attempt: {}", url);
            return false;
        }
    }
    
    // Only allow http/https
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
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::Result<std::string>::Err("Failed to initialize CURL");
    }
    
    std::string response_body;
    std::string payload_str = payload.dump();
    
    // Set CURL options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);  // 10 second timeout
    
    // Set headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: HunterClaw-Cron/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        std::string error = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return core::Result<std::string>::Err("CURL error: " + error);
    }
    
    // Check HTTP status code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (http_code < 200 || http_code >= 300) {
        return core::Result<std::string>::Err(
            "HTTP " + std::to_string(http_code) + ": " + response_body
        );
    }
    
    return core::Result<std::string>::Ok(response_body);
}

// ============================================================================
// Helpers
// ============================================================================

std::string DeliveryService::format_announce_message(
    const Job& job,
    const std::string& result_text
) {
    std::ostringstream oss;
    oss << "🤖 **Cron Job Completed**\n\n";
    oss << "**Job:** " << job.name << "\n";
    oss << "**Status:** " << job.state.last_run_status.value_or("unknown") << "\n";
    
    if (job.state.last_run_error.has_value()) {
        oss << "**Error:** " << *job.state.last_run_error << "\n";
    }
    
    oss << "\n**Result:**\n";
    oss << result_text;
    
    if (job.state.next_run_at_ms.has_value()) {
        int64_t next_in_sec = (*job.state.next_run_at_ms - 
                               std::chrono::system_clock::now().time_since_epoch().count()) / 1000;
        oss << "\n\n**Next run in:** " << next_in_sec << " seconds";
    }
    
    return oss.str();
}

std::string DeliveryService::schedule_kind_to_string(ScheduleKind kind) {
    switch (kind) {
        case ScheduleKind::At: return "at";
        case ScheduleKind::Every: return "every";
        case ScheduleKind::Cron: return "cron";
        default: return "unknown";
    }
}

} // namespace cron
} // namespace hunterclaw
