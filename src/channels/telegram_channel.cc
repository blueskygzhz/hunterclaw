#include "hunterclaw/channels/channel.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/core/json.h"
#include <thread>
#include <chrono>
#include <sstream>

namespace hunterclaw {
namespace channels {

// ============================================================================
// Telegram Bot API Client
// ============================================================================

class TelegramBotAPI {
public:
    explicit TelegramBotAPI(const std::string& token)
        : token_(token), base_url_("https://api.telegram.org/bot" + token) {}
    
    // Get bot info
    core::Result<core::JsonValue> getMe() {
        return call_api("getMe", {});
    }
    
    // Send text message
    core::Result<core::JsonValue> sendMessage(
        const std::string& chat_id,
        const std::string& text,
        const std::map<std::string, std::string>& opts = {}
    ) {
        std::map<std::string, std::string> params = {
            {"chat_id", chat_id},
            {"text", text}
        };
        
        // Merge optional parameters
        for (const auto& [key, value] : opts) {
            params[key] = value;
        }
        
        return call_api("sendMessage", params);
    }
    
    // Send photo
    core::Result<core::JsonValue> sendPhoto(
        const std::string& chat_id,
        const std::string& photo_url,
        const std::string& caption = ""
    ) {
        std::map<std::string, std::string> params = {
            {"chat_id", chat_id},
            {"photo", photo_url}
        };
        if (!caption.empty()) {
            params["caption"] = caption;
        }
        return call_api("sendPhoto", params);
    }
    
    // Send document
    core::Result<core::JsonValue> sendDocument(
        const std::string& chat_id,
        const std::string& document_url,
        const std::string& caption = ""
    ) {
        std::map<std::string, std::string> params = {
            {"chat_id", chat_id},
            {"document", document_url}
        };
        if (!caption.empty()) {
            params["caption"] = caption;
        }
        return call_api("sendDocument", params);
    }
    
    // Get updates (polling)
    core::Result<core::JsonValue> getUpdates(
        int64_t offset = 0,
        int timeout = 30
    ) {
        std::map<std::string, std::string> params = {
            {"offset", std::to_string(offset)},
            {"timeout", std::to_string(timeout)},
            {"allowed_updates", "[\"message\",\"edited_message\",\"callback_query\"]"}
        };
        return call_api("getUpdates", params);
    }
    
    // Set webhook
    core::Result<core::JsonValue> setWebhook(
        const std::string& url,
        const std::string& secret_token = ""
    ) {
        std::map<std::string, std::string> params = {
            {"url", url}
        };
        if (!secret_token.empty()) {
            params["secret_token"] = secret_token;
        }
        return call_api("setWebhook", params);
    }
    
    // Delete webhook (for polling mode)
    core::Result<core::JsonValue> deleteWebhook() {
        return call_api("deleteWebhook", {});
    }

private:
    core::Result<core::JsonValue> call_api(
        const std::string& method,
        const std::map<std::string, std::string>& params
    ) {
        // Build URL with query parameters
        std::ostringstream url;
        url << base_url_ << "/" << method;
        
        if (!params.empty()) {
            url << "?";
            bool first = true;
            for (const auto& [key, value] : params) {
                if (!first) url << "&";
                url << core::url_encode(key) << "=" << core::url_encode(value);
                first = false;
            }
        }
        
        // Call HTTP client
        core::HttpClient client;
        auto response = client.get(url.str());
        
        if (!response.success) {
            return core::Result<core::JsonValue>::error(
                "HTTP request failed: " + response.error_message
            );
        }
        
        // Parse JSON response
        auto json = core::JsonParser::parse(response.body);
        if (!json.success) {
            return core::Result<core::JsonValue>::error(
                "JSON parse failed: " + json.error_message
            );
        }
        
        // Check Telegram API result
        auto& obj = json.value.as_object();
        if (obj["ok"].as_bool() == false) {
            std::string error_desc = "Unknown error";
            if (obj.contains("description")) {
                error_desc = obj["description"].as_string();
            }
            return core::Result<core::JsonValue>::error(
                "Telegram API error: " + error_desc
            );
        }
        
        return core::Result<core::JsonValue>::ok(obj["result"]);
    }
    
    std::string token_;
    std::string base_url_;
};

// ============================================================================
// Telegram Channel Adapter
// ============================================================================

class TelegramChannelAdapter : public ChannelAdapter {
public:
    TelegramChannelAdapter()
        : api_(nullptr), running_(false), polling_thread_(nullptr) {}
    
    ~TelegramChannelAdapter() override {
        stop_account();
    }
    
    ChannelMeta meta() const override {
        return {
            "telegram",
            "Telegram",
            "Telegram Bot API integration",
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
        
        // Create API client
        api_ = std::make_unique<TelegramBotAPI>(token);
        
        // Verify token by calling getMe
        auto me_result = api_->getMe();
        if (!me_result.success) {
            return core::Result<bool>::error(
                "Failed to verify bot token: " + me_result.error_message
            );
        }
        
        bot_username_ = me_result.value["username"].as_string();
        ctx_.log_info("Telegram bot @" + bot_username_ + " authenticated");
        
        // Check if webhook mode is enabled
        auto config_webhook = ctx.account.config.find("webhook_url");
        if (config_webhook != ctx.account.config.end()) {
            // Webhook mode
            std::string webhook_url = config_webhook->second.as_string();
            std::string secret_token = ctx.account.config.count("webhook_secret")
                ? ctx.account.config.at("webhook_secret").as_string()
                : "";
            
            auto webhook_result = api_->setWebhook(webhook_url, secret_token);
            if (!webhook_result.success) {
                return core::Result<bool>::error(
                    "Failed to set webhook: " + webhook_result.error_message
                );
            }
            
            ctx_.log_info("Telegram webhook set to: " + webhook_url);
        } else {
            // Polling mode (default)
            auto delete_result = api_->deleteWebhook();
            if (!delete_result.success) {
                ctx_.log_error("Warning: Failed to delete webhook");
            }
            
            // Start polling thread
            start_polling();
            ctx_.log_info("Telegram polling started");
        }
        
        connected_ = true;
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> stop_account() override {
        if (running_) {
            running_ = false;
            if (polling_thread_ && polling_thread_->joinable()) {
                polling_thread_->join();
            }
            polling_thread_ = nullptr;
        }
        connected_ = false;
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) override {
        if (!api_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = api_->sendMessage(to, text);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) override {
        if (!api_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = api_->sendPhoto(to, image_url);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
    }
    
    core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) override {
        if (!api_) {
            return core::Result<bool>::error("API not initialized");
        }
        
        auto result = api_->sendDocument(to, file_path);
        if (!result.success) {
            return core::Result<bool>::error(result.error_message);
        }
        
        return core::Result<bool>::ok(true);
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
                "Connected as @" + bot_username_ + 
                (running_ ? " (polling)" : " (webhook)")
            );
        }
        return core::Result<std::string>::ok("Disconnected");
    }

private:
    void start_polling() {
        running_ = true;
        last_update_id_ = 0;
        
        polling_thread_ = std::make_unique<std::thread>([this]() {
            polling_loop();
        });
    }
    
    void polling_loop() {
        while (running_ && (!ctx_.abort_signal || !ctx_.abort_signal->load())) {
            try {
                // Get updates with long polling
                auto updates = api_->getUpdates(last_update_id_ + 1, 30);
                
                if (!updates.success) {
                    ctx_.log_error("Failed to get updates: " + updates.error_message);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    continue;
                }
                
                // Process each update
                auto& updates_array = updates.value.as_array();
                for (auto& update_value : updates_array) {
                    auto& update = update_value.as_object();
                    
                    // Update offset
                    last_update_id_ = update["update_id"].as_int64();
                    
                    // Process message
                    if (update.contains("message")) {
                        process_message(update["message"]);
                    }
                    
                    // Process edited message
                    if (update.contains("edited_message")) {
                        process_message(update["edited_message"]);
                    }
                    
                    // Process callback query
                    if (update.contains("callback_query")) {
                        process_callback_query(update["callback_query"]);
                    }
                }
                
            } catch (const std::exception& e) {
                ctx_.log_error(std::string("Polling error: ") + e.what());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    }
    
    void process_message(const core::JsonValue& msg_json) {
        auto& msg = msg_json.as_object();
        
        // Extract message data
        ChannelMessage channel_msg;
        channel_msg.id = std::to_string(msg["message_id"].as_int64());
        channel_msg.channel_id = "telegram";
        channel_msg.account_id = ctx_.account.account_id;
        
        // Sender info
        if (msg.contains("from")) {
            auto& from = msg["from"].as_object();
            channel_msg.from = std::to_string(from["id"].as_int64());
            if (from.contains("username")) {
                channel_msg.sender_name = from["username"].as_string();
            } else if (from.contains("first_name")) {
                channel_msg.sender_name = from["first_name"].as_string();
            }
        }
        
        // Chat info
        if (msg.contains("chat")) {
            auto& chat = msg["chat"].as_object();
            channel_msg.to = std::to_string(chat["id"].as_int64());
        }
        
        // Message content
        if (msg.contains("text")) {
            channel_msg.type = MessageType::Text;
            channel_msg.text = msg["text"].as_string();
        } else if (msg.contains("photo")) {
            channel_msg.type = MessageType::Image;
            // Get largest photo
            auto& photos = msg["photo"].as_array();
            if (!photos.empty()) {
                auto& largest = photos.back().as_object();
                channel_msg.attachments.push_back(largest["file_id"].as_string());
            }
            if (msg.contains("caption")) {
                channel_msg.text = msg["caption"].as_string();
            }
        } else if (msg.contains("document")) {
            channel_msg.type = MessageType::File;
            auto& doc = msg["document"].as_object();
            channel_msg.attachments.push_back(doc["file_id"].as_string());
            if (msg.contains("caption")) {
                channel_msg.text = msg["caption"].as_string();
            }
        } else if (msg.contains("voice")) {
            channel_msg.type = MessageType::Voice;
            auto& voice = msg["voice"].as_object();
            channel_msg.attachments.push_back(voice["file_id"].as_string());
        } else if (msg.contains("video")) {
            channel_msg.type = MessageType::Video;
            auto& video = msg["video"].as_object();
            channel_msg.attachments.push_back(video["file_id"].as_string());
        }
        
        // Timestamp
        if (msg.contains("date")) {
            channel_msg.timestamp = core::Timestamp::from_unix(
                msg["date"].as_int64()
            );
        }
        
        // Deliver to handler
        if (ctx_.on_message_received) {
            ctx_.on_message_received(channel_msg);
        }
    }
    
    void process_callback_query(const core::JsonValue& query_json) {
        // TODO: Handle button callbacks
        ctx_.log_info("Received callback query (not implemented)");
    }
    
    std::unique_ptr<TelegramBotAPI> api_;
    ChannelContext ctx_;
    std::string bot_username_;
    bool connected_ = false;
    
    // Polling state
    bool running_;
    int64_t last_update_id_;
    std::unique_ptr<std::thread> polling_thread_;
};

// ============================================================================
// Telegram Channel Plugin
// ============================================================================

class TelegramChannelPlugin : public ChannelPlugin {
public:
    plugins::PluginMeta meta() const override {
        return {
            "telegram-channel",
            "Telegram Channel",
            "1.0.0",
            "Telegram Bot API integration"
        };
    }
    
    std::shared_ptr<ChannelAdapter> create_adapter() override {
        return std::make_shared<TelegramChannelAdapter>();
    }
};

// Plugin factory export
extern "C" {
    plugins::Plugin* create_plugin() {
        return new TelegramChannelPlugin();
    }
    
    void destroy_plugin(plugins::Plugin* plugin) {
        delete plugin;
    }
}

} // namespace channels
} // namespace hunterclaw
