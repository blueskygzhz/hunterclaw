#include "hunterclaw/gateway/methods.h"
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <functional>
#include <map>
#include <string>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// create_method_handler - RPC Method Dispatcher
// Called from gateway.cc to initialize the method handler
// ============================================================================

std::function<core::Result<json>(const std::string&, const json&)>
create_method_handler(std::shared_ptr<GatewayServer> server) {
    using MethodFn = std::function<core::Result<json>(
        std::shared_ptr<GatewayServer>,
        std::shared_ptr<MethodGuard>,
        const ClientContext&,
        const json&
    )>;

    // Build method dispatch table
    static const std::map<std::string, MethodFn> method_table = {
        // Health
        {"ping",                    methods::method_ping},
        {"echo",                    methods::method_echo},
        {"status",                  methods::method_status},
        // Sessions
        {"session.list",            methods::method_session_list},
        {"session.get",             methods::method_session_get},
        {"session.create",          methods::method_session_create},
        {"session.update",          methods::method_session_update},
        {"session.delete",          methods::method_session_delete},
        // Channels
        {"channel.list",            methods::method_channel_list},
        {"channel.get",             methods::method_channel_get},
        {"channel.start",           methods::method_channel_start},
        {"channel.stop",            methods::method_channel_stop},
        {"channel.send",            methods::method_channel_send},
        {"channel.health",          methods::method_channel_health},
        // Agent
        {"agent.cancel",            methods::method_agent_cancel},
        {"agent.status",            methods::method_agent_status},
        // Chat
        {"chat.send",               methods::method_chat_send},
        {"chat.abort",              methods::method_chat_abort},
        {"chat.history",            methods::method_chat_history},
        {"chat.update_params",      methods::method_chat_update_params},
        {"chat.regenerate",         methods::method_chat_regenerate},
        {"chat.fork",               methods::method_chat_fork},
        {"chat.summarize",          methods::method_chat_summarize},
        {"chat.inject",             methods::method_chat_inject},
        {"chat.edit",               methods::method_chat_edit},
        {"chat.delete",             methods::method_chat_delete},
        {"chat.transcript",         methods::method_chat_transcript},
        {"chat.export",             methods::method_chat_export},
        // Config
        {"config.get",              methods::method_config_get},
        {"config.set",              methods::method_config_set},
        {"config.reload",           methods::method_config_reload},
        {"config.patch",            methods::method_config_patch},
        {"config.list",             methods::method_config_list},
        {"config.validate",         methods::method_config_validate},
        {"config.reset",            methods::method_config_reset},
        {"config.export",           methods::method_config_export},
        // Skills
        {"skill.list",              methods::method_skill_list},
        {"skill.get",               methods::method_skill_get},
        // Cron
        {"wake",                    methods::method_wake},
        {"cron.status",             methods::method_cron_status},
        {"cron.list",               methods::method_cron_list},
        {"cron.add",                methods::method_cron_add},
        {"cron.update",             methods::method_cron_update},
        {"cron.remove",             methods::method_cron_remove},
        {"cron.run",                methods::method_cron_run},
        {"cron.runs",               methods::method_cron_runs},
    };

    return [server](const std::string& method_name, const json& params) -> core::Result<json> {
        auto it = method_table.find(method_name);
        if (it == method_table.end()) {
            return core::Result<json>::Err("Unknown method: " + method_name);
        }

        // Create a default method guard and client context
        // MethodGuard requires AuthManager; pass nullptr for internal dispatch
        auto guard = std::make_shared<MethodGuard>(nullptr);
        ClientContext ctx;
        ctx.authenticated = true;  // Internal dispatch is always authenticated

        try {
            return it->second(server, guard, ctx, params);
        } catch (const std::exception& e) {
            return core::Result<json>::Err(
                std::string("Method ") + method_name + " threw: " + e.what()
            );
        }
    };
}

namespace methods {

// ============================================================================
// Method Registration Implementation
// ============================================================================

void register_all_methods(std::shared_ptr<GatewayServer> server) {
    if (!server) {
        return;
    }
    // Method registration is handled via create_method_handler dispatch table
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
