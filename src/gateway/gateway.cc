#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/connection_pool.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include <map>
#include <mutex>
#include <set>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <random>
#include <cstdio>
#include <boost/system/error_code.hpp>

using json = nlohmann::json;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

namespace hunterclaw {
namespace gateway {

typedef websocketpp::server<websocketpp::config::asio> server;
typedef server::message_ptr message_ptr;

// 🎯 OpenClaw 对齐: 每连接握手状态
// 对应 OpenClaw ws-connection.ts 中的 handshakeState / connectNonce / handshakeTimer
struct ConnectionHandshakeState {
    enum class Phase { Pending, Connected, Failed };
    Phase phase = Phase::Pending;
    std::string connect_nonce;          // 发给客户端的 challenge nonce
    std::chrono::steady_clock::time_point opened_at;
    std::string conn_id;                // 连接唯一 ID
    std::string close_cause;            // 关闭原因 (用于日志)
};

// 握手超时常量 (对齐 OpenClaw DEFAULT_HANDSHAKE_TIMEOUT_MS = 3000)
static constexpr int64_t HANDSHAKE_TIMEOUT_MS = 3000;
// tick 心跳间隔 (对齐 OpenClaw TICK_INTERVAL_MS = 30000)
static constexpr int64_t TICK_INTERVAL_MS = 30000;
// 健康刷新间隔 (对齐 OpenClaw HEALTH_REFRESH_INTERVAL_MS = 60000)
static constexpr int64_t HEALTH_REFRESH_INTERVAL_MS = 60000;
// 握手前最大 payload (对齐 OpenClaw MAX_PREAUTH_PAYLOAD_BYTES = 64KB)
static constexpr size_t MAX_PREAUTH_PAYLOAD_BYTES = 64 * 1024;
// 最大 payload (对齐 OpenClaw MAX_PAYLOAD_BYTES = 25MB)
static constexpr size_t MAX_PAYLOAD_BYTES = 25 * 1024 * 1024;
// 最大缓冲字节 (对齐 OpenClaw MAX_BUFFERED_BYTES = 50MB)
static constexpr size_t MAX_BUFFERED_BYTES = 50 * 1024 * 1024;

// 🎯 OpenClaw 对齐: GATEWAY_EVENTS 列表 (对应 server-methods-list.ts:GATEWAY_EVENTS)
static const std::vector<std::string> GATEWAY_EVENTS = {
    "connect.challenge", "agent", "chat", "presence", "tick",
    "talk.mode", "shutdown", "health", "heartbeat", "cron",
    "node.pair.requested", "node.pair.resolved", "node.invoke.request",
    "device.pair.requested", "device.pair.resolved", "voicewake.changed",
    "exec.approval.requested", "exec.approval.resolved", "update.available",
};

// 🎯 OpenClaw 对齐: BASE_METHODS 列表 (对应 server-methods-list.ts:BASE_METHODS)
static const std::vector<std::string> GATEWAY_METHODS = {
    "health", "doctor.memory.status", "logs.tail",
    "channels.status", "channels.logout",
    "status", "usage.status", "usage.cost",
    "tts.status", "tts.providers", "tts.enable", "tts.disable", "tts.convert", "tts.setProvider",
    "config.get", "config.set", "config.apply", "config.patch",
    "config.schema", "config.schema.lookup",
    "exec.approvals.get", "exec.approvals.set",
    "exec.approvals.node.get", "exec.approvals.node.set",
    "exec.approval.request", "exec.approval.waitDecision", "exec.approval.resolve",
    "talk.config", "talk.mode",
    "models.list", "tools.catalog",
    "agents.list", "agents.create", "agents.update", "agents.delete",
    "agents.files.list", "agents.files.get", "agents.files.set",
    "skills.status", "skills.bins", "skills.install", "skills.update",
    "update.run", "secrets.reload", "secrets.resolve",
    "sessions.list", "sessions.preview", "sessions.patch",
    "sessions.reset", "sessions.delete", "sessions.compact",
    "wake",
    "node.pair.request", "node.pair.list", "node.pair.approve",
    "node.pair.reject", "node.pair.verify",
    "device.pair.list", "device.pair.approve", "device.pair.reject",
    "device.pair.remove", "device.token.rotate", "device.token.revoke",
    "node.rename", "node.list", "node.describe",
    "node.pending.pull", "node.pending.ack",
    "node.invoke", "node.event", "node.canvas.capability.refresh",
    "cron.list", "cron.status", "cron.add", "cron.update",
    "cron.remove", "cron.run", "cron.runs",
    "gateway.identity.get",
    "agent", "agent.identity.get", "agent.wait",
    "browser.request", "push.test",
    "chat.history", "chat.abort", "chat.send",
    "chat.update_params", "chat.regenerate", "chat.fork",
    "chat.summarize", "chat.inject", "chat.edit",
    "chat.delete", "chat.transcript", "chat.export",
    "send.message",
    "sessions.resolve", "sessions.get", "sessions.usage",
    "sessions.usage.timeseries", "sessions.usage.logs",
    "web.login.start", "web.login.wait",
};

// 🎯 OpenClaw 对齐: 事件 scope 守卫 (对应 server-broadcast.ts:EVENT_SCOPE_GUARDS)
// 这些敏感事件只广播给有对应 scope 的 operator 客户端
static const std::map<std::string, std::vector<std::string>> EVENT_SCOPE_GUARDS = {
    {"exec.approval.requested", {"operator.approvals"}},
    {"exec.approval.resolved",  {"operator.approvals"}},
    {"device.pair.requested",   {"operator.pairing"}},
    {"device.pair.resolved",    {"operator.pairing"}},
    {"node.pair.requested",     {"operator.pairing"}},
    {"node.pair.resolved",      {"operator.pairing"}},
};

// 🎯 OpenClaw 对齐: 生成 UUID v4 (对应 randomUUID())
static std::string generate_uuid_v4() {
    // 使用 /dev/urandom 生成随机字节
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);
    // 设置 UUID v4 版本位和变体位
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 1
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (uint32_t)(hi >> 32),
        (uint16_t)(hi >> 16),
        (uint16_t)(hi),
        (uint16_t)(lo >> 48),
        (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL)
    );
    return std::string(buf);
}

// Internal implementation class (不继承 GatewayServer)
class GatewayServerImpl {
public:
    explicit GatewayServerImpl(std::shared_ptr<core::Config> config)
        : config_(config), runtime_state_(std::make_shared<GatewayRuntimeState>()) {
        
        // Set up WebSocket server
        ws_server_.init_asio();
        ws_server_.set_reuse_addr(true);
        
        // Set handlers
        ws_server_.set_open_handler(bind(&GatewayServerImpl::on_open, this, ::_1));
        ws_server_.set_close_handler(bind(&GatewayServerImpl::on_close, this, ::_1));
        ws_server_.set_message_handler(bind(&GatewayServerImpl::on_message, this, ::_1, ::_2));
    }
    
    core::Result<bool> start() {
        try {
            ws_server_.listen(config_->gateway.port);
            ws_server_.start_accept();
            
            // Run in background thread
            server_thread_ = std::thread([this]() {
                ws_server_.run();
            });
            
            // 🎯 OpenClaw 严格对齐: 启动 tick 心跳定时器
            // 对应 OpenClaw server-maintenance.ts:
            //   const tickInterval = setInterval(() => {
            //     broadcast("tick", { ts: Date.now() }, { dropIfSlow: true });
            //   }, TICK_INTERVAL_MS);
            tick_running_ = true;
            tick_thread_ = std::thread([this]() {
                while (tick_running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(TICK_INTERVAL_MS));
                    if (!tick_running_) break;
                    try {
                        int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
                        ).count();
                        json tick_payload;
                        tick_payload["ts"] = ts_ms;
                        
                        json tick_frame;
                        tick_frame["type"] = "event";
                        tick_frame["event"] = "tick";
                        tick_frame["payload"] = tick_payload;
                        // dropIfSlow: true (对齐 OpenClaw)
                        std::string tick_msg = tick_frame.dump();
                        
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (auto hdl : connections_) {
                            // 只向已完成握手的连接发送 tick
                            auto hs_it = handshake_states_.find(hdl);
                            if (hs_it != handshake_states_.end() &&
                                hs_it->second.phase == ConnectionHandshakeState::Phase::Connected) {
                                try {
                                    ws_server_.send(hdl, tick_msg, websocketpp::frame::opcode::text);
                                } catch (...) {}
                            }
                        }
                    } catch (...) {}
                }
            });
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to start gateway: ") + e.what());
        }
    }
    
    core::Result<bool> stop() {
        try {
            // 🎯 OpenClaw 对齐: 先停止 tick 心跳
            tick_running_ = false;
            if (tick_thread_.joinable()) {
                tick_thread_.join();
            }
            
            ws_server_.stop_listening();
            
            // Close all connections
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (auto hdl : connections_) {
                    ws_server_.close(hdl, websocketpp::close::status::going_away, "Server shutting down");
                }
            }
            
            ws_server_.stop();
            
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to stop gateway: ") + e.what());
        }
    }
    
    void broadcast(const core::EventFrame& event,
                    bool drop_if_slow = false,
                    const std::set<connection_hdl, std::owner_less<connection_hdl>>* target_hdls = nullptr) {
        try {
            // 🎯 OpenClaw 严格对齐: broadcast 实现
            // 对应 OpenClaw server-broadcast.ts:broadcastInternal
            //
            // 1. 全局递增 seq (非 targeted broadcast 才递增)
            // 2. scope 过滤 (EVENT_SCOPE_GUARDS)
            // 3. dropIfSlow: bufferedAmount > MAX_BUFFERED_BYTES 时跳过或关闭
            // 4. stateVersion 字段 (presence/health 版本号)
            
            const bool is_targeted = (target_hdls != nullptr);
            int64_t event_seq = -1;
            if (!is_targeted) {
                event_seq = ++broadcast_seq_;  // 全局递增 seq
            }
            
            json j;
            j["type"] = "event";
            j["event"] = event.event;
            if (event_seq >= 0) {
                j["seq"] = event_seq;
            }
            
            // Serialize event.payload
            if (event.payload.is_null()) {
                j["payload"] = nullptr;
            } else if (event.payload.is_object() || event.payload.is_array()) {
                j["payload"] = event.payload.to_json();
            } else if (event.payload.is_string()) {
                j["payload"] = event.payload.as_string();
            } else {
                j["payload"] = event.payload.to_json();
            }
            
            std::string msg = j.dump();
            
            // 🎯 OpenClaw 对齐: 检查事件 scope 守卫
            auto scope_guard_it = EVENT_SCOPE_GUARDS.find(event.event);
            const std::vector<std::string>* required_scopes =
                (scope_guard_it != EVENT_SCOPE_GUARDS.end()) ? &scope_guard_it->second : nullptr;
            
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto hdl : connections_) {
                // targeted broadcast: 只发给指定连接
                if (is_targeted && target_hdls->find(hdl) == target_hdls->end()) {
                    continue;
                }
                
                // 只向已完成握手的连接发送
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it == handshake_states_.end() ||
                    hs_it->second.phase != ConnectionHandshakeState::Phase::Connected) {
                    continue;
                }
                
                // 🎯 OpenClaw 对齐: scope 过滤
                // 对应 OpenClaw: if (!hasEventScope(c, event)) continue;
                if (required_scopes) {
                    // 敏感事件: 检查客户端是否有对应 scope
                    auto conn_it = handle_to_conn_id_.find(hdl);
                    if (conn_it != handle_to_conn_id_.end()) {
                        auto state = runtime_state_;
                        if (state) {
                            std::lock_guard<std::mutex> state_lock(state->mutex);
                            auto client_it = state->clients.find(conn_it->second);
                            if (client_it != state->clients.end()) {
                                const auto& client_scopes = client_it->second->scopes;
                                bool has_admin = std::find(client_scopes.begin(), client_scopes.end(),
                                                           "operator.admin") != client_scopes.end();
                                if (!has_admin) {
                                    bool has_required = false;
                                    for (const auto& req_scope : *required_scopes) {
                                        if (std::find(client_scopes.begin(), client_scopes.end(),
                                                      req_scope) != client_scopes.end()) {
                                            has_required = true;
                                            break;
                                        }
                                    }
                                    if (!has_required) continue;
                                }
                            }
                        }
                    }
                }
                
                // 🎯 OpenClaw 对齐: 慢消费者处理
                // 对应 OpenClaw:
                //   const slow = c.socket.bufferedAmount > MAX_BUFFERED_BYTES;
                //   if (slow && opts?.dropIfSlow) continue;
                //   if (slow) { c.socket.close(1008, "slow consumer"); continue; }
                try {
                    auto conn = ws_server_.get_con_from_hdl(hdl);
                    if (conn) {
                        size_t buffered = conn->get_buffered_amount();
                        if (buffered > MAX_BUFFERED_BYTES) {
                            if (drop_if_slow) {
                                continue;  // dropIfSlow: 跳过慢消费者
                            } else {
                                // 关闭慢消费者连接
                                try { ws_server_.close(hdl, 1008, "slow consumer"); } catch (...) {}
                                continue;
                            }
                        }
                    }
                } catch (...) {}
                
                try {
                    ws_server_.send(hdl, msg, websocketpp::frame::opcode::text);
                } catch (...) {}
            }
            
        } catch (const std::exception& e) {
            // Log error but don't throw
        }
    }
    
    std::shared_ptr<GatewayRuntimeState> runtime_state() const {
        return runtime_state_;
    }
    
    // Send message to specific connection (for targeted broadcast)
    void send_to_handle(connection_hdl hdl, const std::string& msg) {
        try {
            ws_server_.send(hdl, msg, websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            // Log error but don't throw
        }
    }
    
private:
    void on_open(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.insert(hdl);
        
        // 🎯 OpenClaw 严格对齐: 生成 connId (对应 randomUUID())
        // OpenClaw: const connId = randomUUID();
        std::string conn_id = generate_uuid_v4();
        
        // 🎯 OpenClaw 严格对齐: 生成 connect.challenge nonce
        // 对应 OpenClaw: const connectNonce = randomUUID();
        std::string nonce = generate_uuid_v4();
        
        // 初始化握手状态
        ConnectionHandshakeState hs;
        hs.phase = ConnectionHandshakeState::Phase::Pending;
        hs.connect_nonce = nonce;
        hs.opened_at = std::chrono::steady_clock::now();
        hs.conn_id = conn_id;
        handshake_states_[hdl] = hs;
        
        // 保存 handle → conn_id 映射
        handle_to_conn_id_[hdl] = conn_id;
        
        // 🎯 OpenClaw 严格对齐: 立即发送 connect.challenge 事件帧
        // 对应 OpenClaw:
        //   send({ type: "event", event: "connect.challenge",
        //          payload: { nonce: connectNonce, ts: Date.now() } });
        try {
            int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            json challenge;
            challenge["type"] = "event";
            challenge["event"] = "connect.challenge";
            challenge["payload"] = json{{"nonce", nonce}, {"ts", ts_ms}};
            ws_server_.send(hdl, challenge.dump(), websocketpp::frame::opcode::text);
        } catch (const std::exception& e) {
            // 发送 challenge 失败，关闭连接
            try { ws_server_.close(hdl, 1011, "challenge send failed"); } catch (...) {}
            return;
        }
        
        // 🎯 OpenClaw 严格对齐: 握手超时定时器
        // 对应 OpenClaw: const handshakeTimer = setTimeout(() => { ... close() }, handshakeTimeoutMs)
        // 这里用异步方式：在 on_message 中检查超时
        // (websocketpp 不直接支持 per-connection timer，通过 asio 实现)
        try {
            auto& asio_io = ws_server_.get_io_service();
            auto timer = std::make_shared<websocketpp::lib::asio::steady_timer>(asio_io);
            timer->expires_from_now(std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS));
            timer->async_wait([this, hdl, conn_id, timer](const boost::system::error_code& ec) {
                if (ec) return;  // 被取消
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = handshake_states_.find(hdl);
                if (it != handshake_states_.end() &&
                    it->second.phase == ConnectionHandshakeState::Phase::Pending) {
                    // 握手超时，关闭连接
                    it->second.phase = ConnectionHandshakeState::Phase::Failed;
                    it->second.close_cause = "handshake-timeout";
                    try {
                        ws_server_.close(hdl, 1008, "handshake timeout");
                    } catch (...) {}
                }
            });
            handshake_timers_[hdl] = timer;
        } catch (const std::exception& e) {
            // timer 创建失败不影响主流程
        }
    }
    
    void on_close(connection_hdl hdl) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(hdl);
        
        // 🎯 OpenClaw 对齐: 取消握手超时定时器
        auto timer_it = handshake_timers_.find(hdl);
        if (timer_it != handshake_timers_.end()) {
            timer_it->second->cancel();
            handshake_timers_.erase(timer_it);
        }
        
        // 清理握手状态
        handshake_states_.erase(hdl);
        
        // 🎯 OpenClaw 对齐: 从 ConnectionPool 注销连接
        auto it = handle_to_conn_id_.find(hdl);
        if (it != handle_to_conn_id_.end()) {
            auto& pool = ConnectionPool::instance();
            pool.remove_client(it->second);
            handle_to_conn_id_.erase(it);
        }
    }
    
    void on_message(connection_hdl hdl, message_ptr msg) {
        auto start_time = std::chrono::steady_clock::now();
        std::string conn_id;
        std::string method_name;
        
        try {
            std::string payload_str = msg->get_payload();
            
            // 🎯 OpenClaw 严格对齐: 握手前 payload 大小限制
            // 对应 OpenClaw message-handler.ts:
            //   const preauthPayloadBytes = !getClient() ? getRawDataByteLength(data) : undefined;
            //   if (preauthPayloadBytes > MAX_PREAUTH_PAYLOAD_BYTES) close(1009, "preauth payload too large");
            bool is_pre_auth = false;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                is_pre_auth = (hs_it == handshake_states_.end() ||
                               hs_it->second.phase == ConnectionHandshakeState::Phase::Pending);
            }
            if (is_pre_auth && payload_str.size() > MAX_PREAUTH_PAYLOAD_BYTES) {
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    auto hs_it = handshake_states_.find(hdl);
                    if (hs_it != handshake_states_.end()) {
                        hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                        hs_it->second.close_cause = "preauth-payload-too-large";
                    }
                }
                try { ws_server_.close(hdl, 1009, "preauth payload too large"); } catch (...) {}
                return;
            }
            
            json j = json::parse(payload_str);
            
            // 提取 conn_id
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = handle_to_conn_id_.find(hdl);
                if (it != handle_to_conn_id_.end()) {
                    conn_id = it->second;
                }
            }
            
            // 验证是 req 帧
            std::string msg_type = j.contains("type") && j["type"].is_string()
                ? j["type"].get<std::string>() : "";
            if (msg_type != "req") {
                // 非 req 帧: 握手前直接关闭，握手后忽略
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it == handshake_states_.end() ||
                    hs_it->second.phase == ConnectionHandshakeState::Phase::Pending) {
                    try { ws_server_.close(hdl, 1008, "invalid handshake"); } catch (...) {}
                }
                return;
            }
            
            // 解析 req 帧
            std::string req_id = j.contains("id") && j["id"].is_string()
                ? j["id"].get<std::string>() : "";
            std::string req_method = j.contains("method") && j["method"].is_string()
                ? j["method"].get<std::string>() : "";
            method_name = req_method;
            json req_params = j.contains("params") && j["params"].is_object()
                ? j["params"] : json::object();
            
            // ============================================================
            // 🎯 OpenClaw 严格对齐: 握手状态机
            // 对应 OpenClaw message-handler.ts:
            //   if (!client) { /* 握手阶段: 只接受 connect 方法 */ }
            //   else { /* 已连接: 处理普通请求 */ }
            // ============================================================
            bool is_handshake_pending = false;
            std::string connect_nonce;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it != handshake_states_.end()) {
                    is_handshake_pending =
                        (hs_it->second.phase == ConnectionHandshakeState::Phase::Pending);
                    connect_nonce = hs_it->second.connect_nonce;
                }
            }
            
            if (is_handshake_pending) {
                // --------------------------------------------------------
                // 握手阶段: 只接受 method == "connect"
                // 对应 OpenClaw:
                //   if (!isRequestFrame || parsed.method !== "connect" || !validateConnectParams(...))
                // --------------------------------------------------------
                if (req_method != "connect") {
                    // 握手阶段收到非 connect 请求 → 拒绝并关闭
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"},
                                           {"message", "invalid handshake: first request must be connect"}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "invalid-handshake";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, "invalid handshake"); } catch (...) {}
                    return;
                }
                
                // 处理 connect 请求
                // 对应 OpenClaw: 验证 auth、protocol、device nonce 等
                // HunterClaw 简化版: 验证 nonce (如果客户端提供了 device)
                bool auth_ok = true;
                std::string auth_error;
                
                // 验证 device nonce (对齐 OpenClaw nonce 校验)
                if (req_params.contains("device") && req_params["device"].is_object()) {
                    auto& device = req_params["device"];
                    std::string provided_nonce = device.contains("nonce") && device["nonce"].is_string()
                        ? device["nonce"].get<std::string>() : "";
                    if (!provided_nonce.empty() && provided_nonce != connect_nonce) {
                        auth_ok = false;
                        auth_error = "device nonce mismatch";
                    }
                }
                
                if (!auth_ok) {
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", auth_error}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "unauthorized";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, auth_error.c_str()); } catch (...) {}
                    return;
                }
                
                // 🎯 OpenClaw 严格对齐: 取消握手超时定时器
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    auto timer_it = handshake_timers_.find(hdl);
                    if (timer_it != handshake_timers_.end()) {
                        timer_it->second->cancel();
                        handshake_timers_.erase(timer_it);
                    }
                    // 更新握手状态为 Connected
                    auto hs_it = handshake_states_.find(hdl);
                    if (hs_it != handshake_states_.end()) {
                        hs_it->second.phase = ConnectionHandshakeState::Phase::Connected;
                    }
                }
                
                // 🎯 OpenClaw 严格对齐: 发送 hello-ok 响应
                // 对应 OpenClaw:
                //   send({ type: "res", id: frame.id, ok: true, payload: helloOk });
                // helloOk 包含: type, protocol, server, features, snapshot, policy
                //
                // 🎯 OpenClaw 对齐: PROTOCOL_VERSION = 1 (protocol/schema.ts)
                json hello_ok;
                hello_ok["type"] = "hello-ok";
                hello_ok["protocol"] = 1;  // PROTOCOL_VERSION
                hello_ok["server"] = json{{"connId", conn_id}, {"version", "1.0.0"}};
                
                // 🎯 OpenClaw 对齐: features.methods = listGatewayMethods()
                //                   features.events = GATEWAY_EVENTS
                // 对应 OpenClaw message-handler.ts:
                //   features: { methods: gatewayMethods, events }
                json methods_arr = json::array();
                for (const auto& m : GATEWAY_METHODS) methods_arr.push_back(m);
                json events_arr = json::array();
                for (const auto& e : GATEWAY_EVENTS) events_arr.push_back(e);
                hello_ok["features"] = json{{"methods", methods_arr}, {"events", events_arr}};
                
                // 🎯 OpenClaw 对齐: snapshot (对应 buildGatewaySnapshot())
                // 包含: presence, health, stateVersion, uptimeMs, configPath,
                //        stateDir, sessionDefaults, authMode
                int64_t uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                json snapshot;
                snapshot["presence"] = json::array();  // TODO: listSystemPresence()
                snapshot["health"] = json::object();   // async, filled after connect
                snapshot["stateVersion"] = json{{"presence", 1}, {"health", 1}};
                snapshot["uptimeMs"] = uptime_ms;
                snapshot["configPath"] = config_ ? config_->config_dir + "/config.yaml" : "";
                snapshot["stateDir"] = config_ ? config_->workspace_dir : "";
                snapshot["sessionDefaults"] = json{
                    {"defaultAgentId", nullptr},
                    {"mainKey", nullptr},
                    {"mainSessionKey", nullptr},
                    {"scope", "per-sender"}
                };
                snapshot["authMode"] = config_ ? config_->gateway.auth.mode : "none";
                hello_ok["snapshot"] = snapshot;
                
                // 🎯 OpenClaw 对齐: policy (对应 server-constants.ts)
                hello_ok["policy"] = json{
                    {"maxPayload", (int64_t)MAX_PAYLOAD_BYTES},
                    {"maxBufferedBytes", (int64_t)MAX_BUFFERED_BYTES},
                    {"tickIntervalMs", TICK_INTERVAL_MS}
                };
                
                json res_j;
                res_j["type"] = "res";
                res_j["id"] = req_id;
                res_j["ok"] = true;
                res_j["payload"] = hello_ok;
                try { ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                return;
            }
            
            // --------------------------------------------------------
            // 已连接阶段: 处理普通请求
            // 对应 OpenClaw: handleGatewayRequest({ req, respond, client, ... })
            // --------------------------------------------------------
            core::RequestFrame req;
            req.id = req_id;
            req.method = req_method;
            req.params = req_params;
            
            auto response = handle_request(req);
            
            // 发送响应帧
            json res_j;
            res_j["type"] = "res";
            res_j["id"] = req.id;
            res_j["ok"] = response.ok;
            if (response.ok) {
                if (!response.payload.is_null()) {
                    res_j["payload"] = response.payload.to_json();
                }
            } else {
                if (!response.error.empty()) {
                    res_j["error"] = json{{"code", "UNAVAILABLE"}, {"message", response.error}};
                } else {
                    res_j["error"] = json{{"code", "UNAVAILABLE"}, {"message", "method failed"}};
                }
            }
            ws_server_.send(hdl, res_j.dump(), websocketpp::frame::opcode::text);
            
        } catch (const std::exception& e) {
            // 解析/处理错误: 发送错误响应
            json error_res;
            error_res["type"] = "res";
            error_res["ok"] = false;
            error_res["error"] = json{{"code", "UNAVAILABLE"},
                                      {"message", std::string("Failed to process request: ") + e.what()}};
            try {
                ws_server_.send(hdl, error_res.dump(), websocketpp::frame::opcode::text);
            } catch (...) {}
        }
    }
    
    core::ResponseFrame handle_request(const core::RequestFrame& req) {
        // Use new method handler (对齐 OpenClaw server-methods/)
        if (!method_handler_) {
            core::ResponseFrame response;
            response.id = req.id;
            response.ok = false;
            response.error = "Method handler not initialized";
            return response;
        }
        
        try {
            // 🎯 OpenClaw 对齐: 传递 req.params 给 method_handler_
            // 对应 OpenClaw: handler({ req, params: req.params ?? {}, ... })
            nlohmann::json params = req.params.is_null() ? nlohmann::json::object() : req.params.to_json();
            
            // Invoke method
            auto result = method_handler_(req.method, params);
            
            if (result.is_ok()) {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = true;
                return response;
            } else {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = false;
                response.error = result.error;
                return response;
            }
        } catch (const std::exception& e) {
            core::ResponseFrame response;
            response.id = req.id;
            response.ok = false;
            response.error = std::string("Method execution failed: ") + e.what();
            return response;
        }
    }
    
    std::shared_ptr<core::Config> config_;
    server ws_server_;
    std::thread server_thread_;
    std::set<connection_hdl, std::owner_less<connection_hdl>> connections_;
    std::mutex connections_mutex_;
    std::shared_ptr<GatewayRuntimeState> runtime_state_;
    std::function<core::Result<nlohmann::json>(const std::string&, const nlohmann::json&)> method_handler_;
    
    // 🎯 OpenClaw 对齐: 全局广播 seq 计数器 (对应 server-broadcast.ts: let seq = 0)
    std::atomic<int64_t> broadcast_seq_{0};
    
    // handle → conn_id mapping (用于 on_close 时查找)
    std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> handle_to_conn_id_;
    
    // 🎯 OpenClaw 对齐: tick 心跳线程
    std::thread tick_thread_;
    std::atomic<bool> tick_running_{false};
    
    // 🎯 OpenClaw 对齐: 每连接握手状态 (对应 handshakeState / connectNonce)
    std::map<connection_hdl, ConnectionHandshakeState, std::owner_less<connection_hdl>> handshake_states_;
    
    // 🎯 OpenClaw 对齐: 每连接握手超时定时器 (对应 handshakeTimer)
    using AsioTimer = websocketpp::lib::asio::steady_timer;
    std::map<connection_hdl, std::shared_ptr<AsioTimer>, std::owner_less<connection_hdl>> handshake_timers_;
};

// Session Manager Implementation
class SessionManagerImpl : public SessionManager {
public:
    SessionManagerImpl() {
        // Load existing sessions from disk on startup
        load_sessions();
    }
    
    std::string create_session(const core::SessionKey& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string session_id = core::utils::generate_uuid();
        
        core::SessionInfo info;
        info.key = key;
        info.session_id = session_id;
        info.created_at = core::utils::now();
        info.last_active_at = info.created_at;
        
        sessions_[session_id] = info;
        
        return session_id;
    }
    
    std::optional<core::SessionInfo> get_session(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return std::nullopt;
        }
        
        return it->second;
    }
    
    bool update_last_active(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return false;
        }
        
        it->second.last_active_at = core::utils::now();
        return true;
    }
    
    bool delete_session(const std::string& session_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return false;
        }
        
        sessions_.erase(it);
        return true;
    }
    
    std::vector<core::SessionInfo> list_sessions() override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<core::SessionInfo> result;
        for (const auto& pair : sessions_) {
            result.push_back(pair.second);
        }
        return result;
    }
    
    std::vector<core::SessionInfo> find_sessions_by_key(const core::SessionKey& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<core::SessionInfo> results;
        for (const auto& pair : sessions_) {
            if (pair.second.key.device_id == key.device_id &&
                pair.second.key.device_token == key.device_token) {
                results.push_back(pair.second);
            }
        }
        return results;
    }
    
    core::Result<bool> save_session(const core::SessionInfo& session) override {
        // Session persistence (完整实现 - 对齐 OpenClaw)
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            // Create sessions directory if not exists
            std::string sessions_dir = "./data/sessions";
            std::filesystem::create_directories(sessions_dir);
            
            // Serialize session to JSON
            json j;
            j["session_id"] = session.session_id;
            j["channel_id"] = session.key.channel;
            j["account_id"] = session.key.account_id;
            j["conversation_id"] = session.key.target_id;
            j["device_id"] = session.key.device_id;
            j["device_token"] = session.key.device_token;
            j["created_at"] = session.created_at.time_since_epoch().count();
            j["last_active_at"] = session.last_active_at.time_since_epoch().count();
            
            // Serialize metadata
            if (!session.metadata.empty()) {
                j["metadata"] = json::object();
                for (const auto& [key, value] : session.metadata) {
                    j["metadata"][key] = value;
                }
            }
            
            // Write to file
            std::string filepath = sessions_dir + "/" + session.session_id + ".json";
            std::ofstream file(filepath);
            if (!file.is_open()) {
                return core::Result<bool>::Err("Failed to open session file: " + filepath);
            }
            
            file << j.dump(2);
            file.close();
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to save session: ") + e.what());
        }
    }
    
    core::Result<bool> load_sessions() override {
        // Load sessions from disk (完整实现 - 对齐 OpenClaw)
        std::lock_guard<std::mutex> lock(mutex_);
        
        try {
            std::string sessions_dir = "./data/sessions";
            
            // Check if directory exists
            if (!std::filesystem::exists(sessions_dir)) {
                // No sessions to load
                return core::Result<bool>::Ok(true);
            }
            
            // Iterate through all .json files
            for (const auto& entry : std::filesystem::directory_iterator(sessions_dir)) {
                if (entry.path().extension() == ".json") {
                    try {
                        std::ifstream file(entry.path());
                        if (!file.is_open()) {
                            continue;
                        }
                        
                        json j;
                        file >> j;
                        file.close();
                        
                        // Deserialize session
                        core::SessionInfo session;
                        session.session_id = j.value("session_id", "");
                        session.key.channel = j.value("channel_id", "");
                        session.key.account_id = j.value("account_id", "");
                        session.key.target_id = j.value("conversation_id", "");
                        session.key.device_id = j.value("device_id", "");
                        session.key.device_token = j.value("device_token", "");
                        
                        // Parse timestamps
                        if (j.contains("created_at")) {
                            int64_t created_at = j["created_at"];
                            session.created_at = core::Timestamp(std::chrono::nanoseconds(created_at));
                        }
                        if (j.contains("last_active_at")) {
                            int64_t last_active_at = j["last_active_at"];
                            session.last_active_at = core::Timestamp(std::chrono::nanoseconds(last_active_at));
                        }
                        
                        // Parse metadata
                        if (j.contains("metadata") && j["metadata"].is_object()) {
                            for (auto& [key, value] : j["metadata"].items()) {
                                if (value.is_string()) {
                                    session.metadata[key] = value.get<std::string>();
                                } else {
                                    session.metadata[key] = value.dump();
                                }
                            }
                        }
                        
                        // Store in memory
                        sessions_[session.session_id] = session;
                        
                    } catch (const std::exception& e) {
                        // Skip invalid session files
                        continue;
                    }
                }
            }
            
            return core::Result<bool>::Ok(true);
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(std::string("Failed to load sessions: ") + e.what());
        }
    }
    
private:
    mutable std::mutex mutex_;
    std::map<std::string, core::SessionInfo> sessions_;
};

// Factory methods
std::shared_ptr<SessionManager> SessionManager::create() {
    return std::make_shared<SessionManagerImpl>();
}

// GatewayServer Pimpl implementation
struct GatewayServer::Impl {
    std::shared_ptr<GatewayServerImpl> server_impl;
    std::shared_ptr<core::Config> config;
    std::shared_ptr<channels::ChannelManager> channel_mgr;
    std::shared_ptr<skills::SkillManager> skill_mgr;
    std::shared_ptr<plugins::PluginManager> plugin_mgr;
    std::shared_ptr<agents::AgentPool> agent_pool;  // 🏢 Multi-tenant Agent Pool
    std::atomic<bool> running{false};
};

GatewayServer::GatewayServer(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->server_impl = std::make_shared<GatewayServerImpl>(config);
    
    // 🏢 Initialize AgentPool (Multi-tenant support)
    agents::AgentPool::Config pool_config;
    pool_config.max_agents = 100;  // 最大并发 Agent 数
    // 注意：idle_ttl和cleanup_interval应该是chrono::seconds，不是毫秒
    pool_config.idle_ttl = std::chrono::seconds(3600);  // 1 小时闲置超时
    pool_config.cleanup_interval = std::chrono::seconds(300);  // 5 分钟清理间隔
    pool_config.auto_cleanup = true;
    
    impl_->agent_pool = std::make_shared<agents::AgentPool>(
        impl_->config,
        impl_->skill_mgr,
        impl_->channel_mgr,
        pool_config
    );
    
    // Import create_method_handler from methods.cc
    extern std::function<core::Result<nlohmann::json>(const std::string&, const nlohmann::json&)> 
        create_method_handler(std::shared_ptr<GatewayServer> server);
    
    // Initialize method handler in runtime state
    auto self = std::shared_ptr<GatewayServer>(this, [](GatewayServer*){});  // Non-owning shared_ptr
    impl_->server_impl->runtime_state()->method_handler = create_method_handler(self);
}

GatewayServer::~GatewayServer() = default;

std::shared_ptr<GatewayServer> GatewayServer::create(const core::GatewayConfig& config) {
    auto full_config = std::make_shared<core::Config>();
    full_config->gateway = config;
    return std::make_shared<GatewayServer>(full_config);
}

core::Result<bool> GatewayServer::start() {
    if (impl_->running.load()) {
        return core::Result<bool>::Err("Server already running");
    }
    auto result = impl_->server_impl->start();
    if (result.is_ok()) {
        impl_->running.store(true);
    }
    return result;
}

core::Result<bool> GatewayServer::stop() {
    if (!impl_->running.load()) {
        return core::Result<bool>::Err("Server not running");
    }
    auto result = impl_->server_impl->stop();
    if (result.is_ok()) {
        impl_->running.store(false);
    }
    return result;
}

bool GatewayServer::is_running() const {
    return impl_->running.load();
}

void GatewayServer::broadcast(const core::EventFrame& event) {
    if (impl_->server_impl) {
        // 🎯 OpenClaw 对齐: 默认 drop_if_slow=false
        impl_->server_impl->broadcast(event, false, nullptr);
    }
}

void GatewayServer::broadcast_to_conn_ids(
    const std::vector<std::string>& conn_ids,
    const core::EventFrame& event
) {
    // 🎯 OpenClaw 对齐: targeted broadcast
    // 对应 OpenClaw server-broadcast.ts:broadcastToConnIds
    if (!impl_->server_impl) {
        return;
    }
    if (conn_ids.empty()) {
        return;
    }
    
    // 将 conn_id 列表转换为 handle 集合
    auto state = impl_->server_impl->runtime_state();
    if (!state) {
        return;
    }
    
    // 构建目标 handle 集合
    // 注意: 这里需要通过 conn_id 查找 handle
    // 由于 GatewayServerImpl 的 handle_to_conn_id_ 是私有的，
    // 我们通过 runtime_state 中的 clients 映射来查找
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> target_hdls;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& conn_id : conn_ids) {
            auto it = state->clients.find(conn_id);
            if (it != state->clients.end()) {
                target_hdls.insert(it->second->handle);
            }
        }
    }
    
    if (!target_hdls.empty()) {
        impl_->server_impl->broadcast(event, false, &target_hdls);
    }
}

std::shared_ptr<channels::ChannelManager> GatewayServer::channel_manager() const {
    return impl_->channel_mgr;
}

std::shared_ptr<skills::SkillManager> GatewayServer::skill_manager() const {
    return impl_->skill_mgr;
}

std::shared_ptr<plugins::PluginManager> GatewayServer::plugin_manager() const {
    return impl_->plugin_mgr;
}

std::shared_ptr<GatewayRuntimeState> GatewayServer::runtime_state() const {
    if (impl_->server_impl) {
        return impl_->server_impl->runtime_state();
    }
    return nullptr;
}

std::shared_ptr<agents::AgentPool> GatewayServer::agent_pool() const {
    return impl_->agent_pool;
}

} // namespace gateway
} // namespace hunterclaw
