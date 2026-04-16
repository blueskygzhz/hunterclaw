// HunterClaw – session_lifecycle_supplement.cc
// 严格对齐 OC auto-reply/reply/ 会话生命周期子系统:
//   session-updates.ts (11.53KB) — 会话更新持久化
//   session-usage.ts (7.20KB) — 会话用量统计
//   session-reset-model.ts (5.50KB) — 重置时模型选择
//   session-reset-prompt.ts (1.26KB) — 重置提示词构建
//   session-hooks.ts (2.10KB) — 会话 hook 事件
//   session-system-events.ts (3.91KB) — 系统事件格式化
//   session-run-accounting.ts (1.39KB) — 运行记账
// Total OC: ~33KB

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/llm/model_selection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hunterclaw::auto_reply {

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
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

// ═══════════════════════════════════════════════════════════════
// Session Reset Prompt (对齐 OC: session-reset-prompt.ts:4-22)
// ═══════════════════════════════════════════════════════════════

static const std::string BARE_SESSION_RESET_PROMPT_BASE =
    "A new session was started via /new or /reset. If runtime-provided startup "
    "context is included for this first turn, use it before responding to the user. "
    "Then greet the user in your configured persona, if one is provided. Be yourself "
    "- use your defined voice, mannerisms, and mood. Keep it to 1-3 sentences and "
    "ask what they want to do. If the runtime model differs from default_model in "
    "the system prompt, mention the default model. Do not mention internal steps, "
    "files, tools, or reasoning.";

std::string build_bare_session_reset_prompt(const std::string& /*config_json*/, int64_t now) {
    // Append current date/time line (对齐 OC: appendCronStyleCurrentTimeLine)
    if (now <= 0) now = now_ms();
    auto t = std::chrono::system_clock::time_point(std::chrono::milliseconds(now));
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm_val{};
    gmtime_r(&tt, &tm_val);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm_val);
    return BARE_SESSION_RESET_PROMPT_BASE + "\n\nCurrent date/time: " + buf;
}

// ═══════════════════════════════════════════════════════════════
// Session Reset Model (对齐 OC: session-reset-model.ts:17-203)
// ═══════════════════════════════════════════════════════════════

struct SessionResetModelResult {
    std::string provider;
    std::string model;
    std::string cleaned_body;
    bool has_model_override = false;
};

SessionResetModelResult resolve_session_reset_model(
    const std::string& body,
    const std::string& config_json,
    const std::string& agent_id,
    const std::string& default_provider,
    const std::string& default_model) {
    SessionResetModelResult result;
    result.cleaned_body = body;

    if (body.empty()) return result;

    // Split body into tokens
    std::istringstream iss(body);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);

    if (tokens.empty()) return result;

    // Check if first or second token looks like a model reference
    std::string candidate;
    size_t model_token_idx = 0;

    // /new model-name rest... or /reset provider/model rest...
    if (tokens[0].find('/') != std::string::npos ||
        std::regex_match(tokens[0], std::regex("[a-zA-Z0-9_.:-]+"))) {
        // Check if it looks like a model ref (contains / or matches known patterns)
        std::string lower = to_lower(tokens[0]);
        // Skip common non-model words
        static const std::set<std::string> skip_words = {
            "please", "help", "with", "can", "you", "the", "this", "that",
            "what", "how", "why", "when", "where", "which", "who"
        };
        if (skip_words.count(lower) == 0 &&
            (lower.find('/') != std::string::npos ||
             lower.find("claude") != std::string::npos ||
             lower.find("gpt") != std::string::npos ||
             lower.find("gemini") != std::string::npos ||
             lower.find("o1") != std::string::npos ||
             lower.find("o3") != std::string::npos ||
             lower.find("o4") != std::string::npos ||
             lower.find("sonnet") != std::string::npos ||
             lower.find("opus") != std::string::npos)) {
            candidate = tokens[0];
            model_token_idx = 0;
        }
    }

    if (candidate.empty()) return result;

    // Simple model reference parsing (provider/model format)
    auto slash_pos = candidate.find('/');
    if (slash_pos != std::string::npos) {
        result.provider = candidate.substr(0, slash_pos);
        result.model = candidate.substr(slash_pos + 1);
    } else {
        result.provider = default_provider;
        result.model = candidate;
    }
    if (result.model.empty()) return result;
    result.has_model_override = true;

    // Remove model token from body
    std::ostringstream cleaned;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i == model_token_idx) continue;
        if (!cleaned.str().empty()) cleaned << " ";
        cleaned << tokens[i];
    }
    result.cleaned_body = trim(cleaned.str());
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Session Hooks (对齐 OC: session-hooks.ts:1-60)
// ═══════════════════════════════════════════════════════════════

struct SessionStartHookPayload {
    std::string session_key;
    std::string session_id;
    std::string agent_id;
    bool is_new_session = false;
    bool reset_triggered = false;
};

struct SessionEndHookPayload {
    std::string session_key;
    std::string session_id;
    std::string agent_id;
    int64_t duration_ms = 0;
    int total_tokens = 0;
};

// build_session_start_hook_payload 和 build_session_end_hook_payload
// 在 message_dispatcher.cc 的 run 循环中被调用

// ═══════════════════════════════════════════════════════════════
// Session System Events (对齐 OC: session-system-events.ts:1-115)
// ═══════════════════════════════════════════════════════════════

struct SystemEvent {
    std::string type;     // "model_fallback" | "compaction" | "session_reset" | etc.
    std::string message;
    int64_t timestamp = 0;
};

std::string format_system_event(const SystemEvent& event) {
    std::string prefix = "[system";
    if (!event.type.empty()) {
        prefix += ":" + event.type;
    }
    prefix += "] ";
    return prefix + event.message;
}

std::string drain_formatted_system_events(std::vector<SystemEvent>& events) {
    if (events.empty()) return "";
    std::ostringstream ss;
    for (const auto& event : events) {
        ss << format_system_event(event) << "\n";
    }
    events.clear();
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
// Session Run Accounting (对齐 OC: session-run-accounting.ts:1-40)
// ═══════════════════════════════════════════════════════════════

struct SessionRunAccounting {
    int64_t started_at = 0;
    int64_t completed_at = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_write_tokens = 0;
    std::string model_provider;
    std::string model;
    bool is_heartbeat = false;
};

int64_t compute_run_duration_ms(const SessionRunAccounting& acct) {
    if (acct.started_at <= 0 || acct.completed_at <= 0) return 0;
    return std::max<int64_t>(0, acct.completed_at - acct.started_at);
}

int compute_total_tokens(const SessionRunAccounting& acct) {
    return acct.input_tokens + acct.output_tokens +
           acct.cache_read_tokens + acct.cache_write_tokens;
}

// ═══════════════════════════════════════════════════════════════
// Session Usage (对齐 OC: session-usage.ts 核心逻辑)
// ═══════════════════════════════════════════════════════════════

struct SessionUsagePatch {
    std::optional<int> total_tokens;
    std::optional<int> total_tokens_fresh;
    std::optional<int> compaction_count;
    std::optional<std::string> model_provider;
    std::optional<std::string> model;
    std::optional<int64_t> updated_at;
};

SessionUsagePatch build_session_usage_patch(
    const SessionRunAccounting& acct,
    int existing_total_tokens = 0) {
    SessionUsagePatch patch;
    int run_total = compute_total_tokens(acct);
    patch.total_tokens = existing_total_tokens + run_total;
    patch.total_tokens_fresh = run_total;
    patch.model_provider = acct.model_provider;
    patch.model = acct.model;
    patch.updated_at = now_ms();
    return patch;
}

// ═══════════════════════════════════════════════════════════════
// Session Updates (对齐 OC: session-updates.ts 核心逻辑)
// ═══════════════════════════════════════════════════════════════

void persist_session_entry_update(
    sessions::SessionManager& session_manager,
    const std::string& session_key,
    const std::string& agent_id,
    const SessionUsagePatch& patch) {
    (void)agent_id;
    auto entry_result = session_manager.get_session(session_key);
    if (!entry_result.is_ok()) return;
    auto entry = entry_result.get();

    if (patch.total_tokens) entry.total_tokens = *patch.total_tokens;
    if (patch.total_tokens_fresh) entry.total_tokens_fresh = *patch.total_tokens_fresh;
    if (patch.model_provider) entry.model_provider = *patch.model_provider;
    if (patch.model) entry.model = *patch.model;
    if (patch.updated_at) entry.updated_at = *patch.updated_at;

    session_manager.update_session(entry);
}

// Stored Model Override (对齐 OC: stored-model-override.ts)
struct StoredModelOverride {
    std::string provider;
    std::string model;
    std::string source;  // "session" | "parent"
};

std::optional<StoredModelOverride> resolve_stored_model_override(
    sessions::SessionManager& session_manager,
    const std::string& session_key,
    const std::string& parent_session_key,
    const std::string& agent_id) {
    (void)agent_id;
    // Try current session first
    auto entry_result = session_manager.get_session(session_key);
    if (entry_result.is_ok()) {
        auto& entry = entry_result.get();
        if (entry.model_override.has_value() && !entry.model_override->empty()) {
            StoredModelOverride result;
            result.model = *entry.model_override;
            result.provider = entry.provider_override.value_or("");
            result.source = "session";
            return result;
        }
    }
    // Try parent session
    if (!parent_session_key.empty() && parent_session_key != session_key) {
        auto parent_result = session_manager.get_session(parent_session_key);
        if (parent_result.is_ok()) {
            auto& parent = parent_result.get();
            if (parent.model_override.has_value() && !parent.model_override->empty()) {
                StoredModelOverride result;
                result.model = *parent.model_override;
                result.provider = parent.provider_override.value_or("");
                result.source = "parent";
                return result;
            }
        }
    }
    return std::nullopt;
}

}  // namespace hunterclaw::auto_reply
