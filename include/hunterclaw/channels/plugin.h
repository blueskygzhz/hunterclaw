#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/plugins/plugin.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <map>
#include <mutex>
#include <atomic>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace channels {

// Channel identifier type (aligned with manager.h)
using ChannelId = std::string;

// Forward declarations
struct ChannelAccountSnapshot;
struct ChannelContext;
struct ChannelOutboundContext;
struct ChannelOutboundPayloadContext;
struct OutboundDeliveryResult;

// ============================================================================
// ChannelMeta (100% 对齐 OpenClaw)
// ============================================================================

struct ChannelMeta {
    ChannelId id;
    std::string label;                    // "Telegram"
    std::string selection_label;          // "Telegram (Bot API)"
    std::string detail_label;             // "Telegram Bot"
    std::string docs_path;                // "/channels/telegram"
    std::string docs_label;
    std::string blurb;                    // 简短描述
    std::string system_image;             // 系统图标名
    std::optional<int> order;
    std::vector<std::string> aliases;
    std::optional<std::string> selection_docs_prefix;
    bool selection_docs_omit_label = false;
    std::vector<std::string> selection_extras;
    bool show_configured = false;
    bool quickstart_allow_from = false;
    bool force_account_binding = false;
    bool prefer_session_lookup_for_announce_target = false;
    std::vector<std::string> prefer_over;
};

// ============================================================================
// ChannelCapabilities (100% 对齐 OpenClaw)
// ============================================================================

struct ChannelCapabilities {
    // chatTypes: "direct" | "group" | "channel" | "thread"
    std::vector<std::string> chat_types;
    bool polls = false;
    bool reactions = false;
    bool edit = false;
    bool unsend = false;
    bool reply = false;
    bool effects = false;
    bool group_management = false;
    bool threads = false;
    bool media = false;
    bool native_commands = false;
    bool block_streaming = false;
};

// ============================================================================
// ChannelAccountSnapshot (100% 对齐 OpenClaw types.core.ts)
// ============================================================================

struct ChannelAccountSnapshot {
    std::string account_id;
    std::optional<std::string> name;
    std::optional<bool> enabled;
    std::optional<bool> configured;
    std::optional<bool> linked;
    std::optional<bool> running;
    std::optional<bool> connected;
    std::optional<bool> restart_pending;
    std::optional<int> reconnect_attempts;
    std::optional<int64_t> last_connected_at;
    // lastDisconnect: string | { at, status, error, loggedOut } | null
    std::optional<nlohmann::json> last_disconnect;
    std::optional<int64_t> last_message_at;
    std::optional<int64_t> last_event_at;
    std::optional<std::string> last_error;
    std::optional<int64_t> last_start_at;
    std::optional<int64_t> last_stop_at;
    std::optional<int64_t> last_inbound_at;
    std::optional<int64_t> last_outbound_at;
    std::optional<bool> busy;
    std::optional<int> active_runs;
    std::optional<int64_t> last_run_activity_at;
    std::optional<std::string> mode;
    std::optional<std::string> dm_policy;
    std::optional<std::vector<std::string>> allow_from;
    std::optional<std::string> token_source;
    std::optional<std::string> bot_token_source;
    std::optional<std::string> app_token_source;
    std::optional<std::string> signing_secret_source;
    std::optional<std::string> token_status;       // "available" | "configured_unavailable" | "missing"
    std::optional<std::string> bot_token_status;
    std::optional<std::string> app_token_status;
    std::optional<std::string> signing_secret_status;
    std::optional<std::string> user_token_status;
    std::optional<std::string> credential_source;
    std::optional<std::string> secret_source;
    std::optional<std::string> audience_type;
    std::optional<std::string> audience;
    std::optional<std::string> webhook_path;
    std::optional<std::string> webhook_url;
    std::optional<std::string> base_url;
    std::optional<bool> allow_unmentioned_groups;
    std::optional<std::string> cli_path;
    std::optional<std::string> db_path;
    std::optional<int> port;
    std::optional<nlohmann::json> probe;
    std::optional<int64_t> last_probe_at;
    std::optional<nlohmann::json> audit;
    std::optional<nlohmann::json> application;
    std::optional<nlohmann::json> bot;
    std::optional<std::string> public_key;
    std::optional<nlohmann::json> profile;
    std::optional<std::string> channel_access_token;
    std::optional<std::string> channel_secret;

    nlohmann::json to_json() const;
    static ChannelAccountSnapshot from_json(const nlohmann::json& j);
};

// ============================================================================
// ChannelLogSink (100% 对齐 OpenClaw)
// ============================================================================

struct ChannelLogSink {
    std::function<void(const std::string&)> info;
    std::function<void(const std::string&)> warn;
    std::function<void(const std::string&)> error;
    std::function<void(const std::string&)> debug;
};

// ============================================================================
// ChannelGroupContext (100% 对齐 OpenClaw)
// ============================================================================

struct ChannelGroupContext {
    core::Config cfg;
    std::optional<std::string> group_id;
    std::optional<std::string> group_channel;
    std::optional<std::string> group_space;
    std::optional<std::string> account_id;
    std::optional<std::string> sender_id;
    std::optional<std::string> sender_name;
    std::optional<std::string> sender_username;
    std::optional<std::string> sender_e164;
};

// ============================================================================
// ChannelThreadingContext / ChannelThreadingToolContext (100% 对齐 OpenClaw)
// ============================================================================

struct ChannelThreadingContext {
    std::optional<std::string> channel;
    std::optional<std::string> from;
    std::optional<std::string> to;
    std::optional<std::string> chat_type;
    std::optional<std::string> current_message_id;
    std::optional<std::string> reply_to_id;
    std::optional<std::string> reply_to_id_full;
    std::optional<std::string> thread_label;
    std::optional<std::string> message_thread_id;
    std::optional<std::string> native_channel_id;
};

struct ChannelThreadingToolContext {
    std::optional<std::string> current_channel_id;
    std::optional<std::string> current_channel_provider;
    std::optional<std::string> current_thread_ts;
    std::optional<std::string> current_message_id;
    std::optional<std::string> reply_to_mode;  // "off" | "first" | "all"
    std::optional<bool> skip_cross_context_decoration;
};

// ============================================================================
// Adapter Types (100% 对齐 OpenClaw types.adapters.ts)
// ============================================================================

// ChannelConfigAdapter
struct ChannelConfigAdapter {
    std::function<std::vector<std::string>(const core::Config&)> list_account_ids;
    std::function<nlohmann::json(const core::Config&, const std::string&)> resolve_account;
    std::function<nlohmann::json(const core::Config&, const std::string&)> inspect_account;
    std::function<std::string(const core::Config&)> default_account_id;
    std::function<core::Config(const core::Config&, const std::string&, bool)> set_account_enabled;
    std::function<core::Config(const core::Config&, const std::string&)> delete_account;
    std::function<bool(const nlohmann::json&, const core::Config&)> is_enabled;
    std::function<std::string(const nlohmann::json&, const core::Config&)> disabled_reason;
    std::function<bool(const nlohmann::json&, const core::Config&)> is_configured;
    std::function<std::string(const nlohmann::json&, const core::Config&)> unconfigured_reason;
    std::function<ChannelAccountSnapshot(const nlohmann::json&, const core::Config&)> describe_account;
    std::function<std::vector<std::string>(const core::Config&, const std::string&)> resolve_allow_from;
    std::function<std::vector<std::string>(const core::Config&, const std::string&, const std::vector<std::string>&)> format_allow_from;
    std::function<std::optional<std::string>(const core::Config&, const std::string&)> resolve_default_to;
};

// ChannelSetupInput (100% 对齐 OpenClaw)
struct ChannelSetupInput {
    std::optional<std::string> name;
    std::optional<std::string> token;
    std::optional<std::string> token_file;
    std::optional<std::string> bot_token;
    std::optional<std::string> app_token;
    std::optional<std::string> signal_number;
    std::optional<std::string> cli_path;
    std::optional<std::string> db_path;
    std::optional<std::string> service;  // "imessage" | "sms" | "auto"
    std::optional<std::string> region;
    std::optional<std::string> auth_dir;
    std::optional<std::string> http_url;
    std::optional<std::string> http_host;
    std::optional<std::string> http_port;
    std::optional<std::string> webhook_path;
    std::optional<std::string> webhook_url;
    std::optional<std::string> audience_type;
    std::optional<std::string> audience;
    std::optional<bool> use_env;
    std::optional<std::string> homeserver;
    std::optional<std::string> user_id;
    std::optional<std::string> access_token;
    std::optional<std::string> password;
    std::optional<std::string> device_name;
    std::optional<int> initial_sync_limit;
    std::optional<std::string> ship;
    std::optional<std::string> url;
    std::optional<std::string> code;
    std::vector<std::string> group_channels;
    std::vector<std::string> dm_allowlist;
    std::optional<bool> auto_discover_channels;
};

// ChannelSetupAdapter
struct ChannelSetupAdapter {
    std::function<std::string(const core::Config&, const std::string&, const ChannelSetupInput&)> resolve_account_id;
    std::function<std::optional<std::string>(const core::Config&, const std::string&, const std::string&)> resolve_binding_account_id;
    std::function<core::Config(const core::Config&, const std::string&, const std::string&)> apply_account_name;
    std::function<core::Config(const core::Config&, const std::string&, const ChannelSetupInput&)> apply_account_config;
    std::function<std::optional<std::string>(const core::Config&, const std::string&, const ChannelSetupInput&)> validate_input;
};

// ChannelGroupAdapter
struct ChannelGroupAdapter {
    std::function<std::optional<bool>(const ChannelGroupContext&)> resolve_require_mention;
    std::function<std::optional<std::string>(const ChannelGroupContext&)> resolve_group_intro_hint;
    std::function<std::optional<nlohmann::json>(const ChannelGroupContext&)> resolve_tool_policy;
};

// ChannelMentionAdapter
struct ChannelMentionAdapter {
    std::function<std::vector<std::string>(const nlohmann::json&, const core::Config&, const std::string&)> strip_patterns;
    std::function<std::string(const std::string&, const nlohmann::json&, const core::Config&, const std::string&)> strip_mentions;
};

// ChannelStreamingAdapter
struct ChannelStreamingAdapter {
    struct CoalesceDefaults {
        int min_chars = 1500;
        int idle_ms = 1000;
    };
    std::optional<CoalesceDefaults> block_streaming_coalesce_defaults;
};

// ChannelThreadingAdapter
struct ChannelThreadingAdapter {
    // "off" | "first" | "all"
    std::function<std::string(const core::Config&, const std::string&, const std::string&)> resolve_reply_to_mode;
    std::optional<bool> allow_explicit_reply_tags_when_off;
    std::optional<bool> allow_tags_when_off;  // deprecated alias
    std::function<std::optional<ChannelThreadingToolContext>(
        const core::Config&, const std::string&, const ChannelThreadingContext&)> build_tool_context;
};

// ChannelElevatedAdapter
struct ChannelElevatedAdapter {
    std::function<std::optional<std::vector<std::string>>(const core::Config&, const std::string&)> allow_from_fallback;
};

// ChannelCommandAdapter
struct ChannelCommandAdapter {
    bool enforce_owner_for_commands = false;
    bool skip_when_config_empty = false;
};

// ChannelPairingAdapter
struct ChannelPairingAdapter {
    std::string id_label;
    std::function<std::string(const std::string&)> normalize_allow_entry;
    std::function<void(const core::Config&, const std::string&)> notify_approval;
};

// ChannelSecurityDmPolicy
struct ChannelSecurityDmPolicy {
    std::string policy;
    std::optional<std::vector<std::string>> allow_from;
    std::optional<std::string> policy_path;
    std::string allow_from_path;
    std::string approve_hint;
    std::function<std::string(const std::string&)> normalize_entry;
};

// ChannelSecurityAdapter
struct ChannelSecurityAdapter {
    std::function<std::optional<ChannelSecurityDmPolicy>(const core::Config&, const std::string&, const nlohmann::json&)> resolve_dm_policy;
    std::function<std::vector<std::string>(const core::Config&, const std::string&, const nlohmann::json&)> collect_warnings;
};

// OutboundDeliveryResult (100% 对齐 OpenClaw)
struct OutboundDeliveryResult {
    std::string channel;
    std::string message_id;
    std::optional<std::string> chat_id;
    std::optional<std::string> channel_id;
    std::optional<std::string> thread_id;
    std::optional<std::string> conversation_id;
    std::optional<uint64_t> timestamp;
    nlohmann::json meta;
};

// ChannelOutboundContext (100% 对齐 OpenClaw)
struct ChannelOutboundContext {
    core::Config cfg;
    std::string to;
    std::string text;
    std::optional<std::string> media_url;
    std::optional<std::vector<std::string>> media_local_roots;
    std::optional<bool> gif_playback;
    std::optional<std::string> reply_to_id;
    std::optional<std::string> thread_id;
    std::optional<std::string> account_id;
    std::optional<bool> silent;
};

// ChannelOutboundPayloadContext (100% 对齐 OpenClaw)
struct ChannelOutboundPayloadContext : public ChannelOutboundContext {
    nlohmann::json payload;
};

// ChannelPollInput
struct ChannelPollInput {
    std::string question;
    std::vector<std::string> options;
    std::optional<bool> is_anonymous;
    std::optional<bool> allows_multiple_answers;
    std::optional<int> open_period;
};

// ChannelPollContext
struct ChannelPollContext {
    core::Config cfg;
    std::string to;
    ChannelPollInput poll;
    std::optional<std::string> account_id;
    std::optional<std::string> thread_id;
    std::optional<bool> silent;
    std::optional<bool> is_anonymous;
};

// ChannelPollResult
struct ChannelPollResult {
    std::string message_id;
    std::optional<std::string> to_jid;
    std::optional<std::string> channel_id;
    std::optional<std::string> conversation_id;
    std::optional<std::string> poll_id;
};

// ChannelOutboundAdapter (100% 对齐 OpenClaw)
struct ChannelOutboundAdapter {
    // "direct" | "gateway" | "hybrid"
    std::string delivery_mode = "direct";
    std::function<std::vector<std::string>(const std::string&, int)> chunker;
    // Aligned with openclaw: chunkerMode "text" | "markdown"
    std::string chunker_mode = "text";
    // Aligned with openclaw: chunkMode "length" | "newline"
    // "newline": two-level chunking (chunkByParagraph + chunker)
    // "length": single-level chunking (chunker only)
    std::optional<std::string> chunk_mode;  // default: "length"
    std::optional<int> text_chunk_limit;
    std::optional<int> poll_max_options;
    std::function<std::optional<std::string>(const core::Config&, const std::string&, const std::vector<std::string>&, const std::string&, const std::string&)> resolve_target;
    std::function<core::Result<OutboundDeliveryResult>(const ChannelOutboundPayloadContext&)> send_payload;
    std::function<core::Result<OutboundDeliveryResult>(const ChannelOutboundContext&)> send_text;
    std::function<core::Result<OutboundDeliveryResult>(const ChannelOutboundContext&)> send_media;
    std::function<core::Result<ChannelPollResult>(const ChannelPollContext&)> send_poll;
};

// ChannelStatusAdapter (100% 对齐 OpenClaw)
struct ChannelStatusAdapter {
    std::optional<ChannelAccountSnapshot> default_runtime;
    std::function<nlohmann::json(const nlohmann::json&, const core::Config&, const std::string&, const ChannelAccountSnapshot&)> build_channel_summary;
    std::function<nlohmann::json(const nlohmann::json&, int64_t, const core::Config&)> probe_account;
    std::function<nlohmann::json(const nlohmann::json&, int64_t, const core::Config&, const nlohmann::json&)> audit_account;
    std::function<ChannelAccountSnapshot(const nlohmann::json&, const core::Config&, const std::optional<ChannelAccountSnapshot>&, const nlohmann::json&, const nlohmann::json&)> build_account_snapshot;
    std::function<void(const nlohmann::json&, const core::Config&)> log_self_id;
    // "linked" | "not linked" | "configured" | "not configured" | "enabled" | "disabled"
    std::function<std::string(const nlohmann::json&, const core::Config&, bool, bool)> resolve_account_state;
    std::function<std::vector<nlohmann::json>(const std::vector<ChannelAccountSnapshot>&)> collect_status_issues;
};

// ChannelGatewayContext (100% 对齐 OpenClaw)
struct ChannelGatewayContext {
    core::Config cfg;
    std::string account_id;
    nlohmann::json account;
    std::shared_ptr<std::atomic<bool>> abort_signal;
    std::shared_ptr<ChannelLogSink> log;
    std::function<ChannelAccountSnapshot()> get_status;
    std::function<void(const ChannelAccountSnapshot&)> set_status;
    // channelRuntime: optional plugin SDK runtime helpers
    std::optional<nlohmann::json> channel_runtime;
};

// ChannelLoginWithQrStartResult
struct ChannelLoginWithQrStartResult {
    std::optional<std::string> qr_data_url;
    std::string message;
};

// ChannelLoginWithQrWaitResult
struct ChannelLoginWithQrWaitResult {
    bool connected = false;
    std::string message;
};

// ChannelLogoutResult
struct ChannelLogoutResult {
    bool cleared = false;
    std::optional<bool> logged_out;
    nlohmann::json extra;
};

// ChannelLogoutContext
struct ChannelLogoutContext {
    core::Config cfg;
    std::string account_id;
    nlohmann::json account;
    std::shared_ptr<ChannelLogSink> log;
};

// ChannelGatewayAdapter (100% 对齐 OpenClaw)
struct ChannelGatewayAdapter {
    std::function<core::Result<void>(const ChannelGatewayContext&)> start_account;
    std::function<core::Result<void>(const ChannelGatewayContext&)> stop_account;
    std::function<core::Result<ChannelLoginWithQrStartResult>(const std::string&, bool, int64_t, bool)> login_with_qr_start;
    std::function<core::Result<ChannelLoginWithQrWaitResult>(const std::string&, int64_t)> login_with_qr_wait;
    std::function<core::Result<ChannelLogoutResult>(const ChannelLogoutContext&)> logout_account;
};

// ChannelAuthAdapter
struct ChannelAuthAdapter {
    std::function<core::Result<void>(const core::Config&, const std::string&, bool, const std::string&)> login;
};

// ChannelHeartbeatAdapter (100% 对齐 OpenClaw)
struct ChannelHeartbeatAdapter {
    struct ReadyResult {
        bool ok = false;
        std::string reason;
    };
    struct RecipientsResult {
        std::vector<std::string> recipients;
        std::string source;
    };
    std::function<core::Result<ReadyResult>(const core::Config&, const std::string&)> check_ready;
    std::function<RecipientsResult(const core::Config&, const std::string&, bool)> resolve_recipients;
};

// ChannelDirectoryEntry
struct ChannelDirectoryEntry {
    std::string kind;  // "user" | "group" | "channel"
    std::string id;
    std::optional<std::string> name;
    std::optional<std::string> handle;
    std::optional<std::string> avatar_url;
    std::optional<int> rank;
    std::optional<nlohmann::json> raw;
};

// ChannelDirectoryAdapter (100% 对齐 OpenClaw)
struct ChannelDirectoryAdapter {
    std::function<core::Result<std::optional<ChannelDirectoryEntry>>(const core::Config&, const std::string&)> self;
    std::function<core::Result<std::vector<ChannelDirectoryEntry>>(const core::Config&, const std::string&, const std::string&, int)> list_peers;
    std::function<core::Result<std::vector<ChannelDirectoryEntry>>(const core::Config&, const std::string&, const std::string&, int)> list_peers_live;
    std::function<core::Result<std::vector<ChannelDirectoryEntry>>(const core::Config&, const std::string&, const std::string&, int)> list_groups;
    std::function<core::Result<std::vector<ChannelDirectoryEntry>>(const core::Config&, const std::string&, const std::string&, int)> list_groups_live;
    std::function<core::Result<std::vector<ChannelDirectoryEntry>>(const core::Config&, const std::string&, const std::string&, int)> list_group_members;
};

// ChannelResolveResult
struct ChannelResolveResult {
    std::string input;
    bool resolved = false;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<std::string> note;
};

// ChannelResolverAdapter
struct ChannelResolverAdapter {
    std::function<core::Result<std::vector<ChannelResolveResult>>(
        const core::Config&, const std::string&, const std::vector<std::string>&, const std::string&)> resolve_targets;
};

// ChannelMessagingAdapter
struct ChannelMessagingAdapter {
    std::function<std::optional<std::string>(const std::string&)> normalize_target;
};

// ChannelAgentPromptAdapter
struct ChannelAgentPromptAdapter {
    std::function<std::vector<std::string>(const core::Config&, const std::string&)> message_tool_hints;
};

// ChannelMessageActionContext
struct ChannelMessageActionContext {
    std::string channel;
    std::string action;
    core::Config cfg;
    nlohmann::json params;
    std::optional<std::vector<std::string>> media_local_roots;
    std::optional<std::string> account_id;
    std::optional<std::string> requester_sender_id;
    std::optional<nlohmann::json> tool_context;
    bool dry_run = false;
};

// ChannelMessageActionAdapter (100% 对齐 OpenClaw)
struct ChannelMessageActionAdapter {
    std::function<std::vector<std::string>(const core::Config&)> list_actions;
    std::function<bool(const std::string&)> supports_action;
    std::function<bool(const core::Config&)> supports_buttons;
    std::function<bool(const core::Config&)> supports_cards;
    std::function<std::optional<nlohmann::json>(const nlohmann::json&)> extract_tool_send;
    std::function<core::Result<nlohmann::json>(const ChannelMessageActionContext&)> handle_action;
};

// ChannelConfigSchema
struct ChannelConfigSchema {
    nlohmann::json schema;
    std::optional<nlohmann::json> ui_hints;
};

// ChannelOnboardingAdapter (forward declaration, defined in onboarding.h)
struct ChannelOnboardingAdapter;

// ============================================================================
// ChannelPlugin (100% 对齐 OpenClaw types.plugin.ts)
// ============================================================================

struct ChannelPlugin {
    ChannelId id;
    ChannelMeta meta;
    ChannelCapabilities capabilities;

    // defaults
    struct Defaults {
        struct Queue {
            std::optional<int> debounce_ms;
        };
        std::optional<Queue> queue;
    };
    std::optional<Defaults> defaults;

    // reload config prefixes
    struct ReloadConfig {
        std::vector<std::string> config_prefixes;
        std::vector<std::string> noop_prefixes;
    };
    std::optional<ReloadConfig> reload;

    // Adapters (all optional except config)
    std::shared_ptr<ChannelConfigAdapter> config;           // required
    std::optional<ChannelConfigSchema> config_schema;
    std::optional<ChannelSetupAdapter> setup;
    std::optional<ChannelPairingAdapter> pairing;
    std::optional<ChannelSecurityAdapter> security;
    std::optional<ChannelGroupAdapter> groups;
    std::optional<ChannelMentionAdapter> mentions;
    std::optional<ChannelOutboundAdapter> outbound;
    std::optional<ChannelStatusAdapter> status;
    std::vector<std::string> gateway_methods;
    std::optional<ChannelGatewayAdapter> gateway;
    std::optional<ChannelAuthAdapter> auth;
    std::optional<ChannelElevatedAdapter> elevated;
    std::optional<ChannelCommandAdapter> commands;
    std::optional<ChannelStreamingAdapter> streaming;
    std::optional<ChannelThreadingAdapter> threading;
    std::optional<ChannelMessagingAdapter> messaging;
    std::optional<ChannelAgentPromptAdapter> agent_prompt;
    std::optional<ChannelDirectoryAdapter> directory;
    std::optional<ChannelResolverAdapter> resolver;
    std::optional<ChannelMessageActionAdapter> actions;
    std::optional<ChannelHeartbeatAdapter> heartbeat;
    // agent_tools: channel-owned agent tools (login flows, etc.)
    std::vector<nlohmann::json> agent_tools;
    // onboarding: CLI onboarding wizard hooks
    std::shared_ptr<ChannelOnboardingAdapter> onboarding;
};

// ============================================================================
// ChannelPluginRegistry (100% 对齐 OpenClaw)
// ============================================================================

class ChannelPluginRegistry {
public:
    static ChannelPluginRegistry& instance();

    void register_plugin(const ChannelPlugin& plugin, int priority = 0);
    void unregister_plugin(ChannelId channel_id);

    std::optional<ChannelPlugin> get_plugin(ChannelId channel_id) const;
    std::vector<ChannelPlugin> list_plugins() const;
    std::vector<ChannelPlugin> list_plugins_sorted() const;

    // Create a gateway adapter instance for a channel
    std::optional<ChannelGatewayAdapter> get_gateway(ChannelId channel_id) const;

private:
    ChannelPluginRegistry() = default;

    struct PluginEntry {
        ChannelPlugin plugin;
        int priority;
    };

    mutable std::mutex mutex_;
    std::map<ChannelId, PluginEntry> plugins_;
};

// ============================================================================
// ChannelPluginBase (base class for plugin implementations)
// ============================================================================

class ChannelPluginBase : public plugins::Plugin {
public:
    // Plugin interface
    core::Result<bool> init(const plugins::PluginContext& ctx) override;
    core::Result<bool> shutdown() override;
    core::Result<bool> on_load() override;
    core::Result<bool> on_unload() override;

    // Channel-specific: subclasses implement this to provide the ChannelPlugin descriptor
    virtual ChannelPlugin describe() const = 0;

protected:
    std::shared_ptr<ChannelPlugin> plugin_descriptor_;
};

// ============================================================================
// Convenience macro for static plugin registration
// ============================================================================

#define REGISTER_CHANNEL_PLUGIN(plugin_instance, priority) \
    static struct ChannelPluginRegistrar_##priority { \
        ChannelPluginRegistrar_##priority() { \
            ::hunterclaw::channels::ChannelPluginRegistry::instance().register_plugin(plugin_instance, priority); \
        } \
    } _channel_plugin_registrar_##priority;

} // namespace channels
} // namespace hunterclaw
