#include "hunterclaw/channels/channel.h"
#include <memory>

namespace hunterclaw {
namespace channels {

// Stub implementation of Discord channel adapter
class DiscordChannelAdapter : public ChannelAdapter {
public:
    DiscordChannelAdapter() = default;
    ~DiscordChannelAdapter() override = default;

    ChannelMeta meta() const override {
        ChannelMeta m;
        m.id = "discord";
        m.name = "Discord";
        m.description = "Discord channel adapter (stub)";
        m.capabilities = {ChannelCapability::SendText, ChannelCapability::ReceiveText};
        return m;
    }

    core::Result<bool> start_account(const ChannelContext& ctx) override {
        context_ = ctx;
        connected_ = true;
        return core::Result<bool>::Ok(true);
    }

    core::Result<bool> stop_account() override {
        connected_ = false;
        return core::Result<bool>::Ok(true);
    }

    core::Result<bool> send_text(
        const std::string& to,
        const std::string& text
    ) override {
        if (!connected_) {
            return core::Result<bool>::Err("Not connected");
        }
        // Stub: pretend to send
        return core::Result<bool>::Ok(true);
    }

    core::Result<bool> send_image(
        const std::string& to,
        const std::string& image_url
    ) override {
        return core::Result<bool>::Err("Not implemented");
    }

    core::Result<bool> send_file(
        const std::string& to,
        const std::string& file_path
    ) override {
        return core::Result<bool>::Err("Not implemented");
    }

    bool is_connected() const override {
        return connected_;
    }

    core::Result<std::string> get_status() const override {
        return core::Result<std::string>::Ok(connected_ ? "Connected" : "Disconnected");
    }

private:
    bool connected_ = false;
    ChannelContext context_;
};

// Plugin implementation
class DiscordChannelPlugin : public ChannelPlugin {
public:
    std::shared_ptr<ChannelAdapter> create_adapter() override {
        return std::make_shared<DiscordChannelAdapter>();
    }

    // Required Plugin interface methods
    std::string id() const override {
        return "discord-channel";
    }

    std::string name() const override {
        return "Discord Channel";
    }

    std::string version() const override {
        return "1.0.0";
    }
};

// Plugin factory
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
