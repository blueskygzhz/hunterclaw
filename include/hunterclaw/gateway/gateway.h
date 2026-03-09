#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/plugins/plugin.h"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <memory>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// WebSocket Types
// ============================================================================

using WsServer = websocketpp::server<websocketpp::config::asio>;
using WsConnectionPtr = WsServer::connection_ptr;
using WsMessagePtr = WsServer::message_ptr;
using ConnectionHandle = websocketpp::connection_hdl;

// ============================================================================
// Gateway Client
// ============================================================================

struct GatewayClient {
    std::string conn_id;     // UUID
    ConnectionHandle handle;
    
    std::string device_id;
    std::string role;        // admin, user, guest
    std::vector<std::string> scopes;
    
    core::Timestamp connected_at;
    core::Timestamp last_ping_at;
    
    std::map<std::string, core::JsonValue> metadata;
};

// ============================================================================
// Gateway Runtime State
// ============================================================================

struct GatewayRuntimeState {
    // Connected clients
    std::map<std::string, std::shared_ptr<GatewayClient>> clients;
    
    // Session state
    std::map<std::string, core::SessionInfo> sessions;
    
    // Agent run state
    std::map<std::string, uint64_t> agent_run_seq;
    
    // Message deduplication
    std::map<std::string, core::Timestamp> dedupe;
    
    // Abort controllers (for cancelling long-running tasks)
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> abort_controllers;
};

// ============================================================================
// Gateway Server
// ============================================================================

class GatewayServer {
public:
    // Factory method
    static std::shared_ptr<GatewayServer> create(const core::GatewayConfig& config);
    
    explicit GatewayServer(std::shared_ptr<core::Config> config);
    ~GatewayServer();
    
    // Lifecycle
    core::Result<bool> start();
    core::Result<bool> stop();
    bool is_running() const;
    
    // Broadcast
    void broadcast(const core::EventFrame& event);
    void broadcast_to_conn_ids(
        const std::vector<std::string>& conn_ids,
        const core::EventFrame& event
    );
    
    // Component access
    std::shared_ptr<channels::ChannelManager> channel_manager() const;
    std::shared_ptr<skills::SkillManager> skill_manager() const;
    std::shared_ptr<plugins::PluginManager> plugin_manager() const;
    
    // State access
    std::shared_ptr<GatewayRuntimeState> runtime_state() const;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Gateway Methods (RPC handlers)
// ============================================================================

class GatewayMethods {
public:
    using MethodHandler = std::function<core::Result<core::JsonValue>(
        const core::JsonValue& params,
        const GatewayClient& client,
        GatewayRuntimeState& state
    )>;
    
    explicit GatewayMethods(std::shared_ptr<GatewayServer> server);
    
    // Method registration
    void register_method(const std::string& name, MethodHandler handler);
    void unregister_method(const std::string& name);
    
    // Method invocation
    core::Result<core::JsonValue> invoke(
        const std::string& method,
        const core::JsonValue& params,
        const GatewayClient& client,
        GatewayRuntimeState& state
    );
    
    // Built-in methods
    void register_builtin_methods();
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Protocol Handler
// ============================================================================

class ProtocolHandler {
public:
    explicit ProtocolHandler(std::shared_ptr<GatewayServer> server);
    
    // Frame parsing
    static core::Result<core::RequestFrame> parse_request(const std::string& json);
    static std::string serialize_response(const core::ResponseFrame& frame);
    static std::string serialize_event(const core::EventFrame& frame);
    
    // Message handling
    void handle_message(
        const WsConnectionPtr& conn,
        const WsMessagePtr& msg,
        std::shared_ptr<GatewayClient> client
    );
    
    // Connection handling
    void handle_open(const ConnectionHandle& hdl);
    void handle_close(const ConnectionHandle& hdl);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Authentication
// ============================================================================

struct AuthResult {
    bool ok;
    std::string device_id;
    std::vector<std::string> scopes;
    std::string reason;  // If not ok
};

class AuthManager {
public:
    explicit AuthManager(std::shared_ptr<core::Config> config);
    
    // Authenticate connect request
    AuthResult authenticate_connect(
        const core::JsonValue& params,
        const std::string& nonce
    );
    
    // Token validation
    bool validate_token(const std::string& token) const;
    bool validate_password(const std::string& password) const;
    
    // Device authentication (challenge-response)
    struct DeviceChallenge {
        std::string nonce;
        core::Timestamp created_at;
    };
    
    DeviceChallenge create_challenge();
    bool verify_device_signature(
        const std::string& device_id,
        const std::string& nonce,
        const std::string& signature
    );
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Session Manager
// ============================================================================

class SessionManager {
public:
    // Factory method
    static std::shared_ptr<SessionManager> create();
    
    SessionManager() = default;
    virtual ~SessionManager() = default;
    
    // Session lifecycle
    virtual std::string create_session(const core::SessionKey& key) = 0;
    virtual std::optional<core::SessionInfo> get_session(const std::string& session_id) = 0;
    virtual bool update_last_active(const std::string& session_id) = 0;
    virtual bool delete_session(const std::string& session_id) = 0;
    
    // Session querying
    virtual std::vector<core::SessionInfo> list_sessions() = 0;
    virtual std::vector<core::SessionInfo> find_sessions_by_key(const core::SessionKey& key) = 0;
    
    // Session persistence
    virtual core::Result<bool> save_session(const core::SessionInfo& session) = 0;
    virtual core::Result<bool> load_sessions() = 0;
};

// ============================================================================
// Rate Limiter
// ============================================================================

class RateLimiter {
public:
    struct Config {
        size_t max_attempts = 20;
        core::Duration window_ms{60000};  // 1 minute
        bool exempt_loopback = true;
    };
    
    explicit RateLimiter(const Config& config);
    
    // Check rate limit
    bool check_limit(const std::string& identifier);
    
    // Reset counter
    void reset(const std::string& identifier);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
} // namespace hunterclaw
