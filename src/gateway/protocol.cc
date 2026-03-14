#include "hunterclaw/gateway/protocol.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <uuid/uuid.h>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// ErrorShape
// ============================================================================

nlohmann::json ErrorShape::to_json() const {
    nlohmann::json j;
    j["code"] = code;
    j["message"] = message;
    if (retryable) j["retryable"] = *retryable;
    if (retry_after_ms) j["retryAfterMs"] = *retry_after_ms;
    return j;
}

core::Result<ErrorShape> ErrorShape::from_json(const nlohmann::json& j) {
    if (!j.contains("code") || !j.contains("message")) {
        return core::Result<ErrorShape>::err("Missing required fields");
    }
    
    ErrorShape err;
    err.code = j["code"].get<std::string>();
    err.message = j["message"].get<std::string>();
    
    if (j.contains("retryable")) {
        err.retryable = j["retryable"].get<bool>();
    }
    if (j.contains("retryAfterMs")) {
        err.retry_after_ms = j["retryAfterMs"].get<int64_t>();
    }
    
    return core::Result<ErrorShape>::ok(std::move(err));
}

// ============================================================================
// RequestFrame
// ============================================================================

nlohmann::json RequestFrame::to_json() const {
    nlohmann::json j;
    j["type"] = "req";
    j["id"] = id;
    j["method"] = method;
    if (!params.is_null()) {
        j["params"] = params;
    }
    return j;
}

core::Result<RequestFrame> RequestFrame::from_json(const nlohmann::json& j) {
    if (!j.contains("id") || !j.contains("method")) {
        return core::Result<RequestFrame>::err("Missing required fields");
    }
    
    RequestFrame req;
    req.id = j["id"].get<std::string>();
    req.method = j["method"].get<std::string>();
    
    if (j.contains("params")) {
        req.params = j["params"];
    }
    
    return core::Result<RequestFrame>::ok(std::move(req));
}

// ============================================================================
// ResponseFrame
// ============================================================================

nlohmann::json ResponseFrame::to_json() const {
    nlohmann::json j;
    j["type"] = "res";
    j["id"] = id;
    j["ok"] = ok;
    
    if (ok) {
        if (!payload.is_null()) {
            j["payload"] = payload;
        }
    } else {
        if (error) {
            j["error"] = error->to_json();
        }
    }
    
    return j;
}

core::Result<ResponseFrame> ResponseFrame::from_json(const nlohmann::json& j) {
    if (!j.contains("id") || !j.contains("ok")) {
        return core::Result<ResponseFrame>::err("Missing required fields");
    }
    
    ResponseFrame res;
    res.id = j["id"].get<std::string>();
    res.ok = j["ok"].get<bool>();
    
    if (j.contains("payload")) {
        res.payload = j["payload"];
    }
    
    if (j.contains("error")) {
        auto err_result = ErrorShape::from_json(j["error"]);
        if (err_result.is_ok()) {
            res.error = err_result.value;
        }
    }
    
    return core::Result<ResponseFrame>::ok(std::move(res));
}

ResponseFrame ResponseFrame::success(const std::string& id, const nlohmann::json& payload) {
    ResponseFrame res;
    res.id = id;
    res.ok = true;
    res.payload = payload;
    return res;
}

ResponseFrame ResponseFrame::failure(const std::string& id, const ErrorShape& error) {
    ResponseFrame res;
    res.id = id;
    res.ok = false;
    res.error = error;
    return res;
}

// ============================================================================
// EventFrame
// ============================================================================

nlohmann::json EventFrame::to_json() const {
    nlohmann::json j;
    j["type"] = "event";
    j["event"] = event;
    
    if (seq) {
        j["seq"] = *seq;
    }
    if (!payload.is_null()) {
        j["payload"] = payload;
    }
    
    return j;
}

core::Result<EventFrame> EventFrame::from_json(const nlohmann::json& j) {
    if (!j.contains("event")) {
        return core::Result<EventFrame>::err("Missing event field");
    }
    
    EventFrame evt;
    evt.event = j["event"].get<std::string>();
    
    if (j.contains("seq")) {
        evt.seq = j["seq"].get<int64_t>();
    }
    if (j.contains("payload")) {
        evt.payload = j["payload"];
    }
    
    return core::Result<EventFrame>::ok(std::move(evt));
}

// ============================================================================
// FrameValidator
// ============================================================================

bool FrameValidator::validate_request_frame(const nlohmann::json& j) {
    return j.contains("type") && j["type"] == "req" &&
           j.contains("id") && j.contains("method");
}

bool FrameValidator::validate_response_frame(const nlohmann::json& j) {
    return j.contains("type") && j["type"] == "res" &&
           j.contains("id") && j.contains("ok");
}

bool FrameValidator::validate_event_frame(const nlohmann::json& j) {
    return j.contains("type") && j["type"] == "event" &&
           j.contains("event");
}

core::Result<FrameType> FrameValidator::detect_frame_type(const nlohmann::json& j) {
    if (!j.contains("type")) {
        return core::Result<FrameType>::err("Missing type field");
    }
    
    std::string type = j["type"].get<std::string>();
    
    if (type == "req") return core::Result<FrameType>::ok(FrameType::Request);
    if (type == "res") return core::Result<FrameType>::ok(FrameType::Response);
    if (type == "event") return core::Result<FrameType>::ok(FrameType::Event);
    
    return core::Result<FrameType>::err("Unknown frame type: " + type);
}

// ============================================================================
// CloseCodeHints
// ============================================================================

std::string CloseCodeHints::get_hint(int code) {
    switch (code) {
        case 1000: return NORMAL_CLOSURE;
        case 1006: return ABNORMAL_CLOSURE;
        case 1008: return POLICY_VIOLATION;
        case 1012: return SERVICE_RESTART;
        default: return "unknown close code " + std::to_string(code);
    }
}

// ============================================================================
// ConnectChallenge
// ============================================================================

EventFrame ConnectChallenge::create_event(const std::string& nonce) {
    EventFrame evt;
    evt.event = "connect.challenge";
    evt.payload = nlohmann::json{{"nonce", nonce}};
    return evt;
}

// ============================================================================
// ConnectRequest
// ============================================================================

nlohmann::json ConnectRequest::to_params() const {
    nlohmann::json j;
    j["deviceId"] = device_id;
    j["deviceToken"] = device_token;
    j["clientName"] = client_name;
    j["clientVersion"] = client_version;
    
    if (platform) j["platform"] = *platform;
    if (protocol_version) j["protocolVersion"] = *protocol_version;
    
    return j;
}

// ============================================================================
// HelloOk
// ============================================================================

core::Result<HelloOk> HelloOk::from_payload(const nlohmann::json& j) {
    if (!j.contains("connId") || !j.contains("serverTime") || !j.contains("protocolVersion")) {
        return core::Result<HelloOk>::err("Missing required fields in hello.ok");
    }
    
    HelloOk hello;
    hello.conn_id = j["connId"].get<std::string>();
    hello.server_time = j["serverTime"].get<int64_t>();
    hello.protocol_version = j["protocolVersion"].get<int>();
    
    return core::Result<HelloOk>::ok(std::move(hello));
}

// ============================================================================
// TickEvent
// ============================================================================

EventFrame TickEvent::create_event(int64_t ts, 
                                   std::optional<int> presence_ver,
                                   std::optional<int> health_ver) {
    EventFrame evt;
    evt.event = "tick";
    
    nlohmann::json payload;
    payload["ts"] = ts;
    if (presence_ver) payload["presenceVersion"] = *presence_ver;
    if (health_ver) payload["healthVersion"] = *health_ver;
    
    evt.payload = payload;
    return evt;
}

} // namespace gateway
} // namespace hunterclaw
