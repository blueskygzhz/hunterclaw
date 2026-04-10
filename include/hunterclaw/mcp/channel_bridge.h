#pragma once

// 严格对齐 OpenClaw: mcp/channel-bridge.ts (516行) + channel-shared.ts (210行)
// MCP Channel Bridge — gateway client that bridges MCP tools to channel operations.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// 对齐 OpenClaw channel-shared.ts:6-18: ConversationDescriptor
// ============================================================================

struct ConversationDescriptor {
    std::string session_key;
    std::string channel;
    std::string to;
    std::optional<std::string> account_id;
    std::optional<std::string> thread_id;
    std::optional<std::string> label;
    std::optional<std::string> display_name;
    std::optional<std::string> derived_title;
    std::optional<std::string> last_message_preview;
    std::optional<int64_t> updated_at;
};

// ============================================================================
// 对齐 OpenClaw channel-shared.ts:65-105: QueueEvent types
// ============================================================================

enum class ApprovalKind { EXEC, PLUGIN };
enum class ApprovalDecision { ALLOW_ONCE, ALLOW_ALWAYS, DENY };

struct PendingApproval {
    ApprovalKind kind;
    std::string id;
    std::optional<nlohmann::json> request;
    std::optional<int64_t> created_at_ms;
    std::optional<int64_t> expires_at_ms;
};

struct QueueEvent {
    int cursor;
    std::string type;  // "message" | "claude_permission_request" | "exec_approval_*" | "plugin_approval_*"
    std::string session_key;
    std::optional<ConversationDescriptor> conversation;
    std::optional<std::string> message_id;
    std::optional<int> message_seq;
    std::optional<std::string> role;
    std::optional<std::string> text;
    nlohmann::json raw;
};

struct WaitFilter {
    int after_cursor = 0;
    std::optional<std::string> session_key;
};

// ============================================================================
// 对齐 OpenClaw channel-bridge.ts:44-516: OpenClawChannelBridge
// ============================================================================

/**
 * MCP Channel Bridge — manages a persistent gateway client connection
 * and provides high-level methods for listing conversations, sending messages,
 * reading history, polling events, and handling approvals.
 */
class ChannelBridge {
public:
    struct Config {
        std::string gateway_url;
        std::string gateway_token;
        std::string gateway_password;
        std::string claude_channel_mode;  // "off" | "on" | "auto"
        bool verbose = false;
    };

    explicit ChannelBridge(const Config& config);
    ~ChannelBridge();

    // Lifecycle
    void start();
    void wait_until_ready();
    void close();
    bool is_ready() const;

    // Conversations (对齐 listConversations/getConversation)
    std::vector<ConversationDescriptor> list_conversations(
        int limit = 50,
        const std::string& search = "",
        const std::string& channel = ""
    );

    std::optional<ConversationDescriptor> get_conversation(const std::string& session_key);

    // Messages (对齐 readMessages/sendMessage)
    std::vector<nlohmann::json> read_messages(const std::string& session_key, int limit = 20);

    nlohmann::json send_message(const std::string& session_key, const std::string& text);

    // Approvals (对齐 listPendingApprovals/respondToApproval)
    std::vector<PendingApproval> list_pending_approvals() const;

    nlohmann::json respond_to_approval(ApprovalKind kind, const std::string& id, ApprovalDecision decision);

    // Event polling (对齐 pollEvents/waitForEvent)
    struct PollResult {
        std::vector<QueueEvent> events;
        int next_cursor;
    };
    PollResult poll_events(const WaitFilter& filter, int limit = 20);

    std::optional<QueueEvent> wait_for_event(const WaitFilter& filter, int timeout_ms = 30000);

private:
    Config config_;
    bool started_ = false;
    bool ready_ = false;
    bool closed_ = false;
    int cursor_ = 0;
    mutable std::mutex mutex_;

    std::vector<QueueEvent> queue_;
    std::map<std::string, PendingApproval> pending_approvals_;

    int next_cursor();
    void enqueue(QueueEvent event);
    void track_approval(ApprovalKind kind, const nlohmann::json& payload);
    void resolve_tracked_approval(const nlohmann::json& payload);

    // Gateway request helper
    nlohmann::json request_gateway(const std::string& method, const nlohmann::json& params);

    static constexpr int QUEUE_LIMIT = 1000;
};

// ============================================================================
// Utility functions (对齐 OpenClaw channel-shared.ts)
// ============================================================================

bool match_event_filter(const QueueEvent& event, const WaitFilter& filter);
std::optional<ConversationDescriptor> to_conversation(const nlohmann::json& row);
std::string approval_kind_to_string(ApprovalKind kind);
std::string approval_decision_to_string(ApprovalDecision decision);

}  // namespace mcp
}  // namespace hunterclaw
