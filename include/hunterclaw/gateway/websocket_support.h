#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <functional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 1. WebSocket 消息类型
// ============================================================================

enum class WsMessageType {
    Text,           // 文本消息
    Binary,         // 二进制消息
    Ping,           // Ping 心跳
    Pong,           // Pong 响应
    Close           // 关闭连接
};

struct WsMessage {
    WsMessageType type = WsMessageType::Text;
    std::string payload;
    std::optional<uint16_t> close_code;
    std::optional<std::string> close_reason;
};

// ============================================================================
// 2. WebSocket 连接接口
// ============================================================================

class IWebSocketConnection {
public:
    virtual ~IWebSocketConnection() = default;
    
    virtual void send(const WsMessage& message) = 0;
    virtual void close(uint16_t code = 1000, const std::string& reason = "") = 0;
    virtual bool is_open() const = 0;
    virtual std::string remote_address() const = 0;
};

// ============================================================================
// 3. WebSocket 连接状态
// ============================================================================

enum class WsConnectionState {
    Connecting,     // 连接中
    Open,           // 已连接
    Closing,        // 关闭中
    Closed          // 已关闭
};

// ============================================================================
// 4. WebSocket 会话管理
// ============================================================================

class WebSocketSession {
public:
    struct SessionInfo {
        std::string session_id;
        std::string user_id;
        std::string agent_id;
        Clock::time_point connected_at;
        Clock::time_point last_activity;
        std::unordered_map<std::string, std::string> metadata;
    };
    
    WebSocketSession(const std::string& session_id,
                    std::shared_ptr<IWebSocketConnection> connection)
        : session_id_(session_id),
          connection_(connection),
          state_(WsConnectionState::Open),
          connected_at_(Clock::now()),
          last_activity_(Clock::now()) {}
    
    void send_text(const std::string& text) {
        if (state_ != WsConnectionState::Open) {
            throw std::runtime_error("Connection not open");
        }
        
        WsMessage msg;
        msg.type = WsMessageType::Text;
        msg.payload = text;
        connection_->send(msg);
        
        last_activity_ = Clock::now();
    }
    
    void send_json(const json& data) {
        send_text(data.dump());
    }
    
    void send_event(const std::string& event_type, const json& data) {
        json event = {
            {"event", event_type},
            {"data", data},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        };
        send_json(event);
    }
    
    void close(uint16_t code = 1000, const std::string& reason = "") {
        if (state_ == WsConnectionState::Closed) {
            return;
        }
        
        state_ = WsConnectionState::Closing;
        connection_->close(code, reason);
        state_ = WsConnectionState::Closed;
    }
    
    bool is_open() const {
        return state_ == WsConnectionState::Open && connection_->is_open();
    }
    
    std::chrono::milliseconds idle_time() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_activity_);
    }
    
    SessionInfo get_info() const {
        return SessionInfo{
            session_id_,
            user_id_,
            agent_id_,
            connected_at_,
            last_activity_,
            metadata_
        };
    }
    
    void set_user_id(const std::string& user_id) { user_id_ = user_id; }
    void set_agent_id(const std::string& agent_id) { agent_id_ = agent_id; }
    void set_metadata(const std::string& key, const std::string& value) {
        metadata_[key] = value;
    }
    
    void update_activity() {
        last_activity_ = Clock::now();
    }

private:
    std::string session_id_;
    std::string user_id_;
    std::string agent_id_;
    std::shared_ptr<IWebSocketConnection> connection_;
    WsConnectionState state_;
    Clock::time_point connected_at_;
    Clock::time_point last_activity_;
    std::unordered_map<std::string, std::string> metadata_;
};

// ============================================================================
// 5. WebSocket 会话管理器
// ============================================================================

class WebSocketSessionManager {
public:
    using MessageHandler = std::function<void(
        const std::string& session_id,
        const WsMessage& message
    )>;
    
    struct Config {
        std::chrono::milliseconds ping_interval{30000};     // 30秒心跳
        std::chrono::milliseconds idle_timeout{300000};     // 5分钟空闲超时
        bool auto_heartbeat = true;                         // 自动发送心跳
        int max_connections = 1000;                         // 最大连接数
    };
    
    WebSocketSessionManager() : config_() {}
    
    explicit WebSocketSessionManager(const Config& config)
        : config_(config) {}
    
    std::shared_ptr<WebSocketSession> create_session(
        const std::string& session_id,
        std::shared_ptr<IWebSocketConnection> connection
    ) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (sessions_.size() >= static_cast<size_t>(config_.max_connections)) {
            throw std::runtime_error("Max connections reached");
        }
        
        auto session = std::make_shared<WebSocketSession>(session_id, connection);
        sessions_[session_id] = session;
        
        return session;
    }
    
    void remove_session(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session_id);
    }
    
    std::shared_ptr<WebSocketSession> get_session(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return nullptr;
        }
        return it->second;
    }
    
    void broadcast(const json& message, 
                  const std::optional<std::unordered_set<std::string>>& exclude = std::nullopt) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string text = message.dump();
        for (auto& [session_id, session] : sessions_) {
            if (exclude && exclude->count(session_id) > 0) {
                continue;
            }
            
            if (session->is_open()) {
                try {
                    session->send_text(text);
                } catch (const std::exception&) {
                    // 发送失败,标记为待清理
                }
            }
        }
    }
    
    void send_to_user(const std::string& user_id, const json& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string text = message.dump();
        for (auto& [session_id, session] : sessions_) {
            auto info = session->get_info();
            if (info.user_id == user_id && session->is_open()) {
                try {
                    session->send_text(text);
                } catch (const std::exception&) {
                    // 忽略错误
                }
            }
        }
    }
    
    void cleanup_idle_sessions() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<std::string> to_remove;
        for (auto& [session_id, session] : sessions_) {
            if (!session->is_open() || session->idle_time() > config_.idle_timeout) {
                to_remove.push_back(session_id);
                session->close(1001, "Idle timeout");
            }
        }
        
        for (const auto& session_id : to_remove) {
            sessions_.erase(session_id);
        }
    }
    
    void start_heartbeat() {
        // 启动后台线程定期发送 Ping (实现略)
        if (config_.auto_heartbeat) {
            // std::thread([this]() { ... });
        }
    }
    
    size_t active_connections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

private:
    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<WebSocketSession>> sessions_;
};

// ============================================================================
// 6. 流式响应生成器
// ============================================================================

class StreamingResponseGenerator {
public:
    using StreamCallback = std::function<void(const std::string& chunk)>;
    
    explicit StreamingResponseGenerator(
        std::shared_ptr<WebSocketSession> session,
        const std::string& stream_id = ""
    ) : session_(session), stream_id_(stream_id) {}
    
    void send_chunk(const std::string& text) {
        json chunk = {
            {"event", "stream.chunk"},
            {"stream_id", stream_id_},
            {"type", "text"},
            {"data", {{"delta", text}}}
        };
        session_->send_json(chunk);
    }
    
    void send_tool_call(const std::string& tool_name, const json& args) {
        json chunk = {
            {"event", "stream.chunk"},
            {"stream_id", stream_id_},
            {"type", "tool_call"},
            {"data", {
                {"tool", tool_name},
                {"args", args}
            }}
        };
        session_->send_json(chunk);
    }
    
    void send_tool_result(const std::string& tool_name, const json& result) {
        json chunk = {
            {"event", "stream.chunk"},
            {"stream_id", stream_id_},
            {"type", "tool_result"},
            {"data", {
                {"tool", tool_name},
                {"result", result}
            }}
        };
        session_->send_json(chunk);
    }
    
    void send_error(const std::string& error_message) {
        json chunk = {
            {"event", "stream.error"},
            {"stream_id", stream_id_},
            {"error", error_message}
        };
        session_->send_json(chunk);
    }
    
    void finish() {
        json chunk = {
            {"event", "stream.end"},
            {"stream_id", stream_id_}
        };
        session_->send_json(chunk);
    }

private:
    std::shared_ptr<WebSocketSession> session_;
    std::string stream_id_;
};

// ============================================================================
// 7. SSE (Server-Sent Events) 支持
// ============================================================================

class ServerSentEventsStream {
public:
    using WriteCallback = std::function<void(const std::string& data)>;
    
    explicit ServerSentEventsStream(WriteCallback write_callback)
        : write_callback_(std::move(write_callback)) {
        // 发送 SSE 初始头部
        write_headers();
    }
    
    void send_event(const std::string& event_type, const json& data) {
        std::string sse_message;
        
        // event: event_type
        sse_message += "event: " + event_type + "\n";
        
        // data: {...}
        std::string data_str = data.dump();
        sse_message += "data: " + data_str + "\n";
        
        // id: auto-increment
        sse_message += "id: " + std::to_string(event_id_++) + "\n";
        
        // 空行分隔
        sse_message += "\n";
        
        write_callback_(sse_message);
    }
    
    void send_comment(const std::string& comment) {
        write_callback_(": " + comment + "\n\n");
    }
    
    void send_heartbeat() {
        // 发送注释作为心跳
        send_comment("heartbeat");
    }

private:
    void write_headers() {
        // SSE 响应头 (由外部设置)
        // Content-Type: text/event-stream
        // Cache-Control: no-cache
        // Connection: keep-alive
    }
    
    WriteCallback write_callback_;
    uint64_t event_id_ = 0;
};

// ============================================================================
// 8. WebSocket 路由器
// ============================================================================

class WebSocketRouter {
public:
    using RouteHandler = std::function<void(
        std::shared_ptr<WebSocketSession> session,
        const json& request
    )>;
    
    void register_route(const std::string& route, RouteHandler handler) {
        routes_[route] = std::move(handler);
    }
    
    void handle_message(std::shared_ptr<WebSocketSession> session,
                       const WsMessage& message) {
        if (message.type != WsMessageType::Text) {
            return; // 仅处理文本消息
        }
        
        try {
            json request = json::parse(message.payload);
            
            if (!request.contains("route")) {
                session->send_json({
                    {"error", "Missing route field"}
                });
                return;
            }
            
            std::string route = request["route"];
            auto it = routes_.find(route);
            
            if (it == routes_.end()) {
                session->send_json({
                    {"error", "Unknown route: " + route}
                });
                return;
            }
            
            // 调用路由处理器
            it->second(session, request);
            
        } catch (const json::parse_error& e) {
            session->send_json({
                {"error", "Invalid JSON: " + std::string(e.what())}
            });
        }
    }

private:
    std::unordered_map<std::string, RouteHandler> routes_;
};

// ============================================================================
// 9. 完整 WebSocket 服务器
// ============================================================================

class WebSocketServer {
public:
    struct ServerConfig {
        std::string bind_address = "127.0.0.1";
        uint16_t port = 8080;
        WebSocketSessionManager::Config session_config;
    };
    
    WebSocketServer() 
        : config_(),
          session_manager_(config_.session_config) {}
    
    explicit WebSocketServer(const ServerConfig& config)
        : config_(config),
          session_manager_(config.session_config) {}
    
    void register_route(const std::string& route, WebSocketRouter::RouteHandler handler) {
        router_.register_route(route, std::move(handler));
    }
    
    void start() {
        // 启动 HTTP 服务器并升级到 WebSocket (实现略)
        // 使用 Boost.Beast 或 libwebsockets
        session_manager_.start_heartbeat();
    }
    
    void stop() {
        // 停止服务器 (实现略)
    }
    
    std::shared_ptr<WebSocketSessionManager> session_manager() {
        // 返回指向成员对象的shared_ptr (使用空删除器,因为对象生命周期由WebSocketServer管理)
        return std::shared_ptr<WebSocketSessionManager>(&session_manager_, [](WebSocketSessionManager*){});
    }

private:
    ServerConfig config_;
    WebSocketSessionManager session_manager_;
    WebSocketRouter router_;
};

} // namespace gateway
} // namespace hunterclaw
