#pragma once

#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * @brief Discord Actions Tool - Discord 平台集成工具
 * 
 * 对齐 OpenClaw 的 Discord 集成功能,支持:
 * - discord_send: 发送消息到 Discord 频道
 * - discord_react: 添加 Emoji 反应
 * - discord_thread: 管理线程
 * - discord_presence: 更新机器人状态
 * 
 * 对应 OpenClaw 文件:
 * - src/channels/plugins/outbound/discord.ts
 * - src/discord/send.outbound.ts
 * - src/discord/reactions.ts
 */
class DiscordTool {
 public:
  DiscordTool();
  ~DiscordTool();

  // 执行 Discord 动作
  nlohmann::json execute(const std::string& action,
                         const nlohmann::json& params);

 private:
  // ==================== discord_send ====================
  /**
   * @brief 发送消息到 Discord 频道
   * 
   * 参数:
   * - to (string, required): 目标 (channel:ID, user:ID, guild:ID#channel)
   * - message (string, required): 消息内容 (Markdown)
   * - thread_id (string, optional): 线程 ID
   * - reply_to (string, optional): 回复的消息 ID
   * - silent (bool, optional): 静默发送 (不通知)
   * - embeds (array, optional): Embed 对象数组
   * - components (array, optional): 交互组件
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "message_id": "1234567890",
   *   "channel_id": "9876543210"
   * }
   */
  nlohmann::json discord_send(const nlohmann::json& params);

  // ==================== discord_react ====================
  /**
   * @brief 添加 Emoji 反应到消息
   * 
   * 参数:
   * - channel_id (string, required): 频道 ID
   * - message_id (string, required): 消息 ID
   * - emoji (string, required): Emoji (Unicode 或 custom:name:id)
   * - remove (bool, optional): 是否移除反应 (默认 false)
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "action": "added" | "removed"
   * }
   * 
   * OpenClaw 对齐:
   * - src/discord/reactions.ts
   */
  nlohmann::json discord_react(const nlohmann::json& params);

  // ==================== discord_thread ====================
  /**
   * @brief 管理 Discord 线程
   * 
   * 支持操作:
   * - create: 创建线程
   * - archive: 归档线程
   * - unarchive: 取消归档
   * - join: 加入线程
   * - leave: 离开线程
   * 
   * 参数:
   * - operation (string, required): 操作类型
   * - channel_id (string, required): 父频道 ID
   * - thread_id (string, optional): 线程 ID (除 create 外必需)
   * - name (string, optional): 线程名称 (create 时必需)
   * - message_id (string, optional): 起始消息 ID (create 时可选)
   * - auto_archive_duration (int, optional): 自动归档时间 (60, 1440, 4320, 10080)
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "thread_id": "1234567890",
   *   "operation": "created"
   * }
   * 
   * OpenClaw 对齐:
   * - src/discord/monitor/threading.ts
   * - src/discord/send.outbound.ts (DISCORD_THREAD_NAME_LIMIT)
   */
  nlohmann::json discord_thread(const nlohmann::json& params);

  // ==================== discord_presence ====================
  /**
   * @brief 更新机器人状态和活动
   * 
   * 参数:
   * - status (string, optional): 在线状态 ("online", "idle", "dnd", "invisible")
   * - activity_type (string, optional): 活动类型 ("playing", "streaming", "listening", "watching", "competing")
   * - activity_name (string, optional): 活动名称
   * - url (string, optional): 流媒体 URL (streaming 类型)
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "presence": {
   *     "status": "online",
   *     "activity": "Playing HunterClaw"
   *   }
   * }
   * 
   * OpenClaw 对齐:
   * - src/discord/monitor/listeners.ts (DiscordPresenceListener)
   */
  nlohmann::json discord_presence(const nlohmann::json& params);

  // ==================== 辅助方法 ====================
  
  // 验证 Discord Token
  bool validate_token(const std::string& token);
  
  // 解析目标 (channel:ID, user:ID, guild:ID#channel)
  struct DiscordTarget {
    std::string type;  // "channel", "user", "guild"
    std::string id;
    std::optional<std::string> channel_name;
  };
  DiscordTarget parse_target(const std::string& to);
  
  // 构建 Discord API URL
  std::string build_api_url(const std::string& endpoint);
  
  // 发送 Discord API 请求
  nlohmann::json send_request(const std::string& method,
                               const std::string& endpoint,
                               const nlohmann::json& body = {});
  
  // 格式化 Embed 对象
  nlohmann::json format_embeds(const nlohmann::json& embeds);
  
  // 格式化交互组件
  nlohmann::json format_components(const nlohmann::json& components);
  
  // Emoji 解析 (Unicode 或 custom:name:id)
  struct EmojiInfo {
    std::string type;  // "unicode" or "custom"
    std::string value;
    std::optional<std::string> id;
    std::optional<std::string> name;
  };
  EmojiInfo parse_emoji(const std::string& emoji);
  
  // 线程名称限制 (对齐 OpenClaw DISCORD_THREAD_NAME_LIMIT)
  static constexpr size_t DISCORD_THREAD_NAME_LIMIT = 100;
  std::string truncate_thread_name(const std::string& name);
  
  // Webhook 支持 (用于线程绑定)
  struct WebhookInfo {
    std::string id;
    std::string token;
  };
  std::optional<WebhookInfo> resolve_webhook(const std::string& thread_id);
  
  // 活动类型映射
  int activity_type_to_int(const std::string& type);
  
  // 私有成员
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tools
} // namespace hunterclaw
