#pragma once

// ============================================================================
// heartbeat_runner.h
// Aligned with openclaw: src/infra/heartbeat-runner.ts
//
// Provides:
//   - HeartbeatAgentConfig       (aligned with HeartbeatAgent)
//   - HeartbeatRunnerConfig      (aligned with OpenClawConfig heartbeat subset)
//   - HeartbeatRunner            (aligned with HeartbeatRunner return type)
//   - start_heartbeat_runner     (aligned with startHeartbeatRunner)
//   - run_heartbeat_once         (aligned with runHeartbeatOnce)
// ============================================================================

#include "hunterclaw/infra/heartbeat_events.h"
#include "hunterclaw/infra/heartbeat_wake.h"
#include "hunterclaw/infra/heartbeat_active_hours.h"
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <chrono>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ── HeartbeatAgentConfig ─────────────────────────────────────────────────────
// Aligned with openclaw: HeartbeatAgent + HeartbeatConfig

struct HeartbeatAgentConfig {
    std::string agent_id;
    std::string workspace_path;

    // Heartbeat-specific config (aligned with openclaw: HeartbeatConfig)
    std::optional<std::string> every;           // e.g. "30m", "1h"
    std::optional<std::string> prompt;          // custom heartbeat prompt
    std::optional<std::string> model;           // model override
    std::optional<std::string> session;         // session key override
    std::optional<std::string> target;          // delivery target
    std::optional<std::string> account_id;      // accountId override
    std::optional<int>         ack_max_chars;   // max chars for ack suppression
    std::optional<bool>        show_ok;         // show HEARTBEAT_OK messages
    std::optional<bool>        show_alerts;     // show alert messages
    std::optional<bool>        use_indicator;   // emit indicator events
    std::optional<bool>        light_context;   // lightweight context mode
    std::optional<bool>        include_reasoning; // include reasoning payloads
    std::optional<bool>        suppress_tool_error_warnings;
    std::optional<ActiveHoursConfig> active_hours;
};

// ── HeartbeatRunnerConfig ────────────────────────────────────────────────────
// Aligned with openclaw: OpenClawConfig (heartbeat-relevant subset)

struct HeartbeatRunnerConfig {
    std::vector<HeartbeatAgentConfig> agents;   // list of heartbeat agents
    std::string user_timezone;                  // agents.defaults.userTimezone
    std::string default_agent_id;               // resolved default agent
};

// ── HeartbeatReplyRequest ────────────────────────────────────────────────────
// Aligned with openclaw: ctx passed to getReplyFromConfig
//
// Maps directly to the OpenClaw InboundMessage ctx fields:
//   Body                → prompt (with current-time line appended)
//   From / To           → sender (resolveHeartbeatSenderContext)
//   OriginatingChannel  → delivery.channel (if not "none")
//   OriginatingTo       → delivery.to
//   AccountId           → delivery.accountId
//   MessageThreadId     → delivery.threadId
//   Provider            → "heartbeat" | "exec-event" | "cron-event"
//   SessionKey          → resolved session key

struct HeartbeatReplyRequest {
    // ── Core prompt ──────────────────────────────────────────────────────────
    std::string prompt;             // Body: appendCronStyleCurrentTimeLine(prompt, cfg, startedAt)

    // ── Session / agent ──────────────────────────────────────────────────────
    std::string session_key;        // SessionKey
    std::string agent_id;           // used to resolve agent config

    // ── Sender context (resolveHeartbeatSenderContext) ───────────────────────
    std::string from;               // From (sender address)
    std::string to;                 // To   (same as From for heartbeat)

    // ── Delivery target ──────────────────────────────────────────────────────
    std::optional<std::string> originating_channel; // OriginatingChannel (delivery.channel)
    std::optional<std::string> originating_to;      // OriginatingTo      (delivery.to)
    std::optional<std::string> account_id;          // AccountId          (delivery.accountId)
    std::optional<std::string> thread_id;           // MessageThreadId    (delivery.threadId)

    // ── Provider tag ─────────────────────────────────────────────────────────
    // Aligned with openclaw: Provider = "heartbeat" | "exec-event" | "cron-event"
    std::string provider = "heartbeat";

    // ── Model / context overrides ─────────────────────────────────────────────
    std::optional<std::string> model_override;      // heartbeat?.model
    bool suppress_tool_error_warnings = false;      // heartbeat?.suppressToolErrorWarnings
    bool light_context = false;                     // heartbeat?.lightContext → "lightweight"

    // ── Trigger classification (for reply normalization) ──────────────────────
    std::string reason;
    bool is_exec_event = false;
    bool is_cron_event = false;
};

// ── HeartbeatReplyResult ─────────────────────────────────────────────────────
// Aligned with openclaw: resolveHeartbeatReplyPayload + normalizeHeartbeatReply result

struct HeartbeatReplyResult {
    std::string text;                           // normalized reply text (stripped of HEARTBEAT_OK)
    bool should_skip   = false;                 // true if text was HEARTBEAT_OK
    bool has_media     = false;                 // true if reply has media attachments
    bool is_duplicate  = false;                 // true if same as last heartbeat within 24h
    bool has_target    = true;                  // false if delivery.channel=="none" || !delivery.to
    bool ok_sent       = false;                 // true if HEARTBEAT_OK was sent via showOk path
    std::optional<std::string> to;              // delivery recipient
    std::optional<std::string> channel;         // delivery channel
    std::optional<std::string> no_target_reason; // reason when has_target=false
};

// ── HeartbeatRunOnceOptions ──────────────────────────────────────────────────
// Aligned with openclaw: runHeartbeatOnce opts

struct HeartbeatRunOnceOptions {
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> reason;
    // Injected dependencies (for testing / gateway integration)
    std::function<int64_t()> now_ms;
    std::function<int(const std::string& lane)> get_queue_size;
    // Aligned with openclaw: getReplyFromConfig → resolveHeartbeatReplyPayload
    // Must be provided by the caller (gateway) to perform the actual LLM call.
    std::function<HeartbeatReplyResult(const HeartbeatReplyRequest&)> reply_fn;
    // Aligned with openclaw: isHeartbeatContentEffectivelyEmpty(fileContent)
    // Returns true if the HEARTBEAT.md at the given path is effectively empty.
    // If not provided, the file gate is skipped.
    std::function<bool(const std::string& path)> check_heartbeat_file;
};

// ── HeartbeatRunner ──────────────────────────────────────────────────────────
// Aligned with openclaw: HeartbeatRunner = { stop, updateConfig }

class HeartbeatRunnerImpl;

class HeartbeatRunner {
public:
    explicit HeartbeatRunner(std::shared_ptr<HeartbeatRunnerImpl> impl);
    ~HeartbeatRunner();

    // Aligned with openclaw: runner.stop()
    void stop();

    // Aligned with openclaw: runner.updateConfig(cfg)
    void update_config(const HeartbeatRunnerConfig& cfg);

private:
    std::shared_ptr<HeartbeatRunnerImpl> impl_;
};

// ── run_heartbeat_once ───────────────────────────────────────────────────────
// Aligned with openclaw: runHeartbeatOnce
// Runs a single heartbeat cycle for the given agent.
// Returns HeartbeatRunResult (ran/skipped/failed).

HeartbeatRunResult run_heartbeat_once(
    const HeartbeatRunnerConfig& cfg,
    const HeartbeatAgentConfig& agent,
    const HeartbeatRunOnceOptions& opts = {}
);

// ── start_heartbeat_runner ───────────────────────────────────────────────────
// Aligned with openclaw: startHeartbeatRunner
//
// Starts the heartbeat scheduler. Registers a HeartbeatWakeHandler and
// schedules timers based on each agent's configured interval.
// Returns a HeartbeatRunner that can be stopped or reconfigured.

std::shared_ptr<HeartbeatRunner> start_heartbeat_runner(
    const HeartbeatRunnerConfig& cfg,
    std::function<HeartbeatRunResult(const HeartbeatAgentConfig&, const HeartbeatRunOnceOptions&)>
        run_once_fn = nullptr  // nullptr = use default run_heartbeat_once
);

} // namespace infra
} // namespace hunterclaw
