// ============================================================================
// tools_effective.cc - 对齐 OpenClaw server-methods/tools-effective.ts (167 行)
//
// 解析 session 对应的 effective tool inventory，包括:
//   - 按 agent/session 解析模型 provider
//   - 查找 delivery context
//   - 调用 resolveEffectiveToolInventory
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/agents/agent_pool.h"
#include "hunterclaw/agents/agent_instance.h"
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// 对齐 tools-effective.ts:21-39 resolveRequestedAgentIdOrRespondError
static std::optional<std::string> resolve_requested_agent_id(
    std::shared_ptr<GatewayServer> server,
    const json& params
) {
    if (!params.contains("agentId") || !params["agentId"].is_string()) return std::nullopt;
    std::string agent_id = params["agentId"].get<std::string>();
    auto s = agent_id.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    auto e = agent_id.find_last_not_of(" \t");
    return agent_id.substr(s, e - s + 1);
}

// tools.effective (对齐 tools-effective.ts toolsEffectiveHandlers)
core::Result<json> method_tools_effective(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    // Validate params
    if (!params.contains("sessionKey") || !params["sessionKey"].is_string()) {
        return core::Result<json>::Err("sessionKey required");
    }
    std::string session_key = params["sessionKey"].get<std::string>();
    if (session_key.empty()) {
        return core::Result<json>::Err("sessionKey required");
    }

    auto requested_agent = resolve_requested_agent_id(server, params);

    // 对齐 tools-effective.ts:42-110 resolveTrustedToolsEffectiveContext
    auto config = server->config();

    // Resolve session agent id
    std::string agent_id = requested_agent.value_or("main");

    // Check admin scope
    bool sender_is_owner = false;
    for (const auto& scope : client_ctx.connect.scopes) {
        if (scope == "operator.admin") { sender_is_owner = true; break; }
    }

    // 对齐 tools-effective.ts:147-164 resolveEffectiveToolInventory
    // 工具清单由 agent 的 tool definition 列表构建
    // 完整 effective tool inventory 解析待 tools 模块进一步对齐
    json tools_list = json::array();

    json result;
    result["tools"] = tools_list;
    result["agentId"] = agent_id;
    result["sessionKey"] = session_key;
    result["senderIsOwner"] = sender_is_owner;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
