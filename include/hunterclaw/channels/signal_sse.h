#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Signal SSE (Server-Sent Events) Client with Auto-Reconnect
// ============================================================================

struct SignalSseEvent {
    std::string event;  // Event type
    std::string data;   // Event data (usually JSON)
    std::string id;     // Event ID (for resumption)
};

// Reconnect policy (exponential backoff)
struct ReconnectPolicy {
    int initial_ms = 1000;     // Start with 1s
    int max_ms = 30000;        // Max 30s
    float factor = 2.0f;       // Double each time
    float jitter = 0.2f;       // ±20% randomness
};

// SSE client callbacks
struct SseCallbacks {
    std::function<void(const SignalSseEvent&)> on_event;
    std::function<void(const std::string&)> on_error;
    std::function<void()> on_reconnect;
};

// Signal SSE client
class SignalSseClient {
public:
    SignalSseClient(
        const std::string& base_url,
        const std::string& account,
        const SseCallbacks& callbacks,
        const ReconnectPolicy& policy = ReconnectPolicy()
    );
    
    ~SignalSseClient();
    
    // Start SSE connection loop
    void start();
    
    // Stop SSE connection
    void stop();
    
    // Check if running
    bool is_running() const { return running_; }
    
    // Get reconnect attempts
    int get_reconnect_attempts() const { return reconnect_attempts_; }
    
private:
    std::string base_url_;
    std::string account_;
    SseCallbacks callbacks_;
    ReconnectPolicy policy_;
    
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_;
    std::atomic<int> reconnect_attempts_;
    
    std::unique_ptr<std::thread> connection_thread_;
    
    // Main connection loop
    void connection_loop();
    
    // Stream SSE events
    core::Result<bool> stream_events();
    
    // Parse SSE line
    void parse_sse_line(const std::string& line, SignalSseEvent& current_event);
    
    // Compute backoff delay
    int compute_backoff_delay() const;
    
    // Sleep with abort check
    bool sleep_with_abort(int delay_ms);
};

// ============================================================================
// SSE Parser (RFC 8895)
// ============================================================================

class SseParser {
public:
    // Parse SSE stream chunk
    static std::vector<SignalSseEvent> parse_chunk(
        const std::string& chunk,
        SignalSseEvent& incomplete_event
    );
    
    // Parse single SSE line
    static void parse_line(const std::string& line, SignalSseEvent& event);
    
    // Check if event is complete
    static bool is_event_complete(const SignalSseEvent& event);
};

// ============================================================================
// Signal API Client (HTTP + SSE)
// ============================================================================

class SignalClient {
public:
    SignalClient(const std::string& base_url, const std::string& account);
    ~SignalClient();
    
    // Start receiving messages via SSE
    void start_sse(const SseCallbacks& callbacks);
    
    // Stop SSE
    void stop_sse();
    
    // Send message (HTTP POST)
    core::Result<bool> send_message(
        const std::string& recipient,
        const std::string& message,
        const std::vector<std::string>& attachments = {}
    );
    
    // Send reaction
    core::Result<bool> send_reaction(
        const std::string& recipient,
        int64_t timestamp,
        const std::string& emoji
    );
    
    // Mark as read
    core::Result<bool> mark_read(
        const std::string& recipient,
        int64_t timestamp
    );
    
    // Get account info
    core::Result<core::JsonValue> get_account_info();
    
private:
    std::string base_url_;
    std::string account_;
    std::unique_ptr<SignalSseClient> sse_client_;
    
    // Make HTTP request
    core::Result<core::JsonValue> call_api(
        const std::string& method,
        const std::string& endpoint,
        const core::JsonObject& body = {}
    );
};

// ============================================================================
// SSE Utilities
// ============================================================================

namespace sse_utils {

// Compute exponential backoff with jitter
inline int compute_backoff(const ReconnectPolicy& policy, int attempts) {
    int delay = policy.initial_ms * std::pow(policy.factor, attempts - 1);
    delay = std::min(delay, policy.max_ms);
    
    // Add jitter: ±20%
    float jitter_factor = 1.0f + (std::rand() / (float)RAND_MAX * 2.0f - 1.0f) * policy.jitter;
    delay = static_cast<int>(delay * jitter_factor);
    
    return delay;
}

// Parse event stream line
inline void parse_event_line(const std::string& line, SignalSseEvent& event) {
    if (line.empty()) return;
    
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) return;
    
    std::string field = line.substr(0, colon_pos);
    std::string value = colon_pos + 1 < line.size() ? line.substr(colon_pos + 1) : "";
    
    // Trim leading space
    if (!value.empty() && value[0] == ' ') {
        value = value.substr(1);
    }
    
    if (field == "event") {
        event.event = value;
    } else if (field == "data") {
        event.data += value;
    } else if (field == "id") {
        event.id = value;
    }
}

// Build SSE URL with query params
inline std::string build_sse_url(const std::string& base_url, const std::string& account) {
    std::string url = base_url + "/api/v1/events";
    if (!account.empty()) {
        url += "?account=" + account;
    }
    return url;
}

}  // namespace sse_utils

}  // namespace channels
}  // namespace hunterclaw
