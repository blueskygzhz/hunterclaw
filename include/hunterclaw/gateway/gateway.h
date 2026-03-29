#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/manager.h"
#include "hunterclaw/skills/skill.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/gateway/todo_types.h"
#include "hunterclaw/gateway/auth_manager.h"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <memory>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

// Forward declarations
namespace hunterclaw {
namespace agents {
class AgentPool;
}
}

namespace hunterclaw {
namespace auto_reply {
class MessageDispatcher;
}
}

namespace hunterclaw {
namespace queue {
class DeliveryQueue;  // ✅ 对齐 OpenClaw: deliver 函数
}
}

namespace hunterclaw {
namespace cron {
class CronService;
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
// Gateway Constants (对齐 OpenClaw server-constants.ts)
// ============================================================================

// Keep server maxPayload aligned with gateway client maxPayload so high-res
// canvas snapshots don't get disconnected mid-invoke.
constexpr size_t MAX_PAYLOAD_BYTES = 25 * 1024 * 1024;
// Per-connection send buffer limit (2x max payload)
constexpr size_t MAX_BUFFERED_BYTES = 50 * 1024 * 1024;
// Pre-auth payload limit (before connect handshake completes)
constexpr size_t MAX_PREAUTH_PAYLOAD_BYTES = 64 * 1024;

constexpr int64_t TICK_INTERVAL_MS = 30000;
constexpr int64_t HEALTH_REFRESH_INTERVAL_MS = 60000;
constexpr int64_t DEDUPE_TTL_MS = 5 * 60000;
constexpr int DEDUPE_MAX = 1000;
constexpr int64_t DEFAULT_HANDSHAKE_TIMEOUT_MS = 3000;

// ============================================================================
// ConnectParams (对齐 OpenClaw protocol/schema/frames.ts ConnectParams)
// ============================================================================

struct ConnectClientInfo {
    std::string id;                          // client name/id
    std::optional<std::string> display_name;
    std::string version;
    std::string platform;
    std::optional<std::string> device_family;
    std::string mode;                        // "interactive" | "headless" | "backend" | "probe"
    std::optional<std::string> instance_id;
};

struct ConnectAuth {
    std::optional<std::string> token;
    std::optional<std::string> bootstrap_token;
    std::optional<std::string> device_token;
    std::optional<std::string> password;
};

struct ConnectDevice {
    std::string id;
    std::string public_key;
    std::string signature;
    int64_t signed_at;
    std::string nonce;
};

struct ConnectParams {
    int min_protocol = 1;
    int max_protocol = 1;
    ConnectClientInfo client;
    std::vector<std::string> caps;
    std::optional<std::vector<std::string>> commands;
    std::optional<std::map<std::string, bool>> permissions;
    std::optional<std::string> path_env;
    std::optional<ConnectAuth> auth;
    std::string role = "operator";           // "operator" | "node"
    std::vector<std::string> scopes;
    std::optional<ConnectDevice> device;
};

// ============================================================================
// GatewayWsClient (严格对齐 OpenClaw server/ws-types.ts GatewayWsClient)
// ============================================================================

struct GatewayWsClient {
    WsConnectionPtr socket;                  // WebSocket connection
    ConnectParams connect;                   // Connect params (set after handshake)
    std::string conn_id;                     // UUID
    std::optional<std::string> session_id;   // Session ID (set after auth)
    std::optional<std::string> presence_key;
    std::optional<std::string> client_ip;
    std::optional<std::string> canvas_host_url;
    std::optional<std::string> canvas_capability;
    std::optional<int64_t> canvas_capability_expires_at_ms;
};

// ============================================================================
// ClientContext (对齐 OpenClaw server-methods/types.ts GatewayClient)
// ============================================================================
// 方法处理器使用的客户端上下文。
// 定义在此处以避免 method_guard.h 与 gateway.h 的循环依赖。
struct ClientContext {
    ConnectParams connect;                           // Connect params (set after handshake)
    std::string conn_id;                             // Connection UUID
    std::string session_id;                          // Session ID (bound after auth)
    std::string device_id;                           // Device ID (from connect.device)
    std::string account_id;                          // Account ID (from auth token)
    bool authenticated = false;                      // Whether client has completed auth
    std::optional<std::string> client_ip;            // Client IP address
    std::optional<std::string> canvas_host_url;      // Canvas host URL
    std::optional<std::string> canvas_capability;    // Canvas capability token
    std::optional<int64_t> canvas_capability_expires_at_ms;

    // ── Convenience accessors ──

    // Role from connect params
    const std::string& role() const {
        return connect.role;
    }

    // Scopes from connect params
    const std::vector<std::string>& scopes() const {
        return connect.scopes;
    }

    // Client name from connect params
    const std::string& client_name() const {
        return connect.client.id;
    }
};

// ============================================================================
// Dedupe Entry (对齐 OpenClaw server-shared.ts)
// ============================================================================

struct DedupeEntry {
    int64_t ts;
    std::string method;
};

// ============================================================================
// Chat Run State (对齐 OpenClaw server-chat.ts createChatRunState)
// ============================================================================

struct ChatRunEntry {
    std::string session_key;
    std::string client_run_id;
};

struct ChatAbortControllerEntry {
    std::shared_ptr<std::atomic<bool>> aborted;
    int64_t created_at;
};

// Chat run registry (对齐 OpenClaw createChatRunState)
struct ChatRunRegistry {
    // sessionId -> list<ChatRunEntry>
    std::map<std::string, std::vector<ChatRunEntry>> runs;
    mutable std::mutex mutex;

    void add(const std::string& session_id, const ChatRunEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex);
        runs[session_id].push_back(entry);
    }

    std::optional<ChatRunEntry> remove(
        const std::string& session_id,
        const std::string& client_run_id,
        const std::optional<std::string>& session_key = std::nullopt
    ) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = runs.find(session_id);
        if (it == runs.end()) return std::nullopt;
        auto& list = it->second;
        for (auto jt = list.begin(); jt != list.end(); ++jt) {
            if (jt->client_run_id == client_run_id) {
                if (session_key && jt->session_key != *session_key) continue;
                ChatRunEntry entry = *jt;
                list.erase(jt);
                if (list.empty()) runs.erase(it);
                return entry;
            }
        }
        return std::nullopt;
    }
};

struct ChatRunState {
    ChatRunRegistry registry;
    // sessionId -> buffer string (streaming delta buffer)
    std::map<std::string, std::string> buffers;
    // sessionId -> last delta sent timestamp
    std::map<std::string, int64_t> delta_sent_at;
    // runId -> abort timestamp
    std::map<std::string, int64_t> aborted_runs;
    mutable std::mutex buffers_mutex;
};

// ============================================================================
// Broadcast Function Types (对齐 OpenClaw server-broadcast.ts)
// ============================================================================

struct GatewayBroadcastStateVersion {
    std::optional<int> presence;
    std::optional<int> health;
};

struct GatewayBroadcastOpts {
    bool drop_if_slow = false;
    std::optional<GatewayBroadcastStateVersion> state_version;
};

// broadcast(event, payload, opts?)
using GatewayBroadcastFn = std::function<void(
    const std::string& event,
    const core::JsonValue& payload,
    const GatewayBroadcastOpts& opts
)>;

// broadcastToConnIds(event, payload, connIds, opts?)
using GatewayBroadcastToConnIdsFn = std::function<void(
    const std::string& event,
    const core::JsonValue& payload,
    const std::set<std::string>& conn_ids,
    const GatewayBroadcastOpts& opts
)>;

// ============================================================================
// Gateway Runtime State (对齐 OpenClaw createGatewayRuntimeState 返回值)
// ============================================================================

struct GatewayRuntimeState {
    // Connected clients
    // 对齐 OpenClaw: Set<GatewayWsClient>
    // C++ 实现: map<connId, GatewayWsClient> 支持按 conn_id 快速查找
    std::map<std::string, std::shared_ptr<GatewayWsClient>> clients;

    // Broadcast functions (对齐 OpenClaw)
    GatewayBroadcastFn broadcast;
    GatewayBroadcastToConnIdsFn broadcast_to_conn_ids;

    // Agent run sequence (对齐 OpenClaw: Map<string, number>)
    std::map<std::string, int64_t> agent_run_seq;

    // Message deduplication (对齐 OpenClaw: Map<string, DedupeEntry>)
    std::map<std::string, DedupeEntry> dedupe;

    // Chat run state (对齐 OpenClaw: chatRunState)
    std::shared_ptr<ChatRunState> chat_run_state;

    // Chat abort controllers (对齐 OpenClaw: Map<string, ChatAbortControllerEntry>)
    std::map<std::string, ChatAbortControllerEntry> chat_abort_controllers;

    // Tool event recipients (对齐 OpenClaw: toolEventRecipients)
    // runId -> set<connId>
    std::map<std::string, std::set<std::string>> tool_event_recipients;

    // TODO items (Session-scoped, todoId -> TodoItem)
    std::map<std::string, TodoItem> todos;

    // Agent abort controllers (runId -> abort flag)
    // 对齐 OpenClaw: Map<string, AbortController>
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> abort_controllers;

    // Method handler (internal, injected by methods.cc)
    // 对应 OpenClaw: handler({ req, params, client, state, ... })
    std::function<core::Result<nlohmann::json>(
        const std::string&,
        const nlohmann::json&,
        const ClientContext&
    )> method_handler;

    // ── Convenience helpers (对齐 OpenClaw addChatRun / removeChatRun) ──

    void add_chat_run(const std::string& session_id, const ChatRunEntry& entry) {
        if (chat_run_state) {
            chat_run_state->registry.add(session_id, entry);
        }
    }

    std::optional<ChatRunEntry> remove_chat_run(
        const std::string& session_id,
        const std::string& client_run_id,
        const std::optional<std::string>& session_key = std::nullopt
    ) {
        if (!chat_run_state) return std::nullopt;
        return chat_run_state->registry.remove(session_id, client_run_id, session_key);
    }

    // ── Tool Event Recipient Registry helpers ──

    void register_tool_event_recipient(const std::string& run_id, const std::string& conn_id) {
        tool_event_recipients[run_id].insert(conn_id);
    }

    std::optional<std::set<std::string>> get_tool_event_recipients(const std::string& run_id) const {
        auto it = tool_event_recipients.find(run_id);
        if (it == tool_event_recipients.end()) return std::nullopt;
        return it->second;
    }

    void cleanup_tool_event_recipients(const std::string& run_id) {
        tool_event_recipients.erase(run_id);
    }

    // Thread safety
    mutable std::mutex mutex;
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

    // Broadcast (对齐 OpenClaw server-broadcast.ts)
    // 广播事件给所有已连接的客户端
    void broadcast(const core::EventFrame& event);
    // 广播事件给指定 conn_id 列表的客户端
    void broadcast_to_conn_ids(
        const std::vector<std::string>& conn_ids,
        const core::EventFrame& event
    );

    // Component access
    std::shared_ptr<channels::ChannelManager> channel_manager() const;
    std::shared_ptr<skills::SkillManager> skill_manager() const;
    std::shared_ptr<plugins::PluginManager> plugin_manager() const;
    std::shared_ptr<cron::CronService> cron_service() const;

    // Config access
    std::shared_ptr<core::Config> config() const;

    // State access
    std::shared_ptr<GatewayRuntimeState> runtime_state() const;

    // 🏢 Multi-tenant Agent Pool access (对齐 OpenClaw)
    std::shared_ptr<agents::AgentPool> agent_pool() const;

    // ✅ MessageDispatcher access (对齐 OpenClaw: dispatchInboundMessage)
    // chat.send 通过 message_dispatcher()->run_reply_agent() 调度，
    // 使四种 queue mode (steer/followup/collect/interrupt) 对 chat.send 生效
    std::shared_ptr<auto_reply::MessageDispatcher> message_dispatcher() const;

    // ✅ DeliveryQueue access (对齐 OpenClaw: deliver 函数)
    // method_agent 中 deliver=true 时，通过 delivery_queue 构建 deliver 函数
    // 对应 OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver
    std::shared_ptr<queue::DeliveryQueue> delivery_queue() const;

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
        const GatewayWsClient& client,
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
        const GatewayWsClient& client,
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
        std::shared_ptr<GatewayWsClient> client
    );

    // Connection handling
    void handle_open(const ConnectionHandle& hdl);
    void handle_close(const ConnectionHandle& hdl);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Session Manager
// ============================================================================

class SessionManager {
public:
    static std::shared_ptr<SessionManager> create();

    SessionManager() = default;
    virtual ~SessionManager() = default;

    virtual std::string create_session(const core::SessionKey& key) = 0;
    virtual std::optional<core::SessionInfo> get_session(const std::string& session_id) = 0;
    virtual bool update_last_active(const std::string& session_id) = 0;
    virtual bool delete_session(const std::string& session_id) = 0;

    virtual std::vector<core::SessionInfo> list_sessions() = 0;
    virtual std::vector<core::SessionInfo> find_sessions_by_key(const core::SessionKey& key) = 0;

    virtual core::Result<bool> save_session(const core::SessionInfo& session) = 0;
    virtual core::Result<bool> load_sessions() = 0;
};

// ============================================================================
// Rate Limiter (对齐 OpenClaw auth-rate-limit.ts)
// ============================================================================

class RateLimiter {
public:
    struct Config {
        size_t max_attempts = 20;
        core::Duration window_ms{60000};  // 1 minute
        bool exempt_loopback = true;
    };

    explicit RateLimiter(const Config& config);

    bool check_limit(const std::string& identifier);
    void reset(const std::string& identifier);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gateway
} // namespace hunterclaw
