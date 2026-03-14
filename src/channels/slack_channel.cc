#include "hunterclaw/channels/channel.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/websocket.h"
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Slack Web API Client
// ============================================================================

class SlackWebAPIClient {
public:
    explicit SlackWebAPIClient(const std::string& bot_token)
        : bot_token_(bot_token), base_url_("https://slack.com/api") {}
    
    // Auth test
    core::Result<core::JsonValue> authTest() {
        return call_api("auth.test", {});
    }
    
    // Post message
    core::Result<core::JsonValue> chatPostMessage(
        const std::string& channel,
        const std::string& text,
        const std::map<std::string, std::string>& opts = {}
    ) {
        std::map<std::string, std::string> params = {
            {"channel", channel},
            {"text", text}
        };
        
        // Merge options
        for (const auto& [key, value] : opts) {
            params[key] = value;
        }
        
        return call_api("chat.postMessage", params);
    }
    
    // Open conversation (DM or channel)
    core::Result<core::JsonValue> conversationsOpen(const std::string& users) {
        return call_api("conversations.open", {{"users", users}});
    }
    
    // Upload file
    core::Result<core::JsonValue> filesUpload(
        const std::string& channels,
        const std::string& file_path,
        const std::string& title = ""
    ) {
        // TODO: Implement multipart file upload
        return core::Result<core::JsonValue>::error("File upload not implemented");
    }

private:
    core::Result<core::JsonValue> call_api(
        const std::string& method,
        const std::map<std::string, std::string>& params
    ) {
        // Build JSON payload
        core::JsonObject payload;
        for (const auto& [key, value] : params) {
            payload[key] = core::JsonValue(value);
        }
        
        std::string body = core::JsonWriter::write(core::JsonValue(payload));
        
        // Call API
        core::HttpClient client;
        core::HttpRequest request;
        request.method = "POST";
        request.url = base_url_ + "/" + method;
        request.headers = {
            {"Authorization", "Bearer " + bot_token_},
            {"Content-Type", "application/json; charset=utf-8"}
        };
        request.body = body;
        
        auto response = client.request(request);
        if (!response.success) {
            return core::Result<core::JsonValue>::error(
                "HTTP request failed: " + response.error_message
            );
        }
        
        // Parse JSON
        auto json = core::JsonParser::parse(response.body);
        if (!json.success) {
            return core::Result<core::JsonValue>::error(
                "JSON parse failed: " + json.error_message
            );
        }
        
        // Check Slack API response
        auto& obj = json.value.as_object();
        if (!obj["ok"].as_bool()) {
            std::string error = obj.contains("error")
                ? obj["error"].as_string()
                : "Unknown error";
            return core::Result<core::JsonValue>::error(
                "Slack API error: " + error
            );
        }
        
        return core::Result<core::JsonValue>::ok(json.value);
    }
    
    std::string bot_token_;
    std::string base_url_;
};

// ============================================================================
// Slack Socket Mode Client
// ============================================================================

class SlackSocketModeClient {
public:
    explicit SlackSocketModeClient(
        const std::string& app_token,
        std::function<void(const core::JsonValue&)> on_event
    ) : app_token_(app_token), on_event_(on_event) {}
    
    core::Result<bool> connect() {
        // Get WebSocket URL
        auto ws_url_result = get_websocket_url();
        if (!ws_url_result.success) {
            return core::Result<bool>::error(ws_url_result.error_message);
        }
        
        // Connect to WebSocket
        ws_ = std::make_unique<core::WebSocket>(ws_url_result.value);
        auto connect_result = ws_->connect();
        if (!connect_result.success) {
            return core::Result<bool>::error(connect_result.error_message);
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
    core::Result<std::string> get_websocket_url() {
        core::HttpClient client;
        core::HttpRequest request;
        request.method = "POST";
        request.url = "https://slack.com/api/apps.connections.open";
        request.headers = {
            {"Authorization", "Bearer " + app_token_},
            {"Content-Type", "application/json"}
        };
        
        auto response = client.request(request);
        if (!response.success) {
            return core::Result<std::string>::error(
                "HTTP request failed: " + response.error_message
            );
        }
        
        auto json = core::JsonParser::parse(response.body);
        if (!json.success) {
            return core::Result<std::string>::error(
                "JSON parse failed: " + json.error_message
            );
        }
        
        auto& obj = json.value.as_object();
        if (!obj["ok"].as_bool()) {
            return core::Result<std::string>::error(
                "Failed to get WebSocket URL"
            );
        }
        
        return core::Result<std::string>::ok(
            obj["url"].as_string()
        );
    }
    
    void receive_loop() {
        while (running_) {
            auto message = ws_->receive();
            if (!message.success) {
                if (message.error_message != "timeout") {
                    // Connection error
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
            std::string type = payload["type"].as_string();
            
            if (type == "events_api") {
                // Event from Events API
                handle_event(payload);
            } else if (type == "slash_commands") {
                // Slash command
                handle_slash_command(payload);
            } else if (type == "interactive") {
                // Interactive component
                handle_interactive(payload);
            }
            
            // Acknowledge envelope
            if (payload.contains("envelope_id")) {
                acknowledge_envelope(payload["envelope_id"].as_string());
            }
        }
    }
    
    void handle_event(const core::JsonObject& payload) {
        auto& envelope = payload.at("payload").as_object();
        
        if (on_event_) {
            on_event_(core::JsonValue(envelope));
        }
    }
    
    void handle_slash_command(const core::JsonObject& payload) {
        // TODO: Handle slash commands
    }
    
    void handle_interactive(const core::JsonObject& payload) {
        // TODO: Handle interactive components
    }
    
    void acknowledge_envelope(const std::string& envelope_id) {
        core::JsonObject ack;
        ack["envelope_id"] = core::JsonValue(envelope_id);
        
        std::string message = core::JsonWriter::write(core::JsonValue(ack));
        ws_->send(message);
    }
    
    std::string app_token_;
    std::function<void(const core::JsonValue&)> on_event_;
    std::unique_ptr<core::WebSocket> ws_;
    bool running_ = false;
    std::unique_ptr<std::thread> receive_thread_;
};

// ============================================================================
// Slack Channel Adapter
// ============================================================================

class SlackChannelAdapter : public ChannelAdapter {
public:
    SlackChannelAdapter()
        : web_api_(nullptr), socket_client_(nullptr) {}
    
    ~SlackChannelAdapter() override {
        stop_account();
    }
    
    ChannelMeta meta() const override {
        return {
            "slack",
            "Slack",
            "Slack Bot API integration (Socket Mode)",
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
        
        // Resolve tokens
        std::string bot_token = ctx.account.token;
        if (bot_token.empty()) {
            return core::Result<bool>::error("Missing bot token");
        }
        
        std::string app_token;
        if (ctx.account.credentials.count("app_token")) {
            app_token = ctx.account.credentials.at("app_token");
        }
        if (app_token.empty()) {
            return core::Result<bool>::error("Missing app token (for Socket Mode)");
        }
        
        // Create Web API client
        web_api_ = std::make_unique<SlackWebAPIClient>(bot_token);
        
        // Verify token
        auto auth_result = web_api_->authTest();
        if (!auth_result.success) {
            return core::Result<bool>::error(
                "Failed to verify bot token: " + auth_result.error_message
            );
        }
        
        auto& auth = auth_result.value.as_object();
        bot_user_id_ = auth["user_id"].as_string();
        team_name_ = auth["team"].as_string();
        ctx_.log_info("Slack bot authenticated for team: " + team_name_);
        
        // Create Socket Mode client
        socket_client_ = std::make_unique<SlackSocketModeClient>(
            app_token,
            [this](const core::JsonValue& event) {
                handle_event(event);
            }
        );
        
        // Connect to Socket Mode
        auto connect_result = socket_client_->connect();
        if (!connect_result.success) {
            return core::Result<bool>::error(connect_result.error_message);
        }
        
        connected_ = true;
        ctx_.log_info("Slack Socket Mode connected");
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> stop_account() override {
        if (socket_client_) {
            socket_client_->disconnect();
            socket_client_ = nullptr;
        }
        connected_ = false;
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) override {
        if (!web_api_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = web_api_->chatPostMessage(to, text);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) override {
        // Slack displays images from URLs automatically
        return send_text(to, "Image: " + image_url);
    }
    
    core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) override {
        if (!web_api_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = web_api_->filesUpload(to, file_path);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
    }
    
    bool supports_voice() const override { return false; }
    bool supports_video() const override { false; }
    bool supports_group_chat() const override { return true; }
    
    bool is_connected() const override {
        return connected_;
    }
    
    core::Result<std::string> get_status() const override {
        if (connected_) {
            return core::Result<std::string>::ok(
                "Connected to " + team_name_ + " (Bot ID: " + bot_user_id_ + ")"
            );
        }
        return core::Result<std::string>::ok("Disconnected");
    }

private:
    void handle_event(const core::JsonValue& event_json) {
        auto& event = event_json.as_object();
        
        if (event.contains("event")) {
            auto& inner_event = event["event"].as_object();
            std::string type = inner_event["type"].as_string();
            
            if (type == "message") {
                process_message(inner_event);
            }
        }
    }
    
    void process_message(const core::JsonValue& msg_json) {
        auto& msg = msg_json.as_object();
        
        // Ignore bot messages
        if (msg.contains("bot_id")) {
            return;
        }
        
        // Ignore message subtypes (edits, deletes, etc.)
        if (msg.contains("subtype")) {
            return;
        }
        
        // Extract message data
        ChannelMessage channel_msg;
        channel_msg.id = msg.contains("client_msg_id")
            ? msg["client_msg_id"].as_string()
            : msg["ts"].as_string();
        channel_msg.channel_id = "slack";
        channel_msg.account_id = ctx_.account.account_id;
        
        // Sender info
        if (msg.contains("user")) {
            channel_msg.from = msg["user"].as_string();
        }
        
        // Channel ID
        if (msg.contains("channel")) {
            channel_msg.to = msg["channel"].as_string();
        }
        
        // Message text
        if (msg.contains("text")) {
            channel_msg.type = MessageType::Text;
            channel_msg.text = msg["text"].as_string();
        }
        
        // Files/attachments
        if (msg.contains("files")) {
            auto& files = msg["files"].as_array();
            for (auto& file : files) {
                auto& file_obj = file.as_object();
                if (file_obj.contains("url_private")) {
                    channel_msg.attachments.push_back(
                        file_obj["url_private"].as_string()
                    );
                    
                    // Determine type
                    if (file_obj.contains("mimetype")) {
                        std::string mime = file_obj["mimetype"].as_string();
                        if (mime.find("image/") == 0) {
                            channel_msg.type = MessageType::Image;
                        } else if (mime.find("video/") == 0) {
                            channel_msg.type = MessageType::Video;
                        } else {
                            channel_msg.type = MessageType::File;
                        }
                    }
                }
            }
        }
        
        // Timestamp
        if (msg.contains("ts")) {
            // Slack timestamp is Unix timestamp with microseconds
            std::string ts_str = msg["ts"].as_string();
            double ts = std::stod(ts_str);
            channel_msg.timestamp = core::Timestamp::from_unix(
                static_cast<int64_t>(ts)
            );
        }
        
        // Deliver to handler
        if (ctx_.on_message_received) {
            ctx_.on_message_received(channel_msg);
        }
    }
    
    std::unique_ptr<SlackWebAPIClient> web_api_;
    std::unique_ptr<SlackSocketModeClient> socket_client_;
    ChannelContext ctx_;
    std::string bot_user_id_;
    std::string team_name_;
    bool connected_ = false;
};

// ============================================================================
// Slack Channel Plugin
// ============================================================================

class SlackChannelPlugin : public ChannelPlugin {
public:
    plugins::PluginMeta meta() const override {
        return {
            "slack-channel",
            "Slack Channel",
            "1.0.0",
            "Slack Bot API integration (Socket Mode)"
        };
    }
    
    std::shared_ptr<ChannelAdapter> create_adapter() override {
        return std::make_shared<SlackChannelAdapter>();
    }
};

// Plugin factory export
extern "C" {
    plugins::Plugin* create_plugin() {
        return new SlackChannelPlugin();
    }
    
    void destroy_plugin(plugins::Plugin* plugin) {
        delete plugin;
    }
}

} // namespace channels
} // namespace hunterclaw
