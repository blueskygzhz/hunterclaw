#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/http.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Slack Files API (External Upload Flow) + SSRF Protection
// ============================================================================

// Upload URL response (Step 1)
struct SlackUploadUrlResponse {
    std::string upload_url;  // Presigned URL
    std::string file_id;     // For completeUploadExternal
};

// SSRF Protection Policy
struct SsrfPolicy {
    // Allowed URL patterns (whitelist)
    std::vector<std::string> allowed_hosts;
    
    // Blocked private IP ranges
    bool block_private_ips = true;
    bool block_localhost = true;
    bool block_link_local = true;
    
    // Timeout
    int timeout_ms = 30000;
    
    // Audit context (for logging)
    std::string audit_context;
};

// SSRF Guard (URL validator)
class SsrfGuard {
public:
    // Check if URL is safe
    static core::Result<bool> validate_url(const std::string& url, const SsrfPolicy& policy);
    
    // Fetch with SSRF protection
    static core::Result<core::HttpResponse> fetch_with_ssrf_guard(
        const std::string& url,
        const core::HttpRequest& request,
        const SsrfPolicy& policy
    );
    
private:
    // Check if IP is private
    static bool is_private_ip(const std::string& ip);
    
    // Check if IP is localhost
    static bool is_localhost(const std::string& ip);
    
    // Check if IP is link-local
    static bool is_link_local(const std::string& ip);
    
    // Resolve hostname to IP
    static core::Result<std::string> resolve_hostname(const std::string& hostname);
    
    // Check if host matches allowed patterns
    static bool is_allowed_host(const std::string& host, const SsrfPolicy& policy);
};

// Slack Files API client
class SlackFilesAPI {
public:
    SlackFilesAPI(const std::string& bot_token, const std::string& base_url);
    
    // Three-step upload flow (External Upload)
    core::Result<bool> upload_file_external(
        const std::string& channel_id,
        const std::vector<uint8_t>& file_buffer,
        const std::string& filename,
        const std::string& content_type = "application/octet-stream",
        const std::string& initial_comment = "",
        const std::string& thread_ts = ""
    );
    
    // Step 1: Get upload URL
    core::Result<SlackUploadUrlResponse> get_upload_url_external(
        const std::string& filename,
        size_t file_size
    );
    
    // Step 2: Upload to presigned URL (with SSRF protection)
    core::Result<bool> upload_to_presigned_url(
        const std::string& upload_url,
        const std::vector<uint8_t>& file_buffer,
        const std::string& content_type
    );
    
    // Step 3: Complete upload
    core::Result<bool> complete_upload_external(
        const std::string& file_id,
        const std::string& channel_id,
        const std::string& filename,
        const std::string& initial_comment = "",
        const std::string& thread_ts = ""
    );
    
    // Helper: Convert user ID to DM channel
    core::Result<std::string> resolve_dm_channel(const std::string& user_id);
    
    // Get SSRF policy
    static SsrfPolicy get_slack_upload_policy();
    
private:
    std::string bot_token_;
    std::string base_url_;
    
    // Make Slack API call
    core::Result<core::JsonValue> call_api(
        const std::string& method,
        const core::JsonObject& params
    );
};

// ============================================================================
// Slack File Upload Extensions
// ============================================================================

namespace slack_upload_utils {

// Detect content type from file buffer
std::string detect_content_type(
    const std::vector<uint8_t>& buffer,
    const std::string& filename
);

// Check if file size is within Slack limits
inline bool is_file_size_valid(size_t file_size) {
    // Slack free tier: 5GB storage, but single file max is typically lower
    constexpr size_t MAX_FILE_SIZE = 1024 * 1024 * 1024;  // 1GB safe limit
    return file_size > 0 && file_size <= MAX_FILE_SIZE;
}

// Build file metadata for API
core::JsonObject build_file_metadata(
    const std::string& file_id,
    const std::string& filename,
    const std::string& title = ""
);

}  // namespace slack_upload_utils

// ============================================================================
// SSRF Policy Presets
// ============================================================================

namespace ssrf_policies {

// Slack upload policy (strict)
inline SsrfPolicy slack_upload() {
    SsrfPolicy policy;
    policy.allowed_hosts = {
        "*.slack.com",
        "files.slack.com",
        "slack-files.com"
    };
    policy.block_private_ips = true;
    policy.block_localhost = true;
    policy.block_link_local = true;
    policy.timeout_ms = 30000;
    policy.audit_context = "slack-upload-file";
    return policy;
}

// Discord CDN policy
inline SsrfPolicy discord_cdn() {
    SsrfPolicy policy;
    policy.allowed_hosts = {
        "*.discord.com",
        "cdn.discordapp.com",
        "media.discordapp.net"
    };
    policy.block_private_ips = true;
    policy.block_localhost = true;
    policy.timeout_ms = 30000;
    policy.audit_context = "discord-cdn-upload";
    return policy;
}

// Generic webhook policy (more permissive)
inline SsrfPolicy webhook() {
    SsrfPolicy policy;
    policy.allowed_hosts = {};  // Allow all (rely on IP filtering)
    policy.block_private_ips = true;
    policy.block_localhost = true;
    policy.block_link_local = true;
    policy.timeout_ms = 10000;
    policy.audit_context = "webhook-delivery";
    return policy;
}

}  // namespace ssrf_policies

}  // namespace channels
}  // namespace hunterclaw
