// HunterClaw – auto_reply_status.cc
// 严格对齐 OC auto-reply/status.ts (946行)
// /status 状态消息构建 + /tools 工具列表构建
//
// OC 导出:
//   buildStatusMessage(args) → 完整状态文本
//   buildToolsMessage(result, options?) → 工具列表文本
//   formatContextUsageShort(total, ctx) → "Context 12K/128K (9%)"
//   formatTokenCount(n) → "12K" / "1.2M"

#include "hunterclaw/auto_reply/command_status_builders.h"
#include "hunterclaw/auto_reply/fallback_state.h"
#include "hunterclaw/auto_reply/thinking.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/sessions/session_manager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hunterclaw::auto_reply {

// ─── Token Formatting ──────────────────────────────────────────────────
// Aligned: OC status.ts:65 + utils/usage-format.ts

static std::string format_token_count(int64_t n) {
    if (n < 0) return "?";
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        double k = static_cast<double>(n) / 1000.0;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << k << "K";
        return oss.str();
    }
    if (n < 1000000) return std::to_string(n / 1000) + "K";
    double m = static_cast<double>(n) / 1000000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << m << "M";
    return oss.str();
}

// ─── Context Usage Short ───────────────────────────────────────────────
// Aligned: OC status.ts:198-213

static std::string format_tokens_pair(std::optional<int64_t> total, std::optional<int64_t> ctx) {
    std::string total_label = total ? format_token_count(*total) : "?";
    std::string ctx_label = ctx ? format_token_count(*ctx) : "?";
    if (total && ctx && *ctx > 0) {
        int pct = std::min(999, static_cast<int>(std::round(static_cast<double>(*total) / *ctx * 100)));
        return total_label + "/" + ctx_label + " (" + std::to_string(pct) + "%)";
    }
    return total_label + "/" + ctx_label;
}

std::string format_context_usage_short(std::optional<int64_t> total, std::optional<int64_t> ctx) {
    return "Context " + format_tokens_pair(total, ctx);
}

// ─── Queue Details ─────────────────────────────────────────────────────
// Aligned: OC status.ts:215-240

struct QueueStatus {
    std::string mode;
    std::optional<int> depth;
    std::optional<int> debounce_ms;
    std::optional<int> cap;
    std::string drop_policy;
    bool show_details = false;
};

static std::string format_queue_details(const QueueStatus& q) {
    std::string depth_str;
    if (q.depth) depth_str = "depth " + std::to_string(*q.depth);
    if (!q.show_details) {
        return depth_str.empty() ? "" : " (" + depth_str + ")";
    }
    std::vector<std::string> parts;
    if (!depth_str.empty()) parts.push_back(depth_str);
    if (q.debounce_ms) {
        int ms = std::max(0, *q.debounce_ms);
        std::string label;
        if (ms >= 1000) {
            if (ms % 1000 == 0) label = std::to_string(ms / 1000) + "s";
            else {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
                label = oss.str();
            }
        } else {
            label = std::to_string(ms) + "ms";
        }
        parts.push_back("debounce " + label);
    }
    if (q.cap) parts.push_back("cap " + std::to_string(*q.cap));
    if (!q.drop_policy.empty()) parts.push_back("drop " + q.drop_policy);
    if (parts.empty()) return "";
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) joined += " · ";
        joined += parts[i];
    }
    return " (" + joined + ")";
}

// ─── Usage Pair ────────────────────────────────────────────────────────
// Aligned: OC status.ts:319-326

static std::optional<std::string> format_usage_pair(
    std::optional<int64_t> input, std::optional<int64_t> output) {
    if (!input && !output) return std::nullopt;
    std::string in_label = input ? format_token_count(*input) : "?";
    std::string out_label = output ? format_token_count(*output) : "?";
    return "🧮 Tokens: " + in_label + " in / " + out_label + " out";
}

// ─── Cache Line ────────────────────────────────────────────────────────
// Aligned: OC status.ts:328-356

static std::optional<std::string> format_cache_line(
    std::optional<int64_t> input,
    std::optional<int64_t> cache_read,
    std::optional<int64_t> cache_write) {
    if (!cache_read && !cache_write) return std::nullopt;
    int64_t cr = cache_read.value_or(0);
    int64_t cw = cache_write.value_or(0);
    if (cr <= 0 && cw <= 0) return std::nullopt;
    int64_t total_input = cr + cw + input.value_or(0);
    int hit_rate = total_input > 0 ? static_cast<int>(std::round(static_cast<double>(cr) / total_input * 100)) : 0;
    return "🗄️ Cache: " + std::to_string(hit_rate) + "% hit · " +
           format_token_count(cr) + " cached, " + format_token_count(cw) + " new";
}

// ─── Auth Mode ─────────────────────────────────────────────────────────
// Aligned: OC status.ts:105-131

static std::string normalize_auth_mode(const std::string& value) {
    auto v = ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(([](const std::string& s){ auto a=s.find_first_not_of(" \t\n\r"); if(a==std::string::npos) return std::string(); return s.substr(a,s.find_last_not_of(" \t\n\r")-a+1); })(value));
    if (v.empty()) return "";
    if (v == "api-key" || v.substr(0, 8) == "api-key ") return "api-key";
    if (v == "oauth" || v.substr(0, 6) == "oauth ") return "oauth";
    if (v == "token" || v.substr(0, 6) == "token ") return "token";
    if (v == "aws-sdk" || v.substr(0, 8) == "aws-sdk ") return "aws-sdk";
    if (v == "mixed" || v.substr(0, 6) == "mixed ") return "mixed";
    if (v == "unknown") return "unknown";
    return "";
}

// ─── Build Status Message ──────────────────────────────────────────────
// Aligned: OC status.ts:421-860 (core output assembly)

struct StatusArgs {
    nlohmann::json config;
    std::string session_key;
    std::string agent_id;
    std::optional<int64_t> total_tokens;
    std::optional<int64_t> context_tokens;
    std::optional<int64_t> input_tokens;
    std::optional<int64_t> output_tokens;
    std::optional<int64_t> cache_read;
    std::optional<int64_t> cache_write;
    int compaction_count = 0;
    std::string selected_provider;
    std::string selected_model;
    std::string active_provider;
    std::string active_model;
    std::string think_level = "off";
    std::string verbose_level = "off";
    bool fast_mode = false;
    std::string reasoning_level = "off";
    std::string elevated_level = "on";
    std::string trace_level = "off";
    std::string model_auth;
    std::string runtime_label = "direct";
    std::optional<std::string> group_activation;
    QueueStatus queue;
    std::string version = "0.1.0";
    std::optional<std::string> commit_hash;
    std::optional<std::string> usage_line;
    std::optional<std::string> time_line;
    std::optional<std::string> subagents_line;
    std::optional<std::string> task_line;
    int64_t updated_at_ms = 0;
    int64_t now_ms = 0;
    // Fallback state
    std::optional<std::string> fallback_notice_selected;
    std::optional<std::string> fallback_notice_active;
    std::optional<std::string> fallback_notice_reason;
    // Cost
    std::optional<double> cost_usd;
};

std::string build_status_message(const StatusArgs& args) {
    auto now = args.now_ms > 0 ? args.now_ms : std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Model line
    std::string selected_label = args.selected_provider + "/" + args.selected_model;
    std::string active_label = args.active_provider + "/" + args.active_model;
    std::string auth_label;
    auto auth_mode = normalize_auth_mode(args.model_auth);
    if (!auth_mode.empty() && auth_mode != "unknown") {
        auth_label = " · 🔑 " + auth_mode;
    }
    std::string model_line = "🧠 Model: " + selected_label + auth_label;

    // Fallback line
    std::optional<std::string> fallback_line;
    if (selected_label != active_label && args.fallback_notice_active) {
        std::string reason = args.fallback_notice_reason.value_or("selected model unavailable");
        fallback_line = "↪️ Fallback: " + active_label + " (" + reason + ")";
    }

    // Context line
    std::string context_line = format_context_usage_short(args.total_tokens, args.context_tokens) +
        " · 🧹 Compactions: " + std::to_string(args.compaction_count);

    // Session line
    std::string session_line = "Session: " + args.session_key;
    if (args.updated_at_ms > 0) {
        int64_t elapsed = now - args.updated_at_ms;
        if (elapsed >= 0) {
            session_line += " • updated " + ([](int64_t ms)->std::string{ if(ms<60000) return std::to_string(ms/1000)+"s ago"; if(ms<3600000) return std::to_string(ms/60000)+"m ago"; return std::to_string(ms/3600000)+"h ago"; })(elapsed);
        }
    } else {
        session_line += " • no activity";
    }

    // Options line
    std::vector<std::string> opt_parts;
    opt_parts.push_back("Runtime: " + args.runtime_label);
    opt_parts.push_back("Think: " + args.think_level);
    if (args.fast_mode) opt_parts.push_back("Fast: on");
    if (args.verbose_level == "full") opt_parts.push_back("verbose:full");
    else if (args.verbose_level == "on") opt_parts.push_back("verbose");
    if (args.trace_level == "on") opt_parts.push_back("trace");
    if (args.reasoning_level != "off") opt_parts.push_back("Reasoning: " + args.reasoning_level);
    if (args.elevated_level != "off") {
        std::string elabel = args.elevated_level == "on" ? "elevated" : "elevated:" + args.elevated_level;
        opt_parts.push_back(elabel);
    }
    std::string options_line;
    for (size_t i = 0; i < opt_parts.size(); ++i) {
        if (i > 0) options_line += " · ";
        options_line += opt_parts[i];
    }

    // Queue + activation
    std::string queue_label = "🪢 Queue: " + args.queue.mode + format_queue_details(args.queue);
    std::string activation_line;
    if (args.group_activation) {
        activation_line = "👥 Activation: " + *args.group_activation + " · " + queue_label;
    } else {
        activation_line = queue_label;
    }

    // Usage/cost
    auto usage_pair = format_usage_pair(args.input_tokens, args.output_tokens);
    auto cache_line = format_cache_line(args.input_tokens, args.cache_read, args.cache_write);
    std::optional<std::string> cost_line;
    if (args.cost_usd) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4) << "$" << *args.cost_usd;
        cost_line = "💵 Cost: " + oss.str();
    }
    std::optional<std::string> usage_cost_line;
    if (usage_pair && cost_line) usage_cost_line = *usage_pair + " · " + *cost_line;
    else if (usage_pair) usage_cost_line = *usage_pair;
    else if (cost_line) usage_cost_line = *cost_line;

    // Version
    std::string version_line = "🦞 HunterClaw " + args.version;
    if (args.commit_hash) version_line += " (" + *args.commit_hash + ")";

    // Assemble (aligned: OC status.ts:839-859)
    std::vector<std::string> lines;
    auto add = [&](const std::optional<std::string>& line) {
        if (line && !line->empty()) lines.push_back(*line);
    };
    auto add_str = [&](const std::string& line) {
        if (!line.empty()) lines.push_back(line);
    };

    add_str(version_line);
    add(args.time_line);
    add_str(model_line);
    add(fallback_line);
    add(usage_cost_line);
    add(cache_line);
    add_str("📚 " + context_line);
    add(args.usage_line);
    add_str("🧵 " + session_line);
    add(args.subagents_line);
    add(args.task_line);
    add_str("⚙️ " + options_line);
    add_str(activation_line);

    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += "\n";
        result += lines[i];
    }
    return result;
}

// ─── Build Tools Message ───────────────────────────────────────────────
// Aligned: OC status.ts:862-945

struct ToolsMessageItem {
    std::string id;
    std::string name;
    std::string description;
    std::string raw_description;
    std::string source;  // "builtin" | "plugin" | "channel"
    std::string plugin_id;
    std::string channel_id;
};

struct ToolsGroup {
    std::string label;
    std::vector<ToolsMessageItem> tools;
};

struct ToolsInventoryResult {
    std::string profile;
    std::vector<ToolsGroup> groups;
};

static std::string format_compact_tool_entry(const ToolsMessageItem& tool) {
    if (tool.source == "plugin" && !tool.plugin_id.empty()) {
        return tool.id + " (" + tool.plugin_id + ")";
    }
    if (tool.source == "channel" && !tool.channel_id.empty()) {
        return tool.id + " (" + tool.channel_id + ")";
    }
    return tool.id;
}

std::string build_tools_message(const ToolsInventoryResult& result, bool verbose) {
    // Filter empty groups
    std::vector<const ToolsGroup*> non_empty;
    for (const auto& g : result.groups) {
        if (!g.tools.empty()) non_empty.push_back(&g);
    }

    if (non_empty.empty()) {
        return "No tools are available for this agent right now.\n\nProfile: " + result.profile;
    }

    std::vector<std::string> lines;
    if (verbose) {
        lines = {"Available tools", "", "Profile: " + result.profile,
                 "What this agent can use right now:"};
    } else {
        lines = {"Available tools", "", "Profile: " + result.profile};
    }

    for (const auto* group : non_empty) {
        lines.push_back("");
        lines.push_back(group->label);
        if (verbose) {
            // Sort by name
            auto sorted = group->tools;
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.name < b.name; });
            for (const auto& tool : sorted) {
                lines.push_back("  " + tool.name + " - " + tool.description);
            }
        } else {
            auto sorted = group->tools;
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.name < b.name; });
            std::string compact;
            for (size_t i = 0; i < sorted.size(); ++i) {
                if (i > 0) compact += ", ";
                compact += format_compact_tool_entry(sorted[i]);
            }
            lines.push_back("  " + compact);
        }
    }

    if (verbose) {
        lines.push_back("");
        lines.push_back("Tool availability depends on this agent's configuration.");
    } else {
        lines.push_back("");
        lines.push_back("Use /tools verbose for descriptions.");
    }

    std::string result_str;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result_str += "\n";
        result_str += lines[i];
    }
    return result_str;
}

}  // namespace hunterclaw::auto_reply
