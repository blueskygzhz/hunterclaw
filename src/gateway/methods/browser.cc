// ============================================================================
// browser.cc - Browser control proxy
// 🎯 严格对齐 OpenClaw server-methods/browser.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <regex>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// browser.request → proxy browser control requests
// 🎯 OpenClaw 对齐: browser.ts L162-297
core::Result<json> method_browser_request(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    std::string method = params.value("method", "");
    std::string path = params.value("path", "");

    // Normalize
    while (!method.empty() && method.front() == ' ') method.erase(0, 1);
    while (!method.empty() && method.back() == ' ') method.pop_back();
    std::transform(method.begin(), method.end(), method.begin(), ::toupper);

    while (!path.empty() && path.front() == ' ') path.erase(0, 1);
    while (!path.empty() && path.back() == ' ') path.pop_back();

    if (method.empty() || path.empty()) {
        return core::Result<json>::Err("method and path are required");
    }

    if (method != "GET" && method != "POST" && method != "DELETE") {
        return core::Result<json>::Err("method must be GET, POST, or DELETE");
    }

    // 🎯 OpenClaw 对齐: block persistent profile mutations (browser.ts L35-41)
    std::string normalized_path = path;
    if (normalized_path.front() != '/') normalized_path = "/" + normalized_path;
    while (normalized_path.size() > 1 && normalized_path.back() == '/') normalized_path.pop_back();

    if (method == "POST" && normalized_path == "/profiles/create") {
        return core::Result<json>::Err("browser.request cannot create or delete persistent browser profiles");
    }
    if (method == "DELETE" && std::regex_match(normalized_path, std::regex("^/profiles/[^/]+$"))) {
        return core::Result<json>::Err("browser.request cannot create or delete persistent browser profiles");
    }

    // Try node proxy first
    // 🎯 OpenClaw 对齐: browser.ts L162-297 forwards to browser-capable node
    // In HunterClaw, node invoke is done through runtime_state connected clients
    auto state = server->runtime_state();
    if (state) {
        for (const auto& [conn_id, client] : state->clients) {
            if (client && client->connect.role == "node") {
                // Found a node client - browser proxy would forward through it
                // Currently not fully implemented: node invoke via WebSocket RPC
                // would require a request/response flow to the node client
                break;
            }
        }
    }

    // No node available — local browser control is disabled
    return core::Result<json>::Err("browser control is disabled");
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
