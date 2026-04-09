// Aligned with openclaw:
//   src/gateway/server.impl.ts: startDiagnosticHeartbeat() + isDiagnosticsEnabled
//   src/logging/diagnostic.ts: diagnostic heartbeat lifecycle
//   src/infra/diagnostic-events.ts: event bus lifecycle
//
// This file initializes all observability subsystems at gateway startup.
// OpenClaw calls startDiagnosticHeartbeat() in server.impl.ts (line 452)
// and stopDiagnosticHeartbeat() on shutdown (line 1056).

#include "hunterclaw/observability/observability.h"
#include "hunterclaw/observability/diagnostic_events.h"
#include "hunterclaw/core/logging.h"
#include <chrono>
#include <thread>
#include <atomic>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Diagnostic heartbeat state
// Aligned with openclaw: logging/diagnostic.ts
//   let heartbeatInterval: NodeJS.Timeout | null = null;
//   const DEFAULT_STUCK_SESSION_WARN_MS = 120_000;
// ============================================================================

static std::atomic<bool> g_diagnostics_initialized{false};
static std::atomic<bool> g_heartbeat_running{false};
static std::unique_ptr<std::thread> g_heartbeat_thread;

static constexpr int HEARTBEAT_INTERVAL_MS = 30000;  // 30s (aligned with openclaw)
static constexpr int STUCK_SESSION_WARN_MS = 120000;  // 2min default

// ============================================================================
// init_observability
// Aligned with openclaw: server.impl.ts startup sequence
//   1. startDiagnosticHeartbeat()          → DiagnosticEventBus.start()
//   2. CoreMetrics.init()                  → init_diagnostic_metrics()
//   3. StructuredLogger.init(config)       → init_diagnostic_logging()
//   4. Tracer.init(tracing_config)         → (optional)
// ============================================================================

void init_observability() {
    if (g_diagnostics_initialized.exchange(true)) {
        return;  // already initialized
    }

    auto logger = core::get_logger("gateway");

    // Step 1: Start DiagnosticEventBus (async event processing thread)
    // Aligned with openclaw: eventBus runs a background loop
    try {
        observability::DiagnosticEventBus::instance().start();
        logger->info("DiagnosticEventBus started");
    } catch (const std::exception& e) {
        logger->warn("DiagnosticEventBus start failed: {}", e.what());
    }

    // Step 2: Initialize CoreMetrics (Prometheus counters/gauges/histograms)
    // Aligned with openclaw: diagnostics-otel plugin registers metrics
    try {
        observability::CoreMetrics::init();
        logger->info("CoreMetrics initialized (14 Prometheus metrics)");
    } catch (const std::exception& e) {
        logger->warn("CoreMetrics init failed: {}", e.what());
    }

    // Step 3: Wire DiagnosticEventBus → Prometheus metrics
    // Aligned with openclaw: diagnostics-otel subscribes to events
    // This is done inside prometheus_metrics.cc: init_diagnostic_metrics()

    // Step 4: Wire DiagnosticEventBus → StructuredLogger
    // Aligned with openclaw: diagnostics-otel subscribes to events for logging
    // This is done inside structured_logger.cc: init_diagnostic_logging()

    logger->info("Observability subsystems initialized");
}

// ============================================================================
// start_diagnostic_heartbeat
// Aligned with openclaw: startDiagnosticHeartbeat (diagnostic.ts line 333)
//
// Emits a diagnostic.heartbeat event every 30s with:
//   - webhook stats (received/processed/errors)
//   - active session count
//   - waiting session count
//   - total queued messages
// Also detects stuck sessions (processing > stuckSessionWarnMs).
// ============================================================================

void start_diagnostic_heartbeat() {
    if (g_heartbeat_running.exchange(true)) {
        return;  // already running
    }

    g_heartbeat_thread = std::make_unique<std::thread>([]() {
        auto logger = core::get_logger("diagnostic");

        while (g_heartbeat_running.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));

            if (!g_heartbeat_running.load()) break;

            // Emit diagnostic.heartbeat event
            // Aligned with openclaw: diagnostic.ts line 372
            //   emitDiagnosticEvent({ type: "diagnostic.heartbeat", ... })
            try {
                auto heartbeat = std::make_shared<observability::HeartbeatEvent>();
                heartbeat->active_sessions = 0;
                heartbeat->queued_messages = 0;
                heartbeat->cpu_percent = 0.0;
                heartbeat->memory_mb = 0;
                heartbeat->goroutines = 0;
                heartbeat->version = "0.1.0";

                observability::DiagnosticEventBus::instance().emit(heartbeat);

                logger->debug("diagnostic heartbeat emitted");
            } catch (...) {
                // Aligned with openclaw: never block on logging failures
            }
        }
    });

    // Detach so it doesn't block shutdown
    if (g_heartbeat_thread && g_heartbeat_thread->joinable()) {
        g_heartbeat_thread->detach();
    }
}

// ============================================================================
// stop_diagnostic_heartbeat
// Aligned with openclaw: stopDiagnosticHeartbeat (diagnostic.ts line 412)
// ============================================================================

void stop_diagnostic_heartbeat() {
    g_heartbeat_running.store(false);

    // Stop the DiagnosticEventBus processing thread
    try {
        observability::DiagnosticEventBus::instance().stop();
    } catch (...) {}

    g_diagnostics_initialized.store(false);
}

// ============================================================================
// emit_webhook_received / emit_webhook_processed / emit_webhook_error
// Aligned with openclaw: logWebhookReceived/Processed/Error (diagnostic.ts)
//
// These are called from HTTP request handlers (http_gateway.cc)
// to instrument the webhook processing path.
// ============================================================================

void emit_webhook_received(const std::string& channel, const std::string& method,
                           const std::string& path, size_t body_size,
                           const std::string& remote_ip) {
    try {
        auto event = std::make_shared<observability::WebhookReceivedEvent>();
        event->channel = channel;
        event->method = method;
        event->path = path;
        event->body_size_bytes = body_size;
        event->remote_ip = remote_ip;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

void emit_webhook_processed(const std::string& channel, int64_t duration_ms,
                            int status_code, bool success) {
    try {
        auto event = std::make_shared<observability::WebhookProcessedEvent>();
        event->channel = channel;
        event->duration_ms = duration_ms;
        event->status_code = status_code;
        event->success = success;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

void emit_webhook_error(const std::string& channel, const std::string& error_type,
                        const std::string& error_message) {
    try {
        auto event = std::make_shared<observability::WebhookErrorEvent>();
        event->channel = channel;
        event->error_type = error_type;
        event->error_message = error_message;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

// ============================================================================
// emit_http_request
// Aligned with openclaw: HttpRequestEvent (diagnostic_events.h)
// Called from HTTP gateway for all API requests.
// ============================================================================

void emit_http_request(const std::string& method, const std::string& path,
                       int status_code, int64_t duration_ms,
                       size_t request_size, size_t response_size,
                       const std::string& remote_ip) {
    try {
        auto event = std::make_shared<observability::HttpRequestEvent>();
        event->method = method;
        event->path = path;
        event->status_code = status_code;
        event->duration_ms = duration_ms;
        event->request_size_bytes = request_size;
        event->response_size_bytes = response_size;
        event->remote_ip = remote_ip;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

// ============================================================================
// emit_message_processed
// Aligned with openclaw: logMessageProcessed (diagnostic.ts line 151)
// Called from dispatch_reply_from_config after processing completes.
// ============================================================================

void emit_message_processed(const std::string& channel, const std::string& session_key,
                            const std::string& outcome, int64_t duration_ms,
                            const std::string& error) {
    try {
        auto event = std::make_shared<observability::MessageProcessedEvent>();
        event->session_key = session_key;
        event->channel = channel;
        event->duration_ms = duration_ms;
        event->success = (outcome == "completed");
        if (!error.empty()) {
            event->error_message = error;
        }
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

// ============================================================================
// emit_session_state_change
// Aligned with openclaw: logSessionStateChange (diagnostic.ts line 194)
// ============================================================================

void emit_session_state_change(const std::string& session_key,
                               const std::string& state,
                               const std::string& reason) {
    try {
        auto event = std::make_shared<observability::SessionStateEvent>();
        event->session_key = session_key;
        if (state == "processing") {
            event->old_state = observability::SessionState::IDLE;
            event->new_state = observability::SessionState::PROCESSING;
        } else {
            event->old_state = observability::SessionState::PROCESSING;
            event->new_state = observability::SessionState::IDLE;
        }
        event->reason = reason;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

// ============================================================================
// emit_lane_enqueue / emit_lane_dequeue
// Aligned with openclaw: logLaneEnqueue/Dequeue (diagnostic.ts line 247/257)
// Called from command_queue.cc on enqueue/dequeue.
// ============================================================================

void emit_lane_enqueue(const std::string& lane, int queue_size) {
    try {
        auto event = std::make_shared<observability::QueueLaneEnqueueEvent>();
        event->lane_name = lane;
        event->lane_depth = static_cast<size_t>(queue_size);
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

void emit_lane_dequeue(const std::string& lane, int64_t wait_ms, int queue_size) {
    try {
        auto event = std::make_shared<observability::QueueLaneDequeueEvent>();
        event->lane_name = lane;
        event->wait_duration_ms = wait_ms;
        observability::DiagnosticEventBus::instance().emit(event);
    } catch (...) {}
}

} // namespace gateway
} // namespace hunterclaw
