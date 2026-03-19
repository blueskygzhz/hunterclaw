#include "hunterclaw/channels/channel.h"
#include "hunterclaw/plugins/plugin.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

using json = nlohmann::json;
using namespace hunterclaw;

// ============================================================================
// Telegram Channel Adapter
// ============================================================================

class TelegramChannelAdapter : public channels::ChannelAdapter {
public:
    channels::ChannelAdapterMeta meta() const override {
        return {
            .id = "telegram",
            .name = "Telegram",
            .description = "Telegram Bot API adapter",
            .capabilities = {
                channels::ChannelCapability::SendText,
                channels::ChannelCapability::SendImage,
                channels::ChannelCapability::SendFile,
                channels::ChannelCapability::ReceiveText,
                channels::ChannelCapability::ReceiveImage,
                channels::ChannelCapability::GroupChat,
                channels::ChannelCapability::PrivateChat
            }
        };
    }
    
    core::Result<bool> start_account(const channels::ChannelAdapterContext& ctx) override {
        ctx_ = ctx;
        token_ = ctx.account.token;
        
        if (token_.empty()) {
            return core::Result<bool>::Err("Telegram token not configured");
        }
        
        // Create HTTP client
        http_client_ = infra::HttpClient::create();
        
        ctx_.log_info("Starting Telegram adapter for account: " + ctx.account.account_id);
        
        // Probe bot info
        auto bot_info = get_me();
        if (!bot_info.is_ok()) {
            return core::Result<bool>::Err("Failed to probe bot: " + bot_info.error);
        }
        
        ctx_.log_info("Telegram bot started: " + bot_info.value);
        
        // Start polling loop
        is_running_ = true;
        polling_thread_ = std::thread(&TelegramChannelAdapter::polling_loop, this);
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop_account() override {
        ctx_.log_info("Stopping Telegram adapter");
        
        is_running_ = false;
        if (ctx_.abort_signal) {
            ctx_.abort_signal->store(true);
        }
        
        if (polling_thread_.joinable()) {
            polling_thread_.join();
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_text(const std::string& to, const std::string& text) override {
        json payload = {
            {"chat_id", to},
            {"text", text},
            {"parse_mode", "Markdown"}
        };
        
        auto response = call_api("sendMessage", payload);
        if (!response.is_ok()) {
            return core::Result<bool>::Err("Failed to send message: " + response.error);
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_image(const std::string& to, const std::string& image_url) override {
        json payload = {
            {"chat_id", to},
            {"photo", image_url}
        };
        
        auto response = call_api("sendPhoto", payload);
        if (!response.is_ok()) {
            return core::Result<bool>::Err("Failed to send image: " + response.error);
        }
        
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_file(const std::string& to, const std::string& file_path) override {
        // TODO: Implement multipart/form-data upload using native HTTP client
        // For now, use sendDocument with file_id if available
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
    channels::ChannelAdapterContext ctx_;
    std::string token_;
    std::atomic<bool> is_running_{false};
    std::thread polling_thread_;
    int64_t last_update_id_ = 0;
    infra::HttpClient::Ptr http_client_;
    
    // ========================================================================
    // Telegram API Calls
    // ========================================================================
    
    core::Result<std::string> get_me() {
        auto response = call_api("getMe", json::object());
        if (!response.is_ok()) {
            return core::Result<std::string>::Err(response.error);
        }
        
        auto result = response.value;
        if (result.contains("username")) {
            return core::Result<std::string>::Ok(result["username"].get<std::string>());
        }
        
        return core::Result<std::string>::Err("Invalid bot info response");
    }
    
    core::Result<json> call_api(const std::string& method, const json& payload) {
        std::string url = "https://api.telegram.org/bot" + token_ + "/" + method;
        
        // Prepare request options
        infra::HttpRequestOptions options;
        options.timeout_ms = 10000;  // 10 seconds
        options.headers["Content-Type"] = "application/json";
        options.body = payload.dump();
        
        // Make request
        auto http_response = http_client_->post(url, options);
        if (!http_response.is_ok()) {
            return core::Result<json>::Err("HTTP request failed: " + http_response.error);
        }
        
        if (!http_response.value.ok) {
            return core::Result<json>::Err("HTTP " + std::to_string(http_response.value.status_code));
        }
        
        try {
            json response_json = json::parse(http_response.value.body);
            if (!response_json["ok"].get<bool>()) {
                return core::Result<json>::Err(response_json["description"].get<std::string>());
            }
            return core::Result<json>::Ok(response_json["result"]);
        } catch (const std::exception& e) {
            return core::Result<json>::Err(std::string("JSON parse error: ") + e.what());
        }
    }
    
    // ========================================================================
    // Polling Loop (long-polling for updates)
    // ========================================================================
    
    void polling_loop() {
        ctx_.log_info("Telegram polling loop started");
        
        while (is_running_ && (!ctx_.abort_signal || !ctx_.abort_signal->load())) {
            auto updates = get_updates();
            if (updates.is_ok()) {
                for (const auto& update : updates.value) {
                    process_update(update);
                }
            } else {
                ctx_.log_error("Failed to get updates: " + updates.error);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        
        ctx_.log_info("Telegram polling loop stopped");
    }
    
    core::Result<std::vector<json>> get_updates() {
        json payload = {
            {"offset", last_update_id_ + 1},
            {"timeout", 30},
            {"allowed_updates", json::array({"message", "edited_message"})}
        };
        
        auto response = call_api("getUpdates", payload);
        if (!response.is_ok()) {
            return core::Result<std::vector<json>>::Err(response.error);
        }
        
        std::vector<json> updates;
        for (const auto& update : response.value) {
            updates.push_back(update);
            if (update.contains("update_id")) {
                last_update_id_ = std::max(
                    last_update_id_,
                    update["update_id"].get<int64_t>()
                );
            }
        }
        
        return core::Result<std::vector<json>>::Ok(updates);
    }
    
    void process_update(const json& update) {
        if (!update.contains("message")) {
            return;
        }
        
        const auto& msg = update["message"];
        
        channels::ChannelMessage channel_msg{
            .id = std::to_string(msg["message_id"].get<int64_t>()),
            .channel_id = "telegram",
            .account_id = ctx_.account.account_id,
            .from = std::to_string(msg["from"]["id"].get<int64_t>()),
            .to = std::to_string(msg["chat"]["id"].get<int64_t>()),
            .type = channels::MessageType::Text,
            .text = msg.contains("text") ? msg["text"].get<std::string>() : "",
            .timestamp = std::chrono::system_clock::now()
        };
        
        // Deliver to core
        if (ctx_.on_message_received) {
            ctx_.on_message_received(channel_msg);
        }
    }
};

// ============================================================================
// Telegram Channel Plugin
// ============================================================================

class TelegramChannelPlugin : public channels::ChannelPluginBase {
public:
    std::string id() const override { return "telegram-channel"; }
    std::string name() const override { return "Telegram Channel Adapter"; }
    std::string version() const override { return "0.1.0"; }
    
    std::shared_ptr<channels::ChannelAdapter> create_adapter() override {
        return std::make_shared<TelegramChannelAdapter>();
    }
};

// ============================================================================
// Plugin Registration
// ============================================================================

HUNTERCLAW_REGISTER_PLUGIN(TelegramChannelPlugin)

// Auto-register Telegram adapter to registry
#include "hunterclaw/channels/registry.h"
HUNTERCLAW_REGISTER_CHANNEL_ADAPTER(TelegramChannelAdapter, "telegram")
