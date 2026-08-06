// ============================================================================
// acp_stdio_transport.cc
// 🎯 严格对齐 OpenClaw: src/acp/server.ts (ndJsonStream + AgentSideConnection)
//
// stdio 上的 ACP JSON-RPC 传输层实现。每行一个 JSON 消息（ndjson）：
//   入站 request:      {"jsonrpc":"2.0","id":<id>,"method":"session/prompt","params":{...}}
//   入站 notification: {"jsonrpc":"2.0","method":"session/cancel","params":{...}}
//   入站 response:     {"jsonrpc":"2.0","id":<id>,"result":{...}} | {...,"error":{...}}
//   出站同理。
// 所有诊断日志一律写 stderr，绝不写 stdout（stdout 专供 ndjson）。
// ============================================================================

#include "hunterclaw/acp/acp_stdio_transport.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

namespace hunterclaw {
namespace acp {

AcpStdioConnection::AcpStdioConnection() = default;

AcpStdioConnection::~AcpStdioConnection() {
    stop();
}

void AcpStdioConnection::register_method(const std::string& method,
    AcpMethodHandler handler) {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    handlers_[method] = std::move(handler);
}

void AcpStdioConnection::start() {
 bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;
    read_thread_ = std::thread([this] { read_loop(); });
}

void AcpStdioConnection::stop() {
    if (closed_.exchange(true)) {
        if (read_thread_.joinable() && std::this_thread::get_id() != read_thread_.get_id())
 read_thread_.join();
        return;
 }
    // 唤醒所有等待 server-initiated response 的调用方。
  {
        std::lock_guard<std::mutex> lk(outbound_mu_);
     for (auto& [id, slot] : outbound_) {
   std::lock_guard<std::mutex> slk(slot->mu);
            if (!slot->done) {
         slot->done = true;
              slot->result = nlohmann::json{{"__acp_error", true},
 {"code", static_cast<int>(JsonRpcErrorCode::InternalError)},
  {"message", "connection closed"}};
       slot->cv.notify_all();
      }
        }
    }
    // 关闭 stdin 以打断阻塞的 getline。
    if (read_thread_.joinable() && std::this_thread::get_id() != read_thread_.get_id()) {
      // 通过关闭 fd 0 触发 getline 返回。
  std::fclose(stdin);
        read_thread_.join();
    }
    {
     std::lock_guard<std::mutex> lk(closed_cb_mu_);
        if (!on_closed_fired_ && on_closed_) {
      on_closed_fired_ = true;
            on_closed_();
        }
    }
    {
        std::lock_guard<std::mutex> lk(closed_wait_mu_);
        closed_wait_cv_.notify_all();
 }
}

void AcpStdioConnection::wait_closed() {
    std::unique_lock<std::mutex> lk(closed_wait_mu_);
    closed_wait_cv_.wait(lk, [this] { return closed_.load(); });
}

void AcpStdioConnection::read_loop() {
    std::string line;
    while (!closed_.load()) {
     if (!std::getline(std::cin, line)) {
            break;  // EOF 或错误
        }
        if (line.empty()) continue;
      // 去掉可能的尾部 \r（Windows 客户端）
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        dispatch_line(line);
    }
    // stdin EOF → 触发关闭。
    if (!closed_.exchange(true)) {
        {
            std::lock_guard<std::mutex> lk(outbound_mu_);
    for (auto& [id, slot] : outbound_) {
     std::lock_guard<std::mutex> slk(slot->mu);
 if (!slot->done) {
         slot->done = true;
             slot->result = nlohmann::json{{"__acp_error", true},
              {"code", static_cast<int>(JsonRpcErrorCode::InternalError)},
                  {"message", "stdin closed"}};
     slot->cv.notify_all();
  }
          }
     }
      {
         std::lock_guard<std::mutex> lk(closed_cb_mu_);
     if (!on_closed_fired_ && on_closed_) {
on_closed_fired_ = true;
         on_closed_();
      }
        }
        {
            std::lock_guard<std::mutex> lk(closed_wait_mu_);
   closed_wait_cv_.notify_all();
        }
    }
}

void AcpStdioConnection::dispatch_line(const std::string& line) {
    nlohmann::json msg;
    try {
    msg = nlohmann::json::parse(line);
    } catch (const std::exception& e) {
        log(std::string("parse error: ") + e.what());
        // 无法确定 id，回一个 null-id parse error（对齐 JSON-RPC）。
        write_error_response(nullptr, JsonRpcErrorCode::ParseError,
         std::string("Parse error: ") + e.what());
        return;
    }
if (!msg.is_object()) {
        write_error_response(nullptr, JsonRpcErrorCode::InvalidRequest,
         "Invalid Request: not an object");
        return;
    }
    // 区分：入站 response（client → server 对我们主动 request 的回包） vs
    //       入站 request/notification。
    const bool has_method = msg.contains("method") && msg["method"].is_string();
    const bool has_result_or_error = msg.contains("result") || msg.contains("error");
    if (!has_method && has_result_or_error) {
        handle_inbound_response(msg);
        return;
    }
    if (!has_method) {
        write_error_response(msg.value("id", nlohmann::json(nullptr)),
     JsonRpcErrorCode::InvalidRequest,
  "Invalid Request: missing method");
        return;
    }
    handle_inbound_request(msg);
}

void AcpStdioConnection::handle_inbound_request(const nlohmann::json& msg) {
    AcpRequestContext ctx;
    ctx.method = msg["method"].get<std::string>();
    ctx.params = msg.contains("params") ? msg["params"] : nlohmann::json::object();
    if (msg.contains("id") && !msg["id"].is_null()) {
    ctx.id = msg["id"];
    }

    AcpMethodHandler handler;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
   auto it = handlers_.find(ctx.method);
        if (it != handlers_.end()) handler = it->second;
}

    if (!handler) {
     if (!ctx.is_notification()) {
            write_error_response(*ctx.id, JsonRpcErrorCode::MethodNotFound,
"Method not found: " + ctx.method);
        } else {
  log("no handler for notification: " + ctx.method);
        }
        return;
    }

    try {
        nlohmann::json result = handler(ctx);
        if (!ctx.is_notification()) {
    write_frame(nlohmann::json{
           {"jsonrpc", "2.0"},
          {"id", *ctx.id},
     {"result", result},
  });
        }
    } catch (const AcpRpcError& e) {
        if (!ctx.is_notification()) {
      write_error_response(*ctx.id, e.code, e.message, e.data);
    } else {
            log("notification handler error: " + e.message);
        }
} catch (const std::exception& e) {
        if (!ctx.is_notification()) {
 write_error_response(*ctx.id, JsonRpcErrorCode::InternalError,
            std::string("Internal error: ") + e.what());
        } else {
            log(std::string("notification handler exception: ") + e.what());
     }
    }
}

void AcpStdioConnection::handle_inbound_response(const nlohmann::json& msg) {
    if (!msg.contains("id") || !msg["id"].is_number_integer()) {
        log("inbound response without integer id ignored");
   return;
    }
    int64_t id = msg["id"].get<int64_t>();
    std::shared_ptr<PendingOutbound> slot;
    {
        std::lock_guard<std::mutex> lk(outbound_mu_);
   auto it = outbound_.find(id);
        if (it == outbound_.end()) {
   log("inbound response for unknown id ignored");
  return;
        }
        slot = it->second;
    }
    nlohmann::json value;
    if (msg.contains("error")) {
        const auto& err = msg["error"];
        value = nlohmann::json{
      {"__acp_error", true},
      {"code", err.value("code", static_cast<int>(JsonRpcErrorCode::InternalError))},
            {"message", err.value("message", std::string("error"))},
        };
 } else {
        value = msg.contains("result") ? msg["result"] : nlohmann::json::object();
    }
    {
        std::lock_guard<std::mutex> slk(slot->mu);
  slot->done = true;
   slot->result = std::move(value);
        slot->cv.notify_all();
    }
}

void AcpStdioConnection::send_notification(const std::string& method,
      const nlohmann::json& params) {
    if (closed_.load()) return;
    write_frame(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
    });
}

nlohmann::json AcpStdioConnection::send_request(const std::string& method,
        const nlohmann::json& params,
        int64_t timeout_ms) {
    if (closed_.load()) {
        return nlohmann::json{{"__acp_error", true},
        {"code", static_cast<int>(JsonRpcErrorCode::InternalError)},
         {"message", "connection closed"}};
    }
    int64_t id = next_request_id();
    auto slot = std::make_shared<PendingOutbound>();
    {
        std::lock_guard<std::mutex> lk(outbound_mu_);
        outbound_[id] = slot;
    }
    write_frame(nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params},
    });

    nlohmann::json result;
    {
    std::unique_lock<std::mutex> slk(slot->mu);
        if (timeout_ms > 0) {
            slot->cv.wait_for(slk, std::chrono::milliseconds(timeout_ms),
            [&] { return slot->done; });
          if (!slot->done) {
     result = nlohmann::json{{"__acp_error", true},
              {"code", static_cast<int>(JsonRpcErrorCode::RequestCancelled)},
             {"message", "request timed out"}};
            } else {
    result = slot->result;
       }
        } else {
slot->cv.wait(slk, [&] { return slot->done; });
   result = slot->result;
        }
    }
    {
        std::lock_guard<std::mutex> lk(outbound_mu_);
        outbound_.erase(id);
    }
    return result;
}

void AcpStdioConnection::write_frame(const nlohmann::json& frame) {
    std::string line = frame.dump();
    line.push_back('\n');
    std::lock_guard<std::mutex> lk(write_mu_);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void AcpStdioConnection::write_error_response(const nlohmann::json& id,
     JsonRpcErrorCode code,
  const std::string& message,
          const nlohmann::json& data) {
    nlohmann::json err{
        {"code", static_cast<int>(code)},
        {"message", message},
    };
    if (!data.is_null()) err["data"] = data;
    write_frame(nlohmann::json{
{"jsonrpc", "2.0"},
        {"id", id},
        {"error", err},
    });
}

int64_t AcpStdioConnection::next_request_id() {
    return request_seq_.fetch_add(1);
}

void AcpStdioConnection::log(const std::string& msg) {
    if (verbose_.load()) {
        std::cerr << "[acp-stdio] " << msg << "\n";
    }
}

}  // namespace acp
}  // namespace hunterclaw
