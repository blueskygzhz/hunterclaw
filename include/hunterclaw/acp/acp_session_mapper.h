#pragma once

// ============================================================================
// acp_session_mapper.h
// 🎯 严格对齐 OpenClaw: src/acp/session-mapper.ts
//
// 职责:
//   - 解析 ACP 请求 meta 中的 session 路由参数
//   - 按优先级 resolve 最终的 session key
//   - 处理 resetSession 语义（清空 session 后重建）
//   - 在 session metadata 中标记 / 读取 acp_session_id
//
// 设计: session_mapper 不依赖 GatewayClient（避免引入 WebSocket 依赖），
// 所有 gateway 交互通过函数回调注入。
// ============================================================================

#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace hunterclaw {

// Forward declarations
namespace core {
template<typename T> class Result;
}
namespace gateway {
class GatewayClient;
}

namespace acp {

// ── Session Meta (从 ACP new/load/resume 请求 meta 字段解析) ─────────────────

struct AcpSessionMeta_Parsed {
    std::optional<std::string> session_key;
    std::optional<std::string> session_label;
    std::optional<bool> reset_session;
    std::optional<bool> require_existing;
    std::optional<std::string> conversation_id;
};

// ── Session Key Resolution Options ────────────────────────────────────────────

struct SessionKeyResolutionOptions {
    std::optional<std::string> default_session_key;
    std::optional<std::string> default_session_label;
    bool require_existing_session = false;
    bool allow_create = true;
};

struct SessionKeyResolution {
    std::string session_key;
    bool is_new = false;
    std::string source;  // "explicit" | "conversation" | "default" | "new"
};

// ── Gateway function types (callbacks instead of direct dependency) ───────────

// Gateway request function: (method, params) -> Result<json>
using GatewayRequestFn = std::function<core::Result<nlohmann::json>(
    const std::string& method, const nlohmann::json& params)>;

// ── Public API ────────────────────────────────────────────────────────────────

// 🎯 对齐 session-mapper.ts: parseSessionMeta(meta)
AcpSessionMeta_Parsed parse_session_meta(const nlohmann::json& meta);

// 🎯 对齐 session-mapper.ts: resolveSessionKey(parsed, defaults, opts)
SessionKeyResolution resolve_session_key(
    const AcpSessionMeta_Parsed& parsed,
    const SessionKeyResolutionOptions& opts,
    std::function<std::optional<std::string>(const std::string&)> conversation_lookup = {});

// 🎯 对齐 session-mapper.ts: resetSessionIfNeeded(meta, requestFn, sessionKey)
// requestFn: 可选，传入则执行 sessions.reset，否则返回 false
bool reset_session_if_needed(
    const AcpSessionMeta_Parsed& parsed,
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& session_key,
    const std::optional<std::string>& session_label = std::nullopt);

// 🔧 简化版: 通过 GatewayClient 指针调用（forward decl 在 hunterclaw 命名空间顶层）
// 用于已有 GatewayClient 实例的场景（translator.cc / client.cc）
// 注意: 使用这些重载需要 #include "hunterclaw/gateway/client.h"
bool reset_session_if_needed(
    const AcpSessionMeta_Parsed& parsed,
    gateway::GatewayClient* gw,
    const std::string& session_key,
    const std::optional<std::string>& session_label = std::nullopt);

// 🎯 对齐 session-mapper.ts: markSessionWithAcpId(requestFn, sessionKey, acpSessionId)
void mark_session_with_acp_id(
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& session_key,
    const std::string& acp_session_id);

// 🔧 简化版: 通过 GatewayClient 指针标记
void mark_session_with_acp_id(
    gateway::GatewayClient* gw,
    const std::string& session_key,
    const std::string& acp_session_id);

// 🎯 对齐 session-mapper.ts: resolveSessionKeyFromConversationId
std::optional<std::string> resolve_session_key_from_conversation_id(
    const std::optional<GatewayRequestFn>& request_fn,
    const std::string& conversation_id);

// 🔧 简化版: 通过 GatewayClient 指针查找
std::optional<std::string> resolve_session_key_from_conversation_id(
    gateway::GatewayClient* gw,
    const std::string& conversation_id);

// ── ACP Session ID helpers ────────────────────────────────────────────────────

static constexpr const char* ACP_SESSION_ID_PREFIX = "acp:";

inline std::string build_acp_session_id(const std::string& session_id) {
    return std::string(ACP_SESSION_ID_PREFIX) + session_id;
}

inline std::optional<std::string> parse_acp_session_id(const std::string& id) {
    std::string prefix(ACP_SESSION_ID_PREFIX);
    if (id.size() > prefix.size() &&
        id.compare(0, prefix.size(), prefix) == 0) {
        return id.substr(prefix.size());
    }
    return std::nullopt;
}

} // namespace acp
} // namespace hunterclaw
