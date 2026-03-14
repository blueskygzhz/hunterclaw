#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Chat Methods
// ============================================================================

// 13. chat.send - Send message (🏢 Multi-tenant Agent Pool)
core::Result<json> method_chat_send(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id") || !params.contains("message")) {
        return core::Result<json>::Err("Missing required parameters: session_id, message");
    }
    
    std::string session_id = params["session_id"];
    std::string message = params["message"];
    
    auto agent_pool = server->agent_pool();
    if (!agent_pool) {
        return core::Result<json>::Err("AgentPool not initialized");
    }
    
    auto agent = agent_pool->get_or_create_agent(session_id);
    if (!agent) {
        return core::Result<json>::Err("Failed to get or create agent");
    }
    
    json result;
    result["session_id"] = session_id;
    result["agent_id"] = agent->context().agent_id;
    result["message"] = "Message received";
    result["workspace"] = agent->context().workspace_dir;
    
    return core::Result<json>::Ok(result);
}

// 14. chat.abort
core::Result<json> method_chat_abort(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return core::Result<json>::Err("chat.abort handled by gateway protocol handler");
}

// 15. chat.history
core::Result<json> method_chat_history(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    return core::Result<json>::Err("chat.history handled by gateway protocol handler");
}

// 16. chat.updateParams
core::Result<json> method_chat_update_params(
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
    
    json updated_params;
    if (params.contains("temperature")) {
        it->second.metadata["temperature"] = std::to_string(params["temperature"].get<double>());
        updated_params["temperature"] = params["temperature"];
    }
    if (params.contains("model")) {
        it->second.metadata["model"] = params["model"].get<std::string>();
        updated_params["model"] = params["model"];
    }
    
    json result;
    result["session_id"] = session_id;
    result["updated"] = true;
    result["params"] = updated_params;
    return core::Result<json>::Ok(result);
}

// 17. chat.regenerate
core::Result<json> method_chat_regenerate(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }
    
    std::string run_id = core::utils::generate_uuid();
    json result;
    result["session_id"] = params["session_id"];
    result["run_id"] = run_id;
    result["status"] = "regenerating";
    return core::Result<json>::Ok(result);
}

// 18. chat.fork
core::Result<json> method_chat_fork(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id") || !params.contains("message_id")) {
        return core::Result<json>::Err("Missing required parameters");
    }
    
    std::string new_session_id = core::utils::generate_uuid();
    json result;
    result["source_session_id"] = params["session_id"];
    result["new_session_id"] = new_session_id;
    result["fork_from_message_id"] = params["message_id"];
    return core::Result<json>::Ok(result);
}

// 19. chat.summarize
core::Result<json> method_chat_summarize(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }
    
    json result;
    result["session_id"] = params["session_id"];
    result["summary"] = "[Summary placeholder]";
    return core::Result<json>::Ok(result);
}

// 20. chat.inject
core::Result<json> method_chat_inject(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id") || !params.contains("role") || !params.contains("content")) {
        return core::Result<json>::Err("Missing required parameters");
    }
    
    std::string message_id = core::utils::generate_uuid();
    json result;
    result["session_id"] = params["session_id"];
    result["message_id"] = message_id;
    result["injected"] = true;
    return core::Result<json>::Ok(result);
}

// 21. chat.edit
core::Result<json> method_chat_edit(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id") || !params.contains("message_id") || !params.contains("content")) {
        return core::Result<json>::Err("Missing required parameters");
    }
    
    json result;
    result["session_id"] = params["session_id"];
    result["message_id"] = params["message_id"];
    result["edited"] = true;
    return core::Result<json>::Ok(result);
}

// 22. chat.delete
core::Result<json> method_chat_delete(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id") || !params.contains("message_id")) {
        return core::Result<json>::Err("Missing required parameters");
    }
    
    json result;
    result["session_id"] = params["session_id"];
    result["message_id"] = params["message_id"];
    result["deleted"] = true;
    return core::Result<json>::Ok(result);
}

// 23. chat.transcript
core::Result<json> method_chat_transcript(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }
    
    json result;
    result["session_id"] = params["session_id"];
    result["messages"] = json::array();
    result["total_count"] = 0;
    return core::Result<json>::Ok(result);
}

// 24. chat.export
core::Result<json> method_chat_export(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    if (!params.contains("session_id")) {
        return core::Result<json>::Err("Missing session_id");
    }
    
    std::string format = params.value("format", "json");
    json result;
    result["session_id"] = params["session_id"];
    result["format"] = format;
    result["content"] = "[Export placeholder]";
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
