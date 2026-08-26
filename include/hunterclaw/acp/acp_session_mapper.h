#pragma once

// ============================================================================
// acp_session_mapper.h
// 🎯 严格对齐 OpenClaw: src/acp/session-mapper.ts
//
// ⚠ 重要历史说明（避免重复踩坑）：
//   本文件此前的实现与真源出入很大，且整个模块**零外部引用**（死代码）：
//     ① 发明了 mark_session_with_acp_id / resolve_session_key_from_conversation_id
//        两个函数 —— 真源不存在（grep 全仓 0 处）；
//     ② resolve_session_key 的解析顺序自创为
//        explicit → conversationId → default → new，真源是
//        label → key → optsLabel → optsKey → fallback（见下方注释）；
//     ③ reset_session_if_needed 用 `sessions.init` 实现"重置"，
//        真源用的是 **`sessions.reset`** —— init 不会清历史，
//        用它当 reset 会导致 resetSession=true 静默无效；
//     ④ parse_session_meta 漏了 prefixCwd 字段。
//
//   真源 session-mapper.ts 只导出三个函数：
//     parseSessionMeta / resolveSessionKey / resetSessionIfNeeded
//
// 注：ACP 请求路径上真正生效的 meta 解析在 acp_translator.cc 的
//     AcpGatewayAgent::parse_session_meta（已单独对齐）。本模块提供的是
//     与真源同构的独立实现，供后续 ACP client / 独立 server 复用。
// ============================================================================

#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway { class GatewayClient; }
namespace acp {

/// 対齐 session-mapper.ts:6-12 AcpSessionMeta
struct AcpSessionMeta_Parsed {
    std::optional<std::string> session_key;
    std::optional<std::string> session_label;
    std::optional<bool> reset_session;
    std::optional<bool> require_existing;
    std::optional<bool> prefix_cwd;   ///< 対齐:11（此前缺失）
};

/// 対齐 session-mapper.ts:15-27 parseSessionMeta(meta)
///
/// 键别名顺序即"当前名 → 历史名"优先级。
/// 字符串走 readString 语义（trim 后为空视为不存在，并**继续试下一个别名**）；
/// 布尔走 readBool 语义（仅接受真正的 boolean，不认 "true" 字符串）。
AcpSessionMeta_Parsed parse_session_meta(const nlohmann::json& meta);

/// 対齐 AcpServerOptions 中与会话解析相关的字段
struct SessionKeyResolutionOptions {
    std::optional<std::string> default_session_key;    ///< opts.defaultSessionKey
    std::optional<std::string> default_session_label;  ///< opts.defaultSessionLabel
    std::optional<bool> require_existing_session;      ///< opts.requireExistingSession
    std::optional<bool> reset_session;                 ///< opts.resetSession
};

/// gateway 请求回调：method + params → 解析后的 JSON（失败抛异常或返回 null）
using GatewayRequestFn =
    std::function<nlohmann::json(const std::string& method, const nlohmann::json& params)>;

/// 対齐 session-mapper.ts:30-88 resolveSessionKey(params)
///
/// 解析顺序**严格如下**（真源的 5 段 if，顺序不可调换）：
///   ① meta.sessionLabel 存在        → sessions.resolve{label} （必须解析成功，否则抛错）
///   ② meta.sessionKey 存在          → requireExisting ? sessions.resolve{key} : 直接用
///   ③ opts.defaultSessionLabel 存在 → sessions.resolve{label}
///   ④ opts.defaultSessionKey 存在   → requireExisting ? sessions.resolve{key} : 直接用
///   ⑤ 兜底                          → fallback_key
///
/// 关键点：
///   * label 路径**永不**跳过解析 —— label 不是 key，必须查表换算；
///   * key 路径仅在 requireExisting 为真时才查表校验存在性；
///   * requireExisting 的取值链是 meta → opts → false（対齐:38-39）；
///   * 解析失败必须抛错而非静默回落，否则会把消息投到错误会话。
/// @throws std::runtime_error 当 label/key 无法解析时（対齐:46,59,68,81）
std::string resolve_session_key(const AcpSessionMeta_Parsed& meta,
                                const std::string& fallback_key,
                                const SessionKeyResolutionOptions& opts,
                                const GatewayRequestFn& request_fn);

/// 対齐 session-mapper.ts:91-102 resetSessionIfNeeded(params)
///
/// resetSession 取值链：meta → opts → false（対齐:97）。
/// 为真时发 **`sessions.reset`**（不是 sessions.init —— 后者不清历史）。
/// @return 是否真的发出了 reset 请求
bool reset_session_if_needed(const AcpSessionMeta_Parsed& meta,
                             const std::string& session_key,
                             const SessionKeyResolutionOptions& opts,
                             const GatewayRequestFn& request_fn);

/// GatewayClient* 便捷重载（内部包一层 GatewayRequestFn）
std::string resolve_session_key(const AcpSessionMeta_Parsed& meta,
                                const std::string& fallback_key,
                                const SessionKeyResolutionOptions& opts,
                                gateway::GatewayClient* gw);

bool reset_session_if_needed(const AcpSessionMeta_Parsed& meta,
                             const std::string& session_key,
                             const SessionKeyResolutionOptions& opts,
                             gateway::GatewayClient* gw);

} // namespace acp
} // namespace hunterclaw
