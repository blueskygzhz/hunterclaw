// Aligned with openclaw: src/discord/monitor/inbound-worker.ts
#pragma once

#include "hunterclaw/channels/discord_inbound_job.h"
#include "hunterclaw/channels/run_state_machine.h"
#include "hunterclaw/channels/keyed_async_queue.h"
#include <functional>
#include <memory>
#include <atomic>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordInboundWorker
// Aligned with OpenClaw: createDiscordInboundWorker (inbound-worker.ts)
//
// Owns a KeyedAsyncQueue (per-session ordering) and a RunStateMachine
// (busy/activeRuns/lastRunActivityAt status). Processes inbound Discord
// messages with per-session serialization and run-state tracking.
// ============================================================================

// Aligned with openclaw: DiscordMonitorStatusSink
using DiscordMonitorStatusSink = RunStateStatusSink;

struct DiscordInboundWorkerParams {
    // Aligned with openclaw: runtime (RuntimeEnv)
    std::shared_ptr<spdlog::logger> logger;
    // Aligned with openclaw: setStatus? (DiscordMonitorStatusSink)
    DiscordMonitorStatusSink set_status;
    // Aligned with openclaw: abortSignal?
    std::shared_ptr<std::atomic<bool>> abort_signal;
    // Aligned with openclaw: runTimeoutMs?
    std::optional<int> run_timeout_ms;
    // Aligned with openclaw: cfg + dispatch_inbound (injected by monitor)
    core::Config cfg;
    std::string account_id;
    std::function<void(const InboundMessage&)> dispatch_inbound;
};

class DiscordInboundWorker {
public:
    explicit DiscordInboundWorker(DiscordInboundWorkerParams params);
    ~DiscordInboundWorker();

    // Non-copyable
    DiscordInboundWorker(const DiscordInboundWorker&) = delete;
    DiscordInboundWorker& operator=(const DiscordInboundWorker&) = delete;

    // Aligned with openclaw: enqueue(job)
    // Enqueues a job for processing. Jobs with the same queueKey are
    // serialized; different keys run concurrently.
    void enqueue(DiscordInboundJob job);

    // Aligned with openclaw: deactivate()
    // Stops accepting new jobs and deactivates the run state machine.
    void deactivate();

private:
    DiscordInboundWorkerParams params_;
    KeyedAsyncQueue run_queue_;
    std::unique_ptr<RunStateMachine> run_state_;

    void process_job(DiscordInboundJob job);
};

} // namespace channels
} // namespace hunterclaw
