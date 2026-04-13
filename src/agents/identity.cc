// ============================================================================
// identity.cc
// ✅ 严格对齐 OpenClaw: src/agents/identity.ts (168行)
//
// Agent Identity 统一解析模块：
//   - resolveAgentIdentity: 从 config 解析 agent identity
//   - resolveAckReaction: 4层优先级 (account→channel→global→agent emoji)
//   - resolveIdentityNamePrefix: "[AgentName]" 格式
//   - resolveMessagePrefix: 含 allowFrom 逻辑
//   - resolveResponsePrefix: 4层优先级 + "auto" 关键字支持
//   - resolveEffectiveMessagesConfig: messagePrefix + responsePrefix 组合
//   - resolveHumanDelayConfig: agent human delay 配置
// ============================================================================

#include <string>
#include <optional>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace agents {

namespace {
auto log() { return spdlog::get("identity"); }
} // namespace

// ============================================================================
// 对齐 identity.ts:5
// ============================================================================
static const std::string DEFAULT_ACK_REACTION = "👀";
static const std::string DEFAULT_MESSAGE_PREFIX = "[hunterclaw]";

// ============================================================================
// Helper: getChannelConfig (对齐 identity.ts:79-88)
// ============================================================================
static json get_channel_config(const json& cfg, const std::string& channel) {
    if (cfg.contains("channels") && cfg["channels"].is_object()) {
        if (cfg["channels"].contains(channel) && cfg["channels"][channel].is_object()) {
            return cfg["channels"][channel];
        }
    }
    return json();
}

// ============================================================================
// Helper: resolve agent config by agent ID (对齐 agent-scope.ts)
// ============================================================================
static json resolve_agent_config(const json& cfg, const std::string& agent_id) {
    if (cfg.contains("agents") && cfg["agents"].is_object()) {
        if (cfg["agents"].contains("list") && cfg["agents"]["list"].is_array()) {
            for (const auto& agent : cfg["agents"]["list"]) {
                if (agent.contains("id") && agent["id"] == agent_id) {
                    return agent;
                }
            }
        }
    }
    return json();
}

// ============================================================================
// 对齐 identity.ts:7-12: resolveAgentIdentity
// ============================================================================
json resolve_agent_identity(const json& cfg, const std::string& agent_id) {
    auto agent_cfg = resolve_agent_config(cfg, agent_id);
    if (agent_cfg.contains("identity") && agent_cfg["identity"].is_object()) {
        return agent_cfg["identity"];
    }
    return json();
}

// ============================================================================
// 对齐 identity.ts:14-47: resolveAckReaction
// 4层优先级: account → channel → global → agent emoji → default
// ============================================================================
std::string resolve_ack_reaction(
    const json& cfg,
    const std::string& agent_id,
    const std::string& channel,
    const std::string& account_id
) {
    // L1: Channel account level
    if (!channel.empty() && !account_id.empty()) {
        auto channel_cfg = get_channel_config(cfg, channel);
        if (channel_cfg.contains("accounts") && channel_cfg["accounts"].is_object()) {
            if (channel_cfg["accounts"].contains(account_id)) {
                auto& acc = channel_cfg["accounts"][account_id];
                if (acc.contains("ackReaction") && acc["ackReaction"].is_string()) {
                    std::string val = acc["ackReaction"];
                    auto s = val.find_first_not_of(" \t");
                    auto e = val.find_last_not_of(" \t");
                    return (s != std::string::npos) ? val.substr(s, e - s + 1) : "";
                }
            }
        }
    }

    // L2: Channel level
    if (!channel.empty()) {
        auto channel_cfg = get_channel_config(cfg, channel);
        if (channel_cfg.contains("ackReaction") && channel_cfg["ackReaction"].is_string()) {
            std::string val = channel_cfg["ackReaction"];
            auto s = val.find_first_not_of(" \t");
            auto e = val.find_last_not_of(" \t");
            return (s != std::string::npos) ? val.substr(s, e - s + 1) : "";
        }
    }

    // L3: Global messages level
    if (cfg.contains("messages") && cfg["messages"].is_object()) {
        if (cfg["messages"].contains("ackReaction") && cfg["messages"]["ackReaction"].is_string()) {
            std::string val = cfg["messages"]["ackReaction"];
            auto s = val.find_first_not_of(" \t");
            auto e = val.find_last_not_of(" \t");
            return (s != std::string::npos) ? val.substr(s, e - s + 1) : "";
        }
    }

    // L4: Agent identity emoji fallback
    auto identity = resolve_agent_identity(cfg, agent_id);
    if (identity.contains("emoji") && identity["emoji"].is_string()) {
        std::string emoji = identity["emoji"];
        auto s = emoji.find_first_not_of(" \t");
        if (s != std::string::npos) return emoji.substr(s);
    }

    return DEFAULT_ACK_REACTION;
}

// ============================================================================
// 对齐 identity.ts:49-58: resolveIdentityNamePrefix
// ============================================================================
std::optional<std::string> resolve_identity_name_prefix(
    const json& cfg,
    const std::string& agent_id
) {
    auto identity = resolve_agent_identity(cfg, agent_id);
    if (identity.contains("name") && identity["name"].is_string()) {
        std::string name = identity["name"];
        auto s = name.find_first_not_of(" \t");
        auto e = name.find_last_not_of(" \t");
        if (s != std::string::npos) {
            return "[" + name.substr(s, e - s + 1) + "]";
        }
    }
    return std::nullopt;
}

// ============================================================================
// 对齐 identity.ts:60-76: resolveMessagePrefix
// ============================================================================
std::string resolve_message_prefix(
    const json& cfg,
    const std::string& agent_id,
    const std::optional<std::string>& configured,
    bool has_allow_from,
    const std::string& fallback
) {
    // Check explicit config
    std::string effective_configured;
    if (configured.has_value()) {
        effective_configured = *configured;
    } else if (cfg.contains("messages") && cfg["messages"].is_object() &&
               cfg["messages"].contains("messagePrefix") && cfg["messages"]["messagePrefix"].is_string()) {
        effective_configured = cfg["messages"]["messagePrefix"];
    }

    if (!effective_configured.empty()) {
        return effective_configured;
    }

    // allowFrom → empty prefix
    if (has_allow_from) return "";

    // Identity name prefix fallback
    auto name_prefix = resolve_identity_name_prefix(cfg, agent_id);
    return name_prefix.value_or(fallback.empty() ? DEFAULT_MESSAGE_PREFIX : fallback);
}

// ============================================================================
// 对齐 identity.ts:90-129: resolveResponsePrefix
// ============================================================================
std::optional<std::string> resolve_response_prefix(
    const json& cfg,
    const std::string& agent_id,
    const std::string& channel,
    const std::string& account_id
) {
    // L1: Channel account level
    if (!channel.empty() && !account_id.empty()) {
        auto channel_cfg = get_channel_config(cfg, channel);
        if (channel_cfg.contains("accounts") && channel_cfg["accounts"].is_object() &&
            channel_cfg["accounts"].contains(account_id)) {
            auto& acc = channel_cfg["accounts"][account_id];
            if (acc.contains("responsePrefix") && acc["responsePrefix"].is_string()) {
                std::string val = acc["responsePrefix"];
                if (val == "auto") return resolve_identity_name_prefix(cfg, agent_id);
                return val;
            }
        }
    }

    // L2: Channel level
    if (!channel.empty()) {
        auto channel_cfg = get_channel_config(cfg, channel);
        if (channel_cfg.contains("responsePrefix") && channel_cfg["responsePrefix"].is_string()) {
            std::string val = channel_cfg["responsePrefix"];
            if (val == "auto") return resolve_identity_name_prefix(cfg, agent_id);
            return val;
        }
    }

    // L3: Global level
    if (cfg.contains("messages") && cfg["messages"].is_object() &&
        cfg["messages"].contains("responsePrefix") && cfg["messages"]["responsePrefix"].is_string()) {
        std::string val = cfg["messages"]["responsePrefix"];
        if (val == "auto") return resolve_identity_name_prefix(cfg, agent_id);
        return val;
    }

    return std::nullopt;
}

// ============================================================================
// 对齐 identity.ts:131-151: resolveEffectiveMessagesConfig
// ============================================================================
struct EffectiveMessagesConfig {
    std::string message_prefix;
    std::optional<std::string> response_prefix;
};

EffectiveMessagesConfig resolve_effective_messages_config(
    const json& cfg,
    const std::string& agent_id,
    bool has_allow_from,
    const std::string& fallback_message_prefix,
    const std::string& channel,
    const std::string& account_id
) {
    return {
        resolve_message_prefix(cfg, agent_id, std::nullopt, has_allow_from, fallback_message_prefix),
        resolve_response_prefix(cfg, agent_id, channel, account_id)
    };
}

// ============================================================================
// 对齐 identity.ts:153-167: resolveHumanDelayConfig
// ============================================================================
struct HumanDelayConfig {
    std::optional<std::string> mode;
    std::optional<int> min_ms;
    std::optional<int> max_ms;
};

std::optional<HumanDelayConfig> resolve_human_delay_config(
    const json& cfg,
    const std::string& agent_id
) {
    json defaults_hd;
    if (cfg.contains("agents") && cfg["agents"].is_object() &&
        cfg["agents"].contains("defaults") && cfg["agents"]["defaults"].is_object() &&
        cfg["agents"]["defaults"].contains("humanDelay")) {
        defaults_hd = cfg["agents"]["defaults"]["humanDelay"];
    }

    auto agent_cfg = resolve_agent_config(cfg, agent_id);
    json overrides_hd;
    if (agent_cfg.contains("humanDelay")) {
        overrides_hd = agent_cfg["humanDelay"];
    }

    if (defaults_hd.is_null() && overrides_hd.is_null()) return std::nullopt;

    HumanDelayConfig result;
    // Override takes precedence
    if (overrides_hd.contains("mode") && overrides_hd["mode"].is_string())
        result.mode = overrides_hd["mode"];
    else if (defaults_hd.contains("mode") && defaults_hd["mode"].is_string())
        result.mode = defaults_hd["mode"];

    if (overrides_hd.contains("minMs") && overrides_hd["minMs"].is_number())
        result.min_ms = overrides_hd["minMs"];
    else if (defaults_hd.contains("minMs") && defaults_hd["minMs"].is_number())
        result.min_ms = defaults_hd["minMs"];

    if (overrides_hd.contains("maxMs") && overrides_hd["maxMs"].is_number())
        result.max_ms = overrides_hd["maxMs"];
    else if (defaults_hd.contains("maxMs") && defaults_hd["maxMs"].is_number())
        result.max_ms = defaults_hd["maxMs"];

    return result;
}

} // namespace agents
} // namespace hunterclaw
