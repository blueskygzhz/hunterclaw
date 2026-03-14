#include "hunterclaw/channels/channel.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/websocket.h"
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Discord REST API Client
// ============================================================================

class DiscordRESTClient {
public:
    explicit DiscordRESTClient(const std::string& token)
        : token_(token), base_url_("https://discord.com/api/v10") {}
    
    // Get bot user info
    core::Result<core::JsonValue> getCurrentUser() {
        return call_api("GET", "/users/@me", "");
    }
    
    // Send message to channel
    core::Result<core::JsonValue> createMessage(
        const std::string& channel_id,
        const std::string& content,
        const std::map<std::string, std::string>& opts = {}
    ) {
        core::JsonObject payload;
        payload["content"] = core::JsonValue(content);
        
        // Optional fields
        if (opts.count("reply_to")) {
            core::JsonObject ref;
            ref["message_id"] = core::JsonValue(opts.at("reply_to"));
            payload["message_reference"] = core::JsonValue(ref);
        }
        
        std::string body = core::JsonWriter::write(core::JsonValue(payload));
        return call_api("POST", "/channels/" + channel_id + "/messages", body);
    }
    
    // Create DM channel
    core::Result<core::JsonValue> createDM(const std::string& user_id) {
        core::JsonObject payload;
        payload["recipient_id"] = core::JsonValue(user_id);
        
        std::string body = core::JsonWriter::write(core::JsonValue(payload));
        return call_api("POST", "/users/@me/channels", body);
    }

private:
    core::Result<core::JsonValue> call_api(
        const std::string& method,
        const std::string& path,
        const std::string& body
    ) {
        core::HttpClient client;
        core::HttpRequest request;
        request.method = method;
        request.url = base_url_ + path;
        request.headers = {
            {"Authorization", "Bot " + token_},
            {"Content-Type", "application/json"},
            {"User-Agent", "HunterClaw/1.0"}
        };
        request.body = body;
        
        auto response = client.request(request);
        if (!response.success) {
            return core::Result<core::JsonValue>::error(
                "HTTP request failed: " + response.error_message
            );
        }
        
        // Check status code
        if (response.status_code >= 400) {
            return core::Result<core::JsonValue>::error(
                "Discord API error: " + std::to_string(response.status_code)
            );
        }
        
        // Parse JSON
        auto json = core::JsonParser::parse(response.body);
        if (!json.success) {
            return core::Result<core::JsonValue>::error(
                "JSON parse failed: " + json.error_message
            );
        }
        
        return core::Result<core::JsonValue>::ok(json.value);
    }
    
    std::string token_;
    std::string base_url_;
};

// ============================================================================
// Discord Gateway Client
// ============================================================================

class DiscordGatewayClient {
public:
    explicit DiscordGatewayClient(
        const std::string& token,
        std::function<void(const core::JsonValue&)> on_event
    ) : token_(token), on_event_(on_event), sequence_(0) {}
    
    core::Result<bool> connect() {
        // Connect to gateway
        std::string gateway_url = "wss://gateway.discord.gg/?v=10&encoding=json";
        ws_ = std::make_unique<core::WebSocket>(gateway_url);
        
        auto connect_result = ws_->connect();
        if (!connect_result.success) {
            return core::Result<bool>::error(
                "Failed to connect to gateway: " + connect_result.error_message
            );
        }
        
        // Start receive thread
        running_ = true;
        receive_thread_ = std::make_unique<std::thread>([this]() {
            receive_loop();
        });
        
        return core::Result<bool>::ok(true);
    }
    
    void disconnect() {
        running_ = false;
        if (ws_) {
            ws_->close();
        }
        if (receive_thread_ && receive_thread_->joinable()) {
            receive_thread_->join();
        }
    }
    
private:
    void receive_loop() {
        while (running_) {
            auto message = ws_->receive();
            if (!message.success) {
                if (message.error_message != "timeout") {
                    // Connection error, try to reconnect
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
                continue;
            }
            
            // Parse message
            auto json = core::JsonParser::parse(message.value);
            if (!json.success) {
                continue;
            }
            
            auto& payload = json.value.as_object();
            int op = payload["op"].as_int();
            
            // Update sequence
            if (payload.contains("s") && !payload["s"].is_null()) {
                sequence_ = payload["s"].as_int64();
            }
            
            switch (op) {
            case 10:  // Hello
                handle_hello(payload);
                break;
            case 0:   // Dispatch
                handle_dispatch(payload);
                break;
            case 11:  // Heartbeat ACK
                // Received heartbeat ack
                break;
            default:
                break;
            }
        }
    }
    
    void handle_hello(const core::JsonObject& payload) {
        // Extract heartbeat interval
        auto& d = payload.at("d").as_object();
        int heartbeat_interval = d["heartbeat_interval"].as_int();
        
        // Send identify
        send_identify();
        
        // Start heartbeat thread
        heartbeat_thread_ = std::make_unique<std::thread>([this, heartbeat_interval]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval));
                send_heartbeat();
            }
        });
    }
    
    void send_identify() {
        core::JsonObject payload;
        payload["op"] = core::JsonValue(2);  // Identify
        
        core::JsonObject d;
        d["token"] = core::JsonValue(token_);
        d["intents"] = core::JsonValue(37376);  // GUILDS | GUILD_MESSAGES | MESSAGE_CONTENT | DIRECT_MESSAGES
        
        core::JsonObject properties;
        properties["os"] = core::JsonValue("linux");
        properties["browser"] = core::JsonValue("hunterclaw");
        properties["device"] = core::JsonValue("hunterclaw");
        d["properties"] = core::JsonValue(properties);
        
        payload["d"] = core::JsonValue(d);
        
        std::string message = core::JsonWriter::write(core::JsonValue(payload));
        ws_->send(message);
    }
    
    void send_heartbeat() {
        core::JsonObject payload;
        payload["op"] = core::JsonValue(1);  // Heartbeat
        payload["d"] = sequence_ > 0 
            ? core::JsonValue(sequence_)
            : core::JsonValue::null();
        
        std::string message = core::JsonWriter::write(core::JsonValue(payload));
        ws_->send(message);
    }
    
    void handle_dispatch(const core::JsonObject& payload) {
        std::string event_name = payload.at("t").as_string();
        auto& data = payload.at("d");
        
        // Notify event handler
        if (on_event_) {
            core::JsonObject event;
            event["event"] = core::JsonValue(event_name);
            event["data"] = data;
            on_event_(core::JsonValue(event));
        }
    }
    
    std::string token_;
    std::function<void(const core::JsonValue&)> on_event_;
    std::unique_ptr<core::WebSocket> ws_;
    bool running_ = false;
    int64_t sequence_;
    std::unique_ptr<std::thread> receive_thread_;
    std::unique_ptr<std::thread> heartbeat_thread_;
};

// ============================================================================
// Discord Channel Adapter
// ============================================================================

class DiscordChannelAdapter : public ChannelAdapter {
public:
    DiscordChannelAdapter()
        : rest_(nullptr), gateway_(nullptr) {}
    
    ~DiscordChannelAdapter() override {
        stop_account();
    }
    
    ChannelMeta meta() const override {
        return {
            "discord",
            "Discord",
            "Discord Bot API integration",
            {
                ChannelCapability::SendText,
                ChannelCapability::SendImage,
                ChannelCapability::SendFile,
                ChannelCapability::ReceiveText,
                ChannelCapability::ReceiveImage,
                ChannelCapability::ReceiveFile,
                ChannelCapability::GroupChat,
                ChannelCapability::PrivateChat,
                ChannelCapability::ThreadedConversation
            }
        };
    }
    
    core::Result<bool> start_account(const ChannelContext& ctx) override {
        ctx_ = ctx;
        
        // Resolve token
        std::string token = ctx.account.token;
        if (token.empty()) {
            return core::Result<bool>::error("Missing bot token");
        }
        
        // Create REST client
        rest_ = std::make_unique<DiscordRESTClient>(token);
        
        // Verify token
        auto me_result = rest_->getCurrentUser();
        if (!me_result.success) {
            return core::Result<bool>::error(
                "Failed to verify bot token: " + me_result.error_message
            );
        }
        
        auto& me = me_result.value.as_object();
        bot_username_ = me["username"].as_string();
        bot_id_ = me["id"].as_string();
        ctx_.log_info("Discord bot " + bot_username_ + "#" + 
                      me["discriminator"].as_string() + " authenticated");
        
        // Create gateway client
        gateway_ = std::make_unique<DiscordGatewayClient>(
            token,
            [this](const core::JsonValue& event) {
                handle_gateway_event(event);
            }
        );
        
        // Connect to gateway
        auto connect_result = gateway_->connect();
        if (!connect_result.success) {
            return core::Result<bool>::error(connect_result.error_message);
        }
        
        connected_ = true;
        ctx_.log_info("Discord gateway connected");
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> stop_account() override {
        if (gateway_) {
            gateway_->disconnect();
            gateway_ = nullptr;
        }
        connected_ = false;
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) override {
        if (!rest_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = rest_->createMessage(to, text);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) override {
        // Discord requires multipart upload for images
        // For now, send as URL in text
        return send_text(to, "Image: " + image_url);
    }
    
    core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) override {
        // Discord requires multipart upload for files
        // For now, send as path in text
        return send_text(to, "File: " + file_path);
    }
    
    bool supports_voice() const override { return true; }
    bool supports_video() const override { return true; }
    bool supports_group_chat() const override { return true; }
    
    bool is_connected() const override {
        return connected_;
    }
    
    core::Result<std::string> get_status() const override {
        if (connected_) {
            return core::Result<std::string>::ok(
                "Connected as " + bot_username_ + " (ID: " + bot_id_ + ")"
            );
        }
        return core::Result<std::string>::ok("Disconnected");
    }

private:
    void handle_gateway_event(const core::JsonValue& event_json) {
        auto& event = event_json.as_object();
        std::string event_name = event["event"].as_string();
        
        if (event_name == "MESSAGE_CREATE") {
            process_message(event["data"]);
        } else if (event_name == "READY") {
            ctx_.log_info("Discord bot ready");
        }
    }
    
    void process_message(const core::JsonValue& msg_json) {
        auto& msg = msg_json.as_object();
        
        // Ignore bot messages
        if (msg.contains("author")) {
            auto& author = msg["author"].as_object();
            if (author.contains("bot") && author["bot"].as_bool()) {
                return;
            }
        }
        
        // Extract message data
        ChannelMessage channel_msg;
        channel_msg.id = msg["id"].as_string();
        channel_msg.channel_id = "discord";
        channel_msg.account_id = ctx_.account.account_id;
        
        // Sender info
        if (msg.contains("author")) {
            auto& author = msg["author"].as_object();
            channel_msg.from = author["id"].as_string();
            channel_msg.sender_name = author["username"].as_string();
        }
        
        // Channel ID
        if (msg.contains("channel_id")) {
            channel_msg.to = msg["channel_id"].as_string();
        }
        
        // Message content
        if (msg.contains("content")) {
            channel_msg.type = MessageType::Text;
            channel_msg.text = msg["content"].as_string();
        }
        
        // Attachments
        if (msg.contains("attachments")) {
            auto& attachments = msg["attachments"].as_array();
            for (auto& attachment : attachments) {
                auto& att = attachment.as_object();
                channel_msg.attachments.push_back(att["url"].as_string());
                
                // Determine type
                if (att.contains("content_type")) {
                    std::string content_type = att["content_type"].as_string();
                    if (content_type.find("image/") == 0) {
                        channel_msg.type = MessageType::Image;
                    } else if (content_type.find("video/") == 0) {
                        channel_msg.type = MessageType::Video;
                    } else {
                        channel_msg.type = MessageType::File;
                    }
                }
            }
        }
        
        // Timestamp
        if (msg.contains("timestamp")) {
            // Parse ISO 8601 timestamp
            channel_msg.timestamp = core::Timestamp::from_iso8601(
                msg["timestamp"].as_string()
            );
        }
        
        // Deliver to handler
        if (ctx_.on_message_received) {
            ctx_.on_message_received(channel_msg);
        }
    }
    
    std::unique_ptr<DiscordRESTClient> rest_;
    std::unique_ptr<DiscordGatewayClient> gateway_;
    ChannelContext ctx_;
    std::string bot_username_;
    std::string bot_id_;
    bool connected_ = false;
};

// ============================================================================
// Discord Channel Plugin
// ============================================================================

class DiscordChannelPlugin : public ChannelPlugin {
public:
    plugins::PluginMeta meta() const override {
        return {
            "discord-channel",
            "Discord Channel",
            "1.0.0",
            "Discord Bot API integration"
        };
    }
    
    std::shared_ptr<ChannelAdapter> create_adapter() override {
        return std::make_shared<DiscordChannelAdapter>();
    }
};

// Plugin factory export
extern "C" {
    plugins::Plugin* create_plugin() {
        return new DiscordChannelPlugin();
    }
    
    void destroy_plugin(plugins::Plugin* plugin) {
        delete plugin;
    }
}

} // namespace channels
} // namespace hunterclaw
