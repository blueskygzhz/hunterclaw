// Aligned with openclaw: @buape/carbon GatewayPlugin + gateway-plugin.ts
// Discord Gateway WebSocket client: connects to wss://gateway.discord.gg,
// handles IDENTIFY / HEARTBEAT / RESUME / DISPATCH opcodes.
#pragma once

#include "hunterclaw/channels/discord_listener.h"
#include <nlohmann/json.hpp>
#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Discord Gateway opcodes (aligned with discord-api-types GatewayOpcodes)
// ============================================================================
enum class DiscordGatewayOpcode : int {
    Dispatch           = 0,   // Server → Client: event dispatch
    Heartbeat          = 1,   // Bidirectional: heartbeat
    Identify           = 2,   // Client → Server: identify
    PresenceUpdate     = 3,   // Client → Server: update presence
    VoiceStateUpdate   = 4,   // Client → Server: voice state
    Resume             = 6,   // Client → Server: resume session
    Reconnect          = 7,   // Server → Client: reconnect
    RequestGuildMembers = 8,  // Client → Server: request guild members
    InvalidSession     = 9,   // Server → Client: invalid session
    Hello              = 10,  // Server → Client: hello (heartbeat interval)
    HeartbeatAck       = 11,  // Server → Client: heartbeat ack
};

// ============================================================================
// DiscordGatewayClientOptions
// Aligned with openclaw: GatewayPlugin options + createDiscordGatewayPlugin
// ============================================================================
struct DiscordGatewayClientOptions {
    std::string token;
    std::string account_id;
    int intents = 0;  // Aligned with openclaw: resolveDiscordGatewayIntents

    // Aligned with openclaw: reconnect: { maxAttempts: 50 }
    int max_reconnect_attempts = 50;

    // Aligned with openclaw: eventQueue.listenerTimeout = 120_000
    int listener_timeout_ms = 120'000;

    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<std::atomic<bool>> abort_signal;

    // Aligned with openclaw: GatewayPlugin emits "debug" events
    std::function<void(const std::string& msg)> on_debug;

    // Aligned with openclaw: GatewayPlugin emits "error" events
    std::function<void(const std::string& err)> on_error;

    // Aligned with openclaw: gateway.isConnected (READY / RESUMED)
    std::function<void()> on_connected;

    // Aligned with openclaw: "WebSocket connection closed" debug event
    std::function<void(uint16_t code, const std::string& reason)> on_disconnected;

    // Aligned with openclaw: DiscordMessageListener.handle(data, client)
    // Called for every DISPATCH event; listeners decide which events to handle.
    std::function<void(const std::string& event_name, const nlohmann::json& data)> on_dispatch;
};

// ============================================================================
// DiscordGatewayClient
// Aligned with openclaw: GatewayPlugin (buape/carbon/gateway)
//
// Lifecycle:
//   connect()  → WebSocket open → HELLO → IDENTIFY/RESUME → READY/RESUMED
//   run()      → blocks until abort_signal or fatal error
//   disconnect() → graceful close
// ============================================================================
class DiscordGatewayClient {
public:
    explicit DiscordGatewayClient(DiscordGatewayClientOptions opts);
    ~DiscordGatewayClient();

    // Aligned with openclaw: gateway.connect() — start WebSocket connection
    void connect(bool resume = false);

    // Aligned with openclaw: gateway.disconnect()
    void disconnect();

    // Aligned with openclaw: gateway.isConnected
    bool is_connected() const { return connected_.load(); }

    // Aligned with openclaw: gateway.updatePresence(presence)
    void update_presence(const nlohmann::json& presence);

    // Aligned with openclaw: gateway.options.reconnect = { maxAttempts: 0 }
    void set_max_reconnect_attempts(int n) { max_reconnect_attempts_.store(n); }

    // Aligned with openclaw: gateway.state.sessionId / resumeGatewayUrl / sequence
    void clear_resume_state();

private:
    void on_ws_message(const ix::WebSocketMessagePtr& msg);
    void handle_payload(const nlohmann::json& payload);
    void send_identify();
    void send_resume();
    void send_heartbeat();
    void start_heartbeat(int interval_ms);
    void stop_heartbeat();
    void emit_debug(const std::string& msg);

    DiscordGatewayClientOptions opts_;
    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<int> max_reconnect_attempts_;

    // Aligned with openclaw: gateway.state
    mutable std::mutex state_mutex_;
    std::optional<std::string> session_id_;
    std::optional<std::string> resume_gateway_url_;
    std::optional<int64_t> sequence_;

    // Heartbeat
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    std::atomic<int> heartbeat_interval_ms_{0};
    std::atomic<bool> heartbeat_ack_received_{true};
};

// ============================================================================
// resolve_discord_gateway_intents
// Aligned with openclaw: resolveDiscordGatewayIntents (gateway-plugin.ts)
// ============================================================================
int resolve_discord_gateway_intents(const nlohmann::json& intents_config);

} // namespace channels
} // namespace hunterclaw
