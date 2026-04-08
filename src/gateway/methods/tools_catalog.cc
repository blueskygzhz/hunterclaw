// ============================================================================
// tools_catalog.cc - Tool catalog management
// 🎯 严格对齐 OpenClaw server-methods/tools-catalog.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// tools.catalog → return grouped tool catalog
// 🎯 OpenClaw 对齐: tools-catalog.ts L125-167
core::Result<json> method_tools_catalog(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string agent_id = params.value("agentId", "main");
    bool include_plugins = params.value("includePlugins", true);
    (void)include_plugins;

    // Build core tool groups
    json groups = json::array();

    // Core tools group
    json core_group;
    core_group["id"] = "core";
    core_group["label"] = "Core Tools";
    core_group["source"] = "core";
    core_group["tools"] = json::array({
        json::object({
            {"id", "web_search"}, {"label", "Web Search"},
            {"description", "Search the web for information"},
            {"source", "core"}, {"defaultProfiles", json::array({"full", "coding"})}
        }),
        json::object({
            {"id", "web_fetch"}, {"label", "Web Fetch"},
            {"description", "Fetch and read web page content"},
            {"source", "core"}, {"defaultProfiles", json::array({"full", "coding"})}
        }),
        json::object({
            {"id", "file_read"}, {"label", "Read File"},
            {"description", "Read file contents"},
            {"source", "core"}, {"defaultProfiles", json::array({"minimal", "coding", "full"})}
        }),
        json::object({
            {"id", "file_write"}, {"label", "Write File"},
            {"description", "Write or create files"},
            {"source", "core"}, {"defaultProfiles", json::array({"coding", "full"})}
        }),
    });
    groups.push_back(core_group);

    // Profiles
    json profiles = json::array({
        json::object({{"id", "minimal"}, {"label", "Minimal"}}),
        json::object({{"id", "coding"}, {"label", "Coding"}}),
        json::object({{"id", "messaging"}, {"label", "Messaging"}}),
        json::object({{"id", "full"}, {"label", "Full"}}),
    });

    json result;
    result["agentId"] = agent_id;
    result["profiles"] = profiles;
    result["groups"] = groups;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
