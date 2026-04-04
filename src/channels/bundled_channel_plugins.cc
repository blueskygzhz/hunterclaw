// ============================================================================
// Bundled Channel Plugin Registration
// ============================================================================
//
// 对齐 OpenClaw extensions/*/index.ts:
//   每个 channel 是一个独立插件，通过 api.registerChannel({ plugin }) 注册。
//
// HunterClaw 实现:
//   每个内置 channel 提供一个 PluginDefinition，其 register_func 调用
//   api.register_channel(channel_id) 并将 ChannelPlugin 注册到 ChannelPluginRegistry。

#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/plugins/plugin_system.h"

namespace hunterclaw {
namespace channels {

// Forward declarations — each channel provides make_xxx_plugin_definition()
plugins::PluginDefinition make_telegram_plugin_definition();
plugins::PluginDefinition make_slack_plugin_definition();
plugins::PluginDefinition make_discord_plugin_definition();

// ============================================================================
// register_bundled_channel_plugins — called by PluginLoader during startup
// ============================================================================
// Aligned with OpenClaw: bundled plugins are loaded from extensions/ directory
// and each calls api.registerChannel() in its register() function.

void register_bundled_channel_plugins(plugins::PluginLoader& loader, plugins::PluginRegistry& registry) {
    loader.load_plugin_definition(make_telegram_plugin_definition(), registry, "bundled:telegram");
    loader.load_plugin_definition(make_slack_plugin_definition(), registry, "bundled:slack");
    loader.load_plugin_definition(make_discord_plugin_definition(), registry, "bundled:discord");
}

} // namespace channels
} // namespace hunterclaw
