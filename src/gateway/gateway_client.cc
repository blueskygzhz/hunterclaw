// gateway_client.cc
// Aligned with openclaw:
//   src/gateway/client.ts          (GatewayClient / callGateway)
//   src/gateway/client-connect.ts  (connect handshake)
//   src/gateway/client-rpc.ts      (RPC request/response)
//
// Provides:
//   - GatewayClient  (真正的 WebSocket 跨进程 Gateway 客户端)
//   - create_gateway_client
//
// 🎯 对齐 OpenClaw 核心设计:
//   OpenClaw 的 callGateway("agent", params) 通过 WebSocket 连接发到另一个 Gateway 实例。
//   这是 ACP 跨进程通信的基础 —— 子 Agent 可以运行在不同进程/机器上。
//   HunterClaw 此前缺少此实现，导致 ACP 退化为同进程内调用。

#include "hunterclaw/gateway/client.h"
#include "hunterclaw/gateway/protocol.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <ixwebsocket/IXWebSocket.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <functional>
#include <map>
#include <chrono>
#include <random>
#include <stdexcept>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

namespace {

auto log() {
    static auto logger = spdlog::get("gateway_client");
    if (!logger) logger = spdlog::stdout_logger_mt("gateway_client");
    return logger;
}

static std::string gen_req_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // anonymous namespace

// ============================================================================
// GatewayClient
// 🎯 对齐 OpenClaw: src/gateway/client.ts
// ============================================================================

GatewayClient::GatewayClient(const GatewayClientOptions& options)
    : options_(options)
{
    ws_ = std::make_unique<ix::WebSocket>();
}

GatewayClient::~GatewayClient() {
    stop();
}

// ============================================================================
// 🎯 对齐 OpenClaw: GatewayClient.start()
// ============================================================================

void GatewayClient::start() {
    if (closed_.load()) return;

    ws_->setUrl(options_.url);
    ws_->setMaxWaitBetweenReconnectionRetries(static_cast<uint32_t>(MAX_BACKOFF_MS));

    if (!options_.tls_fingerprint.empty()) {
        ix::SocketTLSOptions tls_opts;
        ws_->setTLSOptions(tls_opts);
    }

    // 🎯 对齐 OC: WebSocket message handler
    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        on_message(msg);
    });

    ws_->start();
    log()->info("[GatewayClient] Connecting to {}", options_.url);
    start_tick_watch();
}

void GatewayClient::stop() {
    if (closed_.exchange(true)) return;
    stop_tick_watch();
    flush_pending_errors("gateway client stopped");
    if (ws_) ws_->stop();
    log()->info("[GatewayClient] Stopped");
}

bool GatewayClient::is_connected() const {
    return connected_.load();
}

// ============================================================================
// 🎯 对齐 OpenClaw: callGateway(method, params) → WebSocket RPC
//
// OC 实现:
//   async callGateway<T>(method, params): Promise<T> {
//     const id = crypto.randomUUID();
//     ws.send(JSON.stringify({ id, method, params }));
//     return await waitForResponse(id);  // resolves when { id, ok, payload } arrives
//   }
// ============================================================================

std::future<core::Result<json>> GatewayClient::request_async(
    const std::string& method,
    const json& params
) {
    auto promise = std::make_shared<std::promise<core::Result<json>>>();
    auto future = promise->get_future();

    if (closed_.load()) {
        promise->set_value(core::Result<json>::Err("gateway client is closed"));
        return future;
    }
    if (!connected_.load()) {
        promise->set_value(core::Result<json>::Err("gateway not connected"));
        return future;
    }

    const std::string req_id = gen_req_id();

    // 🎯 对齐 OC: RequestFrame { id, method, params }
    json frame = {
        {"id",     req_id},
        {"method", method},
        {"params", params}
    };

    // Register response callback BEFORE sending (avoid race with fast responses)
    {
        std::lock_guard<std::mutex> lock(response_callbacks_mutex_);
        response_callbacks_[req_id] = [promise](core::Result<json> result) {
            promise->set_value(std::move(result));
        };
    }
    // Also track in pending_ for flush_pending_errors
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto p = std::make_shared<PendingRequest>();
        p->id = req_id;
        p->sent_at = now_ms();
        pending_[req_id] = p;
    }

    // Send frame
    try {
        send_frame(frame);
        total_requests_.fetch_add(1);
        log()->debug("[GatewayClient] → {} id={}", method, req_id);
    } catch (const std::exception& e) {
        // Cleanup and fail immediately
        {
            std::lock_guard<std::mutex> lock(response_callbacks_mutex_);
            response_callbacks_.erase(req_id);
        }
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.erase(req_id);
        }
        promise->set_value(core::Result<json>::Err(
            std::string("send failed: ") + e.what()
        ));
    }

    return future;
}

// ============================================================================
// 🎯 对齐 OpenClaw: streaming request (expects accepted + final response)
// ============================================================================

std::future<core::Result<json>> GatewayClient::request_streaming(
    const std::string& method,
    const json& params
) {
    // For now, same as request_async — streaming responses are handled via events
    return request_async(method, params);
}

// ============================================================================
// 🎯 对齐 OpenClaw: WebSocket message handler
// ============================================================================

void GatewayClient::on_message(const ix::WebSocketMessagePtr& msg) {
    if (!msg) return;
    switch (msg->type) {
        case ix::WebSocketMessageType::Open:    on_open(); break;
        case ix::WebSocketMessageType::Close:   on_close(msg->closeInfo.code, msg->closeInfo.reason); break;
        case ix::WebSocketMessageType::Error:   on_error(msg->errorInfo.reason); break;
        case ix::WebSocketMessageType::Message: handle_message(msg->str); break;
        default: break;
    }
}

void GatewayClient::on_open() {
    log()->info("[GatewayClient] WebSocket connected to {}", options_.url);
    backoff_ms_ = 1000;
    connected_at_.store(now_ms());
    send_connect();  // 🎯 对齐 OC: send connect handshake immediately on open
}

void GatewayClient::on_close(uint16_t code, const std::string& reason) {
    connected_.store(false);
    log()->warn("[GatewayClient] WebSocket closed: code={} reason={}", code, reason);
    flush_pending_errors("gateway closed (" + std::to_string(code) + ": " + reason + ")");
    reconnect_count_.fetch_add(1);
    if (options_.on_close) options_.on_close(code, reason);
}

void GatewayClient::on_error(const std::string& error) {
    log()->error("[GatewayClient] WebSocket error: {}", error);
    if (options_.on_connect_error) options_.on_connect_error(error);
}

// ============================================================================
// 🎯 对齐 OpenClaw: connect handshake
// OC: { method: "connect", params: { client, auth, caps, role, scopes, ... } }
// Server responds with hello.ok event: { event: "hello.ok", payload: { connId, ... } }
// ============================================================================

void GatewayClient::send_connect() {
    connect_nonce_ = gen_req_id();
    connect_sent_ = true;

    json client_info = {
        {"id",      options_.client_name},
        {"version", options_.client_version},
        {"mode",    options_.mode.empty() ? "headless" : options_.mode},
    };
    if (!options_.platform.empty())
        client_info["platform"] = options_.platform;
    if (!options_.device_family.empty())
        client_info["deviceFamily"] = options_.device_family;
    if (!options_.instance_id.empty())
        client_info["instanceId"] = options_.instance_id;
    if (!options_.client_display_name.empty())
        client_info["displayName"] = options_.client_display_name;

    json connect_params = {
        {"minProtocol", options_.min_protocol},
        {"maxProtocol", options_.max_protocol},
        {"client",      client_info},
        {"role",        options_.role.empty() ? "operator" : options_.role},
        {"scopes",      options_.scopes},
        {"caps",        options_.caps},
    };

    if (!options_.commands.empty())
        connect_params["commands"] = options_.commands;
    if (!options_.permissions.empty())
        connect_params["permissions"] = options_.permissions;
    if (!options_.path_env.empty())
        connect_params["pathEnv"] = options_.path_env;

    // Auth
    json auth = json::object();
    if (!options_.token.empty())        auth["token"]       = options_.token;
    if (!options_.device_token.empty()) auth["deviceToken"] = options_.device_token;
    if (!options_.password.empty())     auth["password"]    = options_.password;
    if (!auth.empty())
        connect_params["auth"] = auth;

    // Device identity
    if (!options_.device_identity.device_id.empty()) {
        connect_params["device"] = {
            {"id",   options_.device_identity.device_id},
            {"name", options_.device_identity.device_name},
            {"type", options_.device_identity.device_type},
        };
    }

    json frame = {
        {"id",     gen_req_id()},
        {"method", "connect"},
        {"params", connect_params}
    };

    send_frame(frame);
    log()->debug("[GatewayClient] Sent connect handshake (role={} scopes={})",
        connect_params["role"].get<std::string>(),
        connect_params["scopes"].dump());
}

// ============================================================================
// 🎯 对齐 OpenClaw: parse and dispatch incoming WebSocket message
// ============================================================================

void GatewayClient::handle_message(const std::string& raw) {
    try {
        auto j = json::parse(raw);

        // 🎯 对齐 OC: event frame has "event" field
        if (j.contains("event") && j["event"].is_string()) {
            auto evt_result = EventFrame::from_json(j);
            if (evt_result.is_ok()) {
                handle_event(evt_result.get());
            }
            total_events_.fetch_add(1);
            return;
        }

        // 🎯 对齐 OC: response frame has "id" + "ok"
        if (j.contains("id") && j.contains("ok")) {
            auto res_result = ResponseFrame::from_json(j);
            if (res_result.is_ok()) {
                handle_response(res_result.get());
            }
            return;
        }

        log()->debug("[GatewayClient] Unknown frame: {}", raw.substr(0, 200));
    } catch (const std::exception& e) {
        log()->warn("[GatewayClient] Failed to parse message: {}", e.what());
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: handle event frame
// ============================================================================

void GatewayClient::handle_event(const EventFrame& evt) {
    // 🎯 对齐 OC: hello.ok → connection established, connId assigned
    if (evt.event == "hello.ok") {
        auto hello_result = HelloOk::from_payload(evt.payload);
        if (hello_result.is_ok()) {
            hello_ok_ = hello_result.get();
            conn_id_ = hello_ok_->conn_id;
            connected_.store(true);
            log()->info("[GatewayClient] Connected: connId={} protocol={}",
                conn_id_, hello_ok_->protocol_version);
            if (options_.on_hello_ok) options_.on_hello_ok(*hello_ok_);
        } else {
            log()->warn("[GatewayClient] hello.ok parse failed: {}", hello_result.error);
        }
        return;
    }

    // 🎯 对齐 OC: tick → heartbeat, update last_tick for stale detection
    if (evt.event == "tick") {
        if (evt.payload.contains("ts") && evt.payload["ts"].is_number()) {
            last_tick_ = evt.payload["ts"].get<int64_t>();
        }
        return;
    }

    // 🎯 对齐 OC: gap detection
    if (evt.seq.has_value() && last_seq_.has_value()) {
        int64_t expected = *last_seq_ + 1;
        if (*evt.seq > expected && options_.on_gap) {
            options_.on_gap(expected, *evt.seq);
        }
    }
    if (evt.seq.has_value()) {
        last_seq_ = evt.seq;
    }

    // Forward to user callback
    if (options_.on_event) options_.on_event(evt);
}

// ============================================================================
// 🎯 对齐 OpenClaw: handle response frame
// Route to waiting promise via response_callbacks_
// ============================================================================

void GatewayClient::handle_response(const ResponseFrame& res) {
    // Remove from pending_ tracking
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(res.id);
    }

    // Find and invoke response callback
    std::function<void(core::Result<json>)> cb;
    {
        std::lock_guard<std::mutex> lock(response_callbacks_mutex_);
        auto it = response_callbacks_.find(res.id);
        if (it == response_callbacks_.end()) {
            log()->debug("[GatewayClient] Response for unknown id: {}", res.id);
            return;
        }
        cb = std::move(it->second);
        response_callbacks_.erase(it);
    }

    log()->debug("[GatewayClient] ← response id={} ok={}", res.id, res.ok);

    if (res.ok) {
        cb(core::Result<json>::Ok(res.payload));
    } else {
        std::string err_msg = "gateway error";
        if (res.error.has_value()) {
            err_msg = res.error->message;
            if (!res.error->code.empty()) {
                err_msg = res.error->code + ": " + err_msg;
            }
        }
        cb(core::Result<json>::Err(err_msg));
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: send frame via WebSocket
// ============================================================================

void GatewayClient::send_frame(const json& frame) {
    if (!ws_) throw std::runtime_error("websocket not initialized");
    auto result = ws_->send(frame.dump());
    if (!result.success) {
        throw std::runtime_error("websocket send failed"
            + (result.compressionError ? " (compression error)" : ""));
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: flush all pending requests with error on disconnect
// ============================================================================

void GatewayClient::flush_pending_errors(const std::string& error_message) {
    // Clear pending_ tracking
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.clear();
    }

    // Invoke all waiting callbacks with error
    std::map<std::string, std::function<void(core::Result<json>)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(response_callbacks_mutex_);
        callbacks = std::move(response_callbacks_);
        response_callbacks_.clear();
    }

    for (auto& [id, cb] : callbacks) {
        cb(core::Result<json>::Err(error_message));
    }
}

// ============================================================================
// 🎯 对齐 OpenClaw: tick watch — detect stale connections
// OC: tickWatchMinIntervalMs (default 30s), reconnect if no tick for 3x interval
// ============================================================================

void GatewayClient::start_tick_watch() {
    tick_watch_running_.store(true);
    tick_watch_thread_ = std::thread([this]() {
        while (tick_watch_running_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(options_.tick_watch_min_interval_ms)
            );
            if (!tick_watch_running_.load()) break;
            check_tick_timeout();
        }
    });
}

void GatewayClient::stop_tick_watch() {
    tick_watch_running_.store(false);
    if (tick_watch_thread_.joinable()) {
        tick_watch_thread_.join();
    }
}

void GatewayClient::check_tick_timeout() {
    if (!connected_.load()) return;
    if (!last_tick_.has_value()) return;

    int64_t elapsed = now_ms() - *last_tick_;
    // 🎯 对齐 OC: reconnect if no tick for 3x interval
    if (elapsed > tick_interval_ms_ * 3) {
        log()->warn("[GatewayClient] Tick timeout ({}ms), reconnecting", elapsed);
        connected_.store(false);
        ws_->stop();
        ws_->start();
    }
}

// ============================================================================
// Accessors
// ============================================================================

std::optional<std::string> GatewayClient::get_conn_id() const {
    if (conn_id_.empty()) return std::nullopt;
    return conn_id_;
}

std::optional<HelloOk> GatewayClient::get_hello_ok() const {
    return hello_ok_;
}

GatewayClient::Statistics GatewayClient::get_statistics() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return Statistics{
        .connected_at     = connected_at_.load(),
        .last_tick_at     = last_tick_.value_or(0),
        .reconnect_count  = reconnect_count_.load(),
        .total_requests   = static_cast<int64_t>(total_requests_.load()),
        .total_events     = static_cast<int64_t>(total_events_.load()),
        .pending_requests = static_cast<int64_t>(pending_.size()),
    };
}

// ============================================================================
// 🎯 对齐 OpenClaw: create_gateway_client factory
// ============================================================================

core::Result<std::shared_ptr<GatewayClient>> create_gateway_client(
    const GatewayClientOptions& options
) {
    try {
        auto client = std::make_shared<GatewayClient>(options);
        return core::Result<std::shared_ptr<GatewayClient>>::Ok(client);
    } catch (const std::exception& e) {
        return core::Result<std::shared_ptr<GatewayClient>>::Err(
            std::string("Failed to create gateway client: ") + e.what()
        );
    }
}

} // namespace gateway
} // namespace hunterclaw
