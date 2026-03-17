#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <memory>

using json = nlohmann::json;

namespace hunterclaw {
namespace tools {

/**
 * @brief Slack Actions Tool - Slack 平台集成工具
 * 
 * 对齐 OpenClaw 的 Slack 集成功能,支持:
 * - slack_send: 发送消息到 Slack 频道
 * - slack_upload: 上传文件到 Slack
 * - slack_channel: 管理 Slack 频道
 * 
 * 对应 OpenClaw 文件:
 * - src/channels/plugins/outbound/slack.ts
 * - src/slack/send.ts
 * - src/slack/actions.ts
 */
class SlackTool {
 public:
  SlackTool();
  ~SlackTool();

  // 执行 Slack 动作
  nlohmann::json execute(const std::string& action,
                         const nlohmann::json& params);

 private:
  // ==================== slack_send ====================
  /**
   * @brief 发送消息到 Slack 频道
   * 
   * 参数:
   * - to (string, required): 目标 (channel ID 或 user ID)
   * - message (string, required): 消息内容 (Slack mrkdwn)
   * - thread_ts (string, optional): 线程时间戳 (回复线程)
   * - blocks (array, optional): Block Kit 块数组
   * - identity (object, optional): 自定义身份 {username, icon_url, icon_emoji}
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "message_id": "1234567890.123456",
   *   "channel_id": "C0123456789"
   * }
   * 
   * OpenClaw 对齐:
   * - src/slack/send.ts (sendMessageSlack, postSlackMessageBestEffort)
   * - SLACK_TEXT_LIMIT = 4000
   */
  nlohmann::json slack_send(const nlohmann::json& params);

  // ==================== slack_upload ====================
  /**
   * @brief 上传文件到 Slack
   * 
   * 参数:
   * - channels (string, required): 目标频道 ID (逗号分隔多个)
   * - file_path (string, optional): 本地文件路径
   * - file_url (string, optional): 远程文件 URL
   * - content (string, optional): 文件内容 (文本)
   * - filename (string, optional): 文件名
   * - title (string, optional): 文件标题
   * - initial_comment (string, optional): 初始评论
   * - thread_ts (string, optional): 线程时间戳
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "file_id": "F0123456789",
   *   "url": "https://files.slack.com/..."
   * }
   * 
   * OpenClaw 对齐:
   * - src/slack/send.ts (SLACK_UPLOAD_SSRF_POLICY)
   * - 支持 SSRF 防护
   */
  nlohmann::json slack_upload(const nlohmann::json& params);

  // ==================== slack_channel ====================
  /**
   * @brief 管理 Slack 频道
   * 
   * 支持操作:
   * - list: 列出频道
   * - info: 获取频道信息
   * - create: 创建频道
   * - archive: 归档频道
   * - unarchive: 取消归档
   * - join: 加入频道
   * - leave: 离开频道
   * - set_topic: 设置主题
   * - set_purpose: 设置目的
   * 
   * 参数:
   * - operation (string, required): 操作类型
   * - channel_id (string, optional): 频道 ID (除 list, create 外必需)
   * - name (string, optional): 频道名称 (create 时必需)
   * - is_private (bool, optional): 是否私有 (create 时可选)
   * - topic (string, optional): 主题 (set_topic 时必需)
   * - purpose (string, optional): 目的 (set_purpose 时必需)
   * 
   * 返回:
   * {
   *   "status": "success",
   *   "operation": "created",
   *   "channel": { ... }
   * }
   * 
   * OpenClaw 对齐:
   * - src/slack/actions.ts
   * - src/slack/resolve-channels.ts
   */
  nlohmann::json slack_channel(const nlohmann::json& params);

  // ==================== 辅助方法 ====================
  
  // 验证 Slack Token
  bool validate_token(const std::string& token);
  
  // 解析目标 (channel ID, user ID)
  struct SlackTarget {
    std::string type;  // "channel", "user"
    std::string id;
  };
  SlackTarget parse_target(const std::string& to);
  
  // 构建 Slack API URL
  std::string build_api_url(const std::string& method);
  
  // 发送 Slack API 请求
  nlohmann::json send_request(const std::string& method,
                               const nlohmann::json& params = {});
  
  // 格式化 Block Kit 块
  nlohmann::json format_blocks(const nlohmann::json& blocks);
  
  // 验证 Block Kit 块
  bool validate_blocks(const nlohmann::json& blocks);
  
  // 构建 fallback text (对齐 OpenClaw buildSlackBlocksFallbackText)
  std::string build_blocks_fallback(const nlohmann::json& blocks);
  
  // Markdown 转 Slack mrkdwn
  std::string markdown_to_mrkdwn(const std::string& markdown);
  
  // 文本长度限制
  static constexpr size_t SLACK_TEXT_LIMIT = 4000;
  std::string truncate_text(const std::string& text);
  
  // SSRF 防护 (文件上传)
  struct SsrfPolicy {
    std::vector<std::string> allowed_hostnames;
    bool allow_rfc2544_benchmark_range;
  };
  bool check_ssrf_policy(const std::string& url, const SsrfPolicy& policy);
  
  // 解析 Identity
  struct SlackIdentity {
    std::optional<std::string> username;
    std::optional<std::string> icon_url;
    std::optional<std::string> icon_emoji;
  };
  SlackIdentity parse_identity(const nlohmann::json& identity);
  
  // 检查 chat:write.customize scope
  bool has_customize_scope();
  
  // 频道类型缓存 (对齐 OpenClaw SLACK_CHANNEL_TYPE_CACHE)
  std::string get_channel_type(const std::string& channel_id);
  
  // 私有成员
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tools
} // namespace hunterclaw
