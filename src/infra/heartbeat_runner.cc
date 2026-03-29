// ============================================================================
// heartbeat_runner.cc
// Aligned with openclaw: src/infra/heartbeat-runner.ts
//
// Implements startHeartbeatRunner and runHeartbeatOnce.
// ============================================================================

#include "hunterclaw/infra/heartbeat_runner.h"
#include "hunterclaw/infra/heartbeat_events.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include "hunterclaw/infra/heartbeat_active_hours.h"
#include "hunterclaw/infra/heartbeat_reason.h"
#include "hunterclaw/infra/heartbeat_events_filter.h"
#include "hunterclaw/infra/system_events.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>
#include <map>
#include <algorithm>
#include <stdexcept>

namespace hunterclaw {
namespace infra {

// ── Helpers ──────────────────────────────────────────────────────────────────

static int64_t default_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Aligned with openclaw: parseDurationMs (simplified)
// Supports: "30m", "1h", "60s", "3600" (seconds default)
static int64_t parse_duration_ms(const std::string& raw) {
    if (raw.empty()) return -1;
    std::string s = raw;
    // trim
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return -1;
    size_t end = s.find_last_not_of(" \t\n\r");
    s = s.substr(start, end - start + 1);
    if (s.empty()) return -1;

    char unit = s.back();
    std::string num_str;
    int64_t multiplier = 60 * 1000;  // default: minutes

    if (unit == 'm' || unit == 'M') {
        num_str = s.substr(0, s.size() - 1);
        multiplier = 60 * 1000;
    } else if (unit == 'h' || unit == 'H') {
        num_str = s.substr(0, s.size() - 1);
        multiplier = 3600 * 1000;
    } else if (unit == 's' || unit == 'S') {
        num_str = s.substr(0, s.size() - 1);
        multiplier = 1000;
    } else if (unit >= '0' && unit <= '9') {
        num_str = s;
        multiplier = 60 * 1000;  // default unit: minutes (aligned with openclaw)
    } else {
        return -1;
    }

    try {
        int64_t val = std::stoll(num_str);
        if (val <= 0) return -1;
        return val * multiplier;
    } catch (...) {
        return -1;
    }
}

// Aligned with openclaw: appendCronStyleCurrentTimeLine
// Appends "Current time: <formatted> (<tz>) / <utc>" to the prompt if not already present.
static std::string append_cron_style_current_time_line(
    const std::string& text,
    const std::string& user_timezone,
    int64_t now_ms
) {
    // Trim trailing whitespace
    std::string base = text;
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t'
                              || base.back() == '\n' || base.back() == '\r')) {
        base.pop_back();
    }
    if (base.empty() || base.find("Current time:") != std::string::npos) {
        return base;
    }
    // Format UTC time: "YYYY-MM-DD HH:MM UTC"
    time_t t = static_cast<time_t>(now_ms / 1000);
    struct tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char utc_buf[32];
    strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M UTC", &tm_utc);

    // Use user_timezone as label; formatted time is UTC for simplicity
    // (full tz-aware formatting would require a tz library)
    const std::string& tz = user_timezone.empty() ? "UTC" : user_timezone;
    std::string time_line = std::string("Current time: ") + utc_buf + " (" + tz + ") / " + utc_buf;
    return base + "\n" + time_line;
}

// Aligned with openclaw: resolveHeartbeatIntervalMs
static int64_t resolve_heartbeat_interval_ms(const HeartbeatAgentConfig& agent) {
    const std::string raw = agent.every.value_or("30m");
    return parse_duration_ms(raw);
}

// ── HeartbeatAgentState ──────────────────────────────────────────────────────
// Aligned with openclaw: HeartbeatAgentState

struct HeartbeatAgentState {
    HeartbeatAgentConfig config;
    int64_t interval_ms = 0;
    std::optional<int64_t> last_run_ms;
    int64_t next_due_ms = 0;
};

// ── run_heartbeat_once ───────────────────────────────────────────────────────
// Aligned with openclaw: runHeartbeatOnce
//
// Core heartbeat execution function. Mirrors the full emit logic of OpenClaw:
//   1. Global enable check
//   2. Active hours check
//   3. Queue size check (skip if busy)
//   4. Preflight: reason flags, pending events, HEARTBEAT.md gate
//   5. Visibility check
//   6. Invoke the LLM reply callback (opts.reply_fn)
//   7. Normalize reply → emit ok-empty / ok-token / duplicate / no-target /
//      alerts-disabled / sent / failed

HeartbeatRunResult run_heartbeat_once(
    const HeartbeatRunnerConfig& cfg,
    const HeartbeatAgentConfig& agent,
    const HeartbeatRunOnceOptions& opts
) {
    // Aligned with openclaw: if (!areHeartbeatsEnabled()) return skipped/disabled
    if (!are_heartbeats_enabled()) {
        return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
    }

    const int64_t started_at = opts.now_ms ? opts.now_ms() : default_now_ms();

    // Aligned with openclaw: isWithinActiveHours check
    if (!is_within_active_hours(agent.active_hours, cfg.user_timezone, started_at)) {
        return {HeartbeatRunResult::Status::Skipped, std::nullopt, "quiet-hours"};
    }

    // Aligned with openclaw: queueSize > 0 → skipped/requests-in-flight
    if (opts.get_queue_size) {
        const int queue_size = opts.get_queue_size("main");
        if (queue_size > 0) {
            return {HeartbeatRunResult::Status::Skipped, std::nullopt, "requests-in-flight"};
        }
    }

    // ── Preflight ────────────────────────────────────────────────────────────
    // Aligned with openclaw: resolveHeartbeatReasonFlags + resolveHeartbeatPreflight

    const std::string reason = opts.reason.value_or("");
    const auto reason_kind = resolve_heartbeat_reason_kind(reason);
    const bool is_exec_event_reason = (reason_kind == HeartbeatReasonKind::ExecEvent);
    const bool is_cron_event_reason = (reason_kind == HeartbeatReasonKind::Cron);
    const bool is_wake_reason = (reason_kind == HeartbeatReasonKind::Wake
                               || reason_kind == HeartbeatReasonKind::Hook);

    const std::string session_key = opts.session_key.value_or("");
    const auto pending_events = session_key.empty()
        ? std::vector<SystemEvent>{}
        : peek_system_event_entries(session_key);

    const bool has_tagged_cron_events = std::any_of(
        pending_events.begin(), pending_events.end(),
        [](const SystemEvent& e) {
            return e.context_key.has_value()
                && e.context_key->rfind("cron:", 0) == 0;
        }
    );

    const bool should_bypass_file_gates =
        is_exec_event_reason || is_cron_event_reason || is_wake_reason || has_tagged_cron_events;

    // Aligned with openclaw: HEARTBEAT.md empty check → emit(skipped, empty-heartbeat-file)
    if (!should_bypass_file_gates && !agent.workspace_path.empty()) {
        const std::string heartbeat_md_path = agent.workspace_path + "/HEARTBEAT.md";
        // Check if file exists and is effectively empty via opts.check_heartbeat_file
        if (opts.check_heartbeat_file) {
            const bool effectively_empty = opts.check_heartbeat_file(heartbeat_md_path);
            if (effectively_empty) {
                emit_heartbeat_event({
                    /*ts=*/0,
                    /*status=*/"skipped",
                    /*to=*/std::nullopt,
                    /*account_id=*/agent.account_id,
                    /*preview=*/std::nullopt,
                    /*duration_ms=*/static_cast<int64_t>(default_now_ms() - started_at),
                    /*has_media=*/false,
                    /*reason=*/"empty-heartbeat-file",
                    /*channel=*/std::nullopt,
                    /*silent=*/std::nullopt,
                    /*indicator_type=*/std::nullopt,
                });
                return {HeartbeatRunResult::Status::Skipped, std::nullopt, "empty-heartbeat-file"};
            }
        }
    }

    // ── Visibility ───────────────────────────────────────────────────────────
    // Aligned with openclaw: resolveHeartbeatVisibility
    // For simplicity, use agent-level show_ok/show_alerts/use_indicator if set.
    const bool show_ok       = agent.show_ok.value_or(false);
    const bool show_alerts   = agent.show_alerts.value_or(true);
    const bool use_indicator = agent.use_indicator.value_or(true);

    // Aligned with openclaw: if (!showAlerts && !showOk && !useIndicator) → emit(skipped, alerts-disabled)
    if (!show_alerts && !show_ok && !use_indicator) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"skipped",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/std::nullopt,
            /*duration_ms=*/static_cast<int64_t>(default_now_ms() - started_at),
            /*has_media=*/false,
            /*reason=*/"alerts-disabled",
            /*channel=*/std::nullopt,
            /*silent=*/std::nullopt,
            /*indicator_type=*/std::nullopt,
        });
        return {HeartbeatRunResult::Status::Skipped, std::nullopt, "alerts-disabled"};
    }

    // ── LLM Reply ────────────────────────────────────────────────────────────
    // Aligned with openclaw: getReplyFromConfig → resolveHeartbeatReplyPayload
    // The actual LLM call is injected via opts.reply_fn.
    // If no reply_fn is provided, we cannot proceed with a real LLM call here;
    // the caller (e.g. gateway) is expected to inject it.

    if (!opts.reply_fn) {
        // No LLM callback injected — this is a dry-run / scheduler-only context.
        // Emit a "skipped/no-reply-fn" event so the event stream stays consistent.
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"skipped",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/std::nullopt,
            /*duration_ms=*/static_cast<int64_t>(default_now_ms() - started_at),
            /*has_media=*/false,
            /*reason=*/"no-reply-fn",
            /*channel=*/std::nullopt,
            /*silent=*/std::nullopt,
            /*indicator_type=*/std::nullopt,
        });
        return {HeartbeatRunResult::Status::Skipped, std::nullopt, "no-reply-fn"};
    }

    // Build the prompt for this heartbeat run.
    // Aligned with openclaw: resolveHeartbeatRunPrompt
    const bool should_inspect_pending =
        is_exec_event_reason || is_cron_event_reason || has_tagged_cron_events;

    std::vector<std::string> pending_texts;
    if (should_inspect_pending) {
        for (const auto& e : pending_events) pending_texts.push_back(e.text);
    }

    std::vector<std::string> cron_events;
    for (const auto& e : pending_events) {
        const bool is_cron_ctx = e.context_key.has_value()
            && e.context_key->rfind("cron:", 0) == 0;
        if ((is_cron_event_reason || is_cron_ctx) && is_cron_system_event(e.text)) {
            cron_events.push_back(e.text);
        }
    }

    const bool has_exec_completion = std::any_of(
        pending_texts.begin(), pending_texts.end(), is_exec_completion_event);
    const bool has_cron_events = !cron_events.empty();

    const std::string prompt = has_exec_completion
        ? build_exec_event_prompt(show_alerts)
        : has_cron_events
            ? build_cron_event_prompt(cron_events, show_alerts)
            : agent.prompt.value_or("");

    // ── Build HeartbeatReplyRequest (aligned with openclaw: ctx) ─────────────
    // Aligned with openclaw:
    //   ctx.Body                = appendCronStyleCurrentTimeLine(prompt, cfg, startedAt)
    //   ctx.From / ctx.To       = sender (resolveHeartbeatSenderContext)
    //   ctx.OriginatingChannel  = delivery.channel (if not "none")
    //   ctx.OriginatingTo       = delivery.to
    //   ctx.AccountId           = delivery.accountId
    //   ctx.MessageThreadId     = delivery.threadId
    //   ctx.Provider            = "heartbeat" | "exec-event" | "cron-event"
    //   ctx.SessionKey          = sessionKey
    //
    // The gateway-injected reply_fn is responsible for:
    //   1. Resolving delivery target (resolveHeartbeatDeliveryTarget)
    //   2. Resolving sender context  (resolveHeartbeatSenderContext)
    //   3. Appending current-time line (appendCronStyleCurrentTimeLine)
    //   4. Calling ReplyGenerator::get_reply_from_config with the full ctx
    //   5. Calling resolveHeartbeatReplyPayload + normalizeHeartbeatReply
    //   6. Handling transcript prune + session updatedAt restore
    //   7. Populating HeartbeatReplyResult fields

    HeartbeatReplyRequest req;
    // Aligned with openclaw: ctx.Body = appendCronStyleCurrentTimeLine(prompt, cfg, startedAt)
    req.prompt          = append_cron_style_current_time_line(prompt, cfg.user_timezone, started_at);
    req.session_key     = session_key;
    req.agent_id        = agent.agent_id;
    req.reason          = reason;
    req.is_exec_event   = has_exec_completion;
    req.is_cron_event   = has_cron_events;
    // Provider tag: aligned with openclaw ctx.Provider
    req.provider = has_exec_completion ? "exec-event"
                 : has_cron_events     ? "cron-event"
                 :                       "heartbeat";
    // Model / context overrides: aligned with openclaw replyOpts
    req.model_override                = agent.model;
    req.suppress_tool_error_warnings  = agent.suppress_tool_error_warnings.value_or(false);
    req.light_context                 = agent.light_context.value_or(false);
    // Delivery target fields are resolved by the gateway reply_fn using
    // resolveHeartbeatDeliveryTarget + resolveHeartbeatSenderContext.
    // We pass account_id here so the gateway can use it as a hint.
    req.account_id = agent.account_id;

    // Invoke the LLM reply callback.
    HeartbeatReplyResult reply_result;
    try {
        reply_result = opts.reply_fn(req);
    } catch (const std::exception& ex) {
        // Aligned with openclaw: catch → emit(failed)
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"failed",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/std::nullopt,
            /*duration_ms=*/static_cast<int64_t>(default_now_ms() - started_at),
            /*has_media=*/false,
            /*reason=*/ex.what(),
            /*channel=*/reply_result.channel,
            /*silent=*/std::nullopt,
            /*indicator_type=*/use_indicator
                ? resolve_indicator_type("failed") : std::optional<HeartbeatIndicatorType>{},
        });
        return {HeartbeatRunResult::Status::Failed, std::nullopt, ex.what()};
    }

    const int64_t duration_ms = static_cast<int64_t>(default_now_ms() - started_at);

    // ── ok-empty: reply has no text and no media ──────────────────────────────
    // Aligned with openclaw: !replyPayload || (!text && !mediaUrl && !mediaUrls)
    if (reply_result.text.empty() && !reply_result.has_media) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"ok-empty",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/std::nullopt,
            /*duration_ms=*/duration_ms,
            /*has_media=*/false,
            /*reason=*/opts.reason,
            /*channel=*/reply_result.channel,
            /*silent=*/!reply_result.ok_sent,
            /*indicator_type=*/use_indicator
                ? resolve_indicator_type("ok-empty") : std::optional<HeartbeatIndicatorType>{},
        });
        return {HeartbeatRunResult::Status::Ran,
                duration_ms, std::nullopt};
    }

    // ── ok-token: reply is HEARTBEAT_OK (stripped to empty) ──────────────────
    // Aligned with openclaw: normalized.shouldSkip && !hasMedia && !hasExecCompletion
    if (reply_result.should_skip && !reply_result.has_media && !has_exec_completion) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"ok-token",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/std::nullopt,
            /*duration_ms=*/duration_ms,
            /*has_media=*/false,
            /*reason=*/opts.reason,
            /*channel=*/reply_result.channel,
            /*silent=*/!reply_result.ok_sent,
            /*indicator_type=*/use_indicator
                ? resolve_indicator_type("ok-token") : std::optional<HeartbeatIndicatorType>{},
        });
        return {HeartbeatRunResult::Status::Ran,
                duration_ms, std::nullopt};
    }

    // ── duplicate: same text as last heartbeat within 24h ────────────────────
    // Aligned with openclaw: isDuplicateMain
    if (reply_result.is_duplicate) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"skipped",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/reply_result.text.size() > 200
                ? std::optional<std::string>{reply_result.text.substr(0, 200)}
                : std::optional<std::string>{reply_result.text},
            /*duration_ms=*/duration_ms,
            /*has_media=*/false,
            /*reason=*/"duplicate",
            /*channel=*/reply_result.channel,
            /*silent=*/std::nullopt,
            /*indicator_type=*/std::nullopt,
        });
        return {HeartbeatRunResult::Status::Ran,
                duration_ms, std::nullopt};
    }

    // ── no-target: delivery channel is "none" or no recipient ────────────────
    // Aligned with openclaw: delivery.channel === "none" || !delivery.to
    if (!reply_result.has_target) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"skipped",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/reply_result.text.size() > 200
                ? std::optional<std::string>{reply_result.text.substr(0, 200)}
                : std::optional<std::string>{reply_result.text},
            /*duration_ms=*/duration_ms,
            /*has_media=*/reply_result.has_media,
            /*reason=*/reply_result.no_target_reason.value_or("no-target"),
            /*channel=*/std::nullopt,
            /*silent=*/std::nullopt,
            /*indicator_type=*/std::nullopt,
        });
        return {HeartbeatRunResult::Status::Ran,
                duration_ms, std::nullopt};
    }

    // ── alerts-disabled: showAlerts=false but we have content ────────────────
    // Aligned with openclaw: !visibility.showAlerts
    if (!show_alerts) {
        emit_heartbeat_event({
            /*ts=*/0,
            /*status=*/"skipped",
            /*to=*/std::nullopt,
            /*account_id=*/agent.account_id,
            /*preview=*/reply_result.text.size() > 200
                ? std::optional<std::string>{reply_result.text.substr(0, 200)}
                : std::optional<std::string>{reply_result.text},
            /*duration_ms=*/duration_ms,
            /*has_media=*/reply_result.has_media,
            /*reason=*/"alerts-disabled",
            /*channel=*/reply_result.channel,
            /*silent=*/std::nullopt,
            /*indicator_type=*/use_indicator
                ? resolve_indicator_type("sent") : std::optional<HeartbeatIndicatorType>{},
        });
        return {HeartbeatRunResult::Status::Ran,
                duration_ms, std::nullopt};
    }

    // ── sent: message delivered ───────────────────────────────────────────────
    // Aligned with openclaw: emitHeartbeatEvent({ status: "sent", ... })
    emit_heartbeat_event({
        /*ts=*/0,
        /*status=*/"sent",
        /*to=*/reply_result.to,
        /*account_id=*/agent.account_id,
        /*preview=*/reply_result.text.size() > 200
            ? std::optional<std::string>{reply_result.text.substr(0, 200)}
            : std::optional<std::string>{reply_result.text},
        /*duration_ms=*/duration_ms,
        /*has_media=*/reply_result.has_media,
        /*reason=*/opts.reason,
        /*channel=*/reply_result.channel,
        /*silent=*/std::nullopt,
        /*indicator_type=*/use_indicator
            ? resolve_indicator_type("sent") : std::optional<HeartbeatIndicatorType>{},
    });
    return {HeartbeatRunResult::Status::Ran,
            duration_ms, std::nullopt};
}

// ── HeartbeatRunnerImpl ──────────────────────────────────────────────────────
// Aligned with openclaw: startHeartbeatRunner internal state

class HeartbeatRunnerImpl : public std::enable_shared_from_this<HeartbeatRunnerImpl> {
public:
    using RunOnceFn = std::function<HeartbeatRunResult(
        const HeartbeatAgentConfig&, const HeartbeatRunOnceOptions&)>;

    explicit HeartbeatRunnerImpl(RunOnceFn run_once_fn)
        : run_once_fn_(std::move(run_once_fn))
        , stopped_(false)
        , initialized_(false)
    {}

    ~HeartbeatRunnerImpl() {
        stop();
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        if (dispose_wake_handler_) {
            dispose_wake_handler_();
            dispose_wake_handler_ = nullptr;
        }
        cancel_timer();
    }

    // Aligned with openclaw: updateConfig(cfg)
    void update_config(const HeartbeatRunnerConfig& cfg) {
        if (stopped_.load()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now = default_now_ms();
        const auto prev_agents = agents_;
        const bool prev_enabled = !agents_.empty();

        agents_.clear();
        for (const auto& agent_cfg : cfg.agents) {
            const int64_t interval_ms = resolve_heartbeat_interval_ms(agent_cfg);
            if (interval_ms <= 0) continue;

            HeartbeatAgentState state;
            state.config = agent_cfg;
            state.interval_ms = interval_ms;

            // Aligned with openclaw: resolveNextDue
            auto prev_it = prev_agents.find(agent_cfg.agent_id);
            if (prev_it != prev_agents.end() && prev_it->second.last_run_ms.has_value()) {
                state.last_run_ms = prev_it->second.last_run_ms;
                state.next_due_ms = *prev_it->second.last_run_ms + interval_ms;
            } else if (prev_it != prev_agents.end()
                       && prev_it->second.interval_ms == interval_ms
                       && prev_it->second.next_due_ms > now) {
                state.next_due_ms = prev_it->second.next_due_ms;
            } else {
                state.next_due_ms = now + interval_ms;
            }

            agents_[agent_cfg.agent_id] = std::move(state);
        }

        cfg_ = cfg;
        const bool next_enabled = !agents_.empty();

        if (!initialized_) {
            initialized_ = true;
        }

        schedule_next_locked();
    }

    // Called by HeartbeatWakeHandler
    HeartbeatRunResult run(const HeartbeatWakeParams& params) {
        if (stopped_.load()) {
            return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
        }
        if (!are_heartbeats_enabled()) {
            return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (agents_.empty()) {
            return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
        }

        const std::string reason = params.reason.value_or("");
        const bool is_interval = (reason == "interval");
        const int64_t started_at = default_now_ms();
        const int64_t now = started_at;
        bool ran = false;

        // Aligned with openclaw: targeted run (requestedAgentId or requestedSessionKey)
        if (params.agent_id.has_value() || params.session_key.has_value()) {
            std::string target_agent_id = params.agent_id.value_or("");
            auto it = agents_.find(target_agent_id);
            if (it == agents_.end()) {
                schedule_next_locked();
                lock.unlock();
                return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
            }
            auto& agent_state = it->second;
            HeartbeatRunOnceOptions opts;
            opts.reason = reason;
            opts.agent_id = agent_state.config.agent_id;
            opts.session_key = params.session_key;
            const auto cfg_copy = cfg_;
            const auto agent_copy = agent_state.config;
            lock.unlock();

            HeartbeatRunResult res;
            try {
                res = run_once_fn_(agent_copy, opts);
            } catch (...) {
                lock.lock();
                agent_state.last_run_ms = now;
                agent_state.next_due_ms = now + agent_state.interval_ms;
                schedule_next_locked();
                lock.unlock();
                return {HeartbeatRunResult::Status::Failed, std::nullopt, "unexpected-error"};
            }

            lock.lock();
            if (!res.reason.has_value() || res.reason.value() != "disabled") {
                agent_state.last_run_ms = now;
                agent_state.next_due_ms = now + agent_state.interval_ms;
            }
            schedule_next_locked();
            lock.unlock();
            return res;
        }

        // Aligned with openclaw: iterate all agents
        std::vector<std::pair<std::string, HeartbeatAgentState*>> due_agents;
        for (auto& [id, state] : agents_) {
            if (is_interval && now < state.next_due_ms) continue;
            due_agents.push_back({id, &state});
        }

        for (auto& [id, state_ptr] : due_agents) {
            HeartbeatRunOnceOptions opts;
            opts.reason = reason;
            opts.agent_id = state_ptr->config.agent_id;
            const auto agent_copy = state_ptr->config;
            lock.unlock();

            HeartbeatRunResult res;
            try {
                res = run_once_fn_(agent_copy, opts);
            } catch (...) {
                lock.lock();
                auto it2 = agents_.find(id);
                if (it2 != agents_.end()) {
                    it2->second.last_run_ms = now;
                    it2->second.next_due_ms = now + it2->second.interval_ms;
                }
                continue;
            }

            lock.lock();
            // Aligned with openclaw: requests-in-flight → don't advance schedule
            if (res.reason.has_value() && res.reason.value() == "requests-in-flight") {
                schedule_next_locked();
                lock.unlock();
                return res;
            }
            auto it2 = agents_.find(id);
            if (it2 != agents_.end()) {
                if (!res.reason.has_value() || res.reason.value() != "disabled") {
                    it2->second.last_run_ms = now;
                    it2->second.next_due_ms = now + it2->second.interval_ms;
                }
            }
            if (res.status == HeartbeatRunResult::Status::Ran) {
                ran = true;
            }
        }

        schedule_next_locked();
        lock.unlock();

        if (ran) {
            return {HeartbeatRunResult::Status::Ran,
                    static_cast<int64_t>(default_now_ms() - started_at),
                    std::nullopt};
        }
        return {HeartbeatRunResult::Status::Skipped, std::nullopt,
                is_interval ? "not-due" : "disabled"};
    }

    void set_dispose_wake_handler(std::function<void()> disposer) {
        dispose_wake_handler_ = std::move(disposer);
    }

private:
    void cancel_timer() {
        // Signal the timer thread to stop
        timer_cancelled_.store(true);
        timer_cv_.notify_all();
    }

    // Aligned with openclaw: scheduleNext()
    // Must be called with mutex_ held.
    void schedule_next_locked() {
        cancel_timer();
        if (stopped_.load() || agents_.empty()) return;

        const int64_t now = default_now_ms();
        int64_t next_due = std::numeric_limits<int64_t>::max();
        for (const auto& [id, state] : agents_) {
            if (state.next_due_ms < next_due) next_due = state.next_due_ms;
        }
        if (next_due == std::numeric_limits<int64_t>::max()) return;

        const int64_t delay_ms = std::max(int64_t(0), next_due - now);

        // Aligned with openclaw: setTimeout(() => requestHeartbeatNow({ reason: "interval" }), delay)
        timer_cancelled_.store(false);
        auto self = shared_from_this();
        std::thread([self, delay_ms]() {
            std::unique_lock<std::mutex> lock(self->timer_mutex_);
            self->timer_cv_.wait_for(lock,
                std::chrono::milliseconds(delay_ms),
                [&self]() { return self->timer_cancelled_.load(); });
            if (!self->timer_cancelled_.load() && !self->stopped_.load()) {
                request_heartbeat_now(std::optional<std::string>{"interval"});
            }
        }).detach();
    }

    RunOnceFn run_once_fn_;
    std::atomic<bool> stopped_;
    bool initialized_;
    std::mutex mutex_;
    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::atomic<bool> timer_cancelled_{false};
    HeartbeatRunnerConfig cfg_;
    std::map<std::string, HeartbeatAgentState> agents_;
    std::function<void()> dispose_wake_handler_;
};

// ── HeartbeatRunner ──────────────────────────────────────────────────────────

HeartbeatRunner::HeartbeatRunner(std::shared_ptr<HeartbeatRunnerImpl> impl)
    : impl_(std::move(impl)) {}

HeartbeatRunner::~HeartbeatRunner() {
    if (impl_) impl_->stop();
}

void HeartbeatRunner::stop() {
    if (impl_) impl_->stop();
}

void HeartbeatRunner::update_config(const HeartbeatRunnerConfig& cfg) {
    if (impl_) impl_->update_config(cfg);
}

// ── start_heartbeat_runner ───────────────────────────────────────────────────
// Aligned with openclaw: startHeartbeatRunner

std::shared_ptr<HeartbeatRunner> start_heartbeat_runner(
    const HeartbeatRunnerConfig& cfg,
    std::function<HeartbeatRunResult(const HeartbeatAgentConfig&, const HeartbeatRunOnceOptions&)>
        run_once_fn
) {
    // Default run_once_fn wraps run_heartbeat_once
    if (!run_once_fn) {
        run_once_fn = [cfg](const HeartbeatAgentConfig& agent,
                            const HeartbeatRunOnceOptions& opts) {
            return run_heartbeat_once(cfg, agent, opts);
        };
    }

    auto impl = std::make_shared<HeartbeatRunnerImpl>(run_once_fn);

    // Aligned with openclaw: setHeartbeatWakeHandler(wakeHandler)
    auto impl_weak = std::weak_ptr<HeartbeatRunnerImpl>(impl);
    auto disposer = set_heartbeat_wake_handler(
        [impl_weak](const HeartbeatWakeParams& params) -> HeartbeatRunResult {
            auto impl = impl_weak.lock();
            if (!impl) {
                return {HeartbeatRunResult::Status::Skipped, std::nullopt, "disabled"};
            }
            return impl->run(params);
        }
    );
    impl->set_dispose_wake_handler(std::move(disposer));

    // Aligned with openclaw: updateConfig(state.cfg)
    impl->update_config(cfg);

    return std::make_shared<HeartbeatRunner>(impl);
}

} // namespace infra
} // namespace hunterclaw
