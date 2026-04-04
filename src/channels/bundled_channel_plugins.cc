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
//
// 这个文件在 PluginLoader::load_plugins() 期间被调用，将所有内置 channel
// 作为 bundled 插件加载到 PluginRegistry 中。

#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/plugins/plugin_system.h"

namespace hunterclaw {
namespace channels {

// Forward declarations — each channel provides make_xxx_plugin()
// (defined in their respective xxx_channel.cc files)
namespace {
    // These are defined in the anonymous namespaces of each channel file,
    // so we re-declare them here as extern linkage helpers.
}

// ============================================================================
// Bundled Channel PluginDefinition factories
// ============================================================================

// Aligned with OpenClaw extensions/telegram/index.ts:
//   const plugin = { id: "telegram", register(api) { api.registerChannel({ plugin: telegramPlugin }) } }
PluginDefinition make_telegram_plugin_definition();
PluginDefinition make_slack_plugin_definition();
PluginDefinition make_discord_plugin_definition();

// ============================================================================
// register_bundled_channel_plugins — called by PluginLoader during startup
// ============================================================================
// Aligned with OpenClaw: bundled plugins are loaded from extensions/ directory
// and each calls api.registerChannel() in its register() function.

void register_bundled_channel_plugins(PluginLoader& loader, PluginRegistry& registry) {
    // Load each bundled channel as a PluginDefinition
    // (对齐 OpenClaw: each extension is a separate plugin entry)
    loader.load_plugin_definition(make_telegram_plugin_definition(), registry, "bundled:telegram");
    loader.load_plugin_definition(make_slack_plugin_definition(), registry, "bundled:slack");
    loader.load_plugin_definition(make_discord_plugin_definition(), registry, "bundled:discord");
}

} // namespace channels
} // namespace hunterclaw
