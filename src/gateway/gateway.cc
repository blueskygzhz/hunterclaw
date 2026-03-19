#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/connection_pool.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <map>
#include <mutex>
#include <set>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <boost/system/error_code.hpp>

using json = nlohmann::json;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

namespace hunterclaw {
namespace gateway {

typedef websocketpp::server<websocketpp::config::asio> server;
typedef server::message_ptr message_ptr;

// 🎯 OpenClaw 对齐: 每连接握手状态
// 对应 OpenClaw ws-connection.ts 中的 handshakeState / connectNonce / handshakeTimer
struct ConnectionHandshakeState {
    enum class Phase { Pending, Connected, Failed };
    Phase phase = Phase::Pending;
    std::string connect_nonce;          // 发给客户端的 challenge nonce
    std::chrono::steady_clock::time_point opened_at;
    std::string conn_id;                // 连接唯一 ID
    std::string close_cause;            // 关闭原因 (用于日志)
};

// 握手超时常量 (对齐 OpenClaw DEFAULT_HANDSHAKE_TIMEOUT_MS = 3000)
static constexpr int64_t HANDSHAKE_TIMEOUT_MS = 3000;
// tick 心跳间隔 (对齐 OpenClaw TICK_INTERVAL_MS = 30000)
static constexpr int64_t TICK_INTERVAL_MS = 30000;

// Internal implementation class (不继承 GatewayServer)
class GatewayServerImpl {
public:
    explicit GatewayServerImpl(std::shared_ptr<core::Config> config)
        : config_(config), runtime_state_(std::make_shared<GatewayRuntimeState>()) {
        
        // Set up WebSocket server
        ws_server_.init_asio();
        ws_server_.set_reuse_addr(true);
        
        // Set handlers
        ws_server_.set_open_handler(bind(&GatewayServerImpl::on_open, this, ::_1));
        ws_server_.set_close_handler(bind(&GatewayServerImpl::on_close, this, ::_1));
        ws_server_.set_message_handler(bind(&GatewayServerImpl::on_message, this, ::_1, ::_2));
    }
    
    core::Result<bool> start() {
        try {
            ws_server_.listen(config_->gateway.port);
            ws_server_.start_accept();
            
            // Run in background thread
            server_thread_ = std::thread([this]() {
                ws_server_.run();
            });
            
            // 🎯 OpenClaw 严格对齐: 启动 tick 心跳定时器
            // 对应 OpenClaw server-maintenance.ts:
            //   const tickInterval = setInterval(() => {
            //     broadcast("tick", { ts: Date.now() }, { dropIfSlow: true });
            //   }, TICK_INTERVAL_MS);
            tick_running_ = true;
            tick_thread_ = std::thread([this]() {
                while (tick_running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(TICK_INTERVAL_MS));
                    if (!tick_running_) break;
                    try {
                        int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count();
                        json tick_payload;
                        tick_payload["ts"] = ts_ms;
                        
                        json tick_frame;
                        tick_frame["type"] = "event";
                        tick_frame["event"] = "tick";
                        tick_frame["payload"] = tick_payload;
                        // dropIfSlow: true (对齐 OpenClaw)
                        std::string tick_msg = tick_frame.dump();
                        
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (auto hdl : connections_) {
                            // 只向已完成握手的连接发送 tick
                            auto hs_it = handshake_states_.find(hdl);
                            if (hs_it != handshake_states_.end() &&
                                hs_it->second.phase == ConnectionHandshakeState::Phase::Connected) {
                                try {
                                    ws_server_.send(hdl, tick_msg, websocketpp::frame::opcode::text);
                                } catch (...) {}
                            }
                        }
                    } catch (...) {}
                }
            });
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to start gateway: ") + e.what());
        }
    }
    
    core::Result<bool> stop() {
        try {
            // 🎯 OpenClaw 对齐: 先停止 tick 心跳
            tick_running_ = false;
            if (tick_thread_.joinable()) {
                tick_thread_.join();
            }
            
            ws_server_.stop_listening();
            
            // Close all connections
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (auto hdl : connections_) {
                    ws_server_.close(hdl, websocketpp::close::status::going_away, "Server shutting down");
                }
            }
            
            ws_server_.stop();
            
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to stop gateway: ") + e.what());
        }
    }
    
    void broadcast(const core::EventFrame& event) {
        try {
            json j;
            j["type"] = "event";
            j["event"] = event.event;
            j["seq"] = static_cast<int64_t>(event.seq);  // explicit cast to avoid ambiguity
            
            // Serialize event.payload (完整实现 - 对齐 OpenClaw)
            if (event.payload.is_null()) {
                j["payload"] = nullptr;
            } else if (event.payload.is_object() || event.payload.is_array()) {
                j["payload"] = event.payload.to_json();
            } else if (event.payload.is_string()) {
                j["payload"] = event.payload.as_string();
            } else {
                j["payload"] = event.payload.to_json();
            }
            
            std::string msg = j.dump();
            
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto hdl : connections_) {
                ws_server_.send(hdl, msg, websocketpp::frame::opcode::text);
            }
            
        } catch (const std::exception& e) {
            // Log error but don't throw
        }
    }
    
    std::shared_ptr<GatewayRuntimeState> runtime_state() const {
        return runtime_state_;
    }
    
    // Send message to specific connection (for targeted broadcast)
    void send_to_handle(connection_hdl hdl, const std::string& msg) {
        try {
            ws_server_.send(hdl, msg, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            // Log error but don't throw
        }
    }
    
private:
    void on_open(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(hdl);
        
        // 🎯 OpenClaw 严格对齐: 生成 connId (对应 randomUUID())
        static std::atomic<uint64_t> conn_id_counter{0};
        uint64_t conn_id_num = ++conn_id_counter;
        std::string conn_id = "conn-" + std::to_string(conn_id_num);
        
        // 🎯 OpenClaw 严格对齐: 生成 connect.challenge nonce
        // 对应 OpenClaw: const connectNonce = randomUUID();
        std::string nonce = "nonce-" + std::to_string(conn_id_num) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        
        // 初始化握手状态
        ConnectionHandshakeState hs;
        hs.phase = ConnectionHandshakeState::Phase::Pending;
        hs.connect_nonce = nonce;
        hs.opened_at = std::chrono::steady_clock::now();
        hs.conn_id = conn_id;
        handshake_states_[hdl] = hs;
        
        // 保存 handle → conn_id 映射
        handle_to_conn_id_[hdl] = conn_id;
        
        // 🎯 OpenClaw 严格对齐: 立即发送 connect.challenge 事件帧
        // 对应 OpenClaw:
        //   send({ type: "event", event: "connect.challenge",
        //          payload: { nonce: connectNonce, ts: Date.now() } });
        try {
            int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            json challenge;
            challenge["type"] = "event";
            challenge["event"] = "connect.challenge";
            challenge["payload"] = json{{"nonce", nonce}, {"ts", ts_ms}};
            ws_server_.send(hdl, challenge.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            // 发送 challenge 失败，关闭连接
            try { ws_server_.close(hdl, 1011, "challenge send failed"); } catch (...) {}
            return;
        }
        
        // 🎯 OpenClaw 严格对齐: 握手超时定时器
        // 对应 OpenClaw: const handshakeTimer = setTimeout(() => { ... close() }, handshakeTimeoutMs)
        // 这里用异步方式：在 on_message 中检查超时
        // (websocketpp 不直接支持 per-connection timer，通过 asio 实现)
        try {
            auto& asio_io = ws_server_.get_io_service();
            auto timer = std::make_shared<websocketpp::lib::asio::steady_timer>(asio_io);
            timer->expires_from_now(std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS));
            timer->async_wait([this, hdl, conn_id, timer](const boost::system::error_code& ec) {
                if (ec) return;  // 被取消
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = handshake_states_.find(hdl);
                if (it != handshake_states_.end() &&
                    it->second.phase == ConnectionHandshakeState::Phase::Pending) {
                    // 握手超时，关闭连接
                    it->second.phase = ConnectionHandshakeState::Phase::Failed;
                    it->second.close_cause = "handshake-timeout";
                    try {
                        ws_server_.close(hdl, 1008, "handshake timeout");
                    } catch (...) {}
                }
            });
            handshake_timers_[hdl] = timer;
        } catch (const std::exception& e) {
            // timer 创建失败不影响主流程
        }
    }
    
    void on_close(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);
        
        // 🎯 OpenClaw 对齐: 取消握手超时定时器
        auto timer_it = handshake_timers_.find(hdl);
        if (timer_it != handshake_timers_.end()) {
            timer_it->second->cancel();
            handshake_timers_.erase(timer_it);
        }
        
        // 清理握手状态
        handshake_states_.erase(hdl);
        
        // 🎯 OpenClaw 对齐: 从 ConnectionPool 注销连接
        auto it = handle_to_conn_id_.find(hdl);
        if (it != handle_to_conn_id_.end()) {
            auto& pool = ConnectionPool::instance();
            pool.remove_client(it->second);
            handle_to_conn_id_.erase(it);
        }
    }
    
    void on_message(connection_hdl hdl, message_ptr msg) {
        auto start_time = std::chrono::steady_clock::now();
        std::string conn_id;
        std::string method_name;
        
        try {
            std::string payload_str = msg->get_payload();
            json j = json::parse(payload_str);
            
            // 提取 conn_id
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = handle_to_conn_id_.find(hdl);
                if (it != handle_to_conn_id_.end()) {
                    conn_id = it->second;
                }
            }
            
            // 验证是 req 帧
            std::string msg_type = j.contains("type") && j["type"].is_string()
                ? j["type"].get<std::string>() : "";
            if (msg_type != "req") {
                // 非 req 帧: 握手前直接关闭，握手后忽略
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it == handshake_states_.end() ||
                    hs_it->second.phase == ConnectionHandshakeState::Phase::Pending) {
                    try { ws_server_.close(hdl, 1008, "invalid handshake"); } catch (...) {}
                }
                return;
            }
            
            // 解析 req 帧
            std::string req_id = j.contains("id") && j["id"].is_string()
                ? j["id"].get<std::string>() : "";
            std::string req_method = j.contains("method") && j["method"].is_string()
                ? j["method"].get<std::string>() : "";
            method_name = req_method;
            json req_params = j.contains("params") && j["params"].is_object()
                ? j["params"] : json::object();
            
            // ============================================================
            // 🎯 OpenClaw 严格对齐: 握手状态机
            // 对应 OpenClaw message-handler.ts:
            //   if (!client) { /* 握手阶段: 只接受 connect 方法 */ }
            //   else { /* 已连接: 处理普通请求 */ }
            // ============================================================
            bool is_handshake_pending = false;
            std::string connect_nonce;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it != handshake_states_.end()) {
                    is_handshake_pending =
                        (hs_it->second.phase == ConnectionHandshakeState::Phase::Pending);
                    connect_nonce = hs_it->second.connect_nonce;
                }
            }
            
            if (is_handshake_pending) {
                // --------------------------------------------------------
                // 握手阶段: 只接受 method == "connect"
                // 对应 OpenClaw:
                //   if (!isRequestFrame || parsed.method !== "connect" || !validateConnectParams(...))
                // --------------------------------------------------------
                if (req_method != "connect") {
                    // 握手阶段收到非 connect 请求 → 拒绝并关闭
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"},
                                           {"message", "invalid handshake: first request must be connect"}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "invalid-handshake";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, "invalid handshake"); } catch (...) {}
                    return;
                }
                
                // 处理 connect 请求
                // 对应 OpenClaw: 验证 auth、protocol、device nonce 等
                // HunterClaw 简化版: 验证 nonce (如果客户端提供了 device)
                bool auth_ok = true;
                std::string auth_error;
                
                // 验证 device nonce (对齐 OpenClaw nonce 校验)
                if (req_params.contains("device") && req_params["device"].is_object()) {
                    auto& device = req_params["device"];
                    std::string provided_nonce = device.contains("nonce") && device["nonce"].is_string()
                        ? device["nonce"].get<std::string>() : "";
                    if (!provided_nonce.empty() && provided_nonce != connect_nonce) {
                        auth_ok = false;
                        auth_error = "device nonce mismatch";
                    }
                }
                
                if (!auth_ok) {
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", auth_error}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "unauthorized";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, auth_error.c_str()); } catch (...) {}
                    return;
                }
                
                // 🎯 OpenClaw 严格对齐: 取消握手超时定时器
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    auto timer_it = handshake_timers_.find(hdl);
                    if (timer_it != handshake_timers_.end()) {
                        timer_it->second->cancel();
                        handshake_timers_.erase(timer_it);
                    }
                    // 更新握手状态为 Connected
                    auto hs_it = handshake_states_.find(hdl);
                    if (hs_it != handshake_states_.end()) {
                        hs_it->second.phase = ConnectionHandshakeState::Phase::Connected;
                    }
                }
                
                // 🎯 OpenClaw 严格对齐: 发送 hello-ok 响应
                // 对应 OpenClaw:
                //   send({ type: "res", id: frame.id, ok: true, payload: helloOk });
                // helloOk 包含: type, protocol, server, features, snapshot, policy
                json hello_ok;
                hello_ok["type"] = "hello-ok";
                hello_ok["protocol"] = 1;  // 协议版本
                hello_ok["server"] = json{{"connId", conn_id}, {"version", "1.0.0"}};
                hello_ok["features"] = json{
                    {"methods", json::array()},  // TODO: 填充支持的 methods 列表
                    {"events", json::array()}    // TODO: 填充支持的 events 列表
                };
                hello_ok["snapshot"] = json::object();  // TODO: 填充全量状态快照
                hello_ok["policy"] = json{
                    {"maxPayload", 25 * 1024 * 1024},  // 25MB (对齐 OpenClaw MAX_PAYLOAD_BYTES)
                    {"maxBufferedBytes", 50 * 1024 * 1024},  // 50MB
                    {"tickIntervalMs", TICK_INTERVAL_MS}  // 30s (对齐 OpenClaw TICK_INTERVAL_MS)
                };
                
                json res_j;
                res_j["type"] = "res";
                res_j["id"] = req_id;
                res_j["ok"] = true;
                res_j["payload"] = hello_ok;
                try { ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                return;
            }
            
            // --------------------------------------------------------
            // 已连接阶段: 处理普通请求
            // 对应 OpenClaw: handleGatewayRequest({ req, respond, client, ... })
            // --------------------------------------------------------
            core::RequestFrame req;
            req.id = req_id;
            req.method = req_method;
            req.params = req_params;
            
            auto response = handle_request(req);
            
            // 发送响应帧
            json res_j;
            res_j["type"] = "res";
            res_j["id"] = req.id;
            res_j["ok"] = response.ok;
            if (response.ok) {
                if (!response.payload.is_null()) {
                    res_j["payload"] = response.payload.to_json();
                }
            } else {
                if (!response.error.empty()) {
                    res_j["error"] = json{{"code", "UNAVAILABLE"}, {"message", response.error}};
                } else {
                    res_j["error"] = json{{"code", "UNAVAILABLE"}, {"message", "method failed"}};
                }
            }
            ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text);
            
        } catch (const std::exception& e) {
            // 解析/处理错误: 发送错误响应
            json error_res;
            error_res["type"] = "res";
            error_res["ok"] = false;
            error_res["error"] = json{{"code", "UNAVAILABLE"},
                                      {"message", std::string("Failed to process request: ") + e.what()}};
            try {
                ws_server_.send(hdl, error_res.dump(), websocketpp::frame::opcode::text);
            } catch (...) {}
        }
    }
    
    core::ResponseFrame handle_request(const core::RequestFrame& req) {
        // Use new method handler (对齐 OpenClaw server-methods/)
        if (!method_handler_) {
            core::ResponseFrame response;
            response.id = req.id;
            response.ok = false;
            response.error = "Method handler not initialized";
            return response;
        }
        
        try {
            // Parse params to JSON
            nlohmann::json params;
            // params is already available in req
            
            // Invoke method
            auto result = method_handler_(req.method, params);
            
            if (result.is_ok()) {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = true;
                return response;
            } else {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = false;
                response.error = result.error;
                return response;
            }
        } catch (const std::exception& e) {
            core::ResponseFrame response;
            response.id = req.id;
            response.ok = false;
            response.error = std::string("Method execution failed: ") + e.what();
            return response;
        }
    }
    
    std::shared_ptr<core::Config> config_;
    server ws_server_;
    std::thread server_thread_;
    std::set<connection_hdl, std::owner_less<connection_hdl>> connections_;
    std::mutex connections_mutex_;
    std::shared_ptr<GatewayRuntimeState> runtime_state_;
    std::function<core::Result<nlohmann::json>(const std::string&, const nlohmann::json&)> method_handler_;
    
    // handle → conn_id mapping (用于 on_close 时查找)
    std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> handle_to_conn_id_;
    
    // 🎯 OpenClaw 对齐: tick 心跳线程
    std::thread tick_thread_;
    std::atomic<bool> tick_running_{false};
    
    // 🎯 OpenClaw 对齐: 每连接握手状态 (对应 handshakeState / connectNonce)
    std::map<connection_hdl, ConnectionHandshakeState, std::owner_less<connection_hdl>> handshake_states_;
    
    // 🎯 OpenClaw 对齐: 每连接握手超时定时器 (对应 handshakeTimer)
    using AsioTimer = websocketpp::lib::asio::steady_timer;
    std::map<connection_hdl, std::shared_ptr<AsioTimer>, std::owner_less<connection_hdl>> handshake_timers_;
};

// Session Manager Implementation
class SessionManagerImpl : public SessionManager {
public:
    SessionManagerImpl() {
        // Load existing sessions from disk on startup
        load_sessions();
    }
    
    std::string create_session(const core::SessionKey& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string session_id = core::utils::generate_uuid();
        
        core::SessionInfo info;
        info.key = key;
        info.session_id = session_id;
        info.created_at = core::utils::now();
        info.last_active_at = info.created_at;
        
        sessions_[session_id] = info;
        
        return session_id;
    }
    
    std::optional<core::SessionInfo> get_session(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        
        return it->second;
    }
    
    bool update_last_active(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return false;
        }
        
        it->second.last_active_at = core::utils::now();
        return true;
    }
    
    bool delete_session(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return false;
        }
        
        sessions_.erase(it);
        return true;
    }
    
    std::vector<core::SessionInfo> list_sessions() override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<core::SessionInfo> result;
        for (const auto& pair : sessions_) {
            result.push_back(pair.second);
        }
        return result;
    }
    
    std::vector<core::SessionInfo> find_sessions_by_key(const core::SessionKey& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<core::SessionInfo> results;
        for (const auto& pair : sessions_) {
            if (pair.second.key.device_id == key.device_id &&
                pair.second.key.device_token == key.device_token) {
                results.push_back(pair.second);
            }
        }
        return results;
    }
    
    core::Result<bool> save_session(const core::SessionInfo& session) override {
        // Session persistence (完整实现 - 对齐 OpenClaw)
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            // Create sessions directory if not exists
            std::string sessions_dir = "./data/sessions";
            std::filesystem::create_directories(sessions_dir);
            
            // Serialize session to JSON
            json j;
            j["session_id"] = session.session_id;
            j["channel_id"] = session.key.channel;
            j["account_id"] = session.key.account_id;
            j["conversation_id"] = session.key.target_id;
            j["device_id"] = session.key.device_id;
            j["device_token"] = session.key.device_token;
            j["created_at"] = session.created_at.time_since_epoch().count();
            j["last_active_at"] = session.last_active_at.time_since_epoch().count();
            
            // Serialize metadata
            if (!session.metadata.empty()) {
                j["metadata"] = json::object();
                for (const auto& [key, value] : session.metadata) {
                    j["metadata"][key] = value;
                }
            }
            
            // Write to file
            std::string filepath = sessions_dir + "/" + session.session_id + ".json";
            std::ofstream file(filepath);
            if (!file.is_open()) {
                return core::Result<bool>::Err("Failed to open session file: " + filepath);
            }
            
            file << j.dump(2);
            file.close();
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to save session: ") + e.what());
        }
    }
    
    core::Result<bool> load_sessions() override {
        // Load sessions from disk (完整实现 - 对齐 OpenClaw)
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            std::string sessions_dir = "./data/sessions";
            
            // Check if directory exists
            if (!std::filesystem::exists(sessions_dir)) {
                // No sessions to load
                return core::Result<bool>::Ok(true);
            }
            
            // Iterate through all .json files
            for (const auto& entry : std::filesystem::directory_iterator(sessions_dir)) {
                if (entry.path().extension() == ".json") {
                    try {
                        std::ifstream file(entry.path());
                        if (!file.is_open()) {
                            continue;
                        }
                        
                        json j;
                        file >> j;
                        file.close();
                        
                        // Deserialize session
                        core::SessionInfo session;
                        session.session_id = j.value("session_id", "");
                        session.key.channel = j.value("channel_id", "");
                        session.key.account_id = j.value("account_id", "");
                        session.key.target_id = j.value("conversation_id", "");
                        session.key.device_id = j.value("device_id", "");
                        session.key.device_token = j.value("device_token", "");
                        
                        // Parse timestamps
                        if (j.contains("created_at")) {
                            int64_t created_at = j["created_at"];
                            session.created_at = core::Timestamp(std::chrono::nanoseconds(created_at));
                        }
                        if (j.contains("last_active_at")) {
                            int64_t last_active_at = j["last_active_at"];
                            session.last_active_at = core::Timestamp(std::chrono::nanoseconds(last_active_at));
                        }
                        
                        // Parse metadata
                        if (j.contains("metadata") && j["metadata"].is_object()) {
                            for (auto& [key, value] : j["metadata"].items()) {
                                if (value.is_string()) {
                                    session.metadata[key] = value.get<std::string>();
                                } else {
                                    session.metadata[key] = value.dump();
                                }
                            }
                        }
                        
                        // Store in memory
                        sessions_[session.session_id] = session;
                        
                    } catch (const std::exception& e) {
                        // Skip invalid session files
                        continue;
                    }
                }
            }
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to load sessions: ") + e.what());
        }
    }
    
private:
    mutable std::mutex mutex_;
    std::map<std::string, core::SessionInfo> sessions_;
};

// Factory methods
std::shared_ptr<SessionManager> SessionManager::create() {
    return std::make_shared<SessionManagerImpl>();
}

// GatewayServer Pimpl implementation
struct GatewayServer::Impl {
    std::shared_ptr<GatewayServerImpl> server_impl;
    std::shared_ptr<core::Config> config;
    std::shared_ptr<channels::ChannelManager> channel_mgr;
    std::shared_ptr<skills::SkillManager> skill_mgr;
    std::shared_ptr<plugins::PluginManager> plugin_mgr;
    std::shared_ptr<agents::AgentPool> agent_pool;  // 🏢 Multi-tenant Agent Pool
    std::atomic<bool> running{false};
};

GatewayServer::GatewayServer(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->server_impl = std::make_shared<GatewayServerImpl>(config);
    
    // 🏢 Initialize AgentPool (Multi-tenant support)
    agents::AgentPool::Config pool_config;
    pool_config.max_agents = 100;  // 最大并发 Agent 数
    // 注意：idle_ttl和cleanup_interval应该是chrono::seconds，不是毫秒
    pool_config.idle_ttl = std::chrono::seconds(3600);  // 1 小时闲置超时
    pool_config.cleanup_interval = std::chrono::seconds(300);  // 5 分钟清理间隔
    pool_config.auto_cleanup = true;
    
    impl_->agent_pool = std::make_shared<agents::AgentPool>(
        impl_->config,
        impl_->skill_mgr,
        impl_->channel_mgr,
        pool_config
    );
    
    // Import create_method_handler from methods.cc
    extern std::function<core::Result<nlohmann::json>(const std::string&, const nlohmann::json&)> 
        create_method_handler(std::shared_ptr<GatewayServer> server);
    
    // Initialize method handler in runtime state
    auto self = std::shared_ptr<GatewayServer>(this, [](GatewayServer*){});  // Non-owning shared_ptr
    impl_->server_impl->runtime_state()->method_handler = create_method_handler(self);
}

GatewayServer::~GatewayServer() = default;

std::shared_ptr<GatewayServer> GatewayServer::create(const core::GatewayConfig& config) {
    auto full_config = std::make_shared<core::Config>();
    full_config->gateway = config;
    return std::make_shared<GatewayServer>(full_config);
}

core::Result<bool> GatewayServer::start() {
    if (impl_->running.load()) {
        return core::Result<bool>::Err("Server already running");
    }
    auto result = impl_->server_impl->start();
    if (result.is_ok()) {
        impl_->running.store(true);
    }
    return result;
}

core::Result<bool> GatewayServer::stop() {
    if (!impl_->running.load()) {
        return core::Result<bool>::Err("Server not running");
    }
    auto result = impl_->server_impl->stop();
    if (result.is_ok()) {
        impl_->running.store(false);
    }
    return result;
}

bool GatewayServer::is_running() const {
    return impl_->running.load();
}

void GatewayServer::broadcast(const core::EventFrame& event) {
    if (impl_->server_impl) {
        impl_->server_impl->broadcast(event);
    }
}

void GatewayServer::broadcast_to_conn_ids(
    const std::vector<std::string>& conn_ids,
    const core::EventFrame& event
) {
    // Targeted broadcast (完整实现 - 对齐 OpenClaw)
    if (!impl_->server_impl) {
        return;
    }
    
    try {
        json j;
        j["type"] = "event";
        j["event"] = event.event;
        j["seq"] = static_cast<int64_t>(event.seq);
        
        // Serialize event.payload
        if (event.payload.is_null()) {
            j["payload"]
             = nullptr;
        } else {
            j["payload"] = event.payload.to_json();
        }
        
        std::string msg = j.dump();
        
        // Get runtime state
        auto state = impl_->server_impl->runtime_state();
        if (!state) {
            return;
        }
        
        // Send to specific conn_ids
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& conn_id : conn_ids) {
            auto it = state->clients.find(conn_id);
            if (it != state->clients.end()) {
                try {
                    impl_->server_impl->send_to_handle(it->second->handle, msg);
                } catch (const std::exception& e) {
                    // Log error but continue with other connections
                }
            }
        }
    } catch (const std::exception& e) {
        // Log error but don't throw
    }
}

std::shared_ptr<channels::ChannelManager> GatewayServer::channel_manager() const {
    return impl_->channel_mgr;
}

std::shared_ptr<skills::SkillManager> GatewayServer::skill_manager() const {
    return impl_->skill_mgr;
}

std::shared_ptr<plugins::PluginManager> GatewayServer::plugin_manager() const {
    return impl_->plugin_mgr;
}

std::shared_ptr<GatewayRuntimeState> GatewayServer::runtime_state() const {
    if (impl_->server_impl) {
        return impl_->server_impl->runtime_state();
    }
    return nullptr;
}

std::shared_ptr<agents::AgentPool> GatewayServer::agent_pool() const {
    return impl_->agent_pool;
}

} // namespace gateway
} // namespace hunterclaw
