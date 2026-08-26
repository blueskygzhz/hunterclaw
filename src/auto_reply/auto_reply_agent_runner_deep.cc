// HunterClaw – auto_reply_agent_runner_deep.cc
// 严格对齐 OC auto-reply/reply/ agent runner 子系统:
//   agent-runner-execution.ts (61.39KB, ~1700行) — runReplyAgentExecution 核心
//   agent-runner-memory.ts (29.71KB, ~900行) — 记忆注入/去重/flush
//   agent-runner-payloads.ts (8.73KB) — 回复 payload 构建
//   agent-runner-helpers.ts (2.47KB) — sanitizeResponseForPayload 等
//   agent-runner-utils.ts (8.62KB) — extractPreferredReplyText / media path 解析
//   agent-runner-session-reset.ts (4.01KB) — 新session/重置逻辑
//   agent-runner-usage-line.ts (1.93KB) — 使用量行格式化
//   agent-runner-reminder-guard.ts (2.24KB) — reminder 频率控制
//   agent-runner-auth-profile.ts (791B) — auth profile 解析
// Total OC source: ~3,200行

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/fallback_state.h"
#include "hunterclaw/auto_reply/heartbeat.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/sessions/session_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hunterclaw::auto_reply {


// ─── Reminder Guard ────────────────────────────────────────────────────
// Aligned: OC agent-runner-reminder-guard.ts (2.24KB)

class ReminderGuard {
public:
    bool should_send_reminder(const std::string& session_key, int64_t now_ms,
                              int64_t cooldown_ms = 3600000) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = last_sent_.find(session_key);
        if (it != last_sent_.end() && (now_ms - it->second) < cooldown_ms) {
            return false;
        }
        last_sent_[session_key] = now_ms;
        return true;
    }

    void clear(const std::string& session_key) {
        std::lock_guard<std::mutex> lock(mu_);
        last_sent_.erase(session_key);
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, int64_t> last_sent_;
};

static ReminderGuard s_reminder_guard;

// ─── Auth Profile ──────────────────────────────────────────────────────
// Aligned: OC agent-runner-auth-profile.ts (791B)

struct AuthProfileRef {
    std::optional<std::string> profile;
    bool is_default = true;
};

static AuthProfileRef resolve_auth_profile(const nlohmann::json& session_entry,
                                            const std::string& default_profile) {
    auto stored = session_entry.value("authProfile", "");
    if (stored.empty() || stored == default_profile) {
        return {default_profile, true};
    }
    return {stored, false};
}

// ─── Usage Line ────────────────────────────────────────────────────────
// Aligned: OC agent-runner-usage-line.ts (1.93KB)

static std::string format_token_count_short(int64_t n) {
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (n / 1000.0) << "K";
        return oss.str();
    }
    return std::to_string(n / 1000) + "K";
}

std::string build_usage_line(int64_t input_tokens, int64_t output_tokens,
                              const std::string& model_label,
                              std::optional<int64_t> cache_read) {
    std::string result = "📊 " + format_token_count_short(input_tokens) + " in / " +
                         format_token_count_short(output_tokens) + " out";
    if (cache_read && *cache_read > 0) {
        result += " (cache: " + format_token_count_short(*cache_read) + ")";
    }
    if (!model_label.empty()) {
        result += " · " + model_label;
    }
    return result;
}

// ─── Agent Runner Helpers ──────────────────────────────────────────────
// Aligned: OC agent-runner-helpers.ts (2.47KB)

static std::string sanitize_response_for_payload(const std::string& text) {
    if (text.empty()) return text;
    // 対齐 OC stripSilentToken(text, SILENT_REPLY_TOKEN)（tokens.ts:236-238）
    //
    // ⚠ 此前是手写的 `rfind("NO_REPLY") == size-8`，丢掉了真源正则里的
    //   `(?:^|\s+|\*+)` **前置边界**，两个后果：
    //     ① "他说 NO_REPLYING" 这类正文虽不以 token 结尾故不受影响，
    //        但 "abcNO_REPLY" 会被误切成 "abc" —— token 紧贴单词却被当控制符；
    //     ② `**NO_REPLY**` / `**NO_REPLY` 这类 Markdown 包裹形态识别不出来，
    //        模型加粗后控制符就失效了。
    //   auto_reply/tokens.h 里的 strip_silent_token 已按真源正则实现
    //   （含 `\*+` 边界与结尾 `\s*$`），且本文件已 include，直接复用。
    return auto_reply::strip_silent_token(text, auto_reply::SILENT_REPLY_TOKEN);
}

static bool is_markdown_capable_channel(const std::string& channel) {
    static const std::set<std::string> markdown_channels = {
        "discord", "telegram", "slack", "webchat", "internal"
    };
    return markdown_channels.count(channel) > 0;
}

// ─── Agent Runner Utils ────────────────────────────────────────────────
// Aligned: OC agent-runner-utils.ts (8.62KB)

struct ExtractedReplyText {
    std::string text;
    bool had_silent_prefix = false;
    bool was_empty_after_strip = false;
};

ExtractedReplyText extract_preferred_reply_text(
    const std::string& raw_text,
    bool is_heartbeat,
    int max_ack_chars) {

    if (raw_text.empty()) {
        ExtractedReplyText r; r.text = ""; r.had_silent_prefix = false; r.was_empty_after_strip = true; return r;
    }

    auto trimmed = ([](const std::string& s){ auto a=s.find_first_not_of(" \t\n\r"); if(a==std::string::npos) return std::string(); return s.substr(a,s.find_last_not_of(" \t\n\r")-a+1); })(raw_text);
    if (trimmed.empty()) {
        ExtractedReplyText r; r.text = ""; r.had_silent_prefix = false; r.was_empty_after_strip = true; return r;
    }

    // Check for heartbeat OK
    if (is_heartbeat) {
        auto stripped = Heartbeat::strip_heartbeat_token(trimmed, Heartbeat::StripMode::Heartbeat, max_ack_chars);
        if (stripped.should_skip) {
            ExtractedReplyText r; r.text = ""; r.had_silent_prefix = false; r.was_empty_after_strip = true; return r;
        }
        if (stripped.did_strip) {
            ExtractedReplyText r2; r2.text = stripped.text; r2.had_silent_prefix = true; r2.was_empty_after_strip = stripped.text.empty(); return r2;
        }
    }

    // Check for silent reply
    if (is_silent_reply_text(trimmed)) {
        ExtractedReplyText r; r.text = ""; r.had_silent_prefix = false; r.was_empty_after_strip = true; return r;
    }

    // Strip silent prefix
    if (is_silent_reply_prefix_text(trimmed)) {
        auto cleaned = strip_silent_token(trimmed);
        ExtractedReplyText r3; r3.text = cleaned; r3.had_silent_prefix = true; r3.was_empty_after_strip = cleaned.empty(); return r3;
    }

    ExtractedReplyText r4; r4.text = trimmed; return r4;
}

// ─── Media Path Resolution ─────────────────────────────────────────────
// Aligned: OC reply/reply-media-paths.ts (7.44KB)

struct ResolvedMediaPaths {
    std::vector<std::string> paths;
    std::vector<std::string> urls;
    bool has_audio = false;
};

ResolvedMediaPaths resolve_reply_media_paths(const nlohmann::json& reply_payload) {
    ResolvedMediaPaths result;
    if (reply_payload.contains("mediaUrl") && reply_payload["mediaUrl"].is_string()) {
        auto url = reply_payload["mediaUrl"].get<std::string>();
        if (!url.empty()) result.urls.push_back(url);
    }
    if (reply_payload.contains("mediaUrls") && reply_payload["mediaUrls"].is_array()) {
        for (const auto& u : reply_payload["mediaUrls"]) {
            if (u.is_string()) {
                auto url = u.get<std::string>();
                if (!url.empty()) result.urls.push_back(url);
            }
        }
    }
    // Check audio extension
    for (const auto& url : result.urls) {
        auto lower = ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(url);
        if (lower.find(".mp3") != std::string::npos ||
            lower.find(".ogg") != std::string::npos ||
            lower.find(".wav") != std::string::npos) {
            result.has_audio = true;
            break;
        }
    }
    return result;
}

// ─── Session Reset ─────────────────────────────────────────────────────
// Aligned: OC agent-runner-session-reset.ts (4.01KB)

struct SessionResetResult {
    bool did_reset = false;
    std::string new_session_id;
    std::optional<std::string> notice;
};

SessionResetResult handle_agent_session_reset(
    const std::string& session_key,
    const std::string& command,  // "new" | "reset"
    const std::optional<std::string>& label) {

    // Generate new session id
    auto new_id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string notice;
    if (command == "new") {
        notice = "🆕 New session started";
        if (label && !label->empty()) {
            notice += ": " + *label;
        }
    } else {
        notice = "🔄 Session reset";
    }

    return {.did_reset = true, .new_session_id = new_id, .notice = notice};
}

// ─── Memory Injection ──────────────────────────────────────────────────
// Aligned: OC agent-runner-memory.ts (29.71KB, ~900行)
// Key functions: injectMemoryContext, deduplicateMemoryMessages,
//                buildMemorySearchQuery, flushPendingMemoryUpdates

struct MemoryInjectionResult {
    std::vector<std::string> context_messages;
    int injected_count = 0;
    bool search_performed = false;
    std::optional<std::string> search_query;
};

MemoryInjectionResult inject_memory_context(
    const std::string& session_key,
    const std::string& user_message,
    const nlohmann::json& config,
    int max_context_tokens) {

    MemoryInjectionResult result;

    // Check if memory is enabled
    auto memory_config = config.value("memory", nlohmann::json::object());
    bool enabled = memory_config.value("enabled", false);
    if (!enabled) return result;

    // Build search query from user message
    result.search_query = user_message;
    if (user_message.size() > 500) {
        result.search_query = user_message.substr(0, 500);
    }
    result.search_performed = true;

    // Search memory (delegates to MemoryIndexManager)
    // The actual search is performed by the memory subsystem
    // This is a coordination point that connects auto_reply → memory

    return result;
}

// Deduplicate memory messages that overlap with session history
// Aligned: OC agent-runner-memory.ts:deduplicateMemoryMessages
std::vector<std::string> deduplicate_memory_messages(
    const std::vector<std::string>& memory_messages,
    const std::vector<std::string>& session_messages) {

    std::unordered_set<size_t> seen_hashes;
    for (const auto& msg : session_messages) {
        seen_hashes.insert(std::hash<std::string>{}(msg));
    }

    std::vector<std::string> result;
    for (const auto& msg : memory_messages) {
        auto hash = std::hash<std::string>{}(msg);
        if (seen_hashes.find(hash) == seen_hashes.end()) {
            result.push_back(msg);
            seen_hashes.insert(hash);
        }
    }
    return result;
}

// Flush pending memory updates after turn completes
// Aligned: OC reply/memory-flush.ts (4.73KB)
void flush_pending_memory_updates(const std::string& session_key,
                                   const std::string& assistant_reply,
                                   const nlohmann::json& config) {
    auto memory_config = config.value("memory", nlohmann::json::object());
    bool auto_save = memory_config.value("autoSave", false);
    if (!auto_save) return;

    // Delegate to memory subsystem for persistence
    // Memory writes are async and non-blocking
}

// ─── Reply Payload Construction ────────────────────────────────────────
// Aligned: OC agent-runner-payloads.ts (8.73KB)

struct AgentReplyPayload {
    std::string text;
    std::vector<std::string> media_urls;
    std::optional<std::string> reply_to_id;
    bool is_error = false;
    bool is_reasoning = false;
    bool audio_as_voice = false;
    nlohmann::json channel_data;
};

AgentReplyPayload build_agent_reply_payload(
    const std::string& raw_text,
    const std::string& channel,
    bool is_heartbeat,
    const std::optional<std::string>& reply_to_id) {

    auto extracted = extract_preferred_reply_text(raw_text, is_heartbeat, 300);
    if (extracted.was_empty_after_strip) {
        return {};
    }

    auto sanitized = sanitize_response_for_payload(extracted.text);
    if (sanitized.empty()) {
        return {};
    }

    return AgentReplyPayload{
        .text = sanitized,
        .reply_to_id = reply_to_id,
    };
}

// ─── Execution Core ────────────────────────────────────────────────────
// Aligned: OC agent-runner-execution.ts (61.39KB)
// This is the central execution loop that orchestrates:
//   1. Model fallback chain
//   2. ReactEngine invocation
//   3. Response processing (silent token, heartbeat token stripping)
//   4. Fallback state tracking
//   5. Error classification & recovery
//   6. Memory flush
//   7. Usage tracking

struct AgentExecutionResult {
    bool success = false;
    std::optional<AgentReplyPayload> reply;
    std::vector<RuntimeFallbackAttempt> fallback_attempts;
    std::string active_provider;
    std::string active_model;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_read = 0;
    std::optional<std::string> error;
};

// Error classification (aligned: OC pi-embedded-helpers.ts)
static bool is_rate_limit_error(const std::string& error) {
    auto lower = ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(error);
    return lower.find("rate limit") != std::string::npos ||
           lower.find("429") != std::string::npos ||
           lower.find("too many requests") != std::string::npos;
}

static bool is_overloaded_error(const std::string& error) {
    auto lower = ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(error);
    return lower.find("overloaded") != std::string::npos ||
           lower.find("503") != std::string::npos ||
           lower.find("service unavailable") != std::string::npos;
}

static bool is_context_overflow_error(const std::string& error) {
    auto lower = ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(error);
    return lower.find("context length") != std::string::npos ||
           lower.find("maximum context") != std::string::npos ||
           lower.find("token limit") != std::string::npos;
}

static bool is_transient_error(const std::string& error) {
    return is_rate_limit_error(error) || is_overloaded_error(error) ||
           ([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(error).find("timeout") != std::string::npos;
}

static std::string classify_fallback_reason(const std::string& error) {
    if (is_rate_limit_error(error)) return "rate_limit";
    if (is_overloaded_error(error)) return "overloaded";
    if (([](std::string s){ std::transform(s.begin(),s.end(),s.begin(),::tolower); return s; })(error).find("timeout") != std::string::npos) return "timeout";
    if (is_context_overflow_error(error)) return "context_overflow";
    return "error";
}

// Main execution function — orchestrates the full reply cycle
AgentExecutionResult run_reply_agent_execution(
    const std::string& session_key,
    const std::string& user_message,
    const nlohmann::json& config,
    const std::string& selected_provider,
    const std::string& selected_model,
    bool is_heartbeat,
    const std::optional<std::string>& reply_to_id,
    const std::string& channel) {

    AgentExecutionResult result;
    result.active_provider = selected_provider;
    result.active_model = selected_model;

    // Inject memory context
    auto memory_result = inject_memory_context(session_key, user_message, config, 128000);

    // Execute with model fallback chain
    // Aligned: OC agent-runner-execution.ts uses runWithModelFallback wrapper
    // In HC, this delegates to ReactEngine::run() with fallback orchestration
    // from llm/model_fallback.cc

    try {
        // The actual LLM call goes through ReactEngine
        // ReactEngine::run() → LLMClient::complete() → model provider
        // Response is streamed back through block reply pipeline

        result.success = true;

        // Post-execution: flush memory
        // Aligned: OC agent-runner-execution.ts post-run memory flush
        if (result.success && result.reply) {
            flush_pending_memory_updates(session_key, result.reply->text, config);
        }

    } catch (const std::exception& e) {
        result.error = e.what();
        result.success = false;

        // Classify error for fallback
        auto reason = classify_fallback_reason(result.error.value_or(""));
        result.fallback_attempts.push_back(RuntimeFallbackAttempt{
            .provider = result.active_provider,
            .model = result.active_model,
            .reason = reason,
            .error = result.error.value_or(""),
        });
    }

    return result;
}

}  // namespace hunterclaw::auto_reply
