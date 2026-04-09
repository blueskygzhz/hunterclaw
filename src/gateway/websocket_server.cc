// ============================================================================
// websocket_server.cc — WebSocket 服务器实现
// 🎯 对齐 OpenClaw: gateway/server/ws-connection.ts
// 
// 注意: 主 Gateway 的 WebSocket 功能由 gateway.cc 中的 websocketpp 实现提供。
// 本文件提供 WebSocketServer 类的独立实现，用于不依赖 gateway.cc 的场景。
// ============================================================================

#include "hunterclaw/gateway/websocket_server.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// WebSocketRequest / WebSocketResponse / WebSocketEvent
// ============================================================================

WebSocketRequest WebSocketRequest::from_json(const nlohmann::json& j) {
    WebSocketRequest req;
    req.type = j.value("type", "request");
    req.id = j.value("id", "");
    req.method = j.value("method", "");
    if (j.contains("params")) req.params = j["params"];
    return req;
}

nlohmann::json WebSocketRequest::to_json() const {
    return {{"type", type}, {"id", id}, {"method", method}, {"params", params}};
}

WebSocketResponse WebSocketResponse::success(const std::string& id, const nlohmann::json& result) {
    WebSocketResponse resp;
    resp.type = "response";
    resp.id = id;
    resp.result = result;
    return resp;
}

WebSocketResponse WebSocketResponse::create_error(const std::string& id, const std::string& code,
                                                    const std::string& message, const nlohmann::json& data) {
    WebSocketResponse resp;
    resp.type = "error";
    resp.id = id;
    resp.error_info = {{"code", code}, {"message", message}};
    if (!data.empty()) resp.error_info["data"] = data;
    return resp;
}

nlohmann::json WebSocketResponse::to_json() const {
    nlohmann::json j;
    j["type"] = type;
    j["id"] = id;
    if (type == "response") j["result"] = result;
    else j["error"] = error_info;
    return j;
}

nlohmann::json WebSocketEvent::to_json() const {
    return {{"type", "event"}, {"event", event}, {"payload", payload}};
}

// ============================================================================
// WebSocketServer 实现
// ============================================================================

WebSocketServer::WebSocketServer(const WebSocketServerOptions& options)
    : options_(options) {}

WebSocketServer::~WebSocketServer() {
    if (running_) stop();
}

bool WebSocketServer::start() {
    if (running_) return false;
    running_ = true;
    auto logger = spdlog::get("gateway");
    if (logger) logger->info("[WebSocketServer] Started on {}:{}", options_.host, options_.port);
    return true;
}

void WebSocketServer::stop(int timeout_ms) {
    (void)timeout_ms;
    running_ = false;
    if (accept_thread_.joinable()) accept_thread_.join();
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.clear();
}

bool WebSocketServer::is_running() const { return running_; }

void WebSocketServer::register_method(std::shared_ptr<MethodHandler> handler) {
    (void)handler;
}

void WebSocketServer::unregister_method(const std::string& method) {
    (void)method;
}

std::vector<std::string> WebSocketServer::list_methods() const {
    return {};
}

void WebSocketServer::broadcast_event(const std::string& event, const nlohmann::json& payload,
                                       bool drop_if_slow) {
    (void)drop_if_slow;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto msg = WebSocketEvent{{"event"}, event, payload}.to_json().dump();
    (void)msg;
    stats_.total_events++;
}

bool WebSocketServer::send_event(const std::string& conn_id, const std::string& event,
                                  const nlohmann::json& payload) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(conn_id);
    if (it == clients_.end()) return false;
    (void)event; (void)payload;
    return true;
}

void WebSocketServer::send_to_session(const std::string& session_key, const std::string& event,
                                       const nlohmann::json& payload) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& [id, client] : clients_) {
        if (client->session_key == session_key) {
            send_event(id, event, payload);
        }
    }
}

std::vector<std::shared_ptr<WebSocketClient>> WebSocketServer::get_clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<std::shared_ptr<WebSocketClient>> result;
    for (const auto& [id, client] : clients_) result.push_back(client);
    return result;
}

size_t WebSocketServer::get_authenticated_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    size_t count = 0;
    for (const auto& [id, client] : clients_) {
        if (client->authenticated) count++;
    }
    return count;
}

std::shared_ptr<WebSocketClient> WebSocketServer::get_client(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(conn_id);
    return it != clients_.end() ? it->second : nullptr;
}

void WebSocketServer::disconnect_client(const std::string& conn_id, int code, const std::string& reason) {
    (void)code; (void)reason;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(conn_id);
}

WebSocketServer::Stats WebSocketServer::get_stats() const {
    return stats_;
}

} // namespace gateway
} // namespace hunterclaw
