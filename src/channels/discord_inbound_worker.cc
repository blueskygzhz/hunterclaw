// Aligned with openclaw: src/discord/monitor/inbound-worker.ts
#include "hunterclaw/channels/discord_inbound_worker.h"
#include "hunterclaw/channels/message.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordInboundWorker implementation
// Aligned with OpenClaw: createDiscordInboundWorker (inbound-worker.ts)
// ============================================================================

DiscordInboundWorker::DiscordInboundWorker(DiscordInboundWorkerParams params)
    : params_(std::move(params))
{
    // Aligned with openclaw: createRunStateMachine({ setStatus, abortSignal })
    RunStateMachine::Params rsm_params;
    rsm_params.set_status = params_.set_status;
    run_state_ = std::make_unique<RunStateMachine>(std::move(rsm_params));
}

DiscordInboundWorker::~DiscordInboundWorker() {
    deactivate();
}

// Aligned with openclaw: enqueue(job)
// void runQueue.enqueue(job.queueKey, async () => {
//   if (!runState.isActive()) return;
//   runState.onRunStart();
//   try {
//     if (!runState.isActive()) return;
//     await processDiscordInboundJob({ job, runtime, lifecycleSignal, runTimeoutMs });
//   } finally {
//     runState.onRunEnd();
//   }
// }).catch(err => runtime.error(...))
void DiscordInboundWorker::enqueue(DiscordInboundJob job) {
    auto run_state = run_state_.get();
    auto& queue = run_queue_;

    queue.enqueue(job.queue_key, [this, job = std::move(job), run_state]() mutable {
        // Aligned with openclaw: if (!runState.isActive()) return
        if (!run_state->is_active()) {
            return;
        }
        // Aligned with openclaw: runState.onRunStart()
        run_state->on_run_start();
        try {
            // Aligned with openclaw: if (!runState.isActive()) return
            if (!run_state->is_active()) {
                run_state->on_run_end();
                return;
            }
            // Aligned with openclaw: await processDiscordInboundJob(...)
            process_job(std::move(job));
        } catch (...) {
            // Aligned with openclaw: .catch(err => runtime.error(...))
            if (params_.logger) {
                params_.logger->error("[discord] inbound worker failed (uncaught exception)");
            }
        }
        // Aligned with openclaw: finally { runState.onRunEnd() }
        run_state->on_run_end();
    });
}

// Aligned with openclaw: deactivate()
void DiscordInboundWorker::deactivate() {
    if (run_state_) {
        run_state_->deactivate();
    }
}

// Aligned with openclaw: processDiscordInboundJob → processDiscordMessage
// For now: builds an InboundMessage from the job payload and calls dispatch_inbound.
// When the full message-handler pipeline is implemented, this will call
// processDiscordMessage(materializeDiscordInboundJob(job, abortSignal)).
void DiscordInboundWorker::process_job(DiscordInboundJob job) {
    // Aligned with openclaw: check abortSignal before processing
    if (params_.abort_signal && params_.abort_signal->load()) {
        return;
    }

    // Aligned with openclaw: materializeDiscordInboundJob(job, abortSignal)
    // → processDiscordMessage(ctx)
    // Build InboundMessage from payload and dispatch
    if (!params_.dispatch_inbound) {
        return;
    }

    try {
        InboundMessage msg;
        msg.channel    = job.payload.channel;
        msg.account_id = job.payload.account_id;  // optional<string>
        // 🎯 对齐 OpenClaw: 使用路由系统构建的 session_key
        msg.session_key = job.payload.session_key.value_or(
            job.payload.message_channel_id);
        // Store raw event data in metadata
        if (!job.payload.data.is_null()) {
            msg.metadata["raw"] = core::JsonValue(job.payload.data.dump());
        }

        // Extract sender/text from message JSON
        const auto& m = job.payload.message;
        if (m.contains("author") && m["author"].is_object()) {
            const auto& author = m["author"];
            if (author.contains("id") && author["id"].is_string()) {
                msg.sender_id = author["id"].get<std::string>();
            }
            if (author.contains("username") && author["username"].is_string()) {
                msg.sender_name = author["username"].get<std::string>();
            }
        }
        if (m.contains("content") && m["content"].is_string()) {
            msg.text = m["content"].get<std::string>();
        }
        if (m.contains("id") && m["id"].is_string()) {
            msg.message_id = m["id"].get<std::string>();
        }
        if (m.contains("channel_id") && m["channel_id"].is_string()) {
            msg.chat_id = m["channel_id"].get<std::string>();
        }

        params_.dispatch_inbound(msg);
    } catch (const std::exception& e) {
        if (params_.logger) {
            params_.logger->error("[discord] inbound worker process_job failed: {}", e.what());
        }
    }
}

} // namespace channels
} // namespace hunterclaw
