// 🎯 严格对齐 OpenClaw: Gateway WebSocket Server
// 使用 POSIX socket + RFC 6455 协议实现
// 不依赖 Boost、Crow 或任何外部 WebSocket 库
// 对齐 OpenClaw: server-runtime-state.ts + server/ws-connection.ts + server-broadcast.ts

#include "hunterclaw/gateway/websocket_server.h"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <set>
#include <cstring>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// WebSocketResponse / WebSocketEvent 实现 (不变)
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
    // 🎯 OpenClaw 严格对齐: {type:"event", event, payload}
    json j;
    j["type"] = "event";
    j["event"] = event;
    if (!payload.is_null()) {
        j["payload"] = payload;
    }
    return j;
}

// ============================================================================
// RFC 6455 Helpers (internal)
// ============================================================================

namespace {

auto get_logger() {
    static auto logger = []() {
        auto log = spdlog::get("gateway-ws");
        if (!log) {
            log = spdlog::stdout_logger_mt("gateway-ws");
        }
        return log;
    }();
    return logger;
}

std::string base64_encode_ws(const unsigned char* data, size_t len) {
    BIO* bio = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    bio = BIO_push(bio, bmem);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data, static_cast<int>(len));
    BIO_flush(bio);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(bio, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(bio);
    return result;
}

// Compute Sec-WebSocket-Accept from client key (RFC 6455 §4.2.2)
std::string compute_accept(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-5AB5DC525DB5";
    std::string concat = key + GUID;
    unsigned char sha1[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), sha1);
    return base64_encode_ws(sha1, SHA_DIGEST_LENGTH);
}

// Build unmasked WS frame (server → client: no mask)
std::string build_server_frame(const std::string& payload, uint8_t opcode = 0x1) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(len);
    } else if (len < 65536) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) {
            frame += static_cast<char>((len >> (8 * i)) & 0xFF);
        }
    }
    frame += payload;
    return frame;
}

struct WsFrameParsed {
    uint8_t opcode;
    std::string payload;
    size_t consumed;
    bool valid;
};

WsFrameParsed parse_server_frame(const char* data, size_t len) {
    WsFrameParsed f{0, "", 0, false};
    if (len < 2) return f;
    uint8_t b0 = data[0], b1 = data[1];
    f.opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    size_t plen = b1 & 0x7F;
    size_t offset = 2;
    if (plen == 126) { if (len < 4) return f; plen = (uint8_t(data[2]) << 8) | uint8_t(data[3]); offset = 4; }
    else if (plen == 127) { if (len < 10) return f; plen = 0; for (int i = 0; i < 8; ++i) plen = (plen << 8) | uint8_t(data[2+i]); offset = 10; }
    uint8_t mask[4] = {};
    if (masked) { if (len < offset + 4) return f; memcpy(mask, data + offset, 4); offset += 4; }
    if (len < offset + plen) return f;
    f.payload.resize(plen);
    for (size_t i = 0; i < plen; ++i) f.payload[i] = data[offset + i] ^ (masked ? mask[i%4] : 0);
    f.consumed = offset + plen;
    f.valid = true;
    return f;
}

std::string generate_uuid_simple() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::string uuid = "xxxx-xx-xx-xx-xxxxxx";
    for (auto& c : uuid) {
        if (c == 'x') c = "0123456789abcdef"[dis(gen)];
    }
    return uuid;
}

} // namespace

// ============================================================================
// WebSocketServer Implementation (POSIX)
// 🎯 对齐 OpenClaw: noServer:true → 我们自己做 HTTP Upgrade
// ============================================================================

struct WebSocketClientImpl : public WebSocketClient {
    int fd = -1;
    std::string conn_id;
    std::string recv_buf;
    bool authenticated = false;
    std::optional<std::string> session_id;
};

struct WebSocketServer::Impl {
    WebSocketServerOptions options;
    int listen_fd = -1;
    std::atomic<bool> running{false};
    std::thread accept_thread;
    std::mutex clients_mutex;
    std::unordered_map<std::string, std::shared_ptr<WebSocketClientImpl>> clients;
    std::atomic<uint64_t> seq{0};

    // 🎯 对齐 OpenClaw server-broadcast.ts: EVENT_SCOPE_GUARDS
    static bool has_event_scope(const WebSocketClientImpl& c, const std::string& event) {
        // Simplified: all connected clients receive all events
        (void)c; (void)event;
        return true;
    }
};

WebSocketServer::WebSocketServer(const WebSocketServerOptions& options)
    : options_(options), impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    get_logger()->info("[WebSocketServer] Initializing on {}:{}", options.host, options.port);
}

WebSocketServer::~WebSocketServer() {
    stop(1000);
}

// ========================================================================
// Start / Stop
// ========================================================================

bool WebSocketServer::start() {
    // 🎯 对齐 OpenClaw: listenGatewayHttpServer → POSIX listen
    impl_->listen_fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (impl_->listen_fd < 0) {
        impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    }
    if (impl_->listen_fd < 0) {
        get_logger()->error("[WebSocketServer] socket() failed");
        return false;
    }

    int optval = 1;
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in6 addr6{};
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(static_cast<uint16_t>(impl_->options.port));
    addr6.sin6_addr = in6addr_any;

    if (::bind(impl_->listen_fd, reinterpret_cast<struct sockaddr*>(&addr6), sizeof(addr6)) < 0) {
        // Fallback to IPv4
        struct sockaddr_in addr4{};
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(static_cast<uint16_t>(impl_->options.port));
        addr4.sin_addr.s_addr = INADDR_ANY;
        if (::bind(impl_->listen_fd, reinterpret_cast<struct sockaddr*>(&addr4), sizeof(addr4)) < 0) {
            get_logger()->error("[WebSocketServer] bind() failed on port {}", impl_->options.port);
            ::close(impl_->listen_fd);
            impl_->listen_fd = -1;
            return false;
        }
    }

    if (::listen(impl_->listen_fd, 128) < 0) {
        get_logger()->error("[WebSocketServer] listen() failed");
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
        return false;
    }

    impl_->running = true;
    get_logger()->info("[WebSocketServer] Listening on port {}", impl_->options.port);

    // Accept loop in separate thread
    impl_->accept_thread = std::thread([this]() {
        while (impl_->running) {
            struct pollfd pfd{impl_->listen_fd, POLLIN, 0};
            int ret = poll(&pfd, 1, 500);
            if (ret <= 0) continue;

            struct sockaddr_storage client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = ::accept(impl_->listen_fd,
                                     reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
            if (client_fd < 0) continue;

            // Handle WS upgrade in a detached thread
            std::thread([this, client_fd]() {
                handle_new_connection(client_fd);
            }).detach();
        }
    });

    return true;
}

void WebSocketServer::handle_new_connection(int fd) {
    // Read HTTP upgrade request
    char buf[8192];
    std::string request;
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { ::close(fd); return; }
        request.append(buf, n);
        if (request.find("\r\n\r\n") != std::string::npos) break;
        if (request.size() > 65536) { ::close(fd); return; }
    }

    // Check for WebSocket upgrade
    if (request.find("Upgrade: websocket") == std::string::npos &&
        request.find("upgrade: websocket") == std::string::npos) {
        // Not a WS request — send 404
        std::string resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        ::send(fd, resp.data(), resp.size(), 0);
        ::close(fd);
        return;
    }

    // Extract Sec-WebSocket-Key
    std::string ws_key;
    auto key_pos = request.find("Sec-WebSocket-Key: ");
    if (key_pos == std::string::npos) key_pos = request.find("sec-websocket-key: ");
    if (key_pos != std::string::npos) {
        auto end_pos = request.find("\r\n", key_pos);
        ws_key = request.substr(key_pos + 19, end_pos - key_pos - 19);
        // Trim whitespace
        while (!ws_key.empty() && ws_key.back() == ' ') ws_key.pop_back();
    }
    if (ws_key.empty()) {
        std::string resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        ::send(fd, resp.data(), resp.size(), 0);
        ::close(fd);
        return;
    }

    // 🎯 对齐 OpenClaw: RFC 6455 §4.2.2 — send 101 Switching Protocols
    std::string accept_key = compute_accept(ws_key);
    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n";
    resp << "Upgrade: websocket\r\n";
    resp << "Connection: Upgrade\r\n";
    resp << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
    resp << "\r\n";
    std::string response = resp.str();
    ::send(fd, response.data(), response.size(), 0);

    // Create client
    auto client = std::make_shared<WebSocketClientImpl>();
    client->fd = fd;
    client->conn_id = generate_uuid_simple();

    {
        std::lock_guard<std::mutex> lock(impl_->clients_mutex);
        impl_->clients[client->conn_id] = client;
    }

    get_logger()->info("[WebSocketServer] Client connected: {}", client->conn_id);

    // 🎯 对齐 OpenClaw ws-connection.ts:174-179: 发送 connect.challenge
    json challenge = {
        {"type", "event"},
        {"event", "connect.challenge"},
        {"payload", {{"nonce", generate_uuid_simple()}, {"ts", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()}}}
    };
    auto challenge_frame = build_server_frame(challenge.dump());
    ::send(fd, challenge_frame.data(), challenge_frame.size(), MSG_NOSIGNAL);

    // Read loop
    std::string recv_buf;
    char read_buf[65536];
    while (impl_->running) {
        struct pollfd pfd{fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        ssize_t n = ::recv(fd, read_buf, sizeof(read_buf), 0);
        if (n <= 0) break;

        recv_buf.append(read_buf, n);

        while (recv_buf.size() >= 2) {
            auto frame = parse_server_frame(recv_buf.data(), recv_buf.size());
            if (!frame.valid) break;
            recv_buf.erase(0, frame.consumed);

            switch (frame.opcode) {
                case 0x1: case 0x2: {  // Text / Binary
                    // 🎯 对齐 OpenClaw: message-handler.ts — RPC dispatch
                    handle_client_message(client, frame.payload);
                    break;
                }
                case 0x8: {  // Close
                    auto close_frame = build_server_frame("", 0x8);
                    ::send(fd, close_frame.data(), close_frame.size(), MSG_NOSIGNAL);
                    goto cleanup;
                }
                case 0x9: {  // Ping → Pong
                    auto pong = build_server_frame(frame.payload, 0xA);
                    ::send(fd, pong.data(), pong.size(), MSG_NOSIGNAL);
                    break;
                }
                default: break;
            }
        }
    }

cleanup:
    {
        std::lock_guard<std::mutex> lock(impl_->clients_mutex);
        impl_->clients.erase(client->conn_id);
    }
    ::close(fd);
    get_logger()->info("[WebSocketServer] Client disconnected: {}", client->conn_id);
}

void WebSocketServer::handle_client_message(
    std::shared_ptr<WebSocketClientImpl> client,
    const std::string& message
) {
    // 🎯 对齐 OpenClaw message-handler.ts:258-319: parse JSON → dispatch RPC
    try {
        auto parsed = json::parse(message);
        if (!parsed.contains("type") || !parsed["type"].is_string()) return;

        std::string type = parsed["type"].get<std::string>();
        if (type == "req") {
            std::string id = parsed.value("id", "");
            std::string method = parsed.value("method", "");
            auto params = parsed.contains("params") ? parsed["params"] : json::object();

            // Dispatch to registered method handlers (via GatewayServer)
            if (on_request_) {
                auto result = on_request_(client->conn_id, method, params);
                json resp = {
                    {"type", "res"},
                    {"id", id},
                    {"ok", result.is_ok()},
                };
                if (result.is_ok()) resp["payload"] = result.get();
                else resp["error"] = json{{"code", "error"}, {"message", result.error}};

                auto frame = build_server_frame(resp.dump());
                ::send(client->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            }
        }
    } catch (const std::exception& e) {
        get_logger()->warn("[WebSocketServer] Parse error from {}: {}", client->conn_id, e.what());
    }
}

void WebSocketServer::stop(int timeout_ms) {
    (void)timeout_ms;
    impl_->running = false;
    if (impl_->listen_fd >= 0) {
        ::close(impl_->listen_fd);
        impl_->listen_fd = -1;
    }
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    // Close all clients
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& [id, client] : impl_->clients) {
        if (client->fd >= 0) {
            auto close_frame = build_server_frame("", 0x8);
            ::send(client->fd, close_frame.data(), close_frame.size(), MSG_NOSIGNAL);
            ::close(client->fd);
        }
    }
    impl_->clients.clear();
    get_logger()->info("[WebSocketServer] Stopped");
}

bool WebSocketServer::is_running() const {
    return impl_->running;
}

// ========================================================================
// 广播和事件 (🎯 对齐 OpenClaw server-broadcast.ts)
// ========================================================================

void WebSocketServer::broadcast_event(
    const std::string& event,
    const nlohmann::json& payload,
    bool drop_if_slow
) {
    // 🎯 对齐 OpenClaw createGatewayBroadcaster:60-118
    uint64_t event_seq = ++impl_->seq;
    json frame_json = {
        {"type", "event"},
        {"event", event},
        {"payload", payload},
        {"seq", event_seq}
    };
    auto frame = build_server_frame(frame_json.dump());

    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& [id, client] : impl_->clients) {
        if (client->fd < 0) continue;
        // 🎯 对齐 OpenClaw: scope check
        if (!Impl::has_event_scope(*client, event)) continue;
        // 🎯 对齐 OpenClaw: slow consumer check (simplified — check if send would block)
        (void)drop_if_slow;  // Could check with poll(POLLOUT) here
        ::send(client->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    }
}

bool WebSocketServer::send_event(
    const std::string& conn_id,
    const std::string& event,
    const nlohmann::json& payload
) {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    auto it = impl_->clients.find(conn_id);
    if (it == impl_->clients.end()) return false;

    json frame_json = {{"type", "event"}, {"event", event}, {"payload", payload}};
    auto frame = build_server_frame(frame_json.dump());
    ssize_t sent = ::send(it->second->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return sent > 0;
}

void WebSocketServer::send_to_session(
    const std::string& session_key,
    const std::string& event,
    const nlohmann::json& payload
) {
    // 🎯 对齐 OpenClaw: nodeSendToSession — send to all clients with matching session
    json frame_json = {{"type", "event"}, {"event", event}, {"payload", payload}};
    auto frame = build_server_frame(frame_json.dump());

    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    for (auto& [id, client] : impl_->clients) {
        if (client->session_id.has_value() && *client->session_id == session_key) {
            ::send(client->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        }
    }
}

// ========================================================================
// 连接管理
// ========================================================================

std::vector<std::shared_ptr<WebSocketClient>> WebSocketServer::get_clients() const {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    std::vector<std::shared_ptr<WebSocketClient>> result;
    for (auto& [id, client] : impl_->clients) {
        result.push_back(client);
    }
    return result;
}

size_t WebSocketServer::get_authenticated_count() const {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    size_t count = 0;
    for (auto& [id, client] : impl_->clients) {
        if (client->authenticated) count++;
    }
    return count;
}

std::shared_ptr<WebSocketClient> WebSocketServer::get_client(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    auto it = impl_->clients.find(conn_id);
    if (it != impl_->clients.end()) return it->second;
    return nullptr;
}

void WebSocketServer::disconnect_client(
    const std::string& conn_id,
    int code,
    const std::string& reason
) {
    (void)code; (void)reason;
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    auto it = impl_->clients.find(conn_id);
    if (it != impl_->clients.end()) {
        auto close_frame = build_server_frame("", 0x8);
        ::send(it->second->fd, close_frame.data(), close_frame.size(), MSG_NOSIGNAL);
        ::close(it->second->fd);
        it->second->fd = -1;
        impl_->clients.erase(it);
    }
}

// ========================================================================
// 统计信息
// ========================================================================

WebSocketServer::Stats WebSocketServer::get_stats() const {
    Stats stats{};
    std::lock_guard<std::mutex> lock(impl_->clients_mutex);
    stats.total_connections = impl_->clients.size();
    stats.authenticated_connections = 0;
    for (auto& [id, client] : impl_->clients) {
        if (client->authenticated) stats.authenticated_connections++;
    }
    return stats;
}

}  // namespace gateway
}  // namespace hunterclaw
