// Aligned with openclaw:
//   src/gateway/server.impl.ts: startDiagnosticHeartbeat lifecycle
//   src/logging/diagnostic.ts: diagnostic heartbeat + webhook/message/session/lane instrumentation
#pragma once

#include <string>
#include <cstdint>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Observability Initialization (call from GatewayServer::start())
// Aligned with openclaw: server.impl.ts startDiagnosticHeartbeat + isDiagnosticsEnabled
// ============================================================================

void init_observability();
void start_diagnostic_heartbeat();
void stop_diagnostic_heartbeat();

// ============================================================================
// Webhook Instrumentation (call from HTTP handlers)
// Aligned with openclaw: logWebhookReceived/Processed/Error (diagnostic.ts)
// ============================================================================

void emit_webhook_received(const std::string& channel, const std::string& method,
                           const std::string& path, size_t body_size,
                           const std::string& remote_ip);

void emit_webhook_processed(const std::string& channel, int64_t duration_ms,
                            int status_code, bool success);

void emit_webhook_error(const std::string& channel, const std::string& error_type,
                        const std::string& error_message);

// ============================================================================
// HTTP Request Instrumentation
// Aligned with openclaw: HttpRequestEvent
// ============================================================================

void emit_http_request(const std::string& method, const std::string& path,
                       int status_code, int64_t duration_ms,
                       size_t request_size = 0, size_t response_size = 0,
                       const std::string& remote_ip = "");

// ============================================================================
// Message Processing Instrumentation
// Aligned with openclaw: logMessageProcessed (diagnostic.ts)
// ============================================================================

void emit_message_processed(const std::string& channel, const std::string& session_key,
                            const std::string& outcome, int64_t duration_ms,
                            const std::string& error = "");

// ============================================================================
// Session State Instrumentation
// Aligned with openclaw: logSessionStateChange (diagnostic.ts)
// ============================================================================

void emit_session_state_change(const std::string& session_key,
                               const std::string& state,
                               const std::string& reason);

// ============================================================================
// Lane Queue Instrumentation
// Aligned with openclaw: logLaneEnqueue/Dequeue (diagnostic.ts)
// ============================================================================

void emit_lane_enqueue(const std::string& lane, int queue_size);
void emit_lane_dequeue(const std::string& lane, int64_t wait_ms, int queue_size);

} // namespace gateway
} // namespace hunterclaw
