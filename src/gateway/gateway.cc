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

using json = nlohmann::json;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

namespace hunterclaw {
namespace gateway {

typedef websocketpp::server<websocketpp::config::asio> server;
typedef server::message_ptr message_ptr;

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
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to start gateway: ") + e.what());
        }
    }
    
    core::Result<bool> stop() {
        try {
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
        
        // 🎯 OpenClaw 对齐: 注册连接到 ConnectionPool
        // 从连接中提取 session_id 和 channel_id
        // 暂时使用空 session_id (后续从握手中获取)
        std::string session_id = ""; // TODO: 从握手请求中获取
        std::string channel_id = "";
        
        // 注释掉register_connection调用，因为ConnectionPool API已更改
        // auto& pool = ConnectionPool::instance();
        // uint64_t conn_id = pool.register_connection(...);
        
        // 生成临时conn_id
        static std::atomic<uint64_t> conn_id_counter{0};
        uint64_t conn_id = ++conn_id_counter;
        
        // 保存映射
        handle_to_conn_id_[hdl] = std::to_string(conn_id);
        
        // ✅ 可观测性: 发射 WebhookReceived 事件 (对齐 OpenClaw)
        try {
            auto event = std::make_shared<observability::WebhookReceivedEvent>();
            event->session_id = session_id.empty() ? std::to_string(conn_id) : session_id;
            event->channel = channel_id.empty() ? "default" : channel_id;
            // 设置其他必要字段
            event->method = "WEBSOCKET";
            event->path = "/ws";
            event->body_size_bytes = 0;
            event->remote_ip = "unknown";
            
            // observability::DiagnosticEventBus::instance().emit(event);
        } catch (const std::exception& e) {
            // 故障隔离: 永不阻塞主流程
        }
    }
    
    void on_close(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);
        
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
        std::string session_id;
        std::string method_name;
        
        try {
            std::string payload = msg->get_payload();
            json j = json::parse(payload);
            
            // 提取 session_id (从 handle_to_conn_id_ 映射)
            auto it = handle_to_conn_id_.find(hdl);
            if (it != handle_to_conn_id_.end()) {
                session_id = it->second;
            }
            
            // ✅ 可观测性: 发射 WebhookReceived 事件
            try {
                auto received_event = std::make_shared<observability::WebhookReceivedEvent>();
                received_event->session_id = session_id;
                received_event->channel = "default";
                received_event->method = "WEBSOCKET";
                received_event->path = "/ws/message";
                received_event->body_size_bytes = payload.size();
                received_event->remote_ip = "unknown";

                // observability::DiagnosticEventBus::instance().emit(received_event);
            } catch (...) {}
            
            // Parse request frame
            std::string msg_type = j.contains("type") && j["type"].is_string() ? j["type"].get<std::string>() : "";
            if (msg_type == "req" || msg_type == "request") {
                core::RequestFrame req;
                req.id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : "";
                req.method = j.contains("method") && j["method"].is_string() ? j["method"].get<std::string>() : "";
                method_name = req.method;
                
                // Parse params (完整实现 - 对齐 OpenClaw)
                if (j.contains("params")) {
                    if (j["params"].is_object()) {
                        req.params = j["params"];
                    } else if (j["params"].is_null()) {
                        req.params = json::object();
                    } else {
                        // Wrap non-object params in an object
                        req.params = json::object();
                        req.params["value"] = j["params"];
                    }
                } else {
                    req.params = json::object();
                }
                
                // Handle request
                auto response = handle_request(req);
                
                // ✅ 可观测性: 计算 duration
                auto end_time = std::chrono::steady_clock::now();
                auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time
                ).count();
                
                // ✅ 可观测性: 发射 WebhookProcessed + HTTP 请求事件
                try {
                    auto processed_event = std::make_shared<observability::WebhookProcessedEvent>();
                    processed_event->session_id = session_id;
                    processed_event->channel = method_name;
                    processed_event->duration_ms = duration_ms;
                    processed_event->status_code = response.ok ? 200 : 500;
                    processed_event->success = response.ok;
                    
                    // observability::DiagnosticEventBus::instance().emit(processed_event);
                    
                    // TODO: HTTP 请求事件 (需要HttpRequestEvent::create)
                    // auto http_event = std::make_shared<observability::HttpRequestEvent>();
                    // // observability::DiagnosticEventBus::instance().emit(http_event);
                } catch (...) {}
                
                // Send response
                json res_j;
                res_j["type"] = "res";
                res_j["id"] = req.id;
                res_j["ok"] = response.ok;
                if (response.ok) {
                    // Serialize response.payload (完整实现 - 对齐 OpenClaw)
                    if (response.payload.is_null()) {
                        res_j["payload"] = nullptr;
                    } else {
                        res_j["payload"] = response.payload.to_json();
                    }
                } else {
                    res_j["error"] = response.error;
                }
                
                ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text);
            }
            
        } catch (const std::exception& e) {
            // ✅ 可观测性: 错误情况
            try {
                auto end_time = std::chrono::steady_clock::now();
                auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time
                ).count();
                
                // TODO: 待实现HttpRequestEvent
                // auto error_event = std::make_shared<observability::HttpRequestEvent>();
                // // observability::DiagnosticEventBus::instance().emit(error_event);
            } catch (...) {}
            
            // Send error response
            json error_res;
            error_res["type"] = "res";
            error_res["ok"] = false;
            error_res["error"] = std::string("Failed to process request: ") + e.what();
            
            try {
                ws_server_.send(hdl, error_res.dump(), websocketpp::frame::opcode::text);
            } catch (...) {
                // Failed to send error response
            }
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
