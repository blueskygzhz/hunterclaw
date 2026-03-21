#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/gateway/todo_types.h"
#include "hunterclaw/gateway/auth_manager.h"  // ✅ AuthResult, AuthManager
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <memory>
#include <set>

// Forward declarations
namespace hunterclaw {
namespace agents {
class AgentPool;
}
}

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
    
    // 🔐 鉴权信息 (对齐 OpenClaw ClientContext)
    std::string session_id;  // 关联的 Session ID (用于资源隔离)
    std::string device_id;
    std::string account_id;
    std::string channel_id;
    std::string role;        // admin, user, guest
    std::vector<std::string> scopes;
    bool authenticated = false;
    
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
    
    // Device → Connections mapping (反向索引,对齐 OpenClaw)
    std::map<std::string, std::set<std::string>> device_to_conns;
    
    // Tool Event Subscriptions (对齐 OpenClaw ToolEventRecipientRegistry)
    // runId → set<connId>: 哪些连接订阅了该 runId 的工具事件
    std::map<std::string, std::set<std::string>> tool_event_recipients;
    
    // Session state
    std::map<std::string, core::SessionInfo> sessions;
    
    // Agent run state
    std::map<std::string, uint64_t> agent_run_seq;
    
    // Message deduplication
    std::map<std::string, core::Timestamp> dedupe;
    
    // Abort controllers (for cancelling long-running tasks)
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> abort_controllers;
    
    // 🔐 TODO 存储 (对齐 OpenClaw)
    std::map<std::string, TodoItem> todos;
    
    // Method handler (RPC dispatcher) - for HTTP Gateway integration
    std::function<core::Result<nlohmann::json>(const std::string&, const nlohmann::json&)> method_handler;
    
    // Thread safety
    std::mutex mutex;
    
    // ========================================================================
    // Tool Event Recipient Registry (对齐 OpenClaw)
    // ========================================================================
    
    // 注册工具事件订阅
    void register_tool_event_recipient(const std::string& run_id, const std::string& conn_id) {
        std::lock_guard<std::mutex> lock(mutex);
        tool_event_recipients[run_id].insert(conn_id);
    }
    
    // 获取订阅者列表
    std::optional<std::set<std::string>> get_tool_event_recipients(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = tool_event_recipients.find(run_id);
        if (it == tool_event_recipients.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    
    // 清理订阅 (当 run 完成时)
    void cleanup_tool_event_recipients(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mutex);
        tool_event_recipients.erase(run_id);
    }
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
    
    // Config access
    std::shared_ptr<core::Config> config() const;
    
    // State access
    std::shared_ptr<GatewayRuntimeState> runtime_state() const;
    
    // 🏢 Multi-tenant Agent Pool access (对齐 OpenClaw)
    std::shared_ptr<agents::AgentPool> agent_pool() const;
    
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
// Authentication (使用 auth_manager.h 中的定义)
// ============================================================================

// AuthResult 和 AuthManager 定义在 hunterclaw/gateway/auth_manager.h

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
