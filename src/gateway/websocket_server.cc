#include "hunterclaw/gateway/websocket_server.h"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// WebSocketResponse 实现
// ============================================================================

WebSocketResponse WebSocketResponse::success(const std::string& id, const json& result) {
    WebSocketResponse res;
    res.type = "response";
    res.id = id;
    res.result = result;
    return res;
}

WebSocketResponse WebSocketResponse::create_error(const std::string& id, const std::string& code, 
                                                  const std::string& message, const json& data) {
    WebSocketResponse res;
    res.type = "error";
    res.id = id;
    res.error_info = json{
        {"code", code},
        {"message", message}
    };
    if (!data.is_null() && !data.empty()) {
        res.error_info["data"] = data;
    }
    return res;
}

json WebSocketResponse::to_json() const {
    if (type == "error") {
        return json{
            {"type", type},
            {"id", id},
            {"error", error_info}
        };
    } else {
        return json{
            {"type", type},
            {"id", id},
            {"result", result}
        };
    }
}

// ============================================================================
// WebSocketEvent 实现
// ============================================================================

json WebSocketEvent::to_json() const {
    json j;
    j["jsonrpc"] = "2.0";
    j["method"] = event;
    j["params"] = payload;
    
    return j;
}

// ============================================================================
// WebSocketServer 实现 (STUB - Crow 库未安装)
// ============================================================================

WebSocketServer::WebSocketServer(const WebSocketServerOptions& options)
    : options_(options) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->warn("[WebSocketServer] STUB IMPLEMENTATION - Crow library not available");
        logger->info("[WebSocketServer] Would initialize on {}:{}", options.host, options.port);
    }
}

WebSocketServer::~WebSocketServer() = default;

// ========================================================================
// 启动和停止
// ========================================================================

bool WebSocketServer::start() {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->error("[WebSocketServer] Cannot start - stub implementation (Crow library required)");
    }
    return false;
}

void WebSocketServer::stop(int timeout_ms) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->info("[WebSocketServer] Stop called (stub)");
    }
}

bool WebSocketServer::is_running() const {
    return false;
}

// ========================================================================
// 广播和事件 (对齐 OpenClaw)
// ========================================================================

void WebSocketServer::broadcast_event(
    const std::string& event,
    const nlohmann::json& payload,
    bool drop_if_slow
) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[WebSocketServer] Broadcast event: {} (stub)", event);
    }
}

bool WebSocketServer::send_event(
    const std::string& conn_id,
    const std::string& event,
    const nlohmann::json& payload
) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[WebSocketServer] Send event to {}: {} (stub)", conn_id, event);
    }
    return false;
}

void WebSocketServer::send_to_session(
    const std::string& session_key,
    const std::string& event,
    const nlohmann::json& payload
) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[WebSocketServer] Send to session {}: {} (stub)", session_key, event);
    }
}

// ========================================================================
// 连接管理
// ========================================================================

std::vector<std::shared_ptr<WebSocketClient>> WebSocketServer::get_clients() const {
    return {};
}

size_t WebSocketServer::get_authenticated_count() const {
    return 0;
}

std::shared_ptr<WebSocketClient> WebSocketServer::get_client(const std::string& conn_id) const {
    return nullptr;
}

void WebSocketServer::disconnect_client(
    const std::string& conn_id,
    int code,
    const std::string& reason
) {
    auto logger = spdlog::get("console");
    if (logger) {
        logger->debug("[WebSocketServer] Disconnect client {}: {} (stub)", conn_id, reason);
    }
}

// ========================================================================
// 统计信息
// ========================================================================

WebSocketServer::Stats WebSocketServer::get_stats() const {
    return Stats{};
}

}  // namespace gateway
}  // namespace hunterclaw
