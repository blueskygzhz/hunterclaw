// Aligned with openclaw: @buape/carbon GatewayPlugin + gateway-plugin.ts
#include "hunterclaw/channels/discord_gateway_client.h"
#include <ixwebsocket/IXNetSystem.h>
#include <chrono>
#include <sstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace hunterclaw {
namespace channels {

namespace {

// Aligned with openclaw: resolveDiscordGatewayIntents
// GatewayIntents values from discord-api-types
constexpr int INTENT_GUILDS                  = 1 << 0;
constexpr int INTENT_GUILD_MESSAGES          = 1 << 9;
constexpr int INTENT_GUILD_MESSAGE_REACTIONS = 1 << 10;
constexpr int INTENT_GUILD_VOICE_STATES      = 1 << 7;
constexpr int INTENT_GUILD_PRESENCES         = 1 << 8;
constexpr int INTENT_GUILD_MEMBERS           = 1 << 1;
constexpr int INTENT_DIRECT_MESSAGES         = 1 << 12;
constexpr int INTENT_DIRECT_MESSAGE_REACTIONS = 1 << 13;
constexpr int INTENT_MESSAGE_CONTENT         = 1 << 15;

std::shared_ptr<spdlog::logger> get_fallback_logger() {
    auto log = spdlog::get("discord_gateway");
    if (!log) log = spdlog::stdout_logger_mt("discord_gateway");
    return log;
}

}  // namespace

// ============================================================================
// resolve_discord_gateway_intents
// Aligned with openclaw: resolveDiscordGatewayIntents (gateway-plugin.ts)
// ============================================================================
int resolve_discord_gateway_intents(const nlohmann::json& intents_config) {
    int intents = INTENT_GUILDS
                | INTENT_GUILD_MESSAGES
                | INTENT_MESSAGE_CONTENT
                | INTENT_DIRECT_MESSAGES
                | INTENT_GUILD_MESSAGE_REACTIONS
                | INTENT_DIRECT_MESSAGE_REACTIONS
                | INTENT_GUILD_VOICE_STATES;
    if (!intents_config.is_null() && intents_config.is_object()) {
        if (intents_config.value("presence", false)) {
            intents |= INTENT_GUILD_PRESENCES;
        }
        if (intents_config.value("guildMembers", false)) {
            intents |= INTENT_GUILD_MEMBERS;
        }
    }
    return intents;
}

// ============================================================================
// DiscordGatewayClient
// ============================================================================
DiscordGatewayClient::DiscordGatewayClient(DiscordGatewayClientOptions opts)
    : opts_(std::move(opts))
{
    max_reconnect_attempts_.store(opts_.max_reconnect_attempts);
    ix::initNetSystem();
}

DiscordGatewayClient::~DiscordGatewayClient() {
    stopping_.store(true);
    stop_heartbeat();
    if (ws_) {
        ws_->stop();
    }
    ix::uninitNetSystem();
}

void DiscordGatewayClient::emit_debug(const std::string& msg) {
    if (opts_.on_debug) opts_.on_debug(msg);
}

void DiscordGatewayClient::clear_resume_state() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session_id_.reset();
    resume_gateway_url_.reset();
    sequence_.reset();
}

// Aligned with openclaw: gateway.connect(resume)
void DiscordGatewayClient::connect(bool resume) {
    if (stopping_.load()) return;

    // Aligned with openclaw: fetchDiscordGatewayInfo → use resumeGatewayUrl if available
    std::string gateway_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (resume && resume_gateway_url_.has_value()) {
            gateway_url = *resume_gateway_url_;
        }
    }
    if (gateway_url.empty()) {
        gateway_url = "wss://gateway.discord.gg/?v=10&encoding=json";
    } else {
        // Append query params if not present
        if (gateway_url.find('?') == std::string::npos) {
            gateway_url += "/?v=10&encoding=json";
        }
    }

    ws_ = std::make_unique<ix::WebSocket>();
    ws_->setUrl(gateway_url);
    ws_->setHandshakeTimeout(30);
    ws_->setPingInterval(0);  // We manage heartbeat ourselves

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        on_ws_message(msg);
    });

    // Aligned with openclaw: "WebSocket connection opened" debug event
    emit_debug("WebSocket connection opened (connecting to " + gateway_url + ")");
    ws_->start();
}

// Aligned with openclaw: gateway.disconnect()
void DiscordGatewayClient::disconnect() {
    stopping_.store(true);
    stop_heartbeat();
    connected_.store(false);
    if (ws_) {
        ws_->stop();
        // Aligned with openclaw: "WebSocket connection closed" debug event
        emit_debug("WebSocket connection closed (disconnect requested)");
    }
}

void DiscordGatewayClient::update_presence(const nlohmann::json& presence) {
    if (!ws_ || !connected_.load()) return;
    nlohmann::json payload = {
        {"op", static_cast<int>(DiscordGatewayOpcode::PresenceUpdate)},
        {"d", presence}
    };
    ws_->sendText(payload.dump());
}

// ============================================================================
// WebSocket message handler
// ============================================================================
void DiscordGatewayClient::on_ws_message(const ix::WebSocketMessagePtr& msg) {
    if (!msg) return;

    switch (msg->type) {
    case ix::WebSocketMessageType::Open:
        emit_debug("WebSocket connection opened");
        break;

    case ix::WebSocketMessageType::Close: {
        connected_.store(false);
        stop_heartbeat();
        const uint16_t code = msg->closeInfo.code;
        const std::string reason = msg->closeInfo.reason;
        // Aligned with openclaw: "WebSocket connection closed" debug event
        emit_debug("WebSocket connection closed code " + std::to_string(code) + " " + reason);
        if (opts_.on_disconnected) opts_.on_disconnected(code, reason);
        break;
    }

    case ix::WebSocketMessageType::Error: {
        const std::string err = msg->errorInfo.reason;
        emit_debug("WebSocket error: " + err);
        if (opts_.on_error) opts_.on_error(err);
        break;
    }

    case ix::WebSocketMessageType::Message: {
        try {
            auto payload = nlohmann::json::parse(msg->str);
            handle_payload(payload);
        } catch (const std::exception& e) {
            const auto& log = opts_.logger ? opts_.logger : get_fallback_logger();
            log->error("[discord_gateway] failed to parse payload: {}", e.what());
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// Payload handler — aligned with openclaw: GatewayPlugin opcode handling
// ============================================================================
void DiscordGatewayClient::handle_payload(const nlohmann::json& payload) {
    if (!payload.is_object()) return;

    const int op = payload.value("op", -1);
    const auto& d = payload.contains("d") ? payload["d"] : nlohmann::json{};

    // Aligned with openclaw: update sequence number on every dispatch
    if (payload.contains("s") && !payload["s"].is_null()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sequence_ = payload["s"].get<int64_t>();
    }

    switch (static_cast<DiscordGatewayOpcode>(op)) {

    // Aligned with openclaw: op 10 HELLO → start heartbeat + IDENTIFY/RESUME
    case DiscordGatewayOpcode::Hello: {
        const int interval_ms = d.value("heartbeat_interval", 41250);
        emit_debug("HELLO heartbeat_interval=" + std::to_string(interval_ms));
        start_heartbeat(interval_ms);

        bool should_resume = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            should_resume = session_id_.has_value() && sequence_.has_value();
        }
        if (should_resume) {
            send_resume();
        } else {
            send_identify();
        }
        break;
    }

    // Aligned with openclaw: op 11 HEARTBEAT_ACK
    case DiscordGatewayOpcode::HeartbeatAck:
        heartbeat_ack_received_.store(true);
        emit_debug("HEARTBEAT_ACK");
        break;

    // Aligned with openclaw: op 1 HEARTBEAT (server requests immediate heartbeat)
    case DiscordGatewayOpcode::Heartbeat:
        send_heartbeat();
        break;

    // Aligned with openclaw: op 7 RECONNECT → disconnect + reconnect with resume
    case DiscordGatewayOpcode::Reconnect:
        emit_debug("RECONNECT requested by server");
        connected_.store(false);
        stop_heartbeat();
        if (ws_) ws_->stop();
        if (opts_.on_disconnected) opts_.on_disconnected(0, "reconnect");
        break;

    // Aligned with openclaw: op 9 INVALID_SESSION → clear state if not resumable
    case DiscordGatewayOpcode::InvalidSession: {
        const bool resumable = d.is_boolean() && d.get<bool>();
        emit_debug(std::string("INVALID_SESSION resumable=") + (resumable ? "true" : "false"));
        if (!resumable) {
            clear_resume_state();
        }
        connected_.store(false);
        stop_heartbeat();
        if (ws_) ws_->stop();
        if (opts_.on_disconnected) opts_.on_disconnected(0, "invalid-session");
        break;
    }

    // Aligned with openclaw: op 0 DISPATCH → route to listeners
    case DiscordGatewayOpcode::Dispatch: {
        const std::string event_name = payload.value("t", std::string{});

        // Aligned with openclaw: READY → extract sessionId, resumeGatewayUrl, botUserId
        if (event_name == "READY") {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (d.contains("session_id") && d["session_id"].is_string()) {
                    session_id_ = d["session_id"].get<std::string>();
                }
                if (d.contains("resume_gateway_url") && d["resume_gateway_url"].is_string()) {
                    resume_gateway_url_ = d["resume_gateway_url"].get<std::string>();
                }
            }
            connected_.store(true);
            emit_debug("READY session_id=" + session_id_.value_or("<missing>"));
            if (opts_.on_connected) opts_.on_connected();
        }
        // Aligned with openclaw: RESUMED → mark connected
        else if (event_name == "RESUMED") {
            connected_.store(true);
            emit_debug("RESUMED");
            if (opts_.on_connected) opts_.on_connected();
        }

        // Aligned with openclaw: dispatch to all registered listeners
        if (opts_.on_dispatch && !event_name.empty()) {
            opts_.on_dispatch(event_name, d);
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// IDENTIFY
// Aligned with openclaw: GatewayPlugin.identify()
// ============================================================================
void DiscordGatewayClient::send_identify() {
    if (!ws_) return;
    nlohmann::json payload = {
        {"op", static_cast<int>(DiscordGatewayOpcode::Identify)},
        {"d", {
            {"token", opts_.token},
            {"intents", opts_.intents},
            {"properties", {
                {"os", "linux"},
                {"browser", "hunterclaw"},
                {"device", "hunterclaw"}
            }}
        }}
    };
    emit_debug("IDENTIFY intents=" + std::to_string(opts_.intents));
    ws_->sendText(payload.dump());
}

// ============================================================================
// RESUME
// Aligned with openclaw: GatewayPlugin.resume()
// ============================================================================
void DiscordGatewayClient::send_resume() {
    if (!ws_) return;
    std::string session_id;
    int64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        session_id = session_id_.value_or("");
        seq = sequence_.value_or(0);
    }
    nlohmann::json payload = {
        {"op", static_cast<int>(DiscordGatewayOpcode::Resume)},
        {"d", {
            {"token", opts_.token},
            {"session_id", session_id},
            {"seq", seq}
        }}
    };
    emit_debug("RESUME session_id=" + session_id + " seq=" + std::to_string(seq));
    ws_->sendText(payload.dump());
}

// ============================================================================
// HEARTBEAT
// Aligned with openclaw: GatewayPlugin heartbeat loop
// ============================================================================
void DiscordGatewayClient::send_heartbeat() {
    if (!ws_) return;
    std::optional<int64_t> seq;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        seq = sequence_;
    }
    nlohmann::json payload = {
        {"op", static_cast<int>(DiscordGatewayOpcode::Heartbeat)},
        {"d", seq.has_value() ? nlohmann::json(*seq) : nlohmann::json(nullptr)}
    };
    ws_->sendText(payload.dump());
}

void DiscordGatewayClient::start_heartbeat(int interval_ms) {
    stop_heartbeat();
    heartbeat_interval_ms_.store(interval_ms);
    heartbeat_ack_received_.store(true);
    heartbeat_running_.store(true);

    heartbeat_thread_ = std::thread([this, interval_ms]() {
        // Aligned with openclaw: jitter on first heartbeat (random 0..interval)
        const int jitter_ms = interval_ms > 0
            ? static_cast<int>(std::rand() % interval_ms)
            : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms));

        while (heartbeat_running_.load() && !stopping_.load()) {
            // Aligned with openclaw: if no ACK received → zombie connection → disconnect
            if (!heartbeat_ack_received_.load()) {
                emit_debug("heartbeat ACK not received — zombie connection, disconnecting");
                connected_.store(false);
                if (ws_) ws_->stop();
                if (opts_.on_disconnected) opts_.on_disconnected(0, "heartbeat-zombie");
                break;
            }
            heartbeat_ack_received_.store(false);
            send_heartbeat();

            const int sleep_ms = heartbeat_interval_ms_.load();
            for (int i = 0; i < sleep_ms / 50 && heartbeat_running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

void DiscordGatewayClient::stop_heartbeat() {
    heartbeat_running_.store(false);
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

}  // namespace channels
}  // namespace hunterclaw
