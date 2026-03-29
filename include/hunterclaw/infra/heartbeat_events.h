#pragma once

// ============================================================================
// heartbeat_events.h
// Aligned with openclaw: src/infra/heartbeat-events.ts
//
// Provides:
//   - HeartbeatEventPayload  (aligned with HeartbeatEventPayload)
//   - HeartbeatIndicatorType (aligned with HeartbeatIndicatorType)
//   - emit_heartbeat_event   (aligned with emitHeartbeatEvent)
//   - on_heartbeat_event     (aligned with onHeartbeatEvent)
//   - get_last_heartbeat_event (aligned with getLastHeartbeatEvent)
//   - resolve_indicator_type (aligned with resolveIndicatorType)
// ============================================================================

#include <string>
#include <optional>
#include <functional>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ============================================================================
// HeartbeatIndicatorType
// Aligned with openclaw: HeartbeatIndicatorType = "ok" | "alert" | "error"
// ============================================================================

enum class HeartbeatIndicatorType {
    Ok,     // "ok"
    Alert,  // "alert"
    Error,  // "error"
};

// ============================================================================
// HeartbeatEventPayload
// Aligned with openclaw: HeartbeatEventPayload
// ============================================================================

struct HeartbeatEventPayload {
    int64_t ts = 0;                                         // timestamp ms
    std::string status;                                     // "sent"|"ok-empty"|"ok-token"|"skipped"|"failed"
    std::optional<std::string> to;                          // recipient
    std::optional<std::string> account_id;                  // accountId
    std::optional<std::string> preview;                     // message preview (max 200 chars)
    std::optional<int64_t> duration_ms;                     // durationMs
    std::optional<bool> has_media;                          // hasMedia
    std::optional<std::string> reason;                      // skip/fail reason
    std::optional<std::string> channel;                     // channel name
    std::optional<bool> silent;                             // silently suppressed (showOk: false)
    std::optional<HeartbeatIndicatorType> indicator_type;   // indicatorType
};

// ============================================================================
// resolve_indicator_type
// Aligned with openclaw: resolveIndicatorType
// ============================================================================

inline std::optional<HeartbeatIndicatorType> resolve_indicator_type(const std::string& status) {
    if (status == "ok-empty" || status == "ok-token") return HeartbeatIndicatorType::Ok;
    if (status == "sent")   return HeartbeatIndicatorType::Alert;
    if (status == "failed") return HeartbeatIndicatorType::Error;
    return std::nullopt;  // "skipped"
}

// ============================================================================
// emit_heartbeat_event
// Aligned with openclaw: emitHeartbeatEvent
//
// Stores the event as the last heartbeat and notifies all listeners.
// The ts field is automatically set to now_ms() if not provided.
// ============================================================================

void emit_heartbeat_event(const HeartbeatEventPayload& evt);

// ============================================================================
// on_heartbeat_event
// Aligned with openclaw: onHeartbeatEvent
//
// Registers a listener for heartbeat events.
// Returns a disposer function that removes the listener.
// ============================================================================

using HeartbeatEventListener = std::function<void(const HeartbeatEventPayload&)>;

std::function<void()> on_heartbeat_event(HeartbeatEventListener listener);

// ============================================================================
// get_last_heartbeat_event
// Aligned with openclaw: getLastHeartbeatEvent
//
// Returns the last emitted heartbeat event, or nullopt if none.
// ============================================================================

std::optional<HeartbeatEventPayload> get_last_heartbeat_event();

} // namespace infra
} // namespace hunterclaw
