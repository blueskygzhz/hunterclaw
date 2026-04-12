// ============================================================================
// session_history_state.cc
// 🎯 严格对齐 OpenClaw: src/gateway/session-history-state.ts (220 行)
//
// 会话历史分页和 SSE 推送状态管理：
// - 基于 cursor（seq 序号）从消息尾部向前分页
// - SessionHistorySseState 管理 SSE 场景下的内联追加和全量刷新
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: SessionHistoryMessage
using SessionHistoryMessage = json;

// 🎯 对齐: PaginatedSessionHistory
struct PaginatedSessionHistory {
    std::vector<SessionHistoryMessage> items;
    std::vector<SessionHistoryMessage> messages;
    std::optional<std::string> next_cursor;
    bool has_more = false;
};

// 🎯 对齐: SessionHistorySnapshot
struct SessionHistorySnapshot {
    PaginatedSessionHistory history;
    int raw_transcript_seq = 0;
};

// ── Helpers ──────────────────────────────────────────────────────────────────

// 🎯 对齐: resolveCursorSeq(cursor)
static std::optional<int> resolve_cursor_seq(const std::string& cursor) {
    if (cursor.empty()) return std::nullopt;

    std::string normalized = cursor;
    if (normalized.rfind("seq:", 0) == 0) {
        normalized = normalized.substr(4);
    }

    try {
        int value = std::stoi(normalized);
        if (value > 0) return value;
    } catch (...) {}

    return std::nullopt;
}

// 🎯 对齐: resolveMessageSeq(message)
// Extract __openclaw.seq from message
std::optional<int> resolve_message_seq(const SessionHistoryMessage& message) {
    if (message.is_null() || !message.is_object()) return std::nullopt;

    if (message.contains("__openclaw") && message["__openclaw"].is_object()) {
        auto& meta = message["__openclaw"];
        if (meta.contains("seq") && meta["seq"].is_number_integer()) {
            int seq = meta["seq"].get<int>();
            if (seq > 0) return seq;
        }
    }
    return std::nullopt;
}

// ── paginateSessionMessages ─────────────────────────────────────────────────
// 🎯 对齐: paginateSessionMessages(messages, limit, cursor)
// 基于 cursor（seq）和 limit 实现尾部分页

PaginatedSessionHistory paginate_session_messages(
    const std::vector<SessionHistoryMessage>& messages,
    std::optional<int> limit,
    const std::string& cursor) {

    auto cursor_seq = resolve_cursor_seq(cursor);

    int end_exclusive = static_cast<int>(messages.size());

    // 🎯 对齐: 如果有 cursor，找到第一个 seq >= cursorSeq 的消息位置
    if (cursor_seq.has_value()) {
        int found = -1;
        for (int i = 0; i < static_cast<int>(messages.size()); i++) {
            auto seq = resolve_message_seq(messages[i]);
            if (seq.has_value() && *seq >= *cursor_seq) {
                found = i;
                break;
            }
            if (!seq.has_value() && (i + 1) >= *cursor_seq) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            end_exclusive = found;
        }
    }

    // 🎯 对齐: 从 end_exclusive 向前截取 limit 条
    int start = 0;
    if (limit.has_value() && *limit > 0) {
        start = std::max(0, end_exclusive - *limit);
    }

    std::vector<SessionHistoryMessage> paginated(
        messages.begin() + start, messages.begin() + end_exclusive);

    // First message seq for next cursor
    std::optional<int> first_seq;
    if (!paginated.empty()) {
        first_seq = resolve_message_seq(paginated[0]);
    }

    PaginatedSessionHistory result;
    result.items = paginated;
    result.messages = paginated;
    result.has_more = (start > 0);

    if (start > 0 && first_seq.has_value()) {
        result.next_cursor = std::to_string(*first_seq);
    }

    return result;
}

// ── buildSessionHistorySnapshot ─────────────────────────────────────────────
// 🎯 对齐: buildSessionHistorySnapshot(params)

SessionHistorySnapshot build_session_history_snapshot(
    const std::vector<json>& raw_messages,
    int max_chars,
    std::optional<int> limit,
    const std::string& cursor) {

    // Filter valid messages
    std::vector<SessionHistoryMessage> valid_messages;
    for (const auto& msg : raw_messages) {
        if (msg.is_object() && !msg.is_null()) {
            valid_messages.push_back(msg);
        }
    }

    // 🎯 对齐: sanitizeChatHistoryMessages — truncate large text
    std::vector<SessionHistoryMessage> sanitized;
    for (auto msg : valid_messages) {
        // Truncate content text
        if (msg.contains("content") && msg["content"].is_array()) {
            for (auto& item : msg["content"]) {
                if (item.contains("text") && item["text"].is_string()) {
                    std::string text = item["text"].get<std::string>();
                    if (static_cast<int>(text.size()) > max_chars) {
                        item["text"] = text.substr(0, max_chars) + "\n...(truncated)...";
                    }
                }
            }
        }
        sanitized.push_back(msg);
    }

    auto history = paginate_session_messages(sanitized, limit, cursor);

    // Raw transcript seq = last message's seq or count
    int raw_seq = static_cast<int>(valid_messages.size());
    if (!valid_messages.empty()) {
        auto last_seq = resolve_message_seq(valid_messages.back());
        if (last_seq.has_value()) {
            raw_seq = *last_seq;
        }
    }

    SessionHistorySnapshot snap;
    snap.history = history;
    snap.raw_transcript_seq = raw_seq;
    return snap;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json paginated_session_history_to_json(const PaginatedSessionHistory& h) {
    json j;
    j["items"] = h.items;
    j["messages"] = h.messages;
    j["hasMore"] = h.has_more;
    if (h.next_cursor.has_value()) {
        j["nextCursor"] = *h.next_cursor;
    }
    return j;
}

}  // namespace gateway
}  // namespace hunterclaw
