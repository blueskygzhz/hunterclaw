#pragma once

// ============================================================================
// remote_a2a/json_rpc.h
// A2A 的 JSON-RPC 2.0 客户端外壳（只做「发请求 / 解响应」，不含传输）。
//
// ⚠ 这一层单独存在的三个理由 —— 都是踩过的坑：
//
// ① A2A 的错误【走 HTTP 200】。
//    JSON-RPC 规范里业务错误是 body 里的 "error"，HTTP 状态仍是 200。
//    只看 resp.ok 会把「远端明确报 TaskNotFound」当成成功，
//    然后在 result 缺失时拿到一个空 Task —— 状态机会误判为 submitted 并
//    永远轮询下去。所以【必须】先解 body 再判成败。
//
// ② 错误码决定的是【能力结论】，不是重试。
//    -32601 MethodNotFound / -32004 UnsupportedOperation 是【永久性】答案：
//    这个远端不支持该方法，不该再试第二次。而 -32603 InternalError 是瞬时的。
//    把两者都当"失败"处理，会导致每次任务都重复探测不存在的 tasks/get。
//    这正是 capabilities 探测缓存的判据来源（见 remote_agent_client.h）。
//
// ③ id 必须校验。
//    响应替换（response substitution）在共享连接/代理场景是真实攻击面：
//    拿 tasks/get 的响应冒充 message/send 的响应。SDK 会做，手写不做就没有。
//
// ── 与 ADK 的差异 ──────────────────────────────────────────────────────────
// ADK 不碰这一层（全交给 a2a-sdk），因此也就没有「错误码 → 能力结论」的映射：
// 客户端里 grep 不到 tasks/get，唯一兜底是 remote_a2a_agent.py:105 的
// DEFAULT_TIMEOUT = 600.0 —— 一个 httpx 超时。
// ============================================================================

#include <string>

#include <nlohmann/json.hpp>

#include "hunterclaw/core/types.h"

namespace hunterclaw {
namespace remote_a2a {

// ── 方法名 ──────────────────────────────────────────────────────────────────
// ⚠ 一律用字符串常量，不要内联字面量 —— 拼错只会得到 -32601，
//   而 -32601 会被记成"远端不支持"并缓存，排查成本极高。
constexpr const char* M_MESSAGE_SEND = "message/send";
constexpr const char* M_MESSAGE_STREAM = "message/stream";
constexpr const char* M_TASKS_GET = "tasks/get";
constexpr const char* M_TASKS_CANCEL = "tasks/cancel";
constexpr const char* M_TASKS_RESUBSCRIBE = "tasks/resubscribe";
constexpr const char* M_PUSH_SET = "tasks/pushNotificationConfig/set";
constexpr const char* M_PUSH_GET = "tasks/pushNotificationConfig/get";
constexpr const char* M_AUTH_EXTENDED_CARD = "agent/getAuthenticatedExtendedCard";

// ── 错误码 ──────────────────────────────────────────────────────────────────

/// JSON-RPC 标准码 + A2A 扩展码（A2A 规范 §8）
enum RpcErrorCode : int {
    RPC_PARSE_ERROR = -32700,
    RPC_INVALID_REQUEST = -32600,
    RPC_METHOD_NOT_FOUND = -32601,
    RPC_INVALID_PARAMS = -32602,
    RPC_INTERNAL_ERROR = -32603,

    A2A_TASK_NOT_FOUND = -32001,
    A2A_TASK_NOT_CANCELABLE = -32002,
    A2A_PUSH_NOT_SUPPORTED = -32003,
    A2A_UNSUPPORTED_OPERATION = -32004,
    A2A_CONTENT_TYPE_NOT_SUPPORTED = -32005,
    A2A_INVALID_AGENT_RESPONSE = -32006,
};

/// 失败归类。★ 这个枚举是给【调用方做决策】用的，不是给人看的。
enum class RpcFailureKind {
    None,
    /// 连不上 / 读写失败 / DNS 失败 —— 可重试
    Transport,
    /// HTTP 层非 2xx（且不是 401/403）
    HttpStatus,
    /// 401 / 403 —— 映射到 TaskState::AuthRequired，需要外部补凭据
    AuthRequired,
    /// body 里的 error，且不属于下面两类
    RpcError,
    /// ★ 永久性：远端不支持该方法（-32601 / -32004 / -32003）
    ///   调用方应把对应 capability 标成 No 并【不再探测】
    Unsupported,
    /// ★ 终态：任务在远端已不存在（-32001）
    ///   不能重试、不能继续轮询，直接判终态
    TaskNotFound,
    /// 响应不是合法 JSON-RPC / id 不匹配 / result 缺失
    Malformed,
};

const char* rpc_failure_kind_text(RpcFailureKind);

struct RpcFailure {
    RpcFailureKind kind = RpcFailureKind::None;
    int http_status = 0;
    int rpc_code = 0;
    std::string message;

    bool empty() const { return kind == RpcFailureKind::None; }

    /// 是否值得重试。
    /// ⚠ Unsupported / TaskNotFound / AuthRequired / Malformed 一律 false ——
    ///   重试它们只是把同一个确定的答案再问一遍。
    bool retryable() const;

    /// 人类可读（★ 不含任何凭据，可直接进日志）
    std::string to_text() const;
};

// ── 请求 ────────────────────────────────────────────────────────────────────

/// 进程内单调递增的请求 id，形如 "hc-17"。
/// ⚠ 不用随机数：可预测的单调 id 便于把日志和抓包对上。
std::string next_rpc_id();

nlohmann::json build_rpc_request(const std::string& id, const std::string& method,
                                 const nlohmann::json& params);

// ── 响应 ────────────────────────────────────────────────────────────────────

/// 解析 JSON-RPC 响应体，返回 result 节点。
///
/// @param body       原始响应体
/// @param expect_id  期望的 id；空串表示跳过校验（SSE 分片场景）
/// @param out        失败详情（可为 nullptr）
///
/// 判定顺序（★ 顺序即语义，不要重排）：
///   ① body 非法 JSON / 非 object            → Malformed
///   ② id 存在且与 expect_id 不符             → Malformed（响应替换防护）
///   ③ 有 "error"                            → 按 code 分类
///   ④ 无 "result"                            → Malformed
core::Result<nlohmann::json> parse_rpc_response(const std::string& body,
                                               const std::string& expect_id,
                                               RpcFailure* out);

/// 已解析好的 JSON 版本（SSE 每个事件都是一个完整 JSON-RPC 响应，
/// 复用同一套判定，避免两处逻辑漂移）。
core::Result<nlohmann::json> parse_rpc_response_json(const nlohmann::json& body,
                                                     const std::string& expect_id,
                                                     RpcFailure* out);

/// error 节点 → RpcFailure（含 code 分类）
RpcFailure failure_from_rpc_error(const nlohmann::json& error_node);

/// HTTP 状态 → RpcFailure。★ 401/403 单独归为 AuthRequired。
RpcFailure failure_from_http(int status, const std::string& body);

/// 传输层错误 → RpcFailure
RpcFailure failure_from_transport(const std::string& message);

/// ★ 永久性「不支持」：-32601 MethodNotFound / -32004 UnsupportedOperation
///   / -32003 PushNotificationNotSupported
bool is_unsupported_rpc_code(int code);

bool is_task_not_found_code(int code);

}  // namespace remote_a2a
}  // namespace hunterclaw
