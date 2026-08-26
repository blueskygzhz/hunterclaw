// ============================================================================
// remote_a2a/json_rpc.cc
// JSON-RPC 2.0 请求构造与响应解析。
// ============================================================================

#include "hunterclaw/remote_a2a/json_rpc.h"

#include <atomic>

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::atomic<uint64_t> g_rpc_seq{1};

/// 截断远端返回的错误文本。
/// ⚠ 远端可控 —— 不截断就是一条「往日志里灌 100MB」的免费 DoS。
std::string clip(const std::string& s, size_t n = 400) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...(truncated)";
}

}  // namespace

const char* rpc_failure_kind_text(RpcFailureKind k) {
    switch (k) {
        case RpcFailureKind::None: return "none";
        case RpcFailureKind::Transport: return "transport";
        case RpcFailureKind::HttpStatus: return "http_status";
        case RpcFailureKind::AuthRequired: return "auth_required";
        case RpcFailureKind::RpcError: return "rpc_error";
        case RpcFailureKind::Unsupported: return "unsupported";
        case RpcFailureKind::TaskNotFound: return "task_not_found";
        case RpcFailureKind::Malformed: return "malformed";
    }
    return "unknown";
}

bool RpcFailure::retryable() const {
    switch (kind) {
        case RpcFailureKind::Transport:
            return true;
        case RpcFailureKind::HttpStatus:
            // 5xx / 429 是上游抖动；4xx（除 401/403 已单列）是我们发错了，重试无意义
            return http_status >= 500 || http_status == 429;
        case RpcFailureKind::RpcError:
            // 只有 InternalError 是瞬时的；其余业务码重试也是同样答案
            return rpc_code == RPC_INTERNAL_ERROR;
        // ★ 以下一律不重试：答案已经确定
        case RpcFailureKind::Unsupported:
        case RpcFailureKind::TaskNotFound:
        case RpcFailureKind::AuthRequired:
        case RpcFailureKind::Malformed:
        case RpcFailureKind::None:
            return false;
    }
    return false;
}

std::string RpcFailure::to_text() const {
    std::string s = rpc_failure_kind_text(kind);
    if (http_status != 0) s += " http=" + std::to_string(http_status);
    if (rpc_code != 0) s += " code=" + std::to_string(rpc_code);
    if (!message.empty()) s += " " + message;
    return s;
}

std::string next_rpc_id() {
    return "hc-" + std::to_string(g_rpc_seq.fetch_add(1));
}

nlohmann::json build_rpc_request(const std::string& id, const std::string& method,
                                const nlohmann::json& params) {
    nlohmann::json j;
    j["jsonrpc"] = "2.0";
    j["id"] = id;
    j["method"] = method;
    // ⚠ params 必须是 object/array；null 会被部分服务端判 InvalidRequest
    j["params"] = params.is_null() ? nlohmann::json::object() : params;
    return j;
}

bool is_unsupported_rpc_code(int code) {
    return code == RPC_METHOD_NOT_FOUND || code == A2A_UNSUPPORTED_OPERATION ||
           code == A2A_PUSH_NOT_SUPPORTED;
}

bool is_task_not_found_code(int code) { return code == A2A_TASK_NOT_FOUND; }

RpcFailure failure_from_rpc_error(const nlohmann::json& e) {
    RpcFailure f;
    f.kind = RpcFailureKind::RpcError;

    if (e.is_object()) {
        auto c = e.find("code");
        if (c != e.end() && c->is_number_integer()) f.rpc_code = c->get<int>();
        auto m = e.find("message");
        if (m != e.end() && m->is_string()) f.message = clip(m->get<std::string>());
        // data 常带补充信息，但也常带一整个 stack trace —— 只在 message 空时兜底
        if (f.message.empty()) {
            auto d = e.find("data");
            if (d != e.end()) f.message = clip(d->dump());
        }
    } else if (!e.is_null()) {
        f.message = clip(e.dump());
    }

    // ★ 分类优先级：Unsupported / TaskNotFound 是永久性结论，必须先判
    if (is_unsupported_rpc_code(f.rpc_code)) {
        f.kind = RpcFailureKind::Unsupported;
    } else if (is_task_not_found_code(f.rpc_code)) {
        f.kind = RpcFailureKind::TaskNotFound;
    }
    if (f.message.empty()) f.message = "远端返回 JSON-RPC error";
    return f;
}

RpcFailure failure_from_http(int status, const std::string& body) {
    RpcFailure f;
    f.http_status = status;
    // ★ 401/403 不是普通 HTTP 失败，而是「需要补凭据」的信号 ——
    //   调用方要把它映射成 TaskState::AuthRequired 交给上层处理，
    //   而不是当成传输错误去重试（重试一万次也还是 401）。
    f.kind = (status == 401 || status == 403) ? RpcFailureKind::AuthRequired
                                             : RpcFailureKind::HttpStatus;
    f.message = "HTTP " + std::to_string(status);
    if (!body.empty()) f.message += ": " + clip(body, 200);
    return f;
}

RpcFailure failure_from_transport(const std::string& message) {
    RpcFailure f;
    f.kind = RpcFailureKind::Transport;
    f.message = clip(message);
    return f;
}

core::Result<nlohmann::json> parse_rpc_response_json(const nlohmann::json& body,
                                                     const std::string& expect_id,
                                                     RpcFailure* out) {
    auto fail = [&](RpcFailure f) {
        if (out) *out = f;
        return core::Result<nlohmann::json>::Err(f.to_text());
    };

    if (!body.is_object()) {
        RpcFailure f;
        f.kind = RpcFailureKind::Malformed;
        f.message = "响应不是 JSON object";
        return fail(f);
    }

    // ② id 校验 —— 防响应替换。
    //   缺 id 的情况放行：部分服务端在 error 响应里省略 id（规范允许 null）。
    if (!expect_id.empty()) {
        auto it = body.find("id");
        if (it != body.end() && it->is_string()) {
            if (it->get<std::string>() != expect_id) {
                RpcFailure f;
                f.kind = RpcFailureKind::Malformed;
                f.message = "响应 id 不匹配（期望 " + expect_id + "）";
                return fail(f);
            }
        } else if (it != body.end() && it->is_number()) {
            // 数字 id：我们只发字符串 id，收到数字即不匹配
            RpcFailure f;
            f.kind = RpcFailureKind::Malformed;
            f.message = "响应 id 类型不匹配（期望字符串 " + expect_id + "）";
            return fail(f);
        }
    }

    // ③ error 优先 —— ★ A2A 的业务错误走 HTTP 200，只看状态码会漏掉
    auto err = body.find("error");
    if (err != body.end() && !err->is_null()) {
        return fail(failure_from_rpc_error(*err));
    }

    // ④ result 缺失
    auto res = body.find("result");
    if (res == body.end()) {
        RpcFailure f;
        f.kind = RpcFailureKind::Malformed;
        f.message = "响应既无 result 也无 error";
        return fail(f);
    }

    if (out) *out = RpcFailure{};
    return core::Result<nlohmann::json>::Ok(*res);
}

core::Result<nlohmann::json> parse_rpc_response(const std::string& body,
                                               const std::string& expect_id,
                                               RpcFailure* out) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        RpcFailure f;
        f.kind = RpcFailureKind::Malformed;
        f.message = std::string("响应非法 JSON: ") + clip(e.what(), 120);
        if (out) *out = f;
        return core::Result<nlohmann::json>::Err(f.to_text());
    }
    return parse_rpc_response_json(j, expect_id, out);
}

}  // namespace remote_a2a
}  // namespace hunterclaw
