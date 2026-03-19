#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/routing/router.h"
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <map>

namespace hunterclaw {
namespace channels {

// 入站消息 (从外部平台接收)
struct InboundMessage {
    // 消息标识
    std::string message_id;           // 消息ID
    std::string channel;              // 渠道ID (feishu, telegram, discord, etc.)
    std::optional<std::string> account_id;  // 账户ID (aligned with openclaw: ctx.AccountId)
    
    // 发送者信息
    std::string sender_id;            // 发送者ID
    std::string sender_name;          // 发送者昵称/名称
    std::string from_id;              // 别名 (与 sender_id 相同,兼容性)
    
    // 聊天信息
    std::string chat_id;              // 聊天会话ID
    routing::ChatType chat_type = routing::ChatType::Direct;  // 聊天类型
    std::optional<std::string> guild_id;  // 服务器ID (Discord等)
    std::optional<std::string> thread_id; // 线程ID
    
    // 消息内容
    MessageType type = MessageType::Text;  // 消息类型
    std::string text;                 // 文本内容
    std::vector<std::string> attachments;  // 附件URL列表
    
    // 提及信息
    bool mentions_bot = false;        // 是否@机器人
    std::vector<std::string> mentioned_ids;  // 被@的用户ID列表
    
    // 回复信息
    std::optional<std::string> reply_to_id;  // 回复的消息ID
    std::optional<std::string> parent_id;    // 父消息ID (线程)
    
    // 时间戳
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();

    // Cross-provider routing (aligned with openclaw: ctx.OriginatingChannel / ctx.OriginatingTo)
    // Set when a message is relayed from a different provider/surface.
    std::string originating_channel;   // Original channel (e.g. "telegram" when relayed via slack)
    std::string originating_to;        // Original recipient ID in the originating channel

    // Session key (aligned with openclaw: ctx.SessionKey)
    std::string session_key;

    // 元数据
    std::map<std::string, core::JsonValue> metadata;
};

// 出站消息 (发送到外部平台)
struct OutboundMessage {
    // 目标信息
    std::string channel;              // 渠道ID
    std::string account_id;           // 账户ID
    std::string to;                   // 接收者ID (用户/群组/频道)
    std::optional<std::string> thread_id;  // 线程ID (可选)
    std::optional<std::string> reply_to_id; // 回复消息ID (可选)
    
    // 消息内容
    MessageType type = MessageType::Text;  // 消息类型
    std::string text;                 // 文本内容
    std::vector<std::string> attachments;  // 附件URL列表
    std::optional<std::string> markdown;   // Markdown格式内容
    
    // 元数据
    std::map<std::string, core::JsonValue> metadata;
};

} // namespace channels
} // namespace hunterclaw
