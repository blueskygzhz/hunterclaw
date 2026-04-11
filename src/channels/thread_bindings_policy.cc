#include "hunterclaw/channels/thread_bindings_policy.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace hunterclaw {
namespace channels {

namespace {
    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        auto end = s.find_last_not_of(" \t\n\r");
        return s.substr(start, end - start + 1);
    }

    std::string to_lower(const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    }

    std::string spawn_flag_key(ThreadBindingSpawnKind kind) {
        return kind == ThreadBindingSpawnKind::SUBAGENT ? "spawnSubagentSessions" : "spawnAcpSessions";
    }

    int64_t hours_to_ms(double hours) {
        return static_cast<int64_t>(std::floor(hours * 60.0 * 60.0 * 1000.0));
    }

    // prefix system message with 🔧
    std::string prefix_system_message(const std::string& msg) {
        return "\xF0\x9F\x94\xA7 " + msg;  // 🔧
    }
} // namespace

// ============================================================================
// resolve_thread_binding_idle_timeout_ms (对齐 OC)
// ============================================================================

int64_t resolve_thread_binding_idle_timeout_ms(
    const std::optional<double>& channel_idle_hours,
    const std::optional<double>& session_idle_hours) {
    double hours = 24.0; // DEFAULT_THREAD_BINDING_IDLE_HOURS
    if (channel_idle_hours && *channel_idle_hours >= 0) {
        hours = *channel_idle_hours;
    } else if (session_idle_hours && *session_idle_hours >= 0) {
        hours = *session_idle_hours;
    }
    return hours_to_ms(hours);
}

// ============================================================================
// resolve_thread_binding_max_age_ms (对齐 OC)
// ============================================================================

int64_t resolve_thread_binding_max_age_ms(
    const std::optional<double>& channel_max_age_hours,
    const std::optional<double>& session_max_age_hours) {
    double hours = 0.0; // DEFAULT_THREAD_BINDING_MAX_AGE_HOURS
    if (channel_max_age_hours && *channel_max_age_hours >= 0) {
        hours = *channel_max_age_hours;
    } else if (session_max_age_hours && *session_max_age_hours >= 0) {
        hours = *session_max_age_hours;
    }
    return hours_to_ms(hours);
}

// ============================================================================
// resolve_thread_binding_lifecycle (对齐 OC)
// ============================================================================

ThreadBindingLifecycleResult resolve_thread_binding_lifecycle(
    const ThreadBindingLifecycleRecord& record,
    int64_t default_idle_timeout_ms,
    int64_t default_max_age_ms) {
    int64_t idle_ms = record.idle_timeout_ms.value_or(default_idle_timeout_ms);
    if (idle_ms < 0) idle_ms = 0;
    int64_t max_age = record.max_age_ms.value_or(default_max_age_ms);
    if (max_age < 0) max_age = 0;

    std::optional<int64_t> inactivity_expires;
    if (idle_ms > 0) {
        inactivity_expires = std::max(record.last_activity_at, record.bound_at) + idle_ms;
    }

    std::optional<int64_t> max_age_expires;
    if (max_age > 0) {
        max_age_expires = record.bound_at + max_age;
    }

    if (inactivity_expires && max_age_expires) {
        if (*inactivity_expires <= *max_age_expires) {
            return {inactivity_expires, "idle-expired"};
        }
        return {max_age_expires, "max-age-expired"};
    }
    if (inactivity_expires) return {inactivity_expires, "idle-expired"};
    if (max_age_expires) return {max_age_expires, "max-age-expired"};
    return {};
}

// ============================================================================
// resolve_thread_bindings_enabled (对齐 OC)
// ============================================================================

bool resolve_thread_bindings_enabled(
    const std::optional<bool>& channel_enabled,
    const std::optional<bool>& session_enabled) {
    if (channel_enabled) return *channel_enabled;
    if (session_enabled) return *session_enabled;
    return true;
}

// ============================================================================
// resolve_thread_binding_spawn_policy (对齐 OC)
// ============================================================================

ThreadBindingSpawnPolicy resolve_thread_binding_spawn_policy(
    const core::Config& cfg,
    const std::string& channel,
    const std::string& account_id,
    ThreadBindingSpawnKind kind) {
    std::string ch = to_lower(trim(channel));
    std::string aid = account_id.empty() ? "default" : account_id;

    // Read from config: channels.<channel>.threadBindings.enabled / spawnXxx
    bool enabled = true;
    bool spawn_enabled = true; // default for non-child-placement channels

    // Try read from cfg.data["channels"][ch]["threadBindings"]
    if (cfg.data.contains("channels") && cfg.data["channels"].contains(ch)) {
        auto& ch_cfg = cfg.data["channels"][ch];
        if (ch_cfg.contains("threadBindings")) {
            auto& tb = ch_cfg["threadBindings"];
            if (tb.contains("enabled") && tb["enabled"].is_boolean()) {
                enabled = tb["enabled"].get<bool>();
            }
            std::string flag_key = spawn_flag_key(kind);
            if (tb.contains(flag_key) && tb[flag_key].is_boolean()) {
                spawn_enabled = tb[flag_key].get<bool>();
            }
        }
    }

    // Session-level fallback
    if (cfg.data.contains("session") && cfg.data["session"].contains("threadBindings")) {
        auto& stb = cfg.data["session"]["threadBindings"];
        if (!cfg.data.contains("channels") || !cfg.data["channels"].contains(ch) ||
            !cfg.data["channels"][ch].contains("threadBindings") ||
            !cfg.data["channels"][ch]["threadBindings"].contains("enabled")) {
            if (stb.contains("enabled") && stb["enabled"].is_boolean()) {
                enabled = stb["enabled"].get<bool>();
            }
        }
    }

    return ThreadBindingSpawnPolicy{ch, aid, enabled, spawn_enabled};
}

// ============================================================================
// resolve_thread_binding_placement_for_current_context (对齐 OC)
// ============================================================================

std::string resolve_thread_binding_placement_for_current_context(
    const std::string& /*channel*/,
    const std::optional<std::string>& thread_id) {
    // Default: "current" — child placement requires plugin conversationBindings hook
    if (thread_id && !thread_id->empty()) return "current";
    return "current";
}

// ============================================================================
// format_thread_binding_duration_label (对齐 OC)
// ============================================================================

std::string format_thread_binding_duration_label(int64_t duration_ms) {
    if (duration_ms <= 0) return "disabled";
    if (duration_ms < 60000) return "<1m";
    int64_t total_minutes = duration_ms / 60000;
    if (total_minutes % 60 == 0) {
        return std::to_string(total_minutes / 60) + "h";
    }
    return std::to_string(total_minutes) + "m";
}

// ============================================================================
// resolve_thread_binding_thread_name (对齐 OC)
// ============================================================================

std::string resolve_thread_binding_thread_name(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& label) {
    std::string base;
    auto lbl = label ? trim(*label) : "";
    if (!lbl.empty()) {
        base = lbl;
    } else {
        auto aid = agent_id ? trim(*agent_id) : "";
        base = aid.empty() ? "agent" : aid;
    }
    std::string raw = "\xF0\x9F\xA4\x96 " + base; // 🤖
    if (raw.size() > 100) raw = raw.substr(0, 100);
    return raw;
}

// ============================================================================
// resolve_thread_binding_intro_text (对齐 OC)
// ============================================================================

std::string resolve_thread_binding_intro_text(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& label,
    std::optional<int64_t> idle_timeout_ms,
    std::optional<int64_t> max_age_ms,
    const std::optional<std::string>& session_cwd) {
    std::string base;
    auto lbl = label ? trim(*label) : "";
    if (!lbl.empty()) {
        base = lbl;
    } else {
        auto aid = agent_id ? trim(*agent_id) : "";
        base = aid.empty() ? "agent" : aid;
    }
    if (base.size() > 100) base = base.substr(0, 100);
    if (base.empty()) base = "agent";

    std::vector<std::string> lifecycle;
    if (idle_timeout_ms && *idle_timeout_ms > 0) {
        lifecycle.push_back("idle auto-unfocus after " + format_thread_binding_duration_label(*idle_timeout_ms) + " inactivity");
    }
    if (max_age_ms && *max_age_ms > 0) {
        lifecycle.push_back("max age " + format_thread_binding_duration_label(*max_age_ms));
    }

    std::string intro;
    if (!lifecycle.empty()) {
        std::string lc_str;
        for (size_t i = 0; i < lifecycle.size(); ++i) {
            if (i > 0) lc_str += "; ";
            lc_str += lifecycle[i];
        }
        intro = base + " session active (" + lc_str + "). Messages here go directly to this session.";
    } else {
        intro = base + " session active. Messages here go directly to this session.";
    }

    if (session_cwd && !session_cwd->empty()) {
        intro += "\ncwd: " + *session_cwd;
    }

    return prefix_system_message(intro);
}

// ============================================================================
// resolve_thread_binding_farewell_text (对齐 OC)
// ============================================================================

std::string resolve_thread_binding_farewell_text(
    const std::optional<std::string>& reason,
    const std::optional<std::string>& farewell_text,
    int64_t idle_timeout_ms,
    int64_t max_age_ms) {
    auto custom = farewell_text ? trim(*farewell_text) : "";
    if (!custom.empty()) {
        return prefix_system_message(custom);
    }

    if (reason && *reason == "idle-expired") {
        return prefix_system_message(
            "Session ended automatically after " + format_thread_binding_duration_label(idle_timeout_ms) +
            " of inactivity. Messages here will no longer be routed.");
    }

    if (reason && *reason == "max-age-expired") {
        return prefix_system_message(
            "Session ended automatically at max age of " + format_thread_binding_duration_label(max_age_ms) +
            ". Messages here will no longer be routed.");
    }

    return prefix_system_message("Session ended. Messages here will no longer be routed.");
}

// ============================================================================
// resolve_thread_binding_conversation_id_from_binding_id (对齐 OC)
// ============================================================================

std::optional<std::string> resolve_thread_binding_conversation_id_from_binding_id(
    const std::string& account_id,
    const std::optional<std::string>& binding_id) {
    if (!binding_id) return std::nullopt;
    auto bid = trim(*binding_id);
    if (bid.empty()) return std::nullopt;
    std::string prefix = account_id + ":";
    if (bid.size() <= prefix.size() || bid.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    auto conv_id = trim(bid.substr(prefix.size()));
    if (conv_id.empty()) return std::nullopt;
    return conv_id;
}

// ============================================================================
// Error formatting (对齐 OC)
// ============================================================================

std::string format_thread_binding_disabled_error(
    const std::string& channel,
    const std::string& /*account_id*/,
    ThreadBindingSpawnKind /*kind*/) {
    return "Thread bindings are disabled for " + channel +
           " (set channels." + channel + ".threadBindings.enabled=true to override for this account, or session.threadBindings.enabled=true globally).";
}

std::string format_thread_binding_spawn_disabled_error(
    const std::string& channel,
    const std::string& /*account_id*/,
    ThreadBindingSpawnKind kind) {
    std::string flag = spawn_flag_key(kind);
    return "Thread-bound " + std::string(kind == ThreadBindingSpawnKind::SUBAGENT ? "subagent" : "acp") +
           " spawns are disabled for " + channel + " (set channels." + channel +
           ".threadBindings." + flag + "=true to enable).";
}

} // namespace channels
} // namespace hunterclaw
