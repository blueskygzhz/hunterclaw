// ============================================================================
// heartbeat_events_filter.cc
// Aligned with openclaw: src/infra/heartbeat-events-filter.ts
// ============================================================================

#include "hunterclaw/infra/heartbeat_events_filter.h"
#include "hunterclaw/auto_reply/tokens.h"
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace infra {

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

// ── is_exec_completion_event ─────────────────────────────────────────────────
// Aligned with openclaw: isExecCompletionEvent

bool is_exec_completion_event(const std::string& evt) {
    return to_lower(evt).find("exec finished") != std::string::npos;
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

std::string build_exec_event_prompt(bool deliver_to_user) {
    if (!deliver_to_user) {
        return "An async command you ran earlier has completed. The result is shown in the system messages above. "
               "Handle the result internally. Do not relay it to the user unless explicitly requested.";
    }
    return "An async command you ran earlier has completed. The result is shown in the system messages above. "
           "Please relay the command output to the user in a helpful way. If the command succeeded, share the relevant output. "
           "If it failed, explain what went wrong.";
}

} // namespace infra
} // namespace hunterclaw
