#ifndef HUNTERCLAW_GATEWAY_SESSION_DEVICE_ID_H
#define HUNTERCLAW_GATEWAY_SESSION_DEVICE_ID_H

#include <string>
#include <optional>
#include <unordered_map>

// Forward declaration (避免包含 httplib.h)
namespace httplib {
    struct Request;
}

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Device ID 工具 (严格对齐 OpenClaw)
// ============================================================================

/**
 * @brief 从 HTTP 请求中提取 Device ID
 * 
 * 优先级策略 (对齐 OpenClaw):
 * 1. HTTP Header: X-Device-ID / X-OpenClaw-Device-ID
 * 2. Authorization Token: 从 Bearer token 解析
 * 3. Cookie: openclaw_device_id
 * 4. Fallback: 计算稳定 Device ID (IP + User-Agent)
 * 
 * @param req HTTP 请求对象
 * @return Device ID 字符串
 */
std::string extract_device_id_from_request(const httplib::Request& req);

/**
 * @brief 计算稳定的 Device ID (基于 IP 和 User-Agent)
 * 
 * 使用 SHA256(IP + UA) 的前 16 字节十六进制表示作为 Device ID
 * 
 * @param ip 客户端 IP 地址
 * @param user_agent User-Agent 字符串
 * @return 稳定的 Device ID
 */
std::string compute_stable_device_id(
    const std::string& ip,
    const std::string& user_agent
);

/**
 * @brief 从 Bearer Token 解析 Device ID
 * 
 * 假设 token 格式: Base64(JSON{device_id, ...})
 * 
 * @param token Bearer token 字符串
 * @return Device ID (如果成功) 或 nullopt
 */
std::optional<std::string> parse_device_id_from_token(const std::string& token);

/**
 * @brief 解析 Cookie 字符串为 key-value map
 * 
 * @param cookie_header Cookie header 字符串
 * @return Cookie map
 */
std::unordered_map<std::string, std::string> parse_cookies(
    const std::string& cookie_header
);

/**
 * @brief 构造基于 Device ID 的 Session Key
 * 
 * 格式 (对齐 OpenClaw):
 * - 基础: "agent:{agentId}:device:{deviceId}"
 * - 带 channel: "agent:{agentId}:device:{deviceId}:channel:{channelId}"
 * 
 * @param agent_id Agent ID
 * @param device_id Device ID
 * @param channel_id Channel ID (可选)
 * @return Session Key
 */
std::string build_device_session_key(
    const std::string& agent_id,
    const std::string& device_id,
    const std::optional<std::string>& channel_id = std::nullopt
);

/**
 * @brief 验证 Device ID 格式
 * 
 * Device ID 应该是:
 * - 非空
 * - 长度 8-64 字符
 * - 只包含字母数字和连字符
 * 
 * @param device_id Device ID
 * @return 是否有效
 */
bool is_valid_device_id(const std::string& device_id);

} // namespace gateway
} // namespace hunterclaw

#endif // HUNTERCLAW_GATEWAY_SESSION_DEVICE_ID_H
