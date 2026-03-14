#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Session Methods
// ============================================================================

// 3. session.list - List all sessions
core::Result<json> method_session_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    auto sessions = server->runtime_state()->sessions;

    json result = json::array();
    for (const auto& [session_id, session_info] : sessions) {
        json session_json;
        session_json["session_id"] = session_id;
        session_json["channel_id"] = session_info.key.channel;
        session_json["account_id"] = session_info.key.account_id;
        session_json["conversation_id"] = session_info.key.target_id;
        session_json["created_at"] = session_info.created_at.time_since_epoch().count();
        session_json["last_active_at"] = session_info.last_active_at.time_since_epoch().count();
        result.push_back(session_json);
    }

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
