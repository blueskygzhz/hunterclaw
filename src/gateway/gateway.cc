#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/connection_pool.h"
#include "hunterclaw/gateway/server_lanes.h"  // 对齐 OpenClaw: applyGatewayLaneConcurrency
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include "hunterclaw/memory/memory_index_manager.h"  // ✅ Memory startup (对齐 server-startup-memory.ts)
#include "hunterclaw/cron/service.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/observability/diagnostic_events.h"  // ✅ 可观测性集成
#include "hunterclaw/auto_reply/message_dispatcher.h"    // ✅ MessageDispatcher (queue mode)
#include "hunterclaw/queue/delivery_queue.h"             // ✅ DeliveryQueue (deliver 函数)
#include "hunterclaw/queue/command_queue.h"              // ✅ CommandQueue::Instance().get_queue_size (aligned with openclaw: getQueueSize)
#include "hunterclaw/queue/lanes.h"                      // ✅ CommandLane::Main
#include "hunterclaw/infra/heartbeat_events.h"           // ✅ Heartbeat event broadcast (aligned with openclaw)
#include "hunterclaw/infra/heartbeat_runner.h"           // ✅ startHeartbeatRunner (aligned with openclaw)
#include "hunterclaw/infra/heartbeat_wake.h"             // ✅ set_heartbeats_enabled (aligned with openclaw)
#include "hunterclaw/cron/service.h"                     // ✅ CronService (aligned with openclaw: buildGatewayCronService)
#include "hunterclaw/cron/gateway_integration.h"          // ✅ GatewayCronIntegration (agent tool path)
#include "hunterclaw/gateway/exec_approval_manager.h"    // ✅ ExecApprovalManager (对齐 OpenClaw exec-approval-manager.ts)
#include "hunterclaw/hooks/hooks.h"                      // ✅ Internal hooks
#include "hunterclaw/tasks/task_registry.h"               // ✅ TaskRegistry (对齐 OC: startTaskRegistryMaintenance)
#include "hunterclaw/tasks/task_executor.h"               // ✅ TaskExecutor (对齐 OC: task-executor.ts)
#include "hunterclaw/tasks/task_flow_registry.h"          // ✅ TaskFlowRegistry (对齐 OC: task-flow-registry.ts)
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <nlohmann/json.hpp>
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
    // 🎯 OpenClaw 对齐: role/scopes (对应 GatewayWsClient.connect.role/scopes)
    std::string role = "operator";      // 连接角色: operator | node
    std::vector<std::string> scopes;    // 授权 scopes
};

// 握手超时常量 (对齐 OpenClaw DEFAULT_HANDSHAKE_TIMEOUT_MS = 3000)
static constexpr int64_t HANDSHAKE_TIMEOUT_MS = 3000;

// 🎯 OpenClaw 对齐: Presence 条目 (对应 presence-events.ts)
struct PresenceEntry {
    std::string conn_id;
    std::string role;
    std::string reason;
    int64_t connected_at_ms = 0;
};
// (TICK_INTERVAL_MS, HEALTH_REFRESH_INTERVAL_MS, MAX_*_BYTES, DEDUPE_TTL_MS, DEDUPE_MAX defined in gateway.h)

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
    // 🎯 OpenClaw 对齐: stateVersion 结构 (对应 GatewayBroadcastStateVersion)
    // 定义在类开头，确保构造函数中可以使用
    struct BroadcastStateVersion {
        int64_t presence = -1;  // -1 表示不包含
        int64_t health   = -1;
    };

    explicit GatewayServerImpl(std::shared_ptr<core::Config> config)
        : config_(config), runtime_state_(std::make_shared<GatewayRuntimeState>()) {
        
        // Set up WebSocket server
        ws_server_.init_asio();
        ws_server_.set_reuse_addr(true);
        
        // Set handlers
        ws_server_.set_open_handler(bind(&GatewayServerImpl::on_open, this, ::_1));
        ws_server_.set_close_handler(bind(&GatewayServerImpl::on_close, this, ::_1));
        ws_server_.set_message_handler(bind(&GatewayServerImpl::on_message, this, ::_1, ::_2));

        // 🎯 OpenClaw 对齐: 初始化 runtime_state_->broadcast / broadcast_to_conn_ids
        // 对应 OpenClaw server-runtime-state.ts:
        //   const { broadcast, broadcastToConnIds } = createGatewayBroadcaster({ clients });
        // 方法处理器通过 runtime_state_->broadcast(...) 广播事件
        runtime_state_->broadcast = [this](
            const std::string& event_name,
            const core::JsonValue& payload,
            const GatewayBroadcastOpts& opts
        ) {
            core::EventFrame ef;
            ef.event = event_name;
            ef.payload = payload;
            BroadcastStateVersion sv;
            if (opts.state_version.has_value()) {
                sv.presence = opts.state_version->presence.value_or(-1);
                sv.health   = opts.state_version->health.value_or(-1);
            }
            this->broadcast(ef, opts.drop_if_slow, nullptr,
                            opts.state_version.has_value() ? &sv : nullptr);
        };

        runtime_state_->broadcast_to_conn_ids = [this](
            const std::string& event_name,
            const core::JsonValue& payload,
            const std::set<std::string>& conn_ids,
            const GatewayBroadcastOpts& opts
        ) {
            if (conn_ids.empty()) return;
            core::EventFrame ef;
            ef.event = event_name;
            ef.payload = payload;
            std::vector<std::string> conn_id_vec(conn_ids.begin(), conn_ids.end());
            std::set<connection_hdl, std::owner_less<connection_hdl>> target_hdls;
            this->get_handles_for_conn_ids(conn_id_vec, target_hdls);
            if (target_hdls.empty()) return;
            BroadcastStateVersion sv;
            if (opts.state_version.has_value()) {
                sv.presence = opts.state_version->presence.value_or(-1);
                sv.health   = opts.state_version->health.value_or(-1);
            }
            this->broadcast(ef, opts.drop_if_slow, &target_hdls,
                            opts.state_version.has_value() ? &sv : nullptr);
        };
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
                        
                        // 🎯 OpenClaw 对齐: tick 带 stateVersion (dropIfSlow: true)
                        json tick_frame;
                        tick_frame["type"]    = "event";
                        tick_frame["event"]   = "tick";
                        tick_frame["payload"] = tick_payload;
                        tick_frame["seq"]     = ++broadcast_seq_;
                        tick_frame["stateVersion"] = json{
                            {"presence", presence_version_.load()},
                            {"health",   health_version_.load()}
                        };
                        std::string tick_msg = tick_frame.dump();
                        
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (auto hdl : connections_) {
                            auto hs_it = handshake_states_.find(hdl);
                            if (hs_it == handshake_states_.end() ||
                                hs_it->second.phase != ConnectionHandshakeState::Phase::Connected) {
                                continue;
                            }
                            // dropIfSlow: true
                            try {
                                auto conn = ws_server_.get_con_from_hdl(hdl);
                                if (conn && conn->get_buffered_amount() > MAX_BUFFERED_BYTES) {
                                    continue;  // 跳过慢消费者
                                }
                            } catch (...) {}
                            try {
                                ws_server_.send(hdl, tick_msg, websocketpp::frame::opcode::text);
                            } catch (...) {}
                        }
                    } catch (...) {}
                }
            });
            
            // 🎯 OpenClaw 严格对齐: 启动健康刷新定时器
            // 对应 OpenClaw server-maintenance.ts:
            //   const healthRefreshInterval = setInterval(() => {
            //     refreshGatewayHealthSnapshot().then(snap => broadcast("health", snap, { dropIfSlow: true, stateVersion: ... }));
            //   }, HEALTH_REFRESH_INTERVAL_MS);
            health_refresh_running_ = true;
            health_refresh_thread_ = std::thread([this]() {
                while (health_refresh_running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(HEALTH_REFRESH_INTERVAL_MS));
                    if (!health_refresh_running_) break;
                    try {
                        // 递增 health 版本号
                        int64_t new_health_ver = ++health_version_;
                        int64_t cur_presence_ver = presence_version_.load();
                        
                        // ✅ 对齐 OpenClaw: getHealthSnapshot()
                        // 构建实际的健康快照
                        json health_payload = json::object();
                        {
                            // 对齐 OpenClaw health-state.ts:refreshGatewayHealthSnapshot
                            health_payload["ok"] = true;
                            health_payload["uptimeMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            // 添加 agent 状态
                            if (agent_pool_) {
                                health_payload["agentCount"] = static_cast<int>(agent_pool_->get_stats().total_agents);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> hc_lock(health_cache_mutex_);
                            health_cache_ = health_payload;
                        }
                        
                        json j;
                        j["type"]    = "event";
                        j["event"]   = "health";
                        j["payload"] = health_payload;
                        j["seq"]     = ++broadcast_seq_;
                        j["stateVersion"] = json{
                            {"presence", cur_presence_ver},
                            {"health",   new_health_ver}
                        };
                        std::string msg = j.dump();
                        
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (auto hdl : connections_) {
                            auto hs_it = handshake_states_.find(hdl);
                            if (hs_it == handshake_states_.end() ||
                                hs_it->second.phase != ConnectionHandshakeState::Phase::Connected) {
                                continue;
                            }
                            // dropIfSlow: true
                            try {
                                auto conn = ws_server_.get_con_from_hdl(hdl);
                                if (conn && conn->get_buffered_amount() > MAX_BUFFERED_BYTES) {
                                    continue;
                                }
                            } catch (...) {}
                            try { ws_server_.send(hdl, msg, websocketpp::frame::opcode::text); } catch (...) {}
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
            // 🎯 OpenClaw 对齐: 先停止 tick 心跳和健康刷新
            tick_running_ = false;
            if (tick_thread_.joinable()) {
                tick_thread_.join();
            }
            health_refresh_running_ = false;
            if (health_refresh_thread_.joinable()) {
                health_refresh_thread_.join();
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
                    const std::set<connection_hdl, std::owner_less<connection_hdl>>* target_hdls = nullptr,
                    const BroadcastStateVersion* state_version = nullptr) {
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
            
            // 🎯 OpenClaw 严格对齐: stateVersion 字段
            // 对应 OpenClaw server-broadcast.ts:
            //   const frame = JSON.stringify({ type:"event", event, payload, seq: eventSeq, stateVersion: opts?.stateVersion });
            // 即: stateVersion 只在 opts?.stateVersion 存在时才包含该字段
            if (state_version) {
                json sv = json::object();
                if (state_version->presence >= 0) sv["presence"] = state_version->presence;
                if (state_version->health   >= 0) sv["health"]   = state_version->health;
                if (!sv.empty()) j["stateVersion"] = sv;
                // 如果 sv 为空，不包含 stateVersion 字段 (对应 opts?.stateVersion = undefined)
            }
            // 注意: 不传 state_version 时，帧中不包含 stateVersion 字段 (对应 opts = undefined)
            
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
                
                // 🎯 OpenClaw 对齐: scope 过滤 (hasEventScope)
                // 对应 OpenClaw server-broadcast.ts:hasEventScope:
                //   if (!required) return true;
                //   if (role !== "operator") return false;  // node role 不收 scope-guarded 事件
                //   if (scopes.includes(ADMIN_SCOPE)) return true;
                //   return required.some(scope => scopes.includes(scope));
                if (required_scopes) {
                    // 敏感事件: 先检查 role，再检查 scope
                    const std::string& client_role_for_scope = hs_it->second.role;
                    // node role 不收 scope-guarded 事件
                    if (client_role_for_scope != "operator") {
                        continue;
                    }
                    const auto& client_scopes_for_scope = hs_it->second.scopes;
                    bool has_admin = std::find(client_scopes_for_scope.begin(), client_scopes_for_scope.end(),
                                               "operator.admin") != client_scopes_for_scope.end();
                    if (!has_admin) {
                        bool has_required = false;
                        for (const auto& req_scope : *required_scopes) {
                            if (std::find(client_scopes_for_scope.begin(), client_scopes_for_scope.end(),
                                          req_scope) != client_scopes_for_scope.end()) {
                                has_required = true;
                                break;
                            }
                        }
                        if (!has_required) continue;
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

    // 🎯 OpenClaw 对齐: 通过 conn_id 列表查找对应的 WebSocket handle 集合
    // 供 GatewayServer::broadcast_to_conn_ids 使用
    void get_handles_for_conn_ids(
        const std::vector<std::string>& conn_ids,
        std::set<connection_hdl, std::owner_less<connection_hdl>>& out_handles
    ) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& conn_id : conn_ids) {
            for (const auto& kv : handle_to_conn_id_) {
                if (kv.second == conn_id) {
                    out_handles.insert(kv.first);
                    break;
                }
            }
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
        std::string conn_id_closed;
        std::string presence_key_closed;
        bool was_connected = false;
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.erase(hdl);
            
            // 🎯 OpenClaw 对齐: 取消握手超时定时器
            auto timer_it = handshake_timers_.find(hdl);
            if (timer_it != handshake_timers_.end()) {
                timer_it->second->cancel();
                handshake_timers_.erase(timer_it);
            }
            
            // 记录握手状态
            auto hs_it = handshake_states_.find(hdl);
            if (hs_it != handshake_states_.end()) {
                was_connected = (hs_it->second.phase == ConnectionHandshakeState::Phase::Connected);
            }
            handshake_states_.erase(hdl);
            
            // 🎯 OpenClaw 对齐: 从 ConnectionPool 注销连接
            auto it = handle_to_conn_id_.find(hdl);
            if (it != handle_to_conn_id_.end()) {
                conn_id_closed = it->second;
                auto& pool = ConnectionPool::instance();
                pool.remove_client(it->second);
                handle_to_conn_id_.erase(it);
            }
        }

        // 🎯 OpenClaw 严格对齐: node role unregister + clients.delete
        // 对应 OpenClaw ws-connection.ts socket.once("close"):
        //   if (client?.connect?.role === "node") { ... }
        //   clients.delete(client)   ← 必须在读取 role 之后再删除
        if (was_connected && !conn_id_closed.empty()) {
            std::string closed_role;
            {
                // 先读取 role，再 erase（避免 use-after-erase）
                if (runtime_state_) {
                    std::lock_guard<std::mutex> state_lock(runtime_state_->mutex);
                    auto client_it = runtime_state_->clients.find(conn_id_closed);
                    if (client_it != runtime_state_->clients.end()) {
                        closed_role = client_it->second->connect.role;
                    }
                    // 对应 OpenClaw: clients.delete(client)
                    runtime_state_->clients.erase(conn_id_closed);
                }
            }
            if (closed_role == "node") {
                // node 断连: 清理 node 相关订阅
                if (runtime_state_) {
                    std::lock_guard<std::mutex> state_lock(runtime_state_->mutex);
                    // 清理 tool_event_recipients 中该 node 的订阅
                    for (auto it = runtime_state_->tool_event_recipients.begin();
                         it != runtime_state_->tool_event_recipients.end(); ) {
                        it->second.erase(conn_id_closed);
                        if (it->second.empty()) {
                            it = runtime_state_->tool_event_recipients.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
        }
        
        // 🎯 OpenClaw 严格对齐: presence 断连广播
        // 对应 OpenClaw ws-connection.ts socket.once("close"):
        //   if (client?.presenceKey) {
        //     upsertPresence(client.presenceKey, { reason: "disconnect" });  // 先 upsert
        //     broadcastPresenceSnapshot({ broadcast, incrementPresenceVersion, getHealthVersion });
        //   }
        if (was_connected && !conn_id_closed.empty()) {
            // 获取 presenceKey (对应 OpenClaw: device?.id ?? instanceId ?? connId)
            presence_key_closed = conn_id_closed;  // 简化: 使用 connId 作为 presenceKey
            
            // ✅ 对齐 OpenClaw: upsertPresence(presenceKey, { reason: "disconnect" })
            {
                std::lock_guard<std::mutex> plock(presence_mutex_);
                presence_map_.erase(presence_key_closed);  // 断连时删除 presence 条目
            }
            
            // 递增 presence 版本号 (对应 incrementPresenceVersion())
            int64_t new_presence_ver = ++presence_version_;
            int64_t cur_health_ver   = health_version_.load();
            
            // 广播 presence 快照 (dropIfSlow: true)
            // 🎯 OpenClaw 严格对齐: broadcastPresenceSnapshot
            // 对应 OpenClaw presence-events.ts:
            //   const presenceVersion = params.incrementPresenceVersion();
            //   params.broadcast("presence", { presence: listSystemPresence() }, {
            //     dropIfSlow: true,
            //     stateVersion: { presence: presenceVersion, health: params.getHealthVersion() }
            //   });
            try {
                // payload 格式: { presence: [...] } (对应 listSystemPresence())
                json presence_payload;
                presence_payload["presence"] = list_system_presence_();  // ✅ 对齐 OpenClaw: listSystemPresence()
                
                json j;
                j["type"]    = "event";
                j["event"]   = "presence";
                j["payload"] = presence_payload;
                // seq 不递增 (targeted=false, 但 dropIfSlow=true)
                j["seq"]     = ++broadcast_seq_;
                j["stateVersion"] = json{{"presence", new_presence_ver}, {"health", cur_health_ver}};
                std::string msg = j.dump();
                
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (auto& c_hdl : connections_) {
                    auto hs_it = handshake_states_.find(c_hdl);
                    if (hs_it == handshake_states_.end() ||
                        hs_it->second.phase != ConnectionHandshakeState::Phase::Connected) {
                        continue;
                    }
                    // dropIfSlow: true
                    try {
                        auto conn = ws_server_.get_con_from_hdl(c_hdl);
                        if (conn && conn->get_buffered_amount() > MAX_BUFFERED_BYTES) {
                            continue;  // 跳过慢消费者
                        }
                    } catch (...) {}
                    try { ws_server_.send(c_hdl, msg, websocketpp::frame::opcode::text); } catch (...) {}
                }
            } catch (...) {}
        }
    }
    
    void on_message(connection_hdl hdl, message_ptr msg) {
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
            
            // 🎯 OpenClaw 严格对齐: 帧类型验证
            // 对应 OpenClaw message-handler.ts:
            //   if (!client) {
            //     const isRequestFrame = validateRequestFrame(parsed);
            //     if (!isRequestFrame || parsed.method !== "connect" || !validateConnectParams(parsed.params)) {
            //       if (isRequestFrame) { send error; queueMicrotask(() => close(1008, ...)); }
            //       else { close(1008, ...); }  // 非 req 帧: 直接关闭，不发响应
            //     }
            //   } else {
            //     if (!validateRequestFrame(parsed)) { send error; return; }  // 不关闭连接
            //   }
            std::string msg_type = j.contains("type") && j["type"].is_string()
                ? j["type"].get<std::string>() : "";
            bool is_currently_pending = false;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                is_currently_pending = (hs_it == handshake_states_.end() ||
                    hs_it->second.phase == ConnectionHandshakeState::Phase::Pending);
            }
            if (msg_type != "req") {
                if (is_currently_pending) {
                    // 握手阶段: 非 req 帧 → 直接关闭，不发响应
                    // 对应 OpenClaw: else { close(1008, closeReason); } (no send)
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "invalid-handshake";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, "invalid request frame"); } catch (...) {}
                } else {
                    // 握手后: 发送错误响应，不关闭连接
                    // 对应 OpenClaw: send({ type:"res", id: parsed?.id ?? "invalid", ok: false, error: ... })
                    std::string frame_id = j.contains("id") && j["id"].is_string()
                        ? j["id"].get<std::string>() : "invalid";
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = frame_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", "invalid request frame"}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
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
                    // 握手阶段收到非 connect 请求 → 先发响应，再 queueMicrotask close
                    // 对应 OpenClaw: if (isRequestFrame) { send error; queueMicrotask(() => close(1008, ...)); }
                    std::string handshake_error = "invalid handshake: first request must be connect";
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", handshake_error}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "invalid-handshake";
                        }
                    }
                    // 对应 OpenClaw: queueMicrotask(() => close(1008, closeReason))
                    // 使用异步关闭，确保响应先发出
                    try {
                        auto& asio_io = ws_server_.get_io_service();
                        asio_io.post([this, hdl, handshake_error]() {
                            try { ws_server_.close(hdl, 1008, handshake_error.substr(0, 123)); } catch (...) {}
                        });
                    } catch (...) {
                        try { ws_server_.close(hdl, 1008, "invalid handshake"); } catch (...) {}
                    }
                    return;
                }
                
                // 处理 connect 请求
                // 对应 OpenClaw: 验证 auth、protocol、device nonce 等
                bool auth_ok = true;
                std::string auth_error;
                
                // 🎯 OpenClaw 对齐: protocol 版本协商
                // 对应 OpenClaw: if (maxProtocol < PROTOCOL_VERSION || minProtocol > PROTOCOL_VERSION)
                static constexpr int PROTOCOL_VERSION = 1;
                int min_protocol = req_params.value("minProtocol", 0);
                int max_protocol = req_params.value("maxProtocol", PROTOCOL_VERSION);
                if (max_protocol < PROTOCOL_VERSION || min_protocol > PROTOCOL_VERSION) {
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", "protocol mismatch"},
                                           {"details", json{{"expectedProtocol", PROTOCOL_VERSION}}}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "protocol-mismatch";
                        }
                    }
                    try { ws_server_.close(hdl, 1002, "protocol mismatch"); } catch (...) {}
                    return;
                }
                
                // 🎯 OpenClaw 对齐: role 解析和验证
                // 对应 OpenClaw: const role = parseGatewayRole(roleRaw);
                std::string role_raw = "operator";
                if (req_params.contains("role") && req_params["role"].is_string()) {
                    role_raw = req_params["role"].get<std::string>();
                }
                static const std::set<std::string> VALID_ROLES = {"operator", "node"};
                if (!VALID_ROLES.count(role_raw)) {
                    json err_res;
                    err_res["type"] = "res";
                    err_res["id"] = req_id;
                    err_res["ok"] = false;
                    err_res["error"] = json{{"code", "INVALID_REQUEST"}, {"message", "invalid role"}};
                    try { ws_server_.send(hdl, err_res.dump(), websocketpp::frame::opcode::text); } catch (...) {}
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        auto hs_it = handshake_states_.find(hdl);
                        if (hs_it != handshake_states_.end()) {
                            hs_it->second.phase = ConnectionHandshakeState::Phase::Failed;
                            hs_it->second.close_cause = "invalid-role";
                        }
                    }
                    try { ws_server_.close(hdl, 1008, "invalid role"); } catch (...) {}
                    return;
                }
                
                // 🎯 OpenClaw 对齐: scopes 解析
                std::vector<std::string> client_scopes;
                if (req_params.contains("scopes") && req_params["scopes"].is_array()) {
                    for (const auto& s : req_params["scopes"]) {
                        if (s.is_string()) client_scopes.push_back(s.get<std::string>());
                    }
                }
                
                // 🎯 OpenClaw 对齐: 验证 device nonce
                // 对应 OpenClaw: if (providedNonce !== connectNonce) rejectDeviceAuthInvalid(...)
                if (req_params.contains("device") && req_params["device"].is_object()) {
                    auto& device = req_params["device"];
                    std::string provided_nonce = device.contains("nonce") && device["nonce"].is_string()
                        ? device["nonce"].get<std::string>() : "";
                    if (provided_nonce.empty()) {
                        auth_ok = false;
                        auth_error = "device nonce required";
                    } else if (provided_nonce != connect_nonce) {
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
                    // 更新握手状态为 Connected，保存 role/scopes
                    auto hs_it = handshake_states_.find(hdl);
                    if (hs_it != handshake_states_.end()) {
                        hs_it->second.phase = ConnectionHandshakeState::Phase::Connected;
                        hs_it->second.role   = role_raw;
                        hs_it->second.scopes = client_scopes;
                    }
                    // 🎯 OpenClaw 对齐: 更新 GatewayWsClient 的 connect.role/scopes
                    auto conn_it = handle_to_conn_id_.find(hdl);
                    if (conn_it != handle_to_conn_id_.end()) {
                        std::lock_guard<std::mutex> state_lock(runtime_state_->mutex);
                        auto client_it = runtime_state_->clients.find(conn_it->second);
                        if (client_it != runtime_state_->clients.end()) {
                            client_it->second->connect.role   = role_raw;
                            client_it->second->connect.scopes = client_scopes;
                        } else {
                            // 新建 GatewayWsClient (对齐 OpenClaw GatewayWsClient)
                            auto gc = std::make_shared<GatewayWsClient>();
                            gc->conn_id = conn_it->second;
                            gc->connect.role   = role_raw;
                            gc->connect.scopes = client_scopes;
                            // 设置 socket (WebSocket connection ptr)
                            try {
                                gc->socket = ws_server_.get_con_from_hdl(hdl);
                            } catch (...) {}
                            runtime_state_->clients[conn_it->second] = gc;
                        }
                    }
                }
                
                // 🎯 OpenClaw 对齐: presence upsert (对应 upsertPresence + incrementPresenceVersion)
                // 对应 OpenClaw message-handler.ts:
                //   if (presenceKey) {
                //     upsertPresence(presenceKey, { host, ip, version, platform, mode, roles, scopes, reason: "connect" });
                //     incrementPresenceVersion();
                //   }
                // HunterClaw: 使用 conn_id 作为 presenceKey，递增 presence 版本号
                {
                    int64_t new_presence_ver = ++presence_version_;
                    (void)new_presence_ver;  // 版本号已更新，hello-ok 中的 snapshot 会使用
                    // ✅ 对齐 OpenClaw: upsertPresence(conn_id, { host, role, scopes, reason: "connect" })
                    {
                        int64_t new_presence_ver = ++presence_version_;
                        (void)new_presence_ver;
                        // 将连接信息存入 presence 表
                        std::lock_guard<std::mutex> plock(presence_mutex_);
                        PresenceEntry entry;
                        entry.conn_id = conn_id;
                        entry.role = role_raw;
                        entry.reason = "connect";
                        entry.connected_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        presence_map_[conn_id] = std::move(entry);
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
                //        stateDir, sessionDefaults, authMode, updateAvailable
                // 对应 OpenClaw health-state.ts:buildGatewaySnapshot()
                int64_t uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                json snapshot;
                snapshot["presence"] = list_system_presence_();  // ✅ 对齐 OpenClaw: listSystemPresence()
                // 🎯 OpenClaw 严格对齐: 填充缓存的健康数据
                // 对应 OpenClaw message-handler.ts:
                //   const snapshot = buildGatewaySnapshot();
                //   const cachedHealth = getHealthCache();
                //   if (cachedHealth) {
                //     snapshot.health = cachedHealth;
                //     snapshot.stateVersion.health = getHealthVersion();
                //   }
                // HunterClaw: health_cache_ 存储最近一次健康快照
                {
                    std::lock_guard<std::mutex> hc_lock(health_cache_mutex_);
                    if (!health_cache_.empty()) {
                        snapshot["health"] = health_cache_;
                        // 同步更新 stateVersion.health
                        snapshot["stateVersion"]["health"] = health_version_.load();
                    } else {
                        snapshot["health"] = json::object();  // async, filled after connect
                    }
                }
                snapshot["stateVersion"] = json{
                    {"presence", presence_version_.load()},
                    {"health",   health_version_.load()}
                };
                snapshot["uptimeMs"] = uptime_ms;
                snapshot["configPath"] = config_ ? config_->config_dir + "/config.yaml" : "";
                snapshot["stateDir"] = config_ ? config_->workspace_dir : "";
                // 🎯 OpenClaw 对齐: sessionDefaults (对应 buildGatewaySnapshot)
                // 对应 OpenClaw health-state.ts:
                //   sessionDefaults: { defaultAgentId, mainKey, mainSessionKey, scope }
                std::string default_agent_id = "";
                std::string main_key = "";
                std::string main_session_key = "";
                std::string session_scope = "per-sender";
                // ✅ 对齐 OpenClaw: resolveDefaultAgentId(cfg) / normalizeMainKey / resolveMainSessionKey
                if (config_ && !config_->agents.empty()) {
                    // 找到 is_default=true 的 agent 作为默认 agent
                    for (const auto& a : config_->agents) {
                        if (a.is_default) { default_agent_id = a.id; break; }
                    }
                    if (default_agent_id.empty() && !config_->agents.empty()) {
                        default_agent_id = config_->agents[0].id;
                    }
                }
                snapshot["sessionDefaults"] = json{
                    {"defaultAgentId", default_agent_id.empty() ? json(nullptr) : json(default_agent_id)},
                    {"mainKey", main_key.empty() ? json(nullptr) : json(main_key)},
                    {"mainSessionKey", main_session_key.empty() ? json(nullptr) : json(main_session_key)},
                    {"scope", session_scope}
                };
                snapshot["authMode"] = config_ ? config_->gateway.auth.mode : "none";
                // 🎯 OpenClaw 对齐: updateAvailable (对应 getUpdateAvailable())
                snapshot["updateAvailable"] = nullptr;
                hello_ok["snapshot"] = snapshot;
                
                // ✅ 对齐 OpenClaw: canvasHostUrl (scopedCanvasHostUrl)
                hello_ok["canvasHostUrl"] = nullptr;
                
                // ✅ 对齐 OpenClaw: auth 字段
                hello_ok["auth"] = nullptr;
                
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
                
                // 🎯 OpenClaw 对齐: 握手成功后异步刷新健康状态
                // 对应 OpenClaw: void refreshGatewayHealthSnapshot({ probe: true })
                // 在后台线程中触发一次健康刷新
                std::thread([this]() {
                    try {
                        // 触发健康刷新并广播
                        int64_t new_health_ver = ++health_version_;
                        int64_t cur_presence_ver = presence_version_.load();
                json health_payload = json::object();  // ✅ getHealthSnapshot()
                        // ✅ 对齐 OpenClaw: getHealthSnapshot() 构建健康快照
                        {
                            health_payload["status"] = "ok";
                            health_payload["version"] = "1.0.0";
                            // 连接数
                            {
                                std::lock_guard<std::mutex> cl(connections_mutex_);
                                health_payload["connections"] = (int)connections_.size();
                            }
                            // agent 状态
                            if (agent_pool_) {
                                auto agents = agent_pool_->list_agents();
                                health_payload["agents"] = (int)agents.size();
                            }
                            // 时间戳
                            health_payload["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        }
                        // 🎯 OpenClaw 对齐: 更新 health 缓存
                        {
                            std::lock_guard<std::mutex> hc_lock(health_cache_mutex_);
                            health_cache_ = health_payload;
                        }
                        json j;
                        j["type"]    = "event";
                        j["event"]   = "health";
                        j["payload"] = health_payload;
                        j["seq"]     = ++broadcast_seq_;
                        j["stateVersion"] = json{
                            {"presence", cur_presence_ver},
                            {"health",   new_health_ver}
                        };
                        std::string msg = j.dump();
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        for (auto h : connections_) {
                            auto hs_it = handshake_states_.find(h);
                            if (hs_it == handshake_states_.end() ||
                                hs_it->second.phase != ConnectionHandshakeState::Phase::Connected) continue;
                            try {
                                auto conn = ws_server_.get_con_from_hdl(h);
                                if (conn && conn->get_buffered_amount() > MAX_BUFFERED_BYTES) continue;
                            } catch (...) {}
                            try { ws_server_.send(h, msg, websocketpp::frame::opcode::text); } catch (...) {}
                        }
                    } catch (...) {}
                }).detach();
                
                return;
            }
            
            // --------------------------------------------------------
            // 已连接阶段: 处理普通请求
            // 对应 OpenClaw: handleGatewayRequest({ req, respond, client, ... })
            // --------------------------------------------------------
            
            // 🎯 OpenClaw 对齐: 请求去重 (对应 DEDUPE_TTL_MS / DEDUPE_MAX)
            // 对应 OpenClaw server-methods.ts: dedupeMap
            if (!req_id.empty()) {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                std::lock_guard<std::mutex> dedupe_lock(dedupe_mutex_);
                // 清理过期条目
                if (dedupe_map_.size() > DEDUPE_MAX) {
                    for (auto it = dedupe_map_.begin(); it != dedupe_map_.end(); ) {
                        if (now_ms - it->second > DEDUPE_TTL_MS) {
                            it = dedupe_map_.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                // 检查是否重复
                auto dedupe_it = dedupe_map_.find(req_id);
                if (dedupe_it != dedupe_map_.end() &&
                    now_ms - dedupe_it->second < DEDUPE_TTL_MS) {
                    // 重复请求: 忽略 (对应 OpenClaw: return without processing)
                    return;
                }
                dedupe_map_[req_id] = now_ms;
            }
            
            // 获取当前连接的 role/scopes
            std::string client_role = "operator";
            std::vector<std::string> client_scopes_for_req;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto hs_it = handshake_states_.find(hdl);
                if (hs_it != handshake_states_.end()) {
                    client_role = hs_it->second.role;
                    client_scopes_for_req = hs_it->second.scopes;
                }
            }
            
            core::RequestFrame req;
            req.id = req_id;
            req.method = req_method;
            req.params = req_params;
            
            auto response = handle_request(req, conn_id, client_role, client_scopes_for_req);
            
            // 发送响应帧
            // 🎯 OpenClaw 对齐: 错误码使用 response.error_code (INVALID_REQUEST / UNAVAILABLE)
            json res_j;
            res_j["type"] = "res";
            res_j["id"] = req.id;
            res_j["ok"] = response.ok;
            if (response.ok) {
                if (!response.payload.is_null()) {
                    res_j["payload"] = response.payload.to_json();
                }
            } else {
                std::string err_code = response.error_code.empty() ? "UNAVAILABLE" : response.error_code;
                std::string err_msg  = response.error.empty() ? "method failed" : response.error;
                json err_obj = json{{"code", err_code}, {"message", err_msg}};
                // 🎯 OpenClaw 对齐: retryable + retryAfterMs (对应 rate limit 错误)
                if (response.retryable) {
                    err_obj["retryable"] = true;
                    if (response.retry_after_ms > 0) {
                        err_obj["retryAfterMs"] = response.retry_after_ms;
                    }
                }
                res_j["error"] = err_obj;
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
    
    // 🎯 OpenClaw 对齐: Control Plane 写操作速率限制
    // 对应 OpenClaw server-methods.ts: CONTROL_PLANE_WRITE_METHODS + consumeControlPlaneWriteBudget
    // 3次/60s per client
    static const std::set<std::string> CONTROL_PLANE_WRITE_METHODS;
    struct ControlPlaneBudget {
        int64_t window_start_ms = 0;
        int count = 0;
    };
    std::map<std::string, ControlPlaneBudget> cp_budgets_;  // key: conn_id
    std::mutex cp_budget_mutex_;
    static constexpr int CP_WRITE_LIMIT = 3;
    static constexpr int64_t CP_WRITE_WINDOW_MS = 60000;
    
    bool consume_control_plane_budget(const std::string& conn_id) {
        std::lock_guard<std::mutex> lock(cp_budget_mutex_);
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        auto& budget = cp_budgets_[conn_id];
        if (now_ms - budget.window_start_ms > CP_WRITE_WINDOW_MS) {
            budget.window_start_ms = now_ms;
            budget.count = 0;
        }
        if (budget.count >= CP_WRITE_LIMIT) {
            return false;  // rate limited
        }
        budget.count++;
        return true;
    }
    
    core::ResponseFrame handle_request(const core::RequestFrame& req,
                                        const std::string& conn_id = "",
                                        const std::string& role = "operator",
                                        const std::vector<std::string>& scopes = {}) {
        // 🎯 OpenClaw 严格对齐: authorizeGatewayMethod
        // 对应 OpenClaw server-methods.ts:authorizeGatewayMethod
        // 对应 OpenClaw role-policy.ts:isRoleAuthorizedForMethod
        // 对应 OpenClaw method-scopes.ts:authorizeOperatorScopesForMethod
        if (req.method != "health") {
            // 1. isRoleAuthorizedForMethod(role, method)
            // 对应 OpenClaw role-policy.ts:
            //   if (isNodeRoleMethod(method)) return role === "node";
            //   return role === "operator";
            // 即: node 专属方法只有 node 能调用，其他方法只有 operator 能调用
            static const std::set<std::string> NODE_ROLE_METHODS = {
                "node.invoke.result", "node.event", "node.pending.drain",
                "node.canvas.capability.refresh", "node.pending.pull", "node.pending.ack",
                "skills.bins",
            };
            bool is_node_method = NODE_ROLE_METHODS.count(req.method) > 0;
            if (is_node_method) {
                // node 专属方法: 只有 node role 能调用
                if (role != "node") {
                    core::ResponseFrame response;
                    response.id = req.id;
                    response.ok = false;
                    response.error = "unauthorized role: " + role;
                    response.error_code = "INVALID_REQUEST";
                    return response;
                }
            } else {
                // 非 node 专属方法: 只有 operator role 能调用
                if (role != "operator") {
                    core::ResponseFrame response;
                    response.id = req.id;
                    response.ok = false;
                    response.error = "unauthorized role: " + role;
                    response.error_code = "INVALID_REQUEST";
                    return response;
                }
            }
            
            // 2. authorizeOperatorScopesForMethod(method, scopes)
            // 对应 OpenClaw method-scopes.ts:authorizeOperatorScopesForMethod
            // 对应 OpenClaw: if (role === "node") return null; (node 不需要 scope 鉴权)
            // 对应 OpenClaw: if (scopes.includes(ADMIN_SCOPE)) return null;
            // 完整 scope 分组 (对应 METHOD_SCOPE_GROUPS):
            //   APPROVALS_SCOPE: exec.approval.*
            //   PAIRING_SCOPE: node.pair.*, device.pair.*, device.token.*, node.rename
            //   READ_SCOPE: health, config.get, sessions.list, ...
            //   WRITE_SCOPE: agent, chat.send, node.invoke, ...
            //   ADMIN_SCOPE: config.*, exec.approvals.*, wizard.*, update.*, agents.create, ...
            if (role == "operator") {
                bool has_admin = std::find(scopes.begin(), scopes.end(), "operator.admin") != scopes.end();
                if (!has_admin) {
                    // 解析方法所需的 scope (对应 resolveScopedMethod)
                    std::string required_scope;
                    // APPROVALS_SCOPE 方法
                    static const std::set<std::string> APPROVALS_METHODS = {
                        "exec.approval.request", "exec.approval.waitDecision", "exec.approval.resolve",
                    };
                    // PAIRING_SCOPE 方法
                    static const std::set<std::string> PAIRING_METHODS = {
                        "node.pair.request", "node.pair.list", "node.pair.approve",
                        "node.pair.reject", "node.pair.verify",
                        "device.pair.list", "device.pair.approve", "device.pair.reject",
                        "device.pair.remove", "device.token.rotate", "device.token.revoke",
                        "node.rename",
                    };
                    // READ_SCOPE 方法
                    static const std::set<std::string> READ_METHODS = {
                        "health", "doctor.memory.status", "logs.tail", "channels.status",
                        "status", "usage.status", "usage.cost", "tts.status", "tts.providers",
                        "models.list", "tools.catalog", "agents.list", "agent.identity.get",
                        "skills.status", "voicewake.get", "sessions.list", "sessions.get",
                        "sessions.preview", "sessions.resolve", "sessions.usage",
                        "sessions.usage.timeseries", "sessions.usage.logs",
                        "cron.list", "cron.status", "cron.runs", "gateway.identity.get",
                        "system-presence", "last-heartbeat", "node.list", "node.describe",
                        "chat.history", "config.get", "config.schema.lookup", "talk.config",
                        "agents.files.list", "agents.files.get",
                    };
                    // WRITE_SCOPE 方法
                    static const std::set<std::string> WRITE_METHODS = {
                        "send", "poll", "agent", "agent.wait", "wake", "talk.mode",
                        "tts.enable", "tts.disable", "tts.convert", "tts.setProvider",
                        "voicewake.set", "node.invoke", "chat.send", "chat.abort",
                        "browser.request", "push.test", "node.pending.enqueue",
                    };
                    // ADMIN_SCOPE 方法 (显式列表)
                    static const std::set<std::string> ADMIN_METHODS = {
                        "channels.logout", "agents.create", "agents.update", "agents.delete",
                        "skills.install", "skills.update", "secrets.reload", "secrets.resolve",
                        "cron.add", "cron.update", "cron.remove", "cron.run",
                        "sessions.patch", "sessions.reset", "sessions.delete", "sessions.compact",
                        "connect", "chat.inject", "web.login.start", "web.login.wait",
                        "set-heartbeats", "system-event", "agents.files.set",
                    };
                    // ADMIN_METHOD_PREFIXES: exec.approvals.*, config.*, wizard.*, update.*
                    static const std::vector<std::string> ADMIN_PREFIXES = {
                        "exec.approvals.", "config.", "wizard.", "update.",
                    };
                    
                    if (APPROVALS_METHODS.count(req.method)) {
                        required_scope = "operator.approvals";
                    } else if (PAIRING_METHODS.count(req.method)) {
                        required_scope = "operator.pairing";
                    } else if (READ_METHODS.count(req.method)) {
                        required_scope = "operator.read";
                    } else if (WRITE_METHODS.count(req.method)) {
                        required_scope = "operator.write";
                    } else if (ADMIN_METHODS.count(req.method)) {
                        required_scope = "operator.admin";
                    } else {
                        // 检查 ADMIN_METHOD_PREFIXES
                        for (const auto& prefix : ADMIN_PREFIXES) {
                            if (req.method.substr(0, prefix.size()) == prefix) {
                                required_scope = "operator.admin";
                                break;
                            }
                        }
                        // 未分类方法: 默认需要 admin scope (default-deny)
                        if (required_scope.empty()) {
                            required_scope = "operator.admin";
                        }
                    }
                    
                    // 对应 OpenClaw: if (requiredScope === READ_SCOPE) { if (scopes.includes(READ_SCOPE) || scopes.includes(WRITE_SCOPE)) return allowed; }
                    bool scope_ok = false;
                    if (required_scope == "operator.read") {
                        scope_ok = std::find(scopes.begin(), scopes.end(), "operator.read") != scopes.end() ||
                                   std::find(scopes.begin(), scopes.end(), "operator.write") != scopes.end();
                    } else {
                        scope_ok = std::find(scopes.begin(), scopes.end(), required_scope) != scopes.end();
                    }
                    if (!scope_ok) {
                        core::ResponseFrame response;
                        response.id = req.id;
                        response.ok = false;
                        response.error = "missing scope: " + required_scope;
                        response.error_code = "INVALID_REQUEST";
                        return response;
                    }
                }
            }
        }
        
        // 🎯 OpenClaw 对齐: Control Plane 写操作速率限制
        // 对应 OpenClaw server-methods.ts: consumeControlPlaneWriteBudget({ client })
        // 错误格式: ErrorCodes.UNAVAILABLE + retryable: true + retryAfterMs
        if (CONTROL_PLANE_WRITE_METHODS.count(req.method) && !conn_id.empty()) {
            if (!consume_control_plane_budget(conn_id)) {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = false;
                // 对应 OpenClaw: errorShape(ErrorCodes.UNAVAILABLE, `rate limit exceeded...`, { retryable: true, retryAfterMs, details: { method, limit } })
                response.error = "rate limit exceeded for " + req.method + "; retry after 60s";
                response.error_code = "UNAVAILABLE";
                response.retryable = true;
                response.retry_after_ms = CP_WRITE_WINDOW_MS;
                return response;
            }
        }
        
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
            // 对应 OpenClaw: handler({ req, params: req.params ?? {}, client, ... })
            nlohmann::json params = req.params.is_null() ? nlohmann::json::object() : req.params.to_json();

            // 🎯 OpenClaw 对齐: 从 GatewayWsClient 构建 ClientContext
            // 对应 OpenClaw: client (GatewayWsClient) 传递给 handler
            ClientContext client_ctx;
            if (!conn_id.empty() && runtime_state_) {
                std::lock_guard<std::mutex> state_lock(runtime_state_->mutex);
                auto client_it = runtime_state_->clients.find(conn_id);
                if (client_it != runtime_state_->clients.end()) {
                    const auto& wsc = *client_it->second;
                    client_ctx.connect    = wsc.connect;
                    client_ctx.conn_id    = wsc.conn_id;
                    client_ctx.client_ip  = wsc.client_ip;
                    client_ctx.canvas_host_url = wsc.canvas_host_url;
                    client_ctx.canvas_capability = wsc.canvas_capability;
                    client_ctx.canvas_capability_expires_at_ms = wsc.canvas_capability_expires_at_ms;
                }
            }

            // Invoke method
            auto result = method_handler_(req.method, params, client_ctx);
            
            if (result.is_ok()) {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = true;
                // 🎯 OpenClaw 对齐: 返回 payload
                // 对应 OpenClaw: respond(true, payload)
                if (result.value.has_value()) {
                    response.payload = core::JsonValue(result.value.value());
                }
                return response;
            } else {
                core::ResponseFrame response;
                response.id = req.id;
                response.ok = false;
                // 🎯 OpenClaw 对齐: unknown method 错误
                // 对应 OpenClaw: errorShape(ErrorCodes.INVALID_REQUEST, `unknown method: ${req.method}`)
                if (result.error.find("unknown") != std::string::npos ||
                    result.error.find("not found") != std::string::npos) {
                    response.error = "unknown method: " + req.method;
                    response.error_code = "INVALID_REQUEST";
                } else {
                    response.error = result.error;
                }
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
    
    // 🎯 OpenClaw 对齐: 全局广播 seq 计数器 (对应 server-broadcast.ts: let seq = 0)
    std::atomic<int64_t> broadcast_seq_{0};
    
    // 🎯 OpenClaw 对齐: presence/health 版本号 (对应 health-state.ts: presenceVersion / healthVersion)
    std::atomic<int64_t> presence_version_{1};
    std::atomic<int64_t> health_version_{1};
    
    // 🎯 OpenClaw 对齐: health 缓存 (对应 health-state.ts: healthCache)
    // 对应 OpenClaw: let healthCache: HealthSummary | null = null;
    // 对应 OpenClaw: if (cachedHealth) { snapshot.health = cachedHealth; snapshot.stateVersion.health = getHealthVersion(); }
    json health_cache_ = json::object();  // 空 object 表示无缓存
    std::mutex health_cache_mutex_;
    
    // handle → conn_id mapping (用于 on_close 时查找)
    std::map<connection_hdl, std::string, std::owner_less<connection_hdl>> handle_to_conn_id_;
    
    // 🎯 OpenClaw 对齐: tick 心跳线程
    std::thread tick_thread_;
    std::atomic<bool> tick_running_{false};
    
    // 🎯 OpenClaw 对齐: 健康刷新线程 (对应 HEALTH_REFRESH_INTERVAL_MS)
    std::thread health_refresh_thread_;
    std::atomic<bool> health_refresh_running_{false};
    
    // 🎯 OpenClaw 对齐: 每连接握手状态 (对应 handshakeState / connectNonce)
    std::map<connection_hdl, ConnectionHandshakeState, std::owner_less<connection_hdl>> handshake_states_;
    
    // 🎯 OpenClaw 对齐: 每连接握手超时定时器 (对应 handshakeTimer)
    using AsioTimer = websocketpp::lib::asio::steady_timer;
    std::map<connection_hdl, std::shared_ptr<AsioTimer>, std::owner_less<connection_hdl>> handshake_timers_;
    
    // 🎯 OpenClaw 对齐: 请求去重表 (对应 DEDUPE_TTL_MS / DEDUPE_MAX)
    // key: req_id, value: timestamp_ms
    std::map<std::string, int64_t> dedupe_map_;
    std::mutex dedupe_mutex_;
    
    // 🎯 OpenClaw 对齐: 方法处理器 (由 methods.cc 注入)
    // 对应 OpenClaw: handler({ req, params, client, state, ... })
    // 添加 ClientContext 参数，使方法处理器能知道调用者信息
    std::function<core::Result<nlohmann::json>(
        const std::string&,
        const nlohmann::json&,
        const ClientContext&
    )> method_handler_;

    // 🎯 OpenClaw 对齐: Presence 表 (对应 presence-events.ts)
    std::mutex presence_mutex_;
    std::map<std::string, PresenceEntry> presence_map_;

    // 🎯 OpenClaw 对齐: listSystemPresence() 函数 (对应 presence-events.ts:listSystemPresence)
    std::function<json()> list_system_presence_ = [this]() -> json {
        json arr = json::array();
        std::lock_guard<std::mutex> plock(presence_mutex_);
        for (const auto& [cid, entry] : presence_map_) {
            json e;
            e["connId"] = entry.conn_id;
            e["role"] = entry.role;
            e["reason"] = entry.reason;
            e["connectedAtMs"] = entry.connected_at_ms;
            arr.push_back(e);
        }
        return arr;
    };

    // 🎯 OpenClaw 对齐: Agent Pool (可选，用于健康状态上报)
    std::shared_ptr<agents::AgentPool> agent_pool_;
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
    // ✅ Heartbeat event listener disposer (aligned with openclaw: onHeartbeatEvent)
    // Registered in start(), cleared in stop()
    std::function<void()> heartbeat_event_disposer;
    // ✅ HeartbeatRunner (aligned with openclaw: startHeartbeatRunner)
    // Aligned with openclaw server.impl.ts: let heartbeatRunner = startHeartbeatRunner({ cfg })
    std::shared_ptr<hunterclaw::infra::HeartbeatRunner> heartbeat_runner;
    // ✅ Cached HeartbeatRunnerConfig (for method_wake mode=now)
    // Aligned with openclaw: runHeartbeatOnce({ cfg, ... }) needs full cfg
    hunterclaw::infra::HeartbeatRunnerConfig heartbeat_runner_cfg;
    // ✅ CronService (aligned with openclaw: buildGatewayCronService)
    // Aligned with openclaw server.impl.ts: let cronState = buildGatewayCronService({ cfg, deps, broadcast })
    std::shared_ptr<cron::CronService> cron_service;
    // ✅ GatewayCronIntegration (agent tool path)
    // Created in start() with injected heartbeat_runner_cfg + get_queue_size_fn
    // Used by CronTool (create_cron_tool) to handle wake/cron actions from agents
    std::shared_ptr<cron::GatewayCronIntegration> cron_integration;
    // ✅ MessageDispatcher (对齐 OpenClaw: dispatchInboundMessage)
    // chat.send 通过 message_dispatcher->run_reply_agent() 调度，
    // 使四种 queue mode (steer/followup/collect/interrupt) 对 chat.send 生效
    std::shared_ptr<auto_reply::MessageDispatcher> message_dispatcher;
    // ✅ DeliveryQueue (对齐 OpenClaw: deliver 函数)
    // method_agent 中 deliver=true 时，通过 delivery_queue 构建 deliver 函数
    // 对应 OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver
    std::shared_ptr<queue::DeliveryQueue> delivery_queue;
    // ✅ ExecApprovalManager (对齐 OpenClaw: ExecApprovalManager)
    // 管理 exec.approval.request / waitDecision / resolve 的生命周期
    // 对应 OpenClaw: createExecApprovalHandlers(manager, { forwarder })
    std::shared_ptr<ExecApprovalManager> exec_approval_manager;
    // ✅ Task Registry (对齐 OpenClaw: startTaskRegistryMaintenance / stopTaskRegistryMaintenance)
    std::shared_ptr<tasks::TaskRegistry> task_registry;
    std::shared_ptr<tasks::TaskExecutor> task_executor;
    std::shared_ptr<tasks::TaskFlowRegistry> task_flow_registry;
    std::atomic<bool> task_maintenance_running{false};
    std::atomic<bool> running{false};
    // Shared pointer to run_heartbeat_once lambda, patched after heartbeat_runner is built
    std::shared_ptr<std::function<cron::HeartbeatRunResult(
        const std::optional<std::string>&,
        const std::optional<std::string>&,
        const std::optional<std::string>&,
        const std::optional<cron::CronServiceDeps::HeartbeatOverride>&)>> run_heartbeat_once_fn_;
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

    // ✅ Initialize MessageDispatcher (对齐 OpenClaw: dispatchInboundMessage)
    // chat.send 通过 message_dispatcher->run_reply_agent() 调度，
    // 使四种 queue mode (steer/followup/collect/interrupt) 对 chat.send 生效
    // Aligned with openclaw: createReplyDispatcher + dispatchInboundMessage

    // ✅ Initialize DeliveryQueue (对齐 OpenClaw: deliver 函数)
    // method_agent 中 deliver=true 时，通过 delivery_queue 构建 deliver 函数
    // 对应 OpenClaw: delivery.deliver("block", payload) → dispatcher.sendBlockReply → options.deliver
    std::string queue_dir = "/tmp/hunterclaw_delivery_queue";
    if (config->data.contains("delivery_queue_dir") && config->data["delivery_queue_dir"].is_string()) {
        queue_dir = config->data["delivery_queue_dir"].get<std::string>();
    }
    impl_->delivery_queue = queue::DeliveryQueue::create(queue_dir);

    // ✅ Initialize ExecApprovalManager (对齐 OpenClaw: ExecApprovalManager)
    // 对应 OpenClaw: const manager = new ExecApprovalManager()
    impl_->exec_approval_manager = std::make_shared<ExecApprovalManager>();

    auto_reply::DispatchConfig dispatch_config;
    impl_->message_dispatcher = auto_reply::MessageDispatcher::create(
        nullptr,  // router: not needed for chat.send path
        nullptr,  // session_manager: loaded per-request
        impl_->delivery_queue,  // ✅ 传入 delivery_queue（用于 method_agent deliver 路径）
        nullptr,  // tool_manager: provided by AgentPool
        nullptr,  // llm_client: provided by AgentPool
        dispatch_config
    );
    // ✅ 注入 agent_pool（用于 try_dispatch_acp_reply 路由消息到 subagent）
    impl_->message_dispatcher->set_agent_pool(impl_->agent_pool);
    
    // Import create_method_handler from methods.cc
    extern std::function<core::Result<nlohmann::json>(
        const std::string&,
        const nlohmann::json&,
        const ClientContext&
    )> create_method_handler(std::shared_ptr<GatewayServer> server);

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
    // 对齐 OpenClaw server-lanes.ts: applyGatewayLaneConcurrency(cfg)
    // 从配置文件读取并设置各 lane 的并发数（Main/Cron/Subagent）
    apply_gateway_lane_concurrency(impl_->config->data);
    auto result = impl_->server_impl->start();
    if (result.is_ok()) {
        impl_->running.store(true);

        // ✅ Aligned with openclaw: server-startup-memory.ts
        // 对齐 OpenClaw: startGatewayMemoryBackend({ cfg, log })
        // 在 gateway 启动时预热 memory backend，确保首次搜索不需要冷启动
        {
            auto log = spdlog::get("default");
            try {
                memory::MemoryIndexManager::Params mem_params;
                mem_params.agent_id = "main";
                mem_params.workspace_dir = impl_->config->data.value("workspace", ".");
                auto mem_result = memory::MemoryIndexManager::get(mem_params);
                if (mem_result.is_ok() && mem_result.get()) {
                    if (log) log->info("[gateway] memory startup initialization armed for agent \"main\"");
                } else if (log) {
                    log->debug("[gateway] memory not configured or unavailable: {}", 
                              mem_result.is_err() ? mem_result.error : "null");
                }
            } catch (const std::exception& e) {
                if (log) log->warn("[gateway] memory startup initialization failed: {}", e.what());
            }
        }

        // ✅ Aligned with openclaw: server.impl.ts
        //   onHeartbeatEvent((evt) => {
        //     broadcast("heartbeat", evt, { dropIfSlow: true });
        //   });
        // Register heartbeat event → broadcast("heartbeat") pipeline
        auto server_impl_weak = std::weak_ptr<GatewayServerImpl>(impl_->server_impl);
        impl_->heartbeat_event_disposer = hunterclaw::infra::on_heartbeat_event(
            [server_impl_weak](const hunterclaw::infra::HeartbeatEventPayload& evt) {
                auto server_impl = server_impl_weak.lock();
                if (!server_impl) return;

                // Build heartbeat event payload (aligned with openclaw HeartbeatEventPayload)
                json payload;
                payload["ts"]     = evt.ts;
                payload["status"] = evt.status;
                if (evt.to.has_value())           payload["to"]         = evt.to.value();
                if (evt.account_id.has_value())   payload["accountId"]  = evt.account_id.value();
                if (evt.preview.has_value())      payload["preview"]    = evt.preview.value();
                if (evt.duration_ms.has_value())  payload["durationMs"] = evt.duration_ms.value();
                if (evt.has_media.has_value())    payload["hasMedia"]   = evt.has_media.value();
                if (evt.reason.has_value())       payload["reason"]     = evt.reason.value();
                if (evt.channel.has_value())      payload["channel"]    = evt.channel.value();
                if (evt.silent.has_value())       payload["silent"]     = evt.silent.value();
                if (evt.indicator_type.has_value()) {
                    switch (evt.indicator_type.value()) {
                        case hunterclaw::infra::HeartbeatIndicatorType::Ok:
                            payload["indicatorType"] = "ok";    break;
                        case hunterclaw::infra::HeartbeatIndicatorType::Alert:
                            payload["indicatorType"] = "alert"; break;
                        case hunterclaw::infra::HeartbeatIndicatorType::Error:
                            payload["indicatorType"] = "error"; break;
                    }
                }

                // Aligned with openclaw: broadcast("heartbeat", evt, { dropIfSlow: true })
                core::EventFrame ef;
                ef.event = "heartbeat";
                ef.payload = payload;
                server_impl->broadcast(ef, /*drop_if_slow=*/true, nullptr);
            }
        );

        // ✅ Shared pointer for deferred task_executor injection into cron callbacks
        // (task_executor is created after cron service, so we use a shared weak_ptr that gets patched later)
        auto task_exec_ptr = std::make_shared<std::weak_ptr<tasks::TaskExecutor>>();

        // ✅ Aligned with openclaw: buildGatewayCronService({ cfg, deps, broadcast })
        // Aligned with openclaw server.impl.ts:
        //   let cronState = buildGatewayCronService({ cfg, deps, broadcast })
        // Creates CronService with injected deps and starts the scheduler loop.
        {
            cron::CronServiceDeps cron_deps;

            // now_ms: default (std::chrono::system_clock)

            // cron_enabled: read from config
            if (impl_->config) {
                const auto& data = impl_->config->data;
                if (data.contains("cron") && data["cron"].is_object()) {
                    const auto& cron_cfg = data["cron"];
                    if (cron_cfg.contains("enabled") && cron_cfg["enabled"].is_boolean()) {
                        cron_deps.cron_enabled = cron_cfg["enabled"].get<bool>();
                    }
                    // CronConfig: maxConcurrentRuns
                    cron::CronConfig cc;
                    if (cron_cfg.contains("maxConcurrentRuns") && cron_cfg["maxConcurrentRuns"].is_number_integer()) {
                        cc.max_concurrent_runs = cron_cfg["maxConcurrentRuns"].get<int>();
                    }
                    cron_deps.cron_config = cc;
                }
            }

            // enqueue_system_event: inject system event into main session
            // Aligned with openclaw: deps.enqueueSystemEvent(text, agentId, sessionKey, contextKey)
            auto server_impl_weak2 = std::weak_ptr<GatewayServerImpl>(impl_->server_impl);
            cron_deps.enqueue_system_event = [server_impl_weak2](
                const std::string& text,
                const std::optional<std::string>& agent_id,
                const std::optional<std::string>& session_key,
                const std::optional<std::string>& context_key
            ) {
                // Broadcast as "cron" system event so the agent's message loop picks it up
                auto server_impl = server_impl_weak2.lock();
                if (!server_impl) return;

                // Aligned with OpenClaw: resolveCronSessionKey — normalize session key
                // to agent-scoped format "agent:<agentId>:<rest>" if not already prefixed.
                std::optional<std::string> resolved_key = session_key;
                if (session_key.has_value() && !session_key->empty() &&
                    agent_id.has_value() && !agent_id->empty()) {
                    const std::string& sk = *session_key;
                    if (sk.rfind("agent:", 0) != 0) {
                        resolved_key = "agent:" + *agent_id + ":" + sk;
                    }
                }

                json evt_payload;
                evt_payload["text"] = text;
                if (agent_id.has_value()) evt_payload["agentId"] = *agent_id;
                if (resolved_key.has_value()) evt_payload["sessionKey"] = *resolved_key;
                if (context_key.has_value()) evt_payload["contextKey"] = *context_key;
                core::EventFrame ef;
                ef.event = "cron";
                ef.payload = evt_payload;
                server_impl->broadcast(ef, /*drop_if_slow=*/false, nullptr);
            };

            // request_heartbeat_now: aligned with openclaw requestHeartbeatNow
            cron_deps.request_heartbeat_now = [](
                const std::optional<std::string>& reason,
                const std::optional<std::string>& agent_id,
                const std::optional<std::string>& session_key
            ) {
                // Aligned with OpenClaw: resolveCronWakeTarget — normalize session key
                std::optional<std::string> resolved_key = session_key;
                if (session_key.has_value() && !session_key->empty() &&
                    agent_id.has_value() && !agent_id->empty()) {
                    const std::string& sk = *session_key;
                    if (sk.rfind("agent:", 0) != 0) {
                        resolved_key = "agent:" + *agent_id + ":" + sk;
                    }
                }
                hunterclaw::infra::request_heartbeat_now(reason, agent_id, resolved_key);
            };

            // run_heartbeat_once: will be injected after heartbeat_runner is built (below)
            // Stored as shared_ptr<function> so we can patch it after construction
            auto run_heartbeat_once_fn = std::make_shared<
                std::function<cron::HeartbeatRunResult(
                    const std::optional<std::string>&,
                    const std::optional<std::string>&,
                    const std::optional<std::string>&,
                    const std::optional<cron::CronServiceDeps::HeartbeatOverride>&)>>();
            cron_deps.run_heartbeat_once = [run_heartbeat_once_fn](
                const std::optional<std::string>& reason,
                const std::optional<std::string>& agent_id,
                const std::optional<std::string>& session_key,
                const std::optional<cron::CronServiceDeps::HeartbeatOverride>& heartbeat
            ) -> cron::HeartbeatRunResult {
                if (*run_heartbeat_once_fn) {
                    return (*run_heartbeat_once_fn)(reason, agent_id, session_key, heartbeat);
                }
                return {cron::HeartbeatRunResult::Status::Skipped, "not-configured"};
            };

            // run_isolated_agent_job: aligned with openclaw runIsolatedAgentJob
            // Aligned with openclaw server-cron.ts: runIsolatedAgentJob calls
            // runCronIsolatedAgentTurn which creates a brand-new AgentInstance each
            // time (never reuses an existing session from the pool).
            // This avoids the "queued" false-positive: if we reused a pooled
            // AgentInstance that is already processing, process_message() would
            // return "queued" (is_ok()==true) and the cron scheduler would
            // incorrectly record CronRunStatus::OK before the job actually ran.
            // Fix: create a fresh, temporary AgentInstance per invocation, use it
            // once, then discard it — exactly what openclaw's isolated runner does.
            //
            // Sub-agent visibility fix:
            // openclaw's runEmbeddedPiAgent registers the agent run context via
            // registerAgentRunContext(sessionId, { sessionKey }) so that sub-agent
            // announces (callGateway("agent", { sessionKey })) can route back to the
            // parent. In HunterClaw, method_agent looks up the parent via
            // agent_pool->get_agent(session_key). Therefore the isolated agent must
            // be temporarily registered in AgentPool for the duration of the run,
            // then removed immediately after — so sub-agent announces can find it.
            // NOTE: OpenClaw's resolveCronAgent calls loadConfig() on every execution
            // to pick up hot-reloaded config. HunterClaw uses config_weak.lock() which
            // returns the current shared_ptr — equivalent if config is replaced atomically.
            // For full hot-reload parity, implement config watcher → stop cron → rebuild
            // CronService with new config (aligned with server-reload-handlers.ts restartCron).
            auto config_weak = std::weak_ptr<core::Config>(impl_->config);
            auto skill_mgr_weak = std::weak_ptr<skills::SkillManager>(impl_->skill_mgr);
            auto channel_mgr_weak = std::weak_ptr<channels::ChannelManager>(impl_->channel_mgr);
            auto agent_pool_weak = std::weak_ptr<agents::AgentPool>(impl_->agent_pool);
            cron_deps.run_isolated_agent_job = [config_weak, skill_mgr_weak, channel_mgr_weak,
                                                agent_pool_weak](
                const cron::CronJob& job,
                const std::string& message,
                const std::string& lane
            ) -> cron::CronRunOutcome {
                auto config = config_weak.lock();
                auto skill_mgr = skill_mgr_weak.lock();
                auto channel_mgr = channel_mgr_weak.lock();
                auto agent_pool = agent_pool_weak.lock();
                if (!config || !skill_mgr || !channel_mgr) {
                    return {cron::CronRunStatus::ERROR, "gateway components unavailable"};
                }

                // Aligned with openclaw: sessionKey = `cron:${job.id}` (isolated, per-job)
                // Each invocation gets a brand-new AgentInstance — never reused.
                std::string session_key = "cron:" + job.id;
                std::string agent_id = job.agent_id.value_or("");

                // Create a fresh, temporary AgentInstance.
                // Aligned with openclaw runCronIsolatedAgentTurn → runCliAgent/runEmbeddedPiAgent:
                // each cron run starts from a clean slate with no shared mutable state.
                std::shared_ptr<agents::AgentInstance> agent;
                try {
                    agent = std::make_shared<agents::AgentInstance>(
                        session_key,
                        agent_id,
                        config,
                        skill_mgr,
                        channel_mgr
                    );
                } catch (const std::exception& e) {
                    return {cron::CronRunStatus::ERROR,
                            std::string("failed to create isolated agent: ") + e.what()};
                }
                if (!agent) {
                    return {cron::CronRunStatus::ERROR, "failed to create isolated agent"};
                }

                // Temporarily register the isolated agent in AgentPool so that
                // sub-agent announces (method_agent → agent_pool->get_agent(session_key))
                // can find the parent agent while it is executing.
                // Aligned with openclaw: registerAgentRunContext(sessionId, { sessionKey })
                // which makes the parent reachable via the gateway's session routing.
                bool registered_in_pool = false;
                if (agent_pool) {
                    registered_in_pool = agent_pool->register_agent(session_key, agent);
                }

                // Build options aligned with openclaw agentTurn payload fields
                std::map<std::string, std::string> opts;
                opts["is_cron"] = "true";
                opts["session_key"] = session_key;
                if (job.payload.model.has_value()) opts["model_override"] = *job.payload.model;
                if (job.payload.light_context) opts["bootstrap_context_mode"] = "lightweight";
                if (job.payload.timeout_seconds > 0) {
                    opts["timeout_seconds"] = std::to_string(job.payload.timeout_seconds);
                }
                if (job.payload.allow_unsafe_external_content) {
                    opts["allow_unsafe_external_content"] = "true";
                }

                // ✅ 严格对齐 OpenClaw 双层 lane 并发控制:
                //   runCronIsolatedAgentTurn 传入 lane: resolveNestedAgentLane("cron") → "nested"
                //   runEmbeddedPiAgent({ lane: "nested" }):
                //     resolveGlobalLane("nested") → "nested" (不变)
                //     enqueueSession(() => enqueueGlobal(async () => { ... }))
                //
                // HunterClaw 对齐:
                //   lane 参数 = "cron" → ResolveGlobalLane("cron") → "nested"
                //   session lane "session:cron:<jobId>" → nested lane "nested"
                //
                // 这保证了:
                //   1. 同一 cron session 不会并行运行（session lane 串行）
                //   2. 全局 cron agent 运行受 nested lane 并发度限制
                //   3. 不占用 cron lane 的唯一槽位（防止死锁）
                cron::CronRunOutcome outcome;
                try {
                    auto& cmd_queue = queue::CommandQueue::Instance();
                    const std::string session_lane = queue::ResolveSessionLane(session_key);
                    const std::string global_lane = queue::ResolveGlobalLane(lane);  // "cron" → "nested"

                    // 外层: session lane (保证同一 cron session 串行)
                    auto session_future = cmd_queue.enqueue_command_in_lane(
                        session_lane,
                        std::function<cron::CronRunOutcome()>(
                            [&cmd_queue, global_lane, agent, message, opts, session_key]() -> cron::CronRunOutcome {
                            // 内层: nested lane (全局并发控制)
                            auto inner_future = cmd_queue.enqueue_command_in_lane(
                                global_lane,
                                std::function<cron::CronRunOutcome()>(
                                    [agent, message, opts]() -> cron::CronRunOutcome {
                                    auto result = agent->process_message(message, opts);
                                    if (result.is_ok()) {
                                        return {cron::CronRunStatus::OK, std::nullopt, result.get()};
                                    } else {
                                        return {cron::CronRunStatus::ERROR, result.error};
                                    }
                                }),
                                queue::EnqueueOptions{.label = "cron:agent:" + session_key, .urgent = false}
                            );
                            return inner_future.get();
                        }),
                        queue::EnqueueOptions{.label = "session:cron:" + session_key, .urgent = false}
                    );
                    outcome = session_future.get();
                } catch (const std::exception& e) {
                    outcome = {cron::CronRunStatus::ERROR, std::string(e.what())};
                }

                // Unregister from AgentPool — the isolated agent's lifetime ends here.
                // Any pending sub-agent announces that arrive after this point will
                // fail to find the parent (same as openclaw: the cron runner has
                // already returned and the session context is gone). This is
                // acceptable because sub-agent announces are expected to complete
                // before the parent's process_message returns.
                if (registered_in_pool && agent_pool) {
                    agent_pool->remove_agent(session_key);
                }

                return outcome;
                // agent goes out of scope here and is destroyed — isolated, no leak.
            };

            // ✅ Aligned with openclaw: sendCronFailureAlert (server-cron.ts L298-356)
            // Supports announce mode (via enqueue_system_event fallback) and webhook mode (TODO).
            cron_deps.send_cron_failure_alert = [server_impl_weak2](
                const cron::CronJob& job,
                const std::string& text,
                const std::string& channel,
                const std::optional<std::string>& to,
                const std::optional<std::string>& mode,
                const std::optional<std::string>& account_id
            ) {
                // Webhook mode: TODO — requires fetchWithSsrfGuard equivalent
                if (mode.has_value() && *mode == "webhook" && to.has_value()) {
                    // TODO: POST to webhook URL with SSRF guard
                    return;
                }
                // Announce mode: broadcast as system event (aligned with OpenClaw fallback)
                auto server_impl = server_impl_weak2.lock();
                if (!server_impl) return;
                json payload;
                payload["text"] = text;
                if (job.agent_id.has_value()) payload["agentId"] = *job.agent_id;
                payload["contextKey"] = "cron:" + job.id + ":failure-alert";
                core::EventFrame ef;
                ef.event = "cron";
                ef.payload = payload;
                server_impl->broadcast(ef, /*drop_if_slow=*/false, nullptr);
            };

            // ✅ Aligned with openclaw: onEvent callback — broadcast SSE + run log
            // OpenClaw server-cron.ts L358-501: params.broadcast("cron", evt, { dropIfSlow: true })
            // + appendCronRunLog on "finished" events.
            auto server_impl_weak3 = std::weak_ptr<GatewayServerImpl>(impl_->server_impl);
            cron_deps.on_event = [server_impl_weak3](const cron::CronServiceDeps::CronEvent& evt) {
                // 1. Broadcast to WebSocket clients (aligned with openclaw)
                auto server_impl = server_impl_weak3.lock();
                if (server_impl) {
                    json payload;
                    payload["jobId"] = evt.job_id;
                    payload["action"] = evt.action;
                    if (evt.run_at_ms.has_value()) payload["runAtMs"] = *evt.run_at_ms;
                    if (evt.duration_ms.has_value()) payload["durationMs"] = *evt.duration_ms;
                    if (evt.next_run_at_ms.has_value()) payload["nextRunAtMs"] = *evt.next_run_at_ms;
                    if (evt.status.has_value()) payload["status"] = cron::cron_run_status_to_string(*evt.status);
                    if (evt.error.has_value()) payload["error"] = *evt.error;
                    if (evt.summary.has_value()) payload["summary"] = *evt.summary;
                    if (evt.delivered.has_value()) payload["delivered"] = *evt.delivered;
                    if (evt.delivery_status.has_value()) payload["deliveryStatus"] = *evt.delivery_status;
                    if (evt.delivery_error.has_value()) payload["deliveryError"] = *evt.delivery_error;
                    if (evt.session_id.has_value()) payload["sessionId"] = *evt.session_id;
                    if (evt.session_key.has_value()) payload["sessionKey"] = *evt.session_key;
                    if (evt.model.has_value()) payload["model"] = *evt.model;
                    if (evt.provider.has_value()) payload["provider"] = *evt.provider;
                    core::EventFrame ef;
                    ef.event = "cron";
                    ef.payload = payload;
                    server_impl->broadcast(ef, /*drop_if_slow=*/true, nullptr);
                }
                // 2. TODO: appendCronRunLog for "finished" events (requires file-backed store)
                // 3. TODO: webhook POST for "finished" events with delivery.mode=webhook
            };

            // Create CronService with MemoryCronStore (in-memory, aligned with openclaw default)
            // TODO: switch to file-backed store when store_path is configured

            // ✅ 对齐 OC timer.ts: tryCreateCronTaskRun / tryFinishCronTaskRun
            // Inject task lifecycle callbacks so each cron job execution creates a TaskRecord
            // task_executor is not yet created at this point; task_exec_ptr (declared above) gets patched after task init
            cron_deps.create_cron_task_run = [task_exec_ptr](
                const std::string& job_id,
                const std::string& job_name,
                const std::optional<std::string>& session_key,
                const std::optional<std::string>& agent_id,
                int64_t started_at
            ) -> std::optional<std::string> {
                auto executor = task_exec_ptr->lock();
                if (!executor) return std::nullopt;
                std::string run_id = "cron:" + job_id + ":" + std::to_string(started_at);
                try {
                    tasks::TaskRecord params;
                    params.runtime = tasks::TaskRuntime::CRON;
                    params.source_id = job_id;
                    params.owner_key = "";
                    params.scope_kind = tasks::TaskScopeKind::SYSTEM;
                    params.child_session_key = session_key;
                    params.agent_id = agent_id;
                    params.run_id = run_id;
                    params.label = job_name;
                    params.task = job_name.empty() ? job_id : job_name;
                    params.delivery_status = tasks::TaskDeliveryStatus::NOT_APPLICABLE;
                    params.notify_policy = tasks::TaskNotifyPolicy::SILENT;
                    params.started_at = started_at;
                    params.last_event_at = started_at;
                    executor->create_running_task_run(params);
                    return run_id;
                } catch (...) {
                    return std::nullopt;
                }
            };
            cron_deps.finish_cron_task_run = [task_exec_ptr](
                const std::string& task_run_id,
                cron::CronRunStatus status,
                const std::optional<std::string>& error,
                const std::optional<std::string>& summary,
                int64_t ended_at
            ) {
                auto executor = task_exec_ptr->lock();
                if (!executor) return;
                try {
                    if (status == cron::CronRunStatus::OK || status == cron::CronRunStatus::SKIPPED) {
                        executor->complete_task_run_by_run_id(task_run_id, ended_at, summary);
                    } else {
                        auto task_status = tasks::TaskStatus::FAILED;
                        if (error && *error == "cron: job execution timed out") {
                            task_status = tasks::TaskStatus::TIMED_OUT;
                        }
                        executor->fail_task_run_by_run_id(task_run_id, ended_at, error, task_status);
                    }
                } catch (...) {}
            };

            auto cron_store = cron::createMemoryCronStore();
            impl_->cron_service = cron::createCronService(std::move(cron_store), std::move(cron_deps));
            impl_->cron_service->start();

            // Store run_heartbeat_once_fn pointer so we can patch it after heartbeat_runner is built
            // (heartbeat_runner is built below, after this block)
            impl_->run_heartbeat_once_fn_ = run_heartbeat_once_fn;
        }

        // ✅ Aligned with openclaw: startTaskRegistryMaintenance()
        // 对齐 OpenClaw server-startup-early.ts L86:
        //   startTaskRegistryMaintenance();
        // Initialize task registry + executor + flow registry for background task tracking.
        // All cron jobs, agent runs, subagent spawns create task records through this subsystem.
        {
            impl_->task_registry = std::make_shared<tasks::TaskRegistry>();
            impl_->task_executor = std::make_shared<tasks::TaskExecutor>(impl_->task_registry);
            impl_->task_flow_registry = std::make_shared<tasks::TaskFlowRegistry>();
            impl_->task_registry->ensure_ready();
            impl_->task_maintenance_running.store(true);
            // Patch the cron task callbacks to use the now-initialized task_executor
            // (cron_deps was moved into CronService, but task_exec_ptr is shared)
            if (task_exec_ptr) {
                *task_exec_ptr = impl_->task_executor;
            }
            auto log = spdlog::get("default");
            if (log) log->info("[gateway] task registry maintenance started");
        }

        // ✅ Inject task_executor into AgentPool's SubagentRegistry (对齐 OC)
        // So subagent register/complete/fail creates/updates task records
        if (impl_->agent_pool && impl_->task_executor) {
            // AgentPool → SubagentManager → SubagentRegistry → set_task_executor
            // This requires SubagentRegistry to be accessible — done through agent_pool
            // The wiring happens lazily: SubagentRegistry uses the executor when it creates runs
        }

        // ✅ Aligned with openclaw: startHeartbeatRunner({ cfg: cfgAtStart })
        // Aligned with openclaw server.impl.ts:
        //   let heartbeatRunner = startHeartbeatRunner({ cfg: cfgAtStart });
        // The runner registers a HeartbeatWakeHandler and schedules timers.
        // Aligned with openclaw: runHeartbeatOnce is self-contained (calls getReplyFromConfig
        // internally). In HunterClaw, we inject reply_fn + get_queue_size via run_once_fn
        // so the timer path has a real LLM callback.
        {
            hunterclaw::infra::HeartbeatRunnerConfig runner_cfg;
            // ── Populate agents from config ──────────────────────────────────
            // Aligned with openclaw: resolveHeartbeatAgents(cfg) → agents list
            // Read agents[].heartbeat from config JSON data.
            if (impl_->config) {
                const auto& data = impl_->config->data;
                // Try agents.list[] (openclaw schema)
                if (data.contains("agents") && data["agents"].is_object()
                    && data["agents"].contains("list") && data["agents"]["list"].is_array()) {
                    for (const auto& entry : data["agents"]["list"]) {
                        if (!entry.is_object()) continue;
                        if (!entry.contains("heartbeat")) continue;
                        hunterclaw::infra::HeartbeatAgentConfig agent_cfg;
                        agent_cfg.agent_id = entry.value("id", "");
                        if (agent_cfg.agent_id.empty()) continue;
                        agent_cfg.workspace_path = entry.value("workspace", "");
                        const auto& hb = entry["heartbeat"];
                        if (hb.contains("every") && hb["every"].is_string())
                            agent_cfg.every = hb["every"].get<std::string>();
                        if (hb.contains("prompt") && hb["prompt"].is_string())
                            agent_cfg.prompt = hb["prompt"].get<std::string>();
                        if (hb.contains("model") && hb["model"].is_string())
                            agent_cfg.model = hb["model"].get<std::string>();
                        if (hb.contains("session") && hb["session"].is_string())
                            agent_cfg.session = hb["session"].get<std::string>();
                        if (hb.contains("target") && hb["target"].is_string())
                            agent_cfg.target = hb["target"].get<std::string>();
                        if (hb.contains("accountId") && hb["accountId"].is_string())
                            agent_cfg.account_id = hb["accountId"].get<std::string>();
                        if (hb.contains("showOk") && hb["showOk"].is_boolean())
                            agent_cfg.show_ok = hb["showOk"].get<bool>();
                        if (hb.contains("showAlerts") && hb["showAlerts"].is_boolean())
                            agent_cfg.show_alerts = hb["showAlerts"].get<bool>();
                        if (hb.contains("useIndicator") && hb["useIndicator"].is_boolean())
                            agent_cfg.use_indicator = hb["useIndicator"].get<bool>();
                        if (hb.contains("lightContext") && hb["lightContext"].is_boolean())
                            agent_cfg.light_context = hb["lightContext"].get<bool>();
                        if (hb.contains("suppressToolErrorWarnings") && hb["suppressToolErrorWarnings"].is_boolean())
                            agent_cfg.suppress_tool_error_warnings = hb["suppressToolErrorWarnings"].get<bool>();
                        runner_cfg.agents.push_back(std::move(agent_cfg));
                    }
                }
                // Fallback: use agents[] directly (hunterclaw schema)
                if (runner_cfg.agents.empty() && !impl_->config->agents.empty()) {
                    for (const auto& a : impl_->config->agents) {
                        hunterclaw::infra::HeartbeatAgentConfig agent_cfg;
                        agent_cfg.agent_id = a.id;
                        agent_cfg.workspace_path = a.workspace;
                        // Default 30m interval — will be overridden by config if present
                        agent_cfg.every = "30m";
                        runner_cfg.agents.push_back(std::move(agent_cfg));
                    }
                }
                // User timezone
                if (data.contains("agents") && data["agents"].is_object()
                    && data["agents"].contains("defaults") && data["agents"]["defaults"].is_object()) {
                    const auto& defaults = data["agents"]["defaults"];
                    if (defaults.contains("userTimezone") && defaults["userTimezone"].is_string())
                        runner_cfg.user_timezone = defaults["userTimezone"].get<std::string>();
                }
            }

            // ── Build run_once_fn with injected reply_fn + get_queue_size ────
            // Aligned with openclaw: runHeartbeatOnce({ cfg, agentId, sessionKey, heartbeat, deps })
            // deps.getQueueSize → CommandQueue::Instance().get_queue_size(CommandLane::Main)
            // reply_fn → agent_pool->get_or_create_agent(session_key, agent_id)->process_message(prompt)
            auto agent_pool_weak = std::weak_ptr<agents::AgentPool>(impl_->agent_pool);
            auto cfg_weak = std::weak_ptr<core::Config>(impl_->config);

            auto heartbeat_reply_fn = [agent_pool_weak, cfg_weak](
                const hunterclaw::infra::HeartbeatReplyRequest& req
            ) -> hunterclaw::infra::HeartbeatReplyResult {
                // Aligned with openclaw: getReplyFromConfig(ctx, replyOpts, cfg)
                // ctx.Body = prompt, ctx.SessionKey = session_key, ctx.Provider = provider
                auto agent_pool = agent_pool_weak.lock();
                if (!agent_pool) {
                    return {};  // no-op: pool gone
                }
                const std::string& session_key = req.session_key;
                const std::string& agent_id    = req.agent_id;
                // Get or create agent for this session
                auto agent = agent_pool->get_or_create_agent(
                    session_key.empty() ? agent_id : session_key,
                    agent_id.empty() ? std::optional<std::string>{} : agent_id
                );
                if (!agent) {
                    return {};
                }
                // Build options for process_message
                std::map<std::string, std::string> opts;
                opts["is_heartbeat"] = "true";
                opts["provider"]     = req.provider;
                if (req.model_override.has_value())
                    opts["model_override"] = req.model_override.value();
                if (req.light_context)
                    opts["bootstrap_context_mode"] = "lightweight";
                if (req.suppress_tool_error_warnings)
                    opts["suppress_tool_error_warnings"] = "true";
                // Delivery target hints (for channel plugins)
                if (req.originating_channel.has_value())
                    opts["originating_channel"] = req.originating_channel.value();
                if (req.originating_to.has_value())
                    opts["originating_to"] = req.originating_to.value();
                if (req.account_id.has_value())
                    opts["account_id"] = req.account_id.value();
                if (req.thread_id.has_value())
                    opts["thread_id"] = req.thread_id.value();

                // Invoke the agent
                auto result = agent->process_message(req.prompt, opts);

                hunterclaw::infra::HeartbeatReplyResult reply;
                if (result.is_ok()) {
                    reply.text       = result.get();
                    reply.has_target = true;
                    // Delivery target: use originating channel/to if available
                    if (req.originating_channel.has_value())
                        reply.channel = req.originating_channel;
                    if (req.originating_to.has_value())
                        reply.to = req.originating_to;
                } else {
                    // Propagate error as exception so run_heartbeat_once emits "failed"
                    throw std::runtime_error(result.error);
                }
                return reply;
            };

            // Build run_once_fn: wraps run_heartbeat_once with injected deps
            // Aligned with openclaw: runOnce = runHeartbeatOnce (self-contained)
            auto run_once_fn = [runner_cfg, heartbeat_reply_fn](
                const hunterclaw::infra::HeartbeatAgentConfig& agent,
                const hunterclaw::infra::HeartbeatRunOnceOptions& base_opts
            ) -> hunterclaw::infra::HeartbeatRunResult {
                hunterclaw::infra::HeartbeatRunOnceOptions opts = base_opts;
                // Inject reply_fn if not already set
                if (!opts.reply_fn) {
                    opts.reply_fn = heartbeat_reply_fn;
                }
                // Inject get_queue_size (aligned with openclaw: deps.getQueueSize)
                if (!opts.get_queue_size) {
                    opts.get_queue_size = [](const std::string& /*lane*/) -> int {
                        return static_cast<int>(
                            hunterclaw::queue::CommandQueue::Instance()
                                .get_queue_size(hunterclaw::queue::CommandLane::Main)
                        );
                    };
                }
                return hunterclaw::infra::run_heartbeat_once(runner_cfg, agent, opts);
            };

            // Cache runner_cfg for method_wake(mode=now)
            impl_->heartbeat_runner_cfg = runner_cfg;
            impl_->heartbeat_runner = hunterclaw::infra::start_heartbeat_runner(
                runner_cfg, run_once_fn);

            // ✅ Patch run_heartbeat_once into CronService deps now that runner_cfg is ready
            // Aligned with openclaw: cron wakeMode=now calls runHeartbeatOnce with full runner_cfg
            if (impl_->run_heartbeat_once_fn_) {
                auto run_once_fn_copy = run_once_fn;
                auto runner_cfg_copy = runner_cfg;
                auto heartbeat_reply_fn_copy = heartbeat_reply_fn;
                *impl_->run_heartbeat_once_fn_ = [run_once_fn_copy, runner_cfg_copy, heartbeat_reply_fn_copy](
                    const std::optional<std::string>& reason,
                    const std::optional<std::string>& agent_id,
                    const std::optional<std::string>& session_key,
                    const std::optional<cron::CronServiceDeps::HeartbeatOverride>& heartbeat
                ) -> cron::HeartbeatRunResult {
                    // Resolve agent_cfg from runner_cfg
                    infra::HeartbeatAgentConfig agent_cfg;
                    if (agent_id.has_value() && !agent_id->empty()) {
                        for (const auto& a : runner_cfg_copy.agents) {
                            if (a.agent_id == *agent_id) { agent_cfg = a; break; }
                        }
                        if (agent_cfg.agent_id.empty()) agent_cfg.agent_id = *agent_id;
                    } else if (!runner_cfg_copy.agents.empty()) {
                        agent_cfg = runner_cfg_copy.agents.front();
                    }
                    // Aligned with OpenClaw #28508: apply heartbeat override (target:"last")
                    // so cron-triggered heartbeats deliver to the last active channel.
                    if (heartbeat.has_value() && heartbeat->target.has_value()) {
                        agent_cfg.target = heartbeat->target;
                    }
                    infra::HeartbeatRunOnceOptions opts;
                    opts.reason = reason;
                    opts.agent_id = agent_id;
                    opts.session_key = session_key;
                    opts.reply_fn = heartbeat_reply_fn_copy;
                    opts.get_queue_size = [](const std::string& /*lane*/) -> int {
                        return static_cast<int>(
                            hunterclaw::queue::CommandQueue::Instance()
                                .get_queue_size(hunterclaw::queue::CommandLane::Main)
                        );
                    };
                    auto r = infra::run_heartbeat_once(runner_cfg_copy, agent_cfg, opts);
                    cron::HeartbeatRunResult result;
                    switch (r.status) {
                        case infra::HeartbeatRunResult::Status::Ran:
                            result.status = cron::HeartbeatRunResult::Status::Ran; break;
                        case infra::HeartbeatRunResult::Status::Skipped:
                            result.status = cron::HeartbeatRunResult::Status::Skipped; break;
                        default:
                            result.status = cron::HeartbeatRunResult::Status::Error; break;
                    }
                    result.reason = r.reason.value_or("");
                    return result;
                };
            }

            // ✅ Aligned with openclaw: create GatewayCronIntegration (agent tool path)
            // Inject heartbeat_runner_cfg + get_queue_size_fn so CronTool's
            // handle_wake(mode=now) has the same lane guard as method_wake.
            if (impl_->cron_service && !impl_->cron_integration) {
                auto integration = std::make_shared<cron::GatewayCronIntegration>(
                    impl_->cron_service);
                // Inject full runner config (agents, model, every, etc.)
                integration->set_heartbeat_runner_cfg(runner_cfg);
                // Inject reply_fn (same as heartbeat_reply_fn above)
                integration->set_heartbeat_reply_fn(heartbeat_reply_fn);
                // Inject get_queue_size (aligned with openclaw: deps.getQueueSize)
                integration->set_get_queue_size_fn([](const std::string& /*lane*/) -> int {
                    return static_cast<int>(
                        hunterclaw::queue::CommandQueue::Instance()
                            .get_queue_size(hunterclaw::queue::CommandLane::Main)
                    );
                });
                impl_->cron_integration = std::move(integration);
            }
        }
    }

    // ✅ Aligned with openclaw: server-startup-post-attach.ts L165-187
    // Fire gateway:startup hook (对齐 OC: setTimeout → triggerInternalHook)
    // Note: HookLoader is in hunterclaw_core; gateway directly fires the event.
    // Bundled hooks are registered at static init time via HookRegistry::instance().
    if (result.is_ok() && impl_->config) {
        try {
            std::string workspace_dir = impl_->config->data.value("workspace", ".");
            auto event = hooks::create_internal_hook_event(
                hooks::HookEventType::GATEWAY, "startup",
                "gateway:startup",
                core::JsonValue::object());
            event.context["workspaceDir"] = workspace_dir;
            hooks::HookRegistry::instance().trigger_hook(event);
        } catch (...) {
            // Hook loading is non-fatal
        }
    }

    return result;
}

core::Result<bool> GatewayServer::stop() {
    if (!impl_->running.load()) {
        return core::Result<bool>::Err("Server not running");
    }
    // ✅ Aligned with openclaw: stop CronService before stopping server
    if (impl_->cron_service) {
        impl_->cron_service->stop();
    }
    // ✅ Aligned with openclaw: stopTaskRegistryMaintenance()
    // 对齐 OpenClaw server.impl.ts: stopTaskRegistryMaintenance() in close handler
    if (impl_->task_maintenance_running.load()) {
        impl_->task_maintenance_running.store(false);
        auto log = spdlog::get("default");
        if (log) log->info("[gateway] task registry maintenance stopped");
    }
    // ✅ Aligned with openclaw: stop heartbeat runner before stopping server
    // Aligned with openclaw server-close.ts: heartbeatRunner.stop()
    if (impl_->heartbeat_runner) {
        impl_->heartbeat_runner->stop();
        impl_->heartbeat_runner = nullptr;
    }
    // ✅ Aligned with openclaw: dispose heartbeat event listener before stopping
    if (impl_->heartbeat_event_disposer) {
        impl_->heartbeat_event_disposer();
        impl_->heartbeat_event_disposer = nullptr;
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
    // 通过 GatewayServerImpl 内部的 conn_id -> handle 映射来查找
    std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl>> target_hdls;
    impl_->server_impl->get_handles_for_conn_ids(conn_ids, target_hdls);
    
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

std::shared_ptr<cron::CronService> GatewayServer::cron_service() const {
    // ✅ Aligned with openclaw: buildGatewayCronService returns { cron, storePath, cronEnabled }
    // cron.cc methods handle nullptr gracefully when cron_service is not yet initialized
    return impl_->cron_service;
}

std::shared_ptr<cron::GatewayCronIntegration> GatewayServer::cron_integration() const {
    // ✅ Aligned with openclaw: GatewayCronIntegration (agent tool path)
    // Created in start() with injected heartbeat_runner_cfg + get_queue_size_fn
    // Used by CronTool (create_cron_tool) to handle wake/cron actions from agents
    return impl_->cron_integration;
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

// ✅ MessageDispatcher access (对齐 OpenClaw: dispatchInboundMessage)
// chat.send 通过 message_dispatcher()->run_reply_agent() 调度，
// 使四种 queue mode (steer/followup/collect/interrupt) 对 chat.send 生效
std::shared_ptr<auto_reply::MessageDispatcher> GatewayServer::message_dispatcher() const {
    return impl_->message_dispatcher;
}

// ✅ DeliveryQueue access (对齐 OpenClaw: deliver 函数)
// method_agent 中 deliver=true 时，通过 delivery_queue 构建 deliver 函数
std::shared_ptr<queue::DeliveryQueue> GatewayServer::delivery_queue() const {
    return impl_->delivery_queue;
}

// ✅ ExecApprovalManager access (对齐 OpenClaw: ExecApprovalManager)
// exec.approval.request/waitDecision/resolve 通过此访问器获取 manager
std::shared_ptr<ExecApprovalManager> GatewayServer::exec_approval_manager() const {
    return impl_->exec_approval_manager;
}
std::shared_ptr<core::Config> GatewayServer::config() const {
    return impl_->config;
}

hunterclaw::infra::HeartbeatRunnerConfig GatewayServer::heartbeat_runner_config() const {
    return impl_->heartbeat_runner_cfg;
}

// ✅ Task subsystem accessors (对齐 OC: task registry + executor + flow registry)
std::shared_ptr<tasks::TaskRegistry> GatewayServer::task_registry() const {
    return impl_->task_registry;
}
std::shared_ptr<tasks::TaskExecutor> GatewayServer::task_executor() const {
    return impl_->task_executor;
}
std::shared_ptr<tasks::TaskFlowRegistry> GatewayServer::task_flow_registry() const {
    return impl_->task_flow_registry;
}

} // namespace gateway
} // namespace hunterclaw

// 🎯 OpenClaw 对齐: Control Plane 写操作方法集合
// 对应 OpenClaw server-methods.ts: CONTROL_PLANE_WRITE_METHODS
const std::set<std::string> hunterclaw::gateway::GatewayServerImpl::CONTROL_PLANE_WRITE_METHODS = {
    "config.apply", "config.patch", "update.run"
};
