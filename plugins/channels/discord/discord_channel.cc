#include "hunterclaw/channels/channel.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <atomic>

using json = nlohmann::json;
using namespace hunterclaw;

// ============================================================================
// Discord Channel Adapter
// ============================================================================

class DiscordChannelAdapter : public channels::ChannelAdapter {
public:
    channels::ChannelMeta meta() const override {
        return {
            .id = "discord",
            .name = "Discord",
            .description = "Discord Bot API adapter with Gateway support",
            .capabilities = {
                channels::ChannelCapability::SendText,
                channels::ChannelCapability::SendImage,
                channels::ChannelCapability::SendFile,
                channels::ChannelCapability::ReceiveText,
                channels::ChannelCapability::ReceiveImage,
                channels::ChannelCapability::GroupChat,
                channels::ChannelCapability::PrivateChat,
                channels::ChannelCapability::Typing
            }
        };
    }
    
    core::Result<bool> start_account(const channels::ChannelContext& ctx) override {
        ctx_ = ctx;
        token_ = ctx.account.token;
        
        if (token_.empty()) {
            return core::Result<bool>::Err("Discord token not configured");
        }
        
        // Create HTTP client
        http_client_ = infra::HttpClient::create();
        
        ctx_.log_info("Starting Discord adapter for account: " + ctx.account.account_id);
        
        // Probe bot info
        auto bot_info = get_current_user();
        if (!bot_info.is_ok()) {
            return core::Result<bool>::Err("Failed to probe bot: " + bot_info.error);
        }
        
        ctx_.log_info("Discord bot started: " + bot_info.value);
        
        // Get Gateway URL
        auto gateway = get_gateway_bot();
        if (!gateway.is_ok()) {
            return core::Result<bool>::Err("Failed to get gateway: " + gateway.error);
        }
        
        gateway_url_ = gateway.value;
        ctx_.log_info("Gateway URL: " + gateway_url_);
        
        // TODO: Start WebSocket connection to Gateway
        // For now, use a placeholder polling loop
        is_running_ = true;
        polling_thread_ = std::thread(&DiscordChannelAdapter::gateway_loop, this);
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop_account() override {
        ctx_.log_info("Stopping Discord adapter");
        
        is_running_ = false;
        if (ctx_.abort_signal) {
            ctx_.abort_signal->store(true);
        }
        
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    // ========================================================================
    // Message Sending
    // ========================================================================
    
    core::Result<bool> send_text(const std::string& to, const std::string& text) override {
        json payload = {
            {"content", text}
        };
        
        auto response = call_api("POST", "/channels/" + to + "/messages", payload);
        if (!response.is_ok()) {
            return core::Result<bool>::Err("Failed to send message: " + response.error);
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_image(const std::string& to, const std::string& image_url) override {
        json payload = {
            {"content", image_url}  // Discord auto-embeds image URLs
        };
        
        auto response = call_api("POST", "/channels/" + to + "/messages", payload);
        if (!response.is_ok()) {
            return core::Result<bool>::Err("Failed to send image: " + response.error);
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_file(const std::string& to, const std::string& file_path) override {
        // TODO: Implement multipart/form-data upload using native HTTP client
        return core::Result<bool>::Err("File upload not yet implemented with native HTTP client");
    }
    
    bool is_connected() const override {
        return is_running_;
    }
    
    core::Result<std::string> get_status() const override {
        if (is_running_) {
            return core::Result<std::string>::Ok("running");
        } else {
            return core::Result<std::string>::Ok("stopped");
        }
    }

private:
    channels::ChannelContext ctx_;
    std::string token_;
    std::string gateway_url_;
    std::atomic<bool> is_running_{false};
    std::thread polling_thread_;
    int sequence_ = 0;
    std::string session_id_;
    infra::HttpClient::Ptr http_client_;
    
    // ========================================================================
    // Discord API Calls
    // ========================================================================
    
    core::Result<std::string> get_current_user() {
        auto response = call_api("GET", "/users/@me", json::object());
        if (!response.is_ok()) {
            return core::Result<std::string>::Err(response.error);
        }
        
        auto result = response.value;
        if (result.contains("username")) {
            std::string username = result["username"].get<std::string>();
            std::string discriminator = result.value("discriminator", "0");
            return core::Result<std::string>::Ok(username + "#" + discriminator);
        }
        
        return core::Result<std::string>::Err("Invalid user info response");
    }
    
    core::Result<std::string> get_gateway_bot() {
        auto response = call_api("GET", "/gateway/bot", json::object());
        if (!response.is_ok()) {
            return core::Result<std::string>::Err(response.error);
        }
        
        auto result = response.value;
        if (result.contains("url")) {
            return core::Result<std::string>::Ok(result["url"].get<std::string>() + "/?v=10&encoding=json");
        }
        
        return core::Result<std::string>::Err("Invalid gateway response");
    }
    
    core::Result<json> call_api(const std::string& method, const std::string& endpoint, const json& payload) {
        std::string url = "https://discord.com/api/v10" + endpoint;
        
        // Prepare request options
        infra::HttpRequestOptions options;
        options.timeout_ms = 10000;  // 10 seconds
        options.headers["Content-Type"] = "application/json";
        options.headers["Authorization"] = "Bot " + token_;
        
        if (method == "POST" || method == "PUT" || method == "PATCH") {
            options.body = payload.dump();
        }
        
        // Make request
        core::Result<infra::HttpResponse> http_response;
        if (method == "GET") {
            http_response = http_client_->get(url, options);
        } else if (method == "POST") {
            http_response = http_client_->post(url, options);
        } else if (method == "PUT") {
            http_response = http_client_->put(url, options);
        } else if (method == "PATCH") {
            http_response = http_client_->patch(url, options);
        } else if (method == "DELETE") {
            http_response = http_client_->del(url, options);
        } else {
            return core::Result<json>::Err("Unsupported HTTP method: " + method);
        }
        
        if (!http_response.is_ok()) {
            return core::Result<json>::Err("HTTP request failed: " + http_response.error);
        }
        
        if (http_response.value.status_code >= 400) {
            return core::Result<json>::Err("HTTP " + std::to_string(http_response.value.status_code) + ": " + http_response.value.body);
        }
        
        try {
            if (http_response.value.body.empty()) {
                return core::Result<json>::Ok(json::object());
            }
            json response_json = json::parse(http_response.value.body);
            return core::Result<json>::Ok(response_json);
        } catch (const std::exception& e) {
            return core::Result<json>::Err(std::string("JSON parse error: ") + e.what());
        }
    }
    
    // ========================================================================
    // Gateway Loop (WebSocket connection - placeholder for now)
    // ========================================================================
    
    void gateway_loop() {
        ctx_.log_info("Discord gateway loop started (placeholder)");
        ctx_.log_warn("WebSocket support not yet implemented - messages will not be received");
        
        // TODO: Implement WebSocket connection to Discord Gateway
        // 1. Connect to gateway_url_
        // 2. Send IDENTIFY payload with token
        // 3. Receive HELLO and start heartbeat
        // 4. Listen for MESSAGE_CREATE events
        // 5. Handle reconnection and resume
        
        while (is_running_ && (!ctx_.abort_signal || !ctx_.abort_signal->load())) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        
        ctx_.log_info("Discord gateway loop stopped");
    }
    
    void process_gateway_event(const json& event) {
        // Parse Discord Gateway events
        if (!event.contains("t") || !event.contains("d")) {
            return;
        }
        
        std::string event_name = event["t"].get<std::string>();
        const auto& data = event["d"];
        
        if (event_name == "MESSAGE_CREATE") {
            process_message(data);
        } else if (event_name == "READY") {
            session_id_ = data["session_id"].get<std::string>();
            ctx_.log_info("Discord bot ready with session: " + session_id_);
        }
        
        // Update sequence
        if (event.contains("s") && !event["s"].is_null()) {
            sequence_ = event["s"].get<int>();
        }
    }
    
    void process_message(const json& msg) {
        // Ignore bot messages
        if (msg.contains("author") && msg["author"].contains("bot") && msg["author"]["bot"].get<bool>()) {
            return;
        }
        
        channels::ChannelMessage channel_msg{
            .id = msg["id"].get<std::string>(),
            .channel_id = "discord",
            .account_id = ctx_.account.account_id,
            .from = msg["author"]["id"].get<std::string>(),
            .to = msg["channel_id"].get<std::string>(),
            .type = channels::MessageType::Text,
            .text = msg.value("content", ""),
            .timestamp = std::chrono::system_clock::now()
        };
        
        // Deliver to core
        if (ctx_.on_message_received) {
            ctx_.on_message_received(channel_msg);
        }
    }
};

// ============================================================================
// Discord Channel Plugin
// ============================================================================

class DiscordChannelPlugin : public channels::ChannelPlugin {
public:
    std::string id() const override { return "discord-channel"; }
    std::string name() const override { return "Discord Channel Adapter"; }
    std::string version() const override { return "0.1.0"; }
    
    std::shared_ptr<channels::ChannelAdapter> create_adapter() override {
        return std::make_shared<DiscordChannelAdapter>();
    }
};

// ============================================================================
// Plugin Registration
// ============================================================================

HUNTERCLAW_REGISTER_PLUGIN(DiscordChannelPlugin)

// Auto-register Discord adapter to registry
#include "hunterclaw/channels/registry.h"
HUNTERCLAW_REGISTER_CHANNEL_ADAPTER(DiscordChannelAdapter, "discord")
