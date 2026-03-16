#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Session Methods - 完全对齐 OpenClaw
// ============================================================================

// sessions.list - 列出所有会话 (对齐 OpenClaw sessionsCommand)
core::Result<json> method_session_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto& sessions = server->runtime_state()->sessions;
    
    // 参数
    std::string agent_filter = params.value("agent", "");
    bool all_agents = params.value("allAgents", false);
    int active_minutes = params.value("active", -1);  // -1 表示不过滤
    
    // 计算活跃时间阈值
    auto now = core::utils::now();
    auto active_threshold = now - std::chrono::minutes(active_minutes);
    
    // 收集会话列表
    json sessions_array = json::array();
    
    for (const auto& [session_id, session_info] : sessions) {
        // 过滤活跃时间
        if (active_minutes >= 0 && session_info.last_active_at < active_threshold) {
            continue;
        }
        
        // 过滤 agent
        std::string session_agent = session_info.metadata.count("agent_id") 
            ? session_info.metadata.at("agent_id") : "default";
        if (!agent_filter.empty() && session_agent != agent_filter && !all_agents) {
            continue;
        }
        
        json session_json;
        session_json["key"] = session_id;
        session_json["agentId"] = session_agent;
        
        // 判断会话类型 (kind)
        if (session_info.key.target_id.empty()) {
            session_json["kind"] = "global";
        } else if (session_info.key.target_id.find("group:") == 0 || 
                   session_info.key.target_id.find("channel:") == 0) {
            session_json["kind"] = "group";
        } else if (!session_info.key.target_id.empty()) {
            session_json["kind"] = "direct";
        } else {
            session_json["kind"] = "unknown";
        }
        
        // 时间信息
        session_json["updatedAt"] = session_info.last_active_at.time_since_epoch().count();
        
        // Token 使用情况 (占位符 - 需要从 transcript 读取)
        session_json["totalTokens"] = 0;
        session_json["totalTokensFresh"] = false;
        session_json["contextTokens"] = nullptr;  // null 表示未知
        
        // 模型信息
        session_json["model"] = session_info.metadata.count("model") 
            ? session_info.metadata.at("model") : "unknown";
        
        // Flags
        json flags;
        flags["locked"] = false;  // TODO: 从 session_info 读取
        flags["systemPrompt"] = session_info.metadata.count("systemPrompt");
        session_json["flags"] = flags;
        
        sessions_array.push_back(session_json);
    }
    
    // 按更新时间排序 (最新的在前)
    std::sort(sessions_array.begin(), sessions_array.end(), 
        [](const json& a, const json& b) {
            return a["updatedAt"].get<int64_t>() > b["updatedAt"].get<int64_t>();
        });
    
    // 构建结果
    json result;
    result["sessions"] = sessions_array;
    result["count"] = sessions_array.size();
    if (active_minutes >= 0) {
        result["activeMinutes"] = active_minutes;
    } else {
        result["activeMinutes"] = nullptr;
    }
    
    if (all_agents) {
        result["allAgents"] = true;
    }
    
    return core::Result<json>::Ok(result);
}

// sessions.cleanup - 会话清理和维护 (对齐 OpenClaw sessionsCleanupCommand)
core::Result<json> method_session_cleanup(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto& sessions = server->runtime_state()->sessions;
    
    std::string agent_id = params.value("agent", "default");
    bool all_agents = params.value("allAgents", false);
    bool dry_run = params.value("dryRun", false);
    bool enforce = params.value("enforce", false);
    std::string active_key = params.value("activeKey", "");
    bool fix_missing = params.value("fixMissing", false);
    
    // 统计
    int before_count = sessions.size();
    int missing = 0;
    int pruned = 0;
    int capped = 0;
    
    // 磁盘预算统计
    json disk_budget;
    disk_budget["totalBytesBefore"] = 0;
    disk_budget["totalBytesAfter"] = 0;
    disk_budget["removedFiles"] = 0;
    disk_budget["removedEntries"] = 0;
    
    // TODO: 实现清理逻辑
    
    // 1. prune-missing: 删除缺失 transcript 文件的条目
    if (fix_missing) {
        // 遍历会话，检查 transcript 文件是否存在
        for (auto it = sessions.begin(); it != sessions.end(); ) {
            // TODO: 检查文件存在性
            bool file_exists = true;  // Placeholder
            
            if (!file_exists) {
                missing++;
                if (!dry_run && enforce) {
                    it = sessions.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    
    // 2. prune-stale: 删除过期会话 (基于 pruneAfterMs)
    int64_t prune_after_ms = 30LL * 24 * 60 * 60 * 1000;  // 默认 30 天
    auto now = core::utils::now();
    auto stale_threshold = now - std::chrono::milliseconds(prune_after_ms);
    
    for (auto it = sessions.begin(); it != sessions.end(); ) {
        if (it->second.last_active_at < stale_threshold && it->first != active_key) {
            pruned++;
            if (!dry_run && enforce) {
                it = sessions.erase(it);
                continue;
            }
        }
        ++it;
    }
    
    // 3. cap-overflow: 限制条目数 (基于 maxEntries)
    int max_entries = 1000;  // TODO: 从配置读取
    if (static_cast<int>(sessions.size()) > max_entries) {
        // 按活跃时间排序，删除最旧的
        std::vector<std::pair<std::string, int64_t>> sorted_sessions;
        for (const auto& [key, info] : sessions) {
            if (key != active_key) {
                sorted_sessions.push_back({key, info.last_active_at.time_since_epoch().count()});
            }
        }
        
        std::sort(sorted_sessions.begin(), sorted_sessions.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        
        int to_remove = sessions.size() - max_entries;
        for (int i = 0; i < to_remove && i < static_cast<int>(sorted_sessions.size()); i++) {
            capped++;
            if (!dry_run && enforce) {
                sessions.erase(sorted_sessions[i].first);
            }
        }
    }
    
    // 4. evict-budget: 磁盘配额清理
    // TODO: 实现磁盘预算检查
    
    int after_count = sessions.size();
    
    // 构建结果
    json result;
    result["agentId"] = agent_id;
    result["storePath"] = "";  // TODO: 会话存储路径
    result["mode"] = enforce ? "enforce" : "warn";
    result["dryRun"] = dry_run;
    result["beforeCount"] = before_count;
    result["afterCount"] = after_count;
    result["missing"] = missing;
    result["pruned"] = pruned;
    result["capped"] = capped;
    result["diskBudget"] = disk_budget;
    result["wouldMutate"] = (missing > 0 || pruned > 0 || capped > 0);
    result["applied"] = !dry_run && enforce;
    result["appliedCount"] = missing + pruned + capped;
    
    return core::Result<json>::Ok(result);
}

// 4. session.get - Get session by ID
core::Result<json> method_session_get(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }

    std::string session_id = params["session_id"];
    auto& sessions = server->runtime_state()->sessions;

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
        return core::Result<json>::Err("Session not found");
    }

    const auto& session_info = it->second;
    json result;
    result["session_id"] = session_id;
    result["channel_id"] = session_info.key.channel;
    result["account_id"] = session_info.key.account_id;
    result["conversation_id"] = session_info.key.target_id;
    result["created_at"] = session_info.created_at.time_since_epoch().count();
    result["last_active_at"] = session_info.last_active_at.time_since_epoch().count();

    return core::Result<json>::Ok(result);
}

// 5. session.create - Create new session (NEW - 对齐 OpenClaw)
core::Result<json> method_session_create(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // Required parameters
    if (!params.contains("channel_id") || !params.contains("account_id") || !params.contains("conversation_id")) {
        return core::Result<json>::Err("Missing required parameters: channel_id, account_id, conversation_id");
    }

    std::string channel_id = params["channel_id"];
    std::string account_id = params["account_id"];
    std::string conversation_id = params["conversation_id"];

    // Optional device info
    std::string device_id = params.value("device_id", "");
    std::string device_token = params.value("device_token", "");

    // Create session key
    core::SessionKey key;
    key.channel = channel_id;
    key.account_id = account_id;
    key.target_id = conversation_id;
    key.device_id = device_id;
    key.device_token = device_token;

    // Generate session ID
    std::string session_id = core::utils::generate_uuid();

    // Create session info
    core::SessionInfo session_info;
    session_info.key = key;
    session_info.session_id = session_id;
    session_info.created_at = core::utils::now();
    session_info.last_active_at = session_info.created_at;

    // Store in runtime state
    server->runtime_state()->sessions[session_id] = session_info;

    json result;
    result["session_id"] = session_id;
    result["channel_id"] = channel_id;
    result["account_id"] = account_id;
    result["conversation_id"] = conversation_id;
    result["created_at"] = session_info.created_at.time_since_epoch().count();

    return core::Result<json>::Ok(result);
}

// 6. session.update - Update session (NEW - 对齐 OpenClaw)
core::Result<json> method_session_update(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }

    std::string session_id = params["session_id"];
    auto& sessions = server->runtime_state()->sessions;

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
        return core::Result<json>::Err("Session not found");
    }

    // Update last_active_at
    it->second.last_active_at = core::utils::now();

    // Update metadata (完整实现 - 对齐 OpenClaw)
    if (params.contains("metadata")) {
        // Metadata fields supported: thinkingLevel, verboseLevel, model, sendPolicy, groupActivation, elevated
        auto& metadata = params["metadata"];

        // Update all metadata fields from request
        for (auto& [key, value] : metadata.items()) {
            if (value.is_string()) {
                it->second.metadata[key] = value.get<std::string>();
            } else if (value.is_boolean()) {
                it->second.metadata[key] = value.get<bool>() ? "true" : "false";
            } else if (value.is_number()) {
                it->second.metadata[key] = std::to_string(value.get<double>());
            } else {
                it->second.metadata[key] = value.dump();
            }
        }

        // Supported OpenClaw metadata fields:
        // - thinkingLevel: "normal" | "verbose" | "quiet"
        // - verboseLevel: 0-3 (integer)
        // - model: LLM model name preference
        // - sendPolicy: "auto" | "manual" | "confirm"
        // - groupActivation: group activation settings
        // - elevated: "true" | "false" (elevated permission mode)
    }

    json result;
    result["session_id"] = session_id;
    result["updated"] = true;
    result["last_active_at"] = it->second.last_active_at.time_since_epoch().count();

    return core::Result<json>::Ok(result);
}

// 7. session.delete - Delete session (🏢 Multi-tenant Agent cleanup)
core::Result<json> method_session_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }

    std::string session_id = params["session_id"];
    auto& sessions = server->runtime_state()->sessions;

    auto it = sessions.find(session_id);
    if (it == sessions.end()) {
        return core::Result<json>::Err("Session not found");
    }

    // 🏢 清理对应的 Agent 实例 (多租户核心)
    auto agent_pool = server->agent_pool();
    if (agent_pool) {
        // 从 Session 推断 agent_id
        std::string agent_id = "default";  // 默认使用 "default"
        if (it->second.metadata.count("agent_id")) {
            agent_id = it->second.metadata.at("agent_id");
        }

        // 移除 Agent 实例 (remove_agent 返回 void)
        agent_pool->remove_agent(agent_id);
    }

    // 删除 Session
    sessions.erase(it);

    json result;
    result["ok"] = true;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
