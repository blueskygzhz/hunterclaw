#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/channels/manager.h"  // ✅ For ChannelManager::get_runtime_snapshot()
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Core Methods (Health & Debug) - 完全对齐 OpenClaw
// ============================================================================

// ping - 健康检查 (基础)
core::Result<json> method_ping(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    json result;
    result["pong"] = true;
    result["timestamp"] = core::utils::now().time_since_epoch().count();
    result["server"] = "HunterClaw";
    result["version"] = "0.1.0";
    return core::Result<json>::Ok(result);
}

// echo - 调试回显
core::Result<json> method_echo(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return core::Result<json>::Ok(params);
}

// status - 服务器状态 (简化版)
core::Result<json> method_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto runtime = server->runtime_state();
    
    json result;
    result["server"] = "HunterClaw";
    result["version"] = "0.1.0";
    result["uptime_ms"] = 0;  // TODO: 添加启动时间追踪
    result["sessions_count"] = runtime->sessions.size();
    result["channels_count"] = 0;  // TODO: 从 channel_manager 获取
    result["memory_mb"] = 0;       // TODO: 实现内存追踪
    
    return core::Result<json>::Ok(result);
}

// health - 完整健康检查 (对齐 OpenClaw healthCommand)
core::Result<json> method_health(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto start_time = std::chrono::steady_clock::now();
    
    int timeout_ms = params.value("timeoutMs", 5000);
    bool verbose = params.value("verbose", false);
    
    json result;
    result["ok"] = true;
    result["ts"] = core::utils::now().time_since_epoch().count();
    
    // ========== 1. 渠道健康状态 ==========
    auto channel_mgr = server->channel_manager();
    auto runtime_snapshot = channel_mgr->get_runtime_snapshot();
    
    json channels = json::object();
    json channel_order = json::array();
    json channel_labels = json::object();
    
    for (const auto& [channel_id, channel_state] : runtime_snapshot.channels) {
        channel_order.push_back(channel_id);
        channel_labels[channel_id] = channel_id;  // TODO: 从配置获取友好名称
        
        json channel_info;
        json accounts = json::object();
        
        // 检查是否有多账户
        auto multi_accounts_it = runtime_snapshot.channel_accounts.find(channel_id);
        if (multi_accounts_it != runtime_snapshot.channel_accounts.end()) {
            // 有多账户，遍历所有账户
            for (const auto& [account_id, account_state] : multi_accounts_it->second) {
                json account_info;
                account_info["accountId"] = account_id;
                account_info["configured"] = account_state.configured.value_or(false);
                account_info["linked"] = account_state.running.value_or(false) && account_state.connected.value_or(false);
                
                // Probe 探测
                json probe;
                probe["ok"] = account_state.running.value_or(false) && account_state.connected.value_or(false);
                probe["elapsedMs"] = 0;  // TODO: 实际探测延迟
                
                // 错误信息
                if (account_state.last_error && !account_state.last_error->empty()) {
                    probe["error"] = *account_state.last_error;
                }
                
                account_info["probe"] = probe;
                accounts[account_id] = account_info;
            }
        } else {
            // 只有默认账户
            json account_info;
            account_info["accountId"] = channel_state.account_id;
            account_info["configured"] = channel_state.configured.value_or(false);
            account_info["linked"] = channel_state.running.value_or(false) && channel_state.connected.value_or(false);
            
            // Probe 探测
            json probe;
            probe["ok"] = channel_state.running.value_or(false) && channel_state.connected.value_or(false);
            probe["elapsedMs"] = 0;
            
            if (channel_state.last_error && !channel_state.last_error->empty()) {
                probe["error"] = *channel_state.last_error;
            }
            
            account_info["probe"] = probe;
            accounts[channel_state.account_id] = account_info;
        }
        
        channel_info["accounts"] = accounts;
        channels[channel_id] = channel_info;
    }
    
    result["channels"] = channels;
    result["channelOrder"] = channel_order;
    result["channelLabels"] = channel_labels;
    
    // ========== 2. Agent 心跳和会话信息 ==========
    
    // 默认 agent 配置
    result["defaultAgentId"] = "default";  // TODO: 从配置读取
    result["heartbeatSeconds"] = 300;      // TODO: 从配置读取
    
    // Agents 列表
    json agents = json::array();
    
    // TODO: 遍历所有 agent
    json default_agent;
    default_agent["agentId"] = "default";
    default_agent["name"] = "Default Agent";
    default_agent["isDefault"] = true;
    
    // 心跳配置
    default_agent["heartbeat"] = {
        {"everyMs", 300000},
        {"enabled", true}
    };
    
    // 会话统计
    auto runtime = server->runtime_state();
    json agent_sessions;
    agent_sessions["path"] = "";  // TODO: 会话存储路径
    agent_sessions["count"] = runtime->sessions.size();
    
    // 最近活跃会话
    json recent_sessions = json::array();
    int recent_count = 0;
    for (const auto& [session_id, session_info] : runtime->sessions) {
        if (recent_count >= 5) break;  // 最多显示 5 个
        
        json recent;
        recent["key"] = session_id;
        recent["updatedAt"] = session_info.last_active_at.time_since_epoch().count();
        
        // 计算年龄（秒）
        auto now = core::utils::now();
        auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - session_info.last_active_at
        ).count();
        recent["age"] = age_sec;
        
        recent_sessions.push_back(recent);
        recent_count++;
    }
    agent_sessions["recent"] = recent_sessions;
    
    default_agent["sessions"] = agent_sessions;
    agents.push_back(default_agent);
    
    result["agents"] = agents;
    
    // ========== 3. 全局会话摘要 ==========
    json sessions_summary;
    sessions_summary["path"] = "";  // TODO: 全局会话路径
    sessions_summary["count"] = runtime->sessions.size();
    sessions_summary["recent"] = recent_sessions;
    result["sessions"] = sessions_summary;
    
    // ========== 4. 计算总耗时 ==========
    auto end_time = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();
    result["durationMs"] = duration_ms;
    
    // ========== 5. Verbose 模式详细信息 ==========
    if (verbose) {
        // 详细探测信息
        json verbose_info;
        verbose_info["channelProbes"] = channels.size();
        verbose_info["accountsProbed"] = 0;  // TODO: 统计
        verbose_info["timeoutMs"] = timeout_ms;
        result["verbose"] = verbose_info;
    }
    
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
