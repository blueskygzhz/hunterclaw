#include "hunterclaw/gateway/methods.h"
#include "hunterclaw/gateway/gateway.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

// ============================================================================
// Method Registration Implementation
// ============================================================================

/**
 * 方法注册架构说明 (对齐 OpenClaw)
 * 
 * HunterClaw 使用分层的方法注册架构:
 * 
 * 1. 模块化方法实现在 src/gateway/methods/*.cc 中:
 *    - health.cc: method_ping, method_echo, method_status
 *    - sessions.cc: method_session_list, method_session_get, etc.
 *    - channels.cc: method_channel_list, method_channel_start, etc.
 *    - agent.cc: method_agent_cancel, method_agent_status
 *    - chat.cc: method_chat_send, method_chat_abort, etc.
 *    - config.cc: method_config_get, method_config_set, etc.
 *    - skills.cc: method_skill_list, method_skill_get
 *    - cron.cc: method_wake, method_cron_status, etc.
 * 
 * 2. 方法声明在 include/hunterclaw/gateway/methods.h
 * 
 * 3. 实际注册通过 GatewayMethods::register_builtin_methods() 完成
 *    GatewayMethods 类在 gateway.h 中定义,在 gateway.cc 中实现
 * 
 * 4. GatewayServer 不直接有 register_method() 方法
 *    方法注册由 GatewayMethods 类管理
 * 
 * 此函数仅作为占位实现,确保链接器可以找到符号。
 * 实际的方法注册应该通过 GatewayMethods 类完成。
 */
void register_all_methods(std::shared_ptr<GatewayServer> server) {
    if (!server) {
        // Cannot register methods: server is null
        return;
    }
    
    // Note: Method registration is handled by GatewayMethods class
    // All modular method implementations are available:
    //   - Health Methods: ping, echo, status (3 methods)
    //   - Session Methods: list, get, create, update, delete (5 methods)
    //   - Channel Methods: list, get, start, stop, send, health (6 methods)
    //   - Agent Methods: cancel, status (2 methods)
    //   - Chat Methods: send, abort, history, regenerate, etc. (12 methods)
    //   - Config Methods: get, set, reload, patch, etc. (8 methods)
    //   - Skill Methods: list, get (2 methods)
    //   - Cron Methods: wake, status, list, add, etc. (8 methods)
    //   Total: 46 modular methods available
    
    // 🎯 OpenClaw 对齐:
    // 在 OpenClaw 中,方法注册通过 MethodRegistry.registerMethod() 完成
    // HunterClaw 使用 GatewayMethods::register_method() 实现相同功能
    // 
    // 示例:
    // auto methods = std::make_shared<GatewayMethods>(server);
    // methods->register_method("ping", handler_func);
    // methods->register_builtin_methods();
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
