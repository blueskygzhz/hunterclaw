#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

/// 帧类型 (100% 对齐 OpenClaw)
enum class FrameType {
    Request,   // "req"  - RPC 请求
    Response,  // "res"  - RPC 响应
    Event      // "event" - 事件广播
};

/// 错误形状 (100% 对齐 OpenClaw)
struct ErrorShape {
    std::string code;
    std::string message;
    std::optional<bool> retryable;
    std::optional<int64_t> retry_after_ms;
    
    nlohmann::json to_json() const;
    static core::Result<ErrorShape> from_json(const nlohmann::json& j);
};

/// 请求帧 (100% 对齐 OpenClaw)
struct RequestFrame {
    std::string id;          // UUID
    std::string method;      // RPC 方法名
    nlohmann::json params;   // 可选参数
    
    nlohmann::json to_json() const;
    static core::Result<RequestFrame> from_json(const nlohmann::json& j);
};

/// 响应帧 (100% 对齐 OpenClaw)
struct ResponseFrame {
    std::string id;
    bool ok;
    nlohmann::json payload;
    std::optional<ErrorShape> error;
    
    nlohmann::json to_json() const;
    static core::Result<ResponseFrame> from_json(const nlohmann::json& j);
    
    // 便捷构造函数
    static ResponseFrame success(const std::string& id, const nlohmann::json& payload);
    static ResponseFrame failure(const std::string& id, const ErrorShape& error);
};

/// 事件帧 (100% 对齐 OpenClaw)
struct EventFrame {
    std::string event;
    std::optional<int64_t> seq;
    nlohmann::json payload;
    
    nlohmann::json to_json() const;
    static core::Result<EventFrame> from_json(const nlohmann::json& j);
};

/// 帧验证器
class FrameValidator {
public:
    static bool validate_request_frame(const nlohmann::json& j);
    static bool validate_response_frame(const nlohmann::json& j);
    static bool validate_event_frame(const nlohmann::json& j);
    
    static core::Result<FrameType> detect_frame_type(const nlohmann::json& j);
};

/// 错误代码常量 (100% 对齐 OpenClaw)
namespace ErrorCodes {
    constexpr const char* INTERNAL_ERROR = "INTERNAL_ERROR";
    constexpr const char* INVALID_REQUEST = "INVALID_REQUEST";
    constexpr const char* METHOD_NOT_FOUND = "METHOD_NOT_FOUND";
    constexpr const char* PERMISSION_DENIED = "PERMISSION_DENIED";
    constexpr const char* AUTH_FAILED = "AUTH_FAILED";
    constexpr const char* RATE_LIMITED = "RATE_LIMITED";
    constexpr const char* NOT_AUTHENTICATED = "NOT_AUTHENTICATED";
    constexpr const char* DEVICE_TOKEN_MISMATCH = "DEVICE_TOKEN_MISMATCH";
}

/// WebSocket 关闭代码提示 (100% 对齐 OpenClaw)
namespace CloseCodeHints {
    constexpr const char* NORMAL_CLOSURE = "normal closure";
    constexpr const char* ABNORMAL_CLOSURE = "abnormal closure (no close frame)";
    constexpr const char* POLICY_VIOLATION = "policy violation";
    constexpr const char* SERVICE_RESTART = "service restart";
    
    std::string get_hint(int code);
}

/// Hello 挑战和握手 (OpenClaw connect 协议)
struct ConnectChallenge {
    std::string nonce;
    
    static EventFrame create_event(const std::string& nonce);
};

struct ConnectRequest {
    std::string device_id;
    std::string device_token;
    std::string client_name;
    std::string client_version;
    std::optional<std::string> platform;
    std::optional<int> protocol_version;
    
    nlohmann::json to_params() const;
};

struct HelloOk {
    std::string conn_id;
    int64_t server_time;
    int protocol_version;
    
    static core::Result<HelloOk> from_payload(const nlohmann::json& j);
};

/// Tick 心跳事件 (100% 对齐 OpenClaw)
struct TickEvent {
    int64_t ts;
    std::optional<int> presence_version;
    std::optional<int> health_version;
    
    static EventFrame create_event(int64_t ts, 
                                   std::optional<int> presence_ver = std::nullopt,
                                   std::optional<int> health_ver = std::nullopt);
};

} // namespace gateway
} // namespace hunterclaw
