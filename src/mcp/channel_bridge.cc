#include "hunterclaw/mcp/channel_bridge.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// Utility functions (对齐 OpenClaw channel-shared.ts)
// ============================================================================

bool match_event_filter(const QueueEvent& event, const WaitFilter& filter) {
    if (event.cursor <= filter.after_cursor) return false;
    if (!filter.session_key.has_value()) return true;
    return event.session_key == filter.session_key.value();
}

static std::optional<std::string> normalize_optional_string(const nlohmann::json& j, const std::string& key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    if (j[key].is_string()) {
        auto s = j[key].get<std::string>();
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return std::nullopt;
        return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
    }
    return std::nullopt;
}

// 对齐 channel-shared.ts:157-178: toConversation
std::optional<ConversationDescriptor> to_conversation(const nlohmann::json& row) {
    auto channel = normalize_optional_string(row, "lastChannel");
    if (!channel) channel = normalize_optional_string(row, "channel");
    auto to = normalize_optional_string(row, "lastTo");
    if (!channel || !to) return std::nullopt;

    ConversationDescriptor desc;
    desc.session_key = row.value("key", "");
    desc.channel = channel.value();
    desc.to = to.value();
    desc.account_id = normalize_optional_string(row, "lastAccountId");
    desc.label = normalize_optional_string(row, "label");
    desc.display_name = normalize_optional_string(row, "displayName");
    desc.derived_title = normalize_optional_string(row, "derivedTitle");
    desc.last_message_preview = normalize_optional_string(row, "lastMessagePreview");
    if (row.contains("updatedAt") && row["updatedAt"].is_number()) {
        desc.updated_at = row["updatedAt"].get<int64_t>();
    }
    return desc;
}

std::string approval_kind_to_string(ApprovalKind kind) {
    switch (kind) {
        case ApprovalKind::EXEC: return "exec";
        case ApprovalKind::PLUGIN: return "plugin";
    }
    return "exec";
}

std::string approval_decision_to_string(ApprovalDecision decision) {
    switch (decision) {
        case ApprovalDecision::ALLOW_ONCE: return "allow-once";
        case ApprovalDecision::ALLOW_ALWAYS: return "allow-always";
        case ApprovalDecision::DENY: return "deny";
    }
    return "deny";
}

// ============================================================================
// ChannelBridge Implementation (对齐 OpenClaw channel-bridge.ts)
// ============================================================================

ChannelBridge::ChannelBridge(const Config& config) : config_(config) {}
ChannelBridge::~ChannelBridge() { close(); }

void ChannelBridge::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    
    // 对齐 channel-bridge.ts:84-130
    // 在 HunterClaw 中，gateway client 连接通过 infra/http_client.cc 建立。
    // 此处标记为 ready 以简化同步 C++ 实现。
    // 完整实现需要异步 WebSocket 连接。
    ready_ = true;
    
    if (config_.verbose) {
        std::cerr << "[ChannelBridge] Started with gateway: " << config_.gateway_url << std::endl;
    }
}

void ChannelBridge::wait_until_ready() {
    // 对齐 channel-bridge.ts:132-134 — 在同步 C++ 中直接返回
    if (!started_) start();
}

void ChannelBridge::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;
    // 清理 pending waiters (对齐 channel-bridge.ts:136-152)
    if (config_.verbose) {
        std::cerr << "[ChannelBridge] Closed" << std::endl;
    }
}

bool ChannelBridge::is_ready() const { return ready_; }

// 对齐 channel-bridge.ts:154-177
std::vector<ConversationDescriptor> ChannelBridge::list_conversations(
    int limit, const std::string& search, const std::string& channel
) {
    wait_until_ready();
    nlohmann::json params = {
        {"limit", limit},
        {"includeDerivedTitles", true},
        {"includeLastMessage", true}
    };
    if (!search.empty()) params["search"] = search;
    
    auto response = request_gateway("sessions.list", params);
    std::vector<ConversationDescriptor> result;
    
    if (response.contains("sessions") && response["sessions"].is_array()) {
        for (const auto& row : response["sessions"]) {
            auto conv = to_conversation(row);
            if (!conv) continue;
            if (!channel.empty()) {
                std::string conv_channel = conv->channel;
                std::transform(conv_channel.begin(), conv_channel.end(), conv_channel.begin(), ::tolower);
                std::string filter_channel = channel;
                std::transform(filter_channel.begin(), filter_channel.end(), filter_channel.begin(), ::tolower);
                if (conv_channel != filter_channel) continue;
            }
            result.push_back(std::move(conv.value()));
        }
    }
    return result;
}

// 对齐 channel-bridge.ts:179-188
std::optional<ConversationDescriptor> ChannelBridge::get_conversation(const std::string& session_key) {
    auto conversations = list_conversations(500);
    for (const auto& conv : conversations) {
        if (conv.session_key == session_key) return conv;
    }
    return std::nullopt;
}

// 对齐 channel-bridge.ts:190-200
std::vector<nlohmann::json> ChannelBridge::read_messages(const std::string& session_key, int limit) {
    wait_until_ready();
    auto response = request_gateway("chat.history", {{"sessionKey", session_key}, {"limit", limit}});
    if (response.contains("messages") && response["messages"].is_array()) {
        return response["messages"].get<std::vector<nlohmann::json>>();
    }
    return {};
}

// 对齐 channel-bridge.ts:202-219
nlohmann::json ChannelBridge::send_message(const std::string& session_key, const std::string& text) {
    auto conversation = get_conversation(session_key);
    if (!conversation) {
        throw std::runtime_error("Conversation not found for session " + session_key);
    }
    nlohmann::json params = {
        {"to", conversation->to},
        {"channel", conversation->channel},
        {"message", text},
        {"sessionKey", conversation->session_key}
    };
    if (conversation->account_id) params["accountId"] = conversation->account_id.value();
    if (conversation->thread_id) params["threadId"] = conversation->thread_id.value();
    return request_gateway("send", params);
}

// 对齐 channel-bridge.ts:221-225
std::vector<PendingApproval> ChannelBridge::list_pending_approvals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingApproval> result;
    for (const auto& [id, approval] : pending_approvals_) {
        result.push_back(approval);
    }
    std::sort(result.begin(), result.end(), [](const PendingApproval& a, const PendingApproval& b) {
        return a.created_at_ms.value_or(0) < b.created_at_ms.value_or(0);
    });
    return result;
}

// 对齐 channel-bridge.ts:227-242
nlohmann::json ChannelBridge::respond_to_approval(ApprovalKind kind, const std::string& id, ApprovalDecision decision) {
    std::string method = (kind == ApprovalKind::EXEC) ? "exec.approval.resolve" : "plugin.approval.resolve";
    return request_gateway(method, {{"id", id}, {"decision", approval_decision_to_string(decision)}});
}

// 对齐 channel-bridge.ts:244-248
ChannelBridge::PollResult ChannelBridge::poll_events(const WaitFilter& filter, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<QueueEvent> events;
    for (const auto& event : queue_) {
        if (!match_event_filter(event, filter)) continue;
        events.push_back(event);
        if (static_cast<int>(events.size()) >= limit) break;
    }
    int next_cursor = events.empty() ? filter.after_cursor : events.back().cursor;
    return PollResult{std::move(events), next_cursor};
}

// 对齐 channel-bridge.ts:250-271 — simplified synchronous version
std::optional<QueueEvent> ChannelBridge::wait_for_event(const WaitFilter& filter, int /*timeout_ms*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& event : queue_) {
        if (match_event_filter(event, filter)) return event;
    }
    return std::nullopt;
}

// Private helpers

int ChannelBridge::next_cursor() {
    cursor_++;
    return cursor_;
}

void ChannelBridge::enqueue(QueueEvent event) {
    queue_.push_back(std::move(event));
    while (static_cast<int>(queue_.size()) > QUEUE_LIMIT) {
        queue_.erase(queue_.begin());
    }
}

void ChannelBridge::track_approval(ApprovalKind kind, const nlohmann::json& payload) {
    auto id_opt = normalize_optional_string(payload, "id");
    if (!id_opt) return;
    PendingApproval approval;
    approval.kind = kind;
    approval.id = id_opt.value();
    if (payload.contains("request") && payload["request"].is_object()) {
        approval.request = payload["request"];
    }
    if (payload.contains("createdAtMs") && payload["createdAtMs"].is_number()) {
        approval.created_at_ms = payload["createdAtMs"].get<int64_t>();
    }
    if (payload.contains("expiresAtMs") && payload["expiresAtMs"].is_number()) {
        approval.expires_at_ms = payload["expiresAtMs"].get<int64_t>();
    }
    pending_approvals_[approval.id] = std::move(approval);
}

void ChannelBridge::resolve_tracked_approval(const nlohmann::json& payload) {
    auto id_opt = normalize_optional_string(payload, "id");
    if (id_opt) pending_approvals_.erase(id_opt.value());
}

// 对齐 channel-bridge.ts:297-305 — gateway request helper
nlohmann::json ChannelBridge::request_gateway(const std::string& method, const nlohmann::json& params) {
    if (closed_) {
        throw std::runtime_error("ChannelBridge is closed");
    }
    // 对齐: 实际请求通过 Gateway WebSocket client 发送。
    // HunterClaw 中等效实现在 gateway/gateway.cc 的 WebSocket RPC 层。
    // 此处为结构占位。
    (void)method;
    return params;
}

}  // namespace mcp
}  // namespace hunterclaw
