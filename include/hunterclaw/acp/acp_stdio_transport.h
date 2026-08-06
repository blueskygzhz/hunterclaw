#pragma once

// ============================================================================
// acp_stdio_transport.h
// 🎯 严格对齐 OpenClaw: src/acp/server.ts (ndJsonStream + AgentSideConnection)
//
// C++ 侧没有 @agentclientprotocol/sdk，这里自实现 ACP 的 stdio JSON-RPC 传输层：
//   - 从 stdin 逐行读取 ndjson（每行一个 JSON-RPC 消息）
//   - 向 stdout 逐行写出 ndjson（response / notification / server-initiated request）
//   - 支持三类入站消息：request（有 id，需回 response）、notification（无 id）、
//     以及对 server 主动 request 的 response（client → server 回包）
//   - 支持 server 主动向 client 发 request（如 session/request_permission）并等待 client 响应
//
// 该层不依赖 gateway，只依赖标准库 + nlohmann::json，避免与 gateway 形成循环依赖。
// 上层（AcpGatewayAgent）通过注册 method handler 接入。
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace acp {

// ── JSON-RPC 错误码（对齐 JSON-RPC 2.0 规范 + ACP 约定）──────────────────────
enum class JsonRpcErrorCode : int {
    ParseError     = -32700,
    InvalidRequest = -32600,
 MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,
    // ACP 约定：请求被取消
    RequestCancelled = -32800,
};

// ── 入站请求上下文 ────────────────────────────────────────────────────────────
// method handler 收到的参数；返回值即 JSON-RPC result（对于 notification 忽略返回）。
struct AcpRequestContext {
    std::string method;
    nlohmann::json params;
    // 对于 request 有值（需回包）；对于 notification 为 nullopt。
    std::optional<nlohmann::json> id;
    bool is_notification() const { return !id.has_value(); }
};

// handler 返回：
//   - result：正常结果（仅 request 会被回包）
//   - 抛 AcpRpcError 表示业务错误（转 JSON-RPC error）
using AcpMethodHandler =
    std::function<nlohmann::json(const AcpRequestContext& ctx)>;

// handler 内部可抛出的错误，transport 会转成 JSON-RPC error object。
struct AcpRpcError {
    JsonRpcErrorCode code;
  std::string message;
    nlohmann::json data;
    AcpRpcError(JsonRpcErrorCode c, std::string m, nlohmann::json d = nullptr)
        : code(c), message(std::move(m)), data(std::move(d)) {}
};

// ── AcpStdioConnection ───────────────────────────────────────────────────────
// 对齐 OC: AgentSideConnection over ndJsonStream(stdout, stdin)。
// - start()：起后台线程从 stdin 读帧并分发
// - register_method()：注册入站 method handler
// - send_notification()：向 client 发 notification（如 session/update）
// - send_request()：向 client 发 request（如 session/request_permission）并同步等待响应
class AcpStdioConnection {
public:
    AcpStdioConnection();
    ~AcpStdioConnection();

    // 注册入站 method handler（request 与 notification 共用；用 ctx.is_notification 区分）。
    void register_method(const std::string& method, AcpMethodHandler handler);

  // 启动 stdin 读取循环（后台线程）。
    void start();

    // 停止并等待后台线程退出（stdin EOF 或显式 stop 都会触发）。
    void stop();

    // 阻塞直到连接关闭（stdin EOF / stop）。
    void wait_closed();

    bool is_closed() const { return closed_.load(); }

    // 向 client 发送 notification（无 id，不等待响应）。
    void send_notification(const std::string& method, const nlohmann::json& params);

    // 向 client 发送 request（有 id），同步等待 client 回 response。
    // 返回 client 的 result；若 client 回 error 或超时，返回带 error 的对象：
    //   { "__acp_error": true, "code": <int>, "message": <str> }
    nlohmann::json send_request(const std::string& method,
  const nlohmann::json& params,
 int64_t timeout_ms = 0);

    // 设置连接关闭回调（EOF 或 stop 时触发一次）。
    void set_on_closed(std::function<void()> cb) { on_closed_ = std::move(cb); }

    // verbose 日志开关（写 stderr，绝不写 stdout，避免污染 ndjson）。
    void set_verbose(bool v) { verbose_ = v; }

private:
    void read_loop();
    void dispatch_line(const std::string& line);
    void handle_inbound_request(const nlohmann::json& msg);
    void handle_inbound_response(const nlohmann::json& msg);
  void write_frame(const nlohmann::json& frame);
    void write_error_response(const nlohmann::json& id, JsonRpcErrorCode code,
       const std::string& message,
     const nlohmann::json& data = nullptr);
    int64_t next_request_id();
    void log(const std::string& msg);

    std::map<std::string, AcpMethodHandler> handlers_;
    std::mutex handlers_mu_;

    std::thread read_thread_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> started_{false};

    // stdout 写锁：保证每帧 ndjson 原子写出（多线程 notification/response 并发）。
    std::mutex write_mu_;

    // server-initiated request 的等待表：id → 响应槽。
    struct PendingOutbound {
   std::mutex mu;
        std::condition_variable cv;
  bool done = false;
        nlohmann::json result;
    };
    std::map<int64_t, std::shared_ptr<PendingOutbound>> outbound_;
    std::mutex outbound_mu_;
    std::atomic<int64_t> request_seq_{1};

    std::function<void()> on_closed_;
    bool on_closed_fired_ = false;
    std::mutex closed_cb_mu_;

    std::atomic<bool> verbose_{false};

    // wait_closed 用
    std::mutex closed_wait_mu_;
    std::condition_variable closed_wait_cv_;
};

}  // namespace acp
}  // namespace hunterclaw
