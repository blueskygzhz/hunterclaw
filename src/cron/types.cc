#include "hunterclaw/cron/types.h"

namespace hunterclaw {
namespace cron {

using json = nlohmann::json;

// ============================================================================
// Schedule Helpers
// ============================================================================

Schedule Schedule::at_time(const std::string& timestamp) {
    Schedule s;
    s.kind = ScheduleKind::At;
    s.at = timestamp;
    return s;
}

Schedule Schedule::every_interval(int64_t interval_ms, std::optional<int64_t> anchor) {
    Schedule s;
    s.kind = ScheduleKind::Every;
    s.every_ms = interval_ms;
    s.anchor_ms = anchor;
    return s;
}

Schedule Schedule::cron_expr(const std::string& expression, 
                       std::optional<std::string> timezone,
                       std::optional<int64_t> stagger) {
    Schedule s;
    s.kind = ScheduleKind::Cron;
    s.expr = expression;
    s.tz = timezone;
    s.stagger_ms = stagger;
    return s;
}

// ============================================================================
// Payload Helpers
// ============================================================================

Payload Payload::agent_turn(const std::string& msg) {
    Payload p;
    p.kind = Kind::AgentTurn;
    p.message = msg;
    return p;
}

Payload Payload::system_event(const std::string& text) {
    Payload p;
    p.kind = Kind::SystemEvent;
    p.message = text;
    return p;
}

// ============================================================================
// Delivery Helpers
// ============================================================================

Delivery Delivery::none() {
    Delivery d;
    d.mode = DeliveryMode::None;
    return d;
}

Delivery Delivery::announce(const std::string& channel, const std::string& to) {
    Delivery d;
    d.mode = DeliveryMode::Announce;
    d.channel = channel;
    d.to = to;
    return d;
}

Delivery Delivery::webhook(const std::string& url) {
    Delivery d;
    d.mode = DeliveryMode::Webhook;
    d.webhook_url = url;
    return d;
}

// ============================================================================
// JSON Conversion - Schedule
// ============================================================================

void to_json(json& j, const Schedule& s) {
    switch (s.kind) {
        case ScheduleKind::At:
            j = {
                {"kind", "at"},
                {"at", *s.at}
            };
            break;
            
        case ScheduleKind::Every:
            j = {
                {"kind", "every"},
                {"everyMs", *s.every_ms}
            };
            if (s.anchor_ms) {
                j["anchorMs"] = *s.anchor_ms;
            }
            break;
            
        case ScheduleKind::Cron:
            j = {
                {"kind", "cron"},
                {"expr", *s.expr}
            };
            if (s.tz) {
                j["tz"] = *s.tz;
            }
            if (s.stagger_ms) {
                j["staggerMs"] = *s.stagger_ms;
            }
            break;
    }
}

void from_json(const json& j, Schedule& s) {
    std::string kind = j.at("kind").get<std::string>();
    
    if (kind == "at") {
        s.kind = ScheduleKind::At;
        s.at = j.at("at").get<std::string>();
    } else if (kind == "every") {
        s.kind = ScheduleKind::Every;
        s.every_ms = j.at("everyMs").get<int64_t>();
        if (j.contains("anchorMs")) {
            s.anchor_ms = j.at("anchorMs").get<int64_t>();
        }
    } else if (kind == "cron") {
        s.kind = ScheduleKind::Cron;
        s.expr = j.at("expr").get<std::string>();
        if (j.contains("tz")) {
            s.tz = j.at("tz").get<std::string>();
        }
        if (j.contains("staggerMs")) {
            s.stagger_ms = j.at("staggerMs").get<int64_t>();
        }
    }
}

// ============================================================================
// JSON Conversion - Payload
// ============================================================================

void to_json(json& j, const Payload& p) {
    j = {
        {"kind", p.kind == Payload::Kind::AgentTurn ? "agentTurn" : "systemEvent"},
        {"message", p.message}
    };
    
    if (p.model) j["model"] = *p.model;
    if (p.thinking) j["thinking"] = *p.thinking;
    if (p.timeout_seconds) j["timeoutSeconds"] = *p.timeout_seconds;
}

void from_json(const json& j, Payload& p) {
    std::string kind = j.at("kind").get<std::string>();
    p.kind = (kind == "agentTurn") ? Payload::Kind::AgentTurn : Payload::Kind::SystemEvent;
    p.message = j.at("message").get<std::string>();
    
    if (j.contains("model")) p.model = j.at("model").get<std::string>();
    if (j.contains("thinking")) p.thinking = j.at("thinking").get<bool>();
    if (j.contains("timeoutSeconds")) p.timeout_seconds = j.at("timeoutSeconds").get<int>();
}

// ============================================================================
// JSON Conversion - Delivery
// ============================================================================

void to_json(json& j, const Delivery& d) {
    std::string mode_str;
    switch (d.mode) {
        case DeliveryMode::None: mode_str = "none"; break;
        case DeliveryMode::Announce: mode_str = "announce"; break;
        case DeliveryMode::Webhook: mode_str = "webhook"; break;
    }
    
    j = {{"mode", mode_str}};
    
    if (d.channel) j["channel"] = *d.channel;
    if (d.to) j["to"] = *d.to;
    if (d.account_id) j["accountId"] = *d.account_id;
    if (d.webhook_url) j["webhookUrl"] = *d.webhook_url;
}

void from_json(const json& j, Delivery& d) {
    std::string mode = j.at("mode").get<std::string>();
    
    if (mode == "none") d.mode = DeliveryMode::None;
    else if (mode == "announce") d.mode = DeliveryMode::Announce;
    else if (mode == "webhook") d.mode = DeliveryMode::Webhook;
    
    if (j.contains("channel")) d.channel = j.at("channel").get<std::string>();
    if (j.contains("to")) d.to = j.at("to").get<std::string>();
    if (j.contains("accountId")) d.account_id = j.at("accountId").get<std::string>();
    if (j.contains("webhookUrl")) d.webhook_url = j.at("webhookUrl").get<std::string>();
}

// ============================================================================
// JSON Conversion - JobState
// ============================================================================

void to_json(json& j, const JobState& s) {
    j = json::object();
    
    if (s.next_run_at_ms) j["nextRunAtMs"] = *s.next_run_at_ms;
    if (s.last_run_at_ms) j["lastRunAtMs"] = *s.last_run_at_ms;
    if (s.last_run_status) j["lastRunStatus"] = *s.last_run_status;
    if (s.last_run_error) j["lastRunError"] = *s.last_run_error;
    if (s.last_run_summary) j["lastRunSummary"] = *s.last_run_summary;
    if (s.retry_count) j["retryCount"] = *s.retry_count;
    if (s.retry_at_ms) j["retryAtMs"] = *s.retry_at_ms;
    if (s.consecutive_failures) j["consecutiveFailures"] = *s.consecutive_failures;
    if (s.last_alert_at_ms) j["lastAlertAtMs"] = *s.last_alert_at_ms;
}

void from_json(const json& j, JobState& s) {
    if (j.contains("nextRunAtMs")) s.next_run_at_ms = j.at("nextRunAtMs").get<int64_t>();
    if (j.contains("lastRunAtMs")) s.last_run_at_ms = j.at("lastRunAtMs").get<int64_t>();
    if (j.contains("lastRunStatus")) s.last_run_status = j.at("lastRunStatus").get<std::string>();
    if (j.contains("lastRunError")) s.last_run_error = j.at("lastRunError").get<std::string>();
    if (j.contains("lastRunSummary")) s.last_run_summary = j.at("lastRunSummary").get<std::string>();
    if (j.contains("retryCount")) s.retry_count = j.at("retryCount").get<int>();
    if (j.contains("retryAtMs")) s.retry_at_ms = j.at("retryAtMs").get<int64_t>();
    if (j.contains("consecutiveFailures")) s.consecutive_failures = j.at("consecutiveFailures").get<int>();
    if (j.contains("lastAlertAtMs")) s.last_alert_at_ms = j.at("lastAlertAtMs").get<int64_t>();
}

// ============================================================================
// JSON Conversion - Job
// ============================================================================

void to_json(json& j, const Job& job) {
    j = {
        {"id", job.id},
        {"name", job.name},
        {"enabled", job.enabled},
        {"schedule", job.schedule},
        {"sessionTarget", job.session_target == SessionTarget::Main ? "main" : "isolated"},
        {"wakeMode", job.wake_mode == WakeMode::Now ? "now" : "next-heartbeat"},
        {"payload", job.payload},
        {"state", job.state},
        {"createdAtMs", job.created_at_ms},
        {"updatedAtMs", job.updated_at_ms}
    };
    
    if (job.delivery) j["delivery"] = *job.delivery;
    if (job.agent_id) j["agentId"] = *job.agent_id;
    if (job.session_key) j["sessionKey"] = *job.session_key;
    if (job.lane) j["lane"] = *job.lane;
}

void from_json(const json& j, Job& job) {
    job.id = j.at("id").get<std::string>();
    job.name = j.at("name").get<std::string>();
    job.enabled = j.at("enabled").get<bool>();
    job.schedule = j.at("schedule").get<Schedule>();
    
    std::string session_target = j.at("sessionTarget").get<std::string>();
    job.session_target = (session_target == "main") ? SessionTarget::Main : SessionTarget::Isolated;
    
    std::string wake_mode = j.at("wakeMode").get<std::string>();
    job.wake_mode = (wake_mode == "now") ? WakeMode::Now : WakeMode::NextHeartbeat;
    
    job.payload = j.at("payload").get<Payload>();
    job.state = j.at("state").get<JobState>();
    job.created_at_ms = j.at("createdAtMs").get<int64_t>();
    job.updated_at_ms = j.at("updatedAtMs").get<int64_t>();
    
    if (j.contains("delivery")) job.delivery = j.at("delivery").get<Delivery>();
    if (j.contains("agentId")) job.agent_id = j.at("agentId").get<std::string>();
    if (j.contains("sessionKey")) job.session_key = j.at("sessionKey").get<std::string>();
    if (j.contains("lane")) job.lane = j.at("lane").get<std::string>();
}

// ============================================================================
// RunLogEntry
// ============================================================================

json RunLogEntry::to_json() const {
    json j = {
        {"ts", ts},
        {"jobId", job_id},
        {"action", action},
        {"status", status}
    };
    
    if (error) j["error"] = *error;
    if (summary) j["summary"] = *summary;
    if (delivered) j["delivered"] = *delivered;
    if (session_id) j["sessionId"] = *session_id;
    if (run_at_ms) j["runAtMs"] = *run_at_ms;
    if (duration_ms) j["durationMs"] = *duration_ms;
    if (next_run_at_ms) j["nextRunAtMs"] = *next_run_at_ms;
    if (model) j["model"] = *model;
    
    return j;
}

RunLogEntry RunLogEntry::from_json(const json& j) {
    RunLogEntry entry;
    entry.ts = j.at("ts").get<int64_t>();
    entry.job_id = j.at("jobId").get<std::string>();
    entry.action = j.at("action").get<std::string>();
    entry.status = j.at("status").get<std::string>();
    
    if (j.contains("error")) entry.error = j.at("error").get<std::string>();
    if (j.contains("summary")) entry.summary = j.at("summary").get<std::string>();
    if (j.contains("delivered")) entry.delivered = j.at("delivered").get<bool>();
    if (j.contains("sessionId")) entry.session_id = j.at("sessionId").get<std::string>();
    if (j.contains("runAtMs")) entry.run_at_ms = j.at("runAtMs").get<int64_t>();
    if (j.contains("durationMs")) entry.duration_ms = j.at("durationMs").get<int64_t>();
    if (j.contains("nextRunAtMs")) entry.next_run_at_ms = j.at("nextRunAtMs").get<int64_t>();
    if (j.contains("model")) entry.model = j.at("model").get<std::string>();
    
    return entry;
}

} // namespace cron
} // namespace hunterclaw
