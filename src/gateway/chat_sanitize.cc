// ============================================================================
// chat_sanitize.cc - Chat Message Sanitization
// 🎯 严格对齐 OpenClaw gateway/chat-sanitize.ts (126 行)
//
// 核心功能:
//   - stripEnvelopeFromMessage: 从消息中去除信封元数据
//   - stripEnvelopeFromMessages: 批量处理
//   - extractMessageSenderLabel: 提取发送者标签
// ============================================================================

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// 对齐 chat-sanitize.ts:6 extractInboundSenderLabel
static std::string extract_inbound_sender_label(const std::string& text) {
    // 对齐 auto-reply/reply/strip-inbound-meta.ts
    // 格式: [From: Name] content 或 **Name**: content
    static const std::regex sender_re(R"(^\[From:\s*(.+?)\]\s*)");
    static const std::regex bold_re(R"(^\*\*(.+?)\*\*:\s*)");
    std::smatch m;
    if (std::regex_search(text, m, sender_re)) return m[1].str();
    if (std::regex_search(text, m, bold_re)) return m[1].str();
    return "";
}

// 对齐 chat-sanitize.ts:10-36 extractMessageSenderLabel
static std::string extract_message_sender_label(const json& entry) {
    if (entry.contains("senderLabel") && entry["senderLabel"].is_string()) {
        std::string label = entry["senderLabel"].get<std::string>();
        auto s = label.find_first_not_of(" \t");
        if (s != std::string::npos) return label.substr(s);
    }
    if (entry.contains("content") && entry["content"].is_string()) {
        return extract_inbound_sender_label(entry["content"].get<std::string>());
    }
    if (entry.contains("content") && entry["content"].is_array()) {
        for (const auto& item : entry["content"]) {
            if (item.contains("text") && item["text"].is_string()) {
                auto label = extract_inbound_sender_label(item["text"].get<std::string>());
                if (!label.empty()) return label;
            }
        }
    }
    if (entry.contains("text") && entry["text"].is_string()) {
        return extract_inbound_sender_label(entry["text"].get<std::string>());
    }
    return "";
}

// 对齐 chat-sanitize.ts: stripInboundMetadata
static std::string strip_inbound_metadata(const std::string& text) {
    // 去除 [From: ...] 前缀
    static const std::regex from_re(R"(^\[From:\s*.+?\]\s*)");
    return std::regex_replace(text, from_re, "");
}

// 对齐 chat-sanitize.ts:67-109 stripEnvelopeFromMessage
json strip_envelope_from_message(const json& message) {
    if (!message.is_object()) return message;
    std::string role = message.value("role", "");
    std::transform(role.begin(), role.end(), role.begin(), ::tolower);
    bool strip_user = (role == "user");

    json next = message;
    bool changed = false;

    if (strip_user) {
        auto label = extract_message_sender_label(message);
        if (!label.empty()) {
            next["senderLabel"] = label;
            changed = true;
        }
    }

    if (message.contains("content") && message["content"].is_string()) {
        std::string content = message["content"].get<std::string>();
        std::string stripped = strip_inbound_metadata(content);
        if (stripped != content) {
            next["content"] = stripped;
            changed = true;
        }
    } else if (message.contains("content") && message["content"].is_array()) {
        json new_content = json::array();
        for (const auto& item : message["content"]) {
            if (item.is_object() && item.value("type", "") == "text" && item.contains("text") && item["text"].is_string()) {
                std::string text = item["text"].get<std::string>();
                std::string stripped = strip_inbound_metadata(text);
                if (stripped != text) {
                    json new_item = item;
                    new_item["text"] = stripped;
                    new_content.push_back(new_item);
                    changed = true;
                    continue;
                }
            }
            new_content.push_back(item);
        }
        if (changed) next["content"] = new_content;
    }

    return changed ? next : message;
}

// 对齐 chat-sanitize.ts:112-125 stripEnvelopeFromMessages
json strip_envelope_from_messages(const json& messages) {
    if (!messages.is_array() || messages.empty()) return messages;
    json result = json::array();
    bool changed = false;
    for (const auto& msg : messages) {
        auto stripped = strip_envelope_from_message(msg);
        if (stripped != msg) changed = true;
        result.push_back(stripped);
    }
    return changed ? result : messages;
}

}  // namespace gateway
}  // namespace hunterclaw
