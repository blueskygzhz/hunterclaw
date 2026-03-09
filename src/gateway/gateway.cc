#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/core/utils.h"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include "hunterclaw/core/json.h"
#include <map>
#include <mutex>
#include <set>

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
        : config_(config) {
        
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
            // TODO: Serialize event.payload
            
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
    
private:
    void on_open(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(hdl);
    }
    
    void on_close(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);
    }
    
    void on_message(connection_hdl hdl, message_ptr msg) {
        try {
            std::string payload = msg->get_payload();
            json j = json::parse(payload);
            
            // Parse request frame
            std::string msg_type = j.contains("type") && j["type"].is_string() ? j["type"].get<std::string>() : "";
            if (msg_type == "req" || msg_type == "request") {
                core::RequestFrame req;
                req.id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : "";
                req.method = j.contains("method") && j["method"].is_string() ? j["method"].get<std::string>() : "";
                // TODO: Parse params
                
                // Handle request
                auto response = handle_request(req);
                
                // Send response
                json res_j;
                res_j["type"] = "res";
                res_j["id"] = req.id;
                res_j["ok"] = response.ok;
                if (response.ok) {
                    // TODO: Serialize response.payload
                    res_j["payload"] = nullptr;
                } else {
                    res_j["error"] = response.error;
                }
                
                ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text);
            }
            
        } catch (const std::exception& e) {
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
};

// Session Manager Implementation
class SessionManagerImpl : public SessionManager {
public:
    SessionManagerImpl() {}
    
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
        // TODO: Implement persistence
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> load_sessions() override {
        // TODO: Implement persistence
        return core::Result<bool>::Ok(true);
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
    std::atomic<bool> running{false};
};

GatewayServer::GatewayServer(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->server_impl = std::make_shared<GatewayServerImpl>(config);
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
    // TODO: Implement targeted broadcast
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

} // namespace gateway
} // namespace hunterclaw
