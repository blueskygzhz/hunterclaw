#include "hunterclaw/channels/session_conversation.h"
#include "hunterclaw/channels/registry.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace channels {

namespace {
    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        return s.substr(start, s.find_last_not_of(" \t\n\r") - start + 1);
    }

    std::string to_lower(const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    }

    std::optional<std::string> normalize_opt(const std::optional<std::string>& s) {
        if (!s) return std::nullopt;
        auto t = trim(*s);
        return t.empty() ? std::nullopt : std::make_optional(t);
    }
} // namespace

// ============================================================================
// Session Conversation (对齐 OC: session-conversation.ts generic resolution)
// ============================================================================

std::optional<ResolvedSessionConversation> resolve_session_conversation(
    const std::string& /*channel*/, const std::string& /*kind*/, const std::string& raw_id) {
    auto trimmed = trim(raw_id);
    if (trimmed.empty()) return std::nullopt;

    // Parse thread suffix: "base:thread:threadId"
    std::optional<std::string> thread_id;
    std::string base = trimmed;

    auto thread_pos = trimmed.find(":thread:");
    if (thread_pos != std::string::npos) {
        base = trimmed.substr(0, thread_pos);
        thread_id = trimmed.substr(thread_pos + 8);
    }

    std::vector<std::string> parent_candidates;
    if (thread_id && !base.empty()) {
        parent_candidates.push_back(base);
    }

    return ResolvedSessionConversation{
        thread_id ? *thread_id : base,
        thread_id,
        base,
        parent_candidates
    };
}

std::optional<ResolvedSessionConversationRef> resolve_session_conversation_ref(
    const std::optional<std::string>& session_key) {
    if (!session_key) return std::nullopt;
    auto key = trim(*session_key);
    if (key.empty()) return std::nullopt;

    // Parse session key: "agent:<agentId>:<channel>:<kind>:<rawId>"
    // Minimal parsing — extract channel + kind + rawId from suffix
    auto parts_count = std::count(key.begin(), key.end(), ':');
    if (parts_count < 3) return std::nullopt;

    // Find channel:kind:rawId
    size_t first_colon = key.find(':');
    size_t second_colon = key.find(':', first_colon + 1);
    size_t third_colon = key.find(':', second_colon + 1);
    if (third_colon == std::string::npos) return std::nullopt;

    std::string prefix = key.substr(0, third_colon);
    std::string channel = key.substr(second_colon + 1, third_colon - second_colon - 1);

    size_t fourth_colon = key.find(':', third_colon + 1);
    if (fourth_colon == std::string::npos) return std::nullopt;

    std::string kind = key.substr(third_colon + 1, fourth_colon - third_colon - 1);
    std::string raw_id = key.substr(fourth_colon + 1);

    auto resolved = resolve_session_conversation(channel, kind, raw_id);
    if (!resolved) return std::nullopt;

    return ResolvedSessionConversationRef{
        channel, kind, raw_id,
        resolved->id, resolved->thread_id,
        prefix + ":" + kind + ":" + resolved->base_conversation_id,
        resolved->base_conversation_id,
        resolved->parent_conversation_candidates
    };
}

std::optional<std::string> resolve_session_parent_session_key(
    const std::optional<std::string>& session_key) {
    auto ref = resolve_session_conversation_ref(session_key);
    if (!ref || !ref->thread_id) return std::nullopt;
    return ref->base_session_key;
}

// ============================================================================
// Sender Label (对齐 OC)
// ============================================================================

std::optional<std::string> resolve_sender_label(const SenderLabelParams& params) {
    auto name = normalize_opt(params.name);
    auto username = normalize_opt(params.username);
    auto tag = normalize_opt(params.tag);
    auto e164 = normalize_opt(params.e164);
    auto id = normalize_opt(params.id);

    std::string display;
    if (name) display = *name;
    else if (username) display = *username;
    else if (tag) display = *tag;

    std::string id_part;
    if (e164) id_part = *e164;
    else if (id) id_part = *id;

    if (!display.empty() && !id_part.empty() && display != id_part) {
        return display + " (" + id_part + ")";
    }
    if (!display.empty()) return display;
    if (!id_part.empty()) return id_part;
    return std::nullopt;
}

std::vector<std::string> list_sender_label_candidates(const SenderLabelParams& params) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    auto add = [&](const std::optional<std::string>& s) {
        auto v = normalize_opt(s);
        if (v && seen.insert(*v).second) result.push_back(*v);
    };
    add(params.name);
    add(params.username);
    add(params.tag);
    add(params.e164);
    add(params.id);
    auto resolved = resolve_sender_label(params);
    if (resolved && seen.insert(*resolved).second) result.push_back(*resolved);
    return result;
}

// ============================================================================
// Sender Identity (对齐 OC)
// ============================================================================

std::vector<std::string> validate_sender_identity(
    const std::optional<std::string>& chat_type,
    const std::optional<std::string>& sender_id,
    const std::optional<std::string>& sender_name,
    const std::optional<std::string>& sender_username,
    const std::optional<std::string>& sender_e164) {
    std::vector<std::string> issues;
    bool is_direct = (chat_type && to_lower(trim(*chat_type)) == "direct");

    auto sid = normalize_opt(sender_id).value_or("");
    auto sname = normalize_opt(sender_name).value_or("");
    auto suser = normalize_opt(sender_username).value_or("");
    auto se164 = normalize_opt(sender_e164).value_or("");

    if (!is_direct && sid.empty() && sname.empty() && suser.empty() && se164.empty()) {
        issues.push_back("missing sender identity (SenderId/SenderName/SenderUsername/SenderE164)");
    }
    if (!se164.empty()) {
        std::regex e164_re("^\\+\\d{3,}$");
        if (!std::regex_match(se164, e164_re)) {
            issues.push_back("invalid SenderE164: " + se164);
        }
    }
    if (!suser.empty()) {
        if (suser.find('@') != std::string::npos) {
            issues.push_back("SenderUsername should not include \"@\": " + suser);
        }
        if (std::any_of(suser.begin(), suser.end(), ::isspace)) {
            issues.push_back("SenderUsername should not include whitespace: " + suser);
        }
    }
    if (sender_id.has_value() && sid.empty()) {
        issues.push_back("SenderId is set but empty");
    }
    return issues;
}

// ============================================================================
// Location (对齐 OC)
// ============================================================================

std::string format_location_text(const NormalizedLocation& location) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << location.latitude << ", " << location.longitude;
    std::string coords = oss.str();

    std::string accuracy;
    if (location.accuracy) {
        accuracy = " \xC2\xB1" + std::to_string(static_cast<int>(std::round(*location.accuracy))) + "m";
    }

    std::string header;
    if (location.is_live || location.source == LocationSource::LIVE) {
        header = "\xF0\x9F\x9B\xB0 Live location: " + coords + accuracy;
    } else if (location.name || location.address) {
        std::string label;
        if (location.name) label = *location.name;
        if (location.address) {
            if (!label.empty()) label += " \xE2\x80\x94 ";
            label += *location.address;
        }
        header = "\xF0\x9F\x93\x8D " + label + " (" + coords + accuracy + ")";
    } else {
        header = "\xF0\x9F\x93\x8D " + coords + accuracy;
    }

    auto caption = location.caption ? trim(*location.caption) : "";
    return caption.empty() ? header : header + "\n" + caption;
}

// ============================================================================
// Channel Logging (对齐 OC)
// ============================================================================

void log_inbound_drop(const LogFn& log, const std::string& channel,
                       const std::string& reason, const std::string& target) {
    std::string t = target.empty() ? "" : " target=" + target;
    log(channel + ": drop " + reason + t);
}

void log_typing_failure(const LogFn& log, const std::string& channel,
                         const std::string& error, const std::string& target,
                         const std::string& action) {
    std::string t = target.empty() ? "" : " target=" + target;
    std::string a = action.empty() ? "" : " action=" + action;
    log(channel + " typing" + a + " failed" + t + ": " + error);
}

void log_ack_failure(const LogFn& log, const std::string& channel,
                      const std::string& error, const std::string& target) {
    std::string t = target.empty() ? "" : " target=" + target;
    log(channel + " ack cleanup failed" + t + ": " + error);
}

// ============================================================================
// Channel Config (对齐 OC: channel-config.ts)
// ============================================================================

std::string normalize_channel_slug(const std::string& value) {
    std::string lower = to_lower(trim(value));
    // Remove leading #
    if (!lower.empty() && lower[0] == '#') lower = lower.substr(1);
    // Replace non-alnum with -
    std::string result;
    for (char c : lower) {
        result += (std::isalnum(c) ? c : '-');
    }
    // Trim leading/trailing dashes
    auto start = result.find_first_not_of('-');
    if (start == std::string::npos) return "";
    auto end = result.find_last_not_of('-');
    return result.substr(start, end - start + 1);
}

std::vector<std::string> build_channel_key_candidates(
    const std::vector<std::optional<std::string>>& keys) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& key : keys) {
        if (!key) continue;
        auto t = trim(*key);
        if (t.empty() || !seen.insert(t).second) continue;
        result.push_back(t);
    }
    return result;
}

// ============================================================================
// Config Presence (对齐 OC: config-presence.ts)
// ============================================================================

bool has_meaningful_channel_config(const nlohmann::json& value) {
    if (!value.is_object()) return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "enabled") return true;
    }
    return false;
}

bool has_potential_configured_channels(const core::Config& cfg) {
    if (!cfg.data.contains("channels") || !cfg.data["channels"].is_object()) return false;
    auto& channels = cfg.data["channels"];
    static const std::set<std::string> IGNORED = {"defaults", "modelByChannel"};
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        if (IGNORED.count(it.key())) continue;
        if (has_meaningful_channel_config(it.value())) return true;
    }
    return false;
}

std::vector<std::string> list_potential_configured_channel_ids(const core::Config& cfg) {
    std::vector<std::string> result;
    if (!cfg.data.contains("channels") || !cfg.data["channels"].is_object()) return result;
    auto& channels = cfg.data["channels"];
    static const std::set<std::string> IGNORED = {"defaults", "modelByChannel"};
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        if (IGNORED.count(it.key())) continue;
        if (has_meaningful_channel_config(it.value())) {
            result.push_back(it.key());
        }
    }
    return result;
}

// ============================================================================
// Channel Model Overrides (对齐 OC: model-overrides.ts)
// ============================================================================

std::optional<ChannelModelOverride> resolve_channel_model_override(
    const core::Config& cfg,
    const std::optional<std::string>& channel,
    const std::optional<std::string>& group_id,
    const std::optional<std::string>& /*group_chat_type*/,
    const std::optional<std::string>& group_channel,
    const std::optional<std::string>& group_subject,
    const std::optional<std::string>& /*parent_session_key*/) {
    auto ch = normalize_opt(channel);
    if (!ch) return std::nullopt;

    // Read modelByChannel from config
    if (!cfg.data.contains("channels") || !cfg.data["channels"].is_object()) return std::nullopt;
    auto& channels = cfg.data["channels"];
    if (!channels.contains("modelByChannel") || !channels["modelByChannel"].is_object()) return std::nullopt;
    auto& model_by_channel = channels["modelByChannel"];

    // Find provider entry for this channel
    std::string normalized_ch = to_lower(trim(*ch));
    nlohmann::json provider_entries;
    if (model_by_channel.contains(normalized_ch)) {
        provider_entries = model_by_channel[normalized_ch];
    } else {
        // Try case-insensitive match
        for (auto it = model_by_channel.begin(); it != model_by_channel.end(); ++it) {
            if (to_lower(it.key()) == normalized_ch) {
                provider_entries = it.value();
                break;
            }
        }
    }
    if (!provider_entries.is_object()) return std::nullopt;

    // Build key candidates
    auto candidates = build_channel_key_candidates({group_id, group_channel, group_subject});

    // Try direct match
    for (const auto& key : candidates) {
        if (provider_entries.contains(key) && provider_entries[key].is_string()) {
            auto model = trim(provider_entries[key].get<std::string>());
            if (!model.empty()) {
                return ChannelModelOverride{normalized_ch, model, key, "direct"};
            }
        }
    }

    // Try wildcard
    if (provider_entries.contains("*") && provider_entries["*"].is_string()) {
        auto model = trim(provider_entries["*"].get<std::string>());
        if (!model.empty()) {
            return ChannelModelOverride{normalized_ch, model, "*", "wildcard"};
        }
    }

    return std::nullopt;
}

} // namespace channels
} // namespace hunterclaw
