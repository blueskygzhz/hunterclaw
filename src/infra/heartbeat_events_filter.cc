// ============================================================================
// heartbeat_events_filter.cc
// Aligned with openclaw: src/infra/heartbeat-events-filter.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_events_filter.h"
#include "hunterclaw/auto_reply/tokens.h"
#include <algorithm>
#include <regex>
#include <sstream>

namespace hunterclaw {
namespace infra {

// ── Constants ────────────────────────────────────────────────────────────────

// Aligned with openclaw: const MAX_EXEC_EVENT_PROMPT_CHARS = 8_000
static constexpr size_t MAX_EXEC_EVENT_PROMPT_CHARS = 8000;

// Aligned with openclaw: STRUCTURED_EXEC_COMPLETION_EVENT_RE
//   /^exec (completed|failed) \(([a-z0-9_-]{1,64}), (code -?\d+|signal [^)]+)\)(?: :: ([\s\S]*))?$/i
// C++ std::regex 无 /s 标志，用 [\s\S] 等价写法覆盖换行。
static const std::regex& structured_exec_completion_re() {
    static const std::regex re(
        R"(^exec (completed|failed) \(([a-z0-9_-]{1,64}), (code -?\d+|signal [^)]+)\)(?: :: ([\s\S]*))?$)",
        std::regex::icase);
    return re;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static std::string trim_str(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// ── isHeartbeatAckEvent ──────────────────────────────────────────────────────
// Aligned with openclaw: isHeartbeatAckEvent

static bool is_heartbeat_ack_event(const std::string& evt) {
    const std::string trimmed = trim_str(evt);
    if (trimmed.empty()) return false;
    const std::string lower = to_lower(trimmed);
    // Aligned with openclaw: HEARTBEAT_OK_PREFIX = HEARTBEAT_TOKEN.toLowerCase()
    const std::string prefix = to_lower(auto_reply::HEARTBEAT_TOKEN);
    if (lower.rfind(prefix, 0) != 0) return false;
    const std::string suffix = lower.substr(prefix.size());
    if (suffix.empty()) return true;
    // Aligned with openclaw: !/[a-z0-9_]/.test(suffix[0])
    const char c = suffix[0];
    return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
}

// ── isHeartbeatNoiseEvent ────────────────────────────────────────────────────
// Aligned with openclaw: isHeartbeatNoiseEvent

static bool is_heartbeat_noise_event(const std::string& evt) {
    const std::string lower = to_lower(trim_str(evt));
    if (lower.empty()) return false;
    return is_heartbeat_ack_event(lower)
        || lower.find("heartbeat poll") != std::string::npos
        || lower.find("heartbeat wake") != std::string::npos;
}

// ── StructuredExecCompletionEvent ────────────────────────────────────────────
// Aligned with openclaw: type StructuredExecCompletionEvent + parseStructuredExecCompletionEvent

struct StructuredExecCompletionEvent {
    std::string raw;
    std::string action;
    std::string id;
    std::string result;
    std::string output;
    bool succeeded = false;
};

// Aligned with openclaw: parseStructuredExecCompletionEvent (heartbeat-events-filter.ts:20-36)
static std::optional<StructuredExecCompletionEvent>
parse_structured_exec_completion_event(const std::string& evt) {
    const std::string trimmed = trim_str(evt);
    std::smatch m;
    if (!std::regex_match(trimmed, m, structured_exec_completion_re())) {
        return std::nullopt;
    }
    StructuredExecCompletionEvent parsed;
    parsed.raw = trimmed;
    parsed.action = m[1].matched ? m[1].str() : "";
    parsed.id = m[2].matched ? m[2].str() : "";
    parsed.result = m[3].matched ? m[3].str() : "";
    parsed.output = trim_str(m[4].matched ? m[4].str() : "");
    // Aligned with openclaw: action==="completed" && result==="code 0"
    parsed.succeeded =
        to_lower(parsed.action) == "completed" && to_lower(parsed.result) == "code 0";
    return parsed;
}

// ── is_exec_completion_event ─────────────────────────────────────────────────
// Aligned with openclaw: isExecCompletionEvent (heartbeat-events-filter.ts:204-211)
//   /^exec finished(?::|\s*\()/.test(normalized) || STRUCTURED_EXEC_COMPLETION_EVENT_RE.test(trimmed)
//
// 旧实现只做 find("exec finished") 子串匹配：既漏掉结构化事件
// （"exec completed (id, code 0)"），又会把正文里偶然出现该短语的
// 普通事件误判为 exec 完成。

bool is_exec_completion_event(const std::string& evt) {
    // trimStart 语义：仅去头部空白
    size_t start = evt.find_first_not_of(" \t\n\r");
    const std::string trimmed_start = (start == std::string::npos) ? "" : evt.substr(start);
    const std::string normalized = to_lower(trimmed_start);

    static const std::regex exec_finished_re(R"(^exec finished(?::|\s*\())");
    if (std::regex_search(normalized, exec_finished_re)) return true;

    return std::regex_match(trim_str(evt), structured_exec_completion_re());
}

// ── is_relayable_exec_completion_event ───────────────────────────────────────
// Aligned with openclaw: isRelayableExecCompletionEvent (heartbeat-events-filter.ts:38-47)

bool is_relayable_exec_completion_event(const std::string& evt) {
    auto parsed = parse_structured_exec_completion_event(evt);
    if (!parsed) return is_exec_completion_event(evt);
    if (!parsed->output.empty()) return true;
    return !parsed->succeeded;
}

// ── is_cron_system_event ─────────────────────────────────────────────────────
// Aligned with openclaw: isCronSystemEvent

bool is_cron_system_event(const std::string& evt) {
    if (trim_str(evt).empty()) return false;
    return !is_heartbeat_noise_event(evt) && !is_exec_completion_event(evt);
}

// ── build_cron_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildCronEventPrompt

std::string build_cron_event_prompt(
    const std::vector<std::string>& pending_events,
    bool deliver_to_user
) {
    // Join events
    std::string event_text;
    for (size_t i = 0; i < pending_events.size(); ++i) {
        if (i > 0) event_text += "\n";
        event_text += pending_events[i];
    }
    // trim
    size_t s = event_text.find_first_not_of(" \t\n\r");
    if (s != std::string::npos) {
        size_t e = event_text.find_last_not_of(" \t\n\r");
        event_text = event_text.substr(s, e - s + 1);
    } else {
        event_text = "";
    }

    if (event_text.empty()) {
        if (!deliver_to_user) {
            return "A scheduled cron event was triggered, but no event content was found. "
                   "Handle this internally and reply HEARTBEAT_OK when nothing needs user-facing follow-up.";
        }
        return "A scheduled cron event was triggered, but no event content was found. "
               "Reply HEARTBEAT_OK.";
    }

    if (!deliver_to_user) {
        return "A scheduled reminder has been triggered. The reminder content is:\n\n"
             + event_text
             + "\n\nHandle this reminder internally. Do not relay it to the user unless explicitly requested.";
    }
    return "A scheduled reminder has been triggered. The reminder content is:\n\n"
         + event_text
         + "\n\nPlease relay this reminder to the user in a helpful and friendly way.";
}

// ── build_exec_event_prompt ──────────────────────────────────────────────────
// Aligned with openclaw: buildExecEventPrompt

// Aligned with openclaw: formatExecEventPromptText (heartbeat-events-filter.ts:49-72)
// 返回 {text, has_missing_output_failure}
static void format_exec_event_prompt_text(
    const std::vector<std::string>& pending_events,
    std::string& out_text,
    bool& out_has_missing_output_failure
) {
    out_has_missing_output_failure = false;
    std::vector<std::string> lines;
    for (const auto& event : pending_events) {
        auto parsed = parse_structured_exec_completion_event(event);
        if (!parsed) {
            // 非结构化事件：原样保留（去空白后非空才收）
            const std::string trimmed = trim_str(event);
            if (!trimmed.empty()) lines.push_back(trimmed);
            continue;
        }
        if (!parsed->output.empty()) {
            lines.push_back(parsed->raw);
            continue;
        }
        if (parsed->succeeded) {
            // 成功且无输出 → 无需惊动模型，整条丢弃
            continue;
        }
        out_has_missing_output_failure = true;
        lines.push_back("Exec " + parsed->action + " (" + parsed->id + ", " + parsed->result +
                        ") without captured stdout/stderr.");
    }
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) joined += "\n";
        joined += lines[i];
    }
    out_text = trim_str(joined);
}

// Aligned with openclaw: buildExecEventPrompt (heartbeat-events-filter.ts:119-172)
//
// ⚠ 旧实现只收 deliver_to_user，文案写 "The result is shown in the system messages
//   above" —— 但系统事件队列从不注入 LLM 上下文，模型实际看不到任何结果。
//   真源在 buildCronEventPrompt 的注释里明确否决了这种写法：
//     "This ensures the model sees the reminder text directly instead of relying on
//      'shown in the system messages above' which may not be visible in context."
//   现改为把事件文本直接嵌入 prompt。
std::string build_exec_event_prompt(
    const std::vector<std::string>& pending_events,
    bool deliver_to_user
) {
    std::string raw_event_text;
    bool has_missing_output_failure = false;
    format_exec_event_prompt_text(pending_events, raw_event_text, has_missing_output_failure);

    // Aligned with openclaw: truncateUtf16Safe + "\n\n[truncated]"
    std::string event_text = raw_event_text;
    if (event_text.size() > MAX_EXEC_EVENT_PROMPT_CHARS) {
        size_t cut = MAX_EXEC_EVENT_PROMPT_CHARS;
        // 不在 UTF-8 continuation byte (10xxxxxx) 中间切断
        while (cut > 0 && (static_cast<unsigned char>(event_text[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        event_text = event_text.substr(0, cut) + "\n\n[truncated]";
    }

    if (event_text.empty()) {
        return "An async command completion event was triggered, but no command output was found. "
               "Reply HEARTBEAT_OK only. Do not mention, summarize, or reuse output from any earlier run.";
    }

    if (!deliver_to_user) {
        return "An async command completion event was triggered, but user delivery is disabled for this run. "
               "Handle the result internally and reply HEARTBEAT_OK only. "
               "Do not mention, summarize, or reuse command output.";
    }

    if (has_missing_output_failure) {
        return "An async command you ran earlier completed without captured stdout/stderr. "
               "The completion details are:\n\n" + event_text + "\n\n" +
               "Tell the user the command completed without captured output and include the exit status or signal. "
               "Do not ask the user to provide missing logs, and do not try to retrieve logs from an exec/session id.";
    }

    return "An async command you ran earlier has completed. The command completion details are:\n\n" +
           event_text + "\n\n" +
           "Please relay the command output to the user in a helpful way. "
           "If the command succeeded, share the relevant output. If it failed, explain what went wrong.";
}

} // namespace infra
} // namespace hunterclaw
