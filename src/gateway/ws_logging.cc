// ✅ 严格对齐 OpenClaw: src/gateway/ws-logging.ts (14行)
// WebSocket 日志样式实现

#include "hunterclaw/gateway/ws_logging.h"
#include <atomic>

namespace hunterclaw {
namespace gateway {

// ✅ 对齐 OpenClaw: let gatewayWsLogStyle: GatewayWsLogStyle = "auto"
static std::atomic<GatewayWsLogStyle> g_ws_log_style{GatewayWsLogStyle::AUTO};

// ✅ 对齐 OpenClaw: setGatewayWsLogStyle (L5-7)
void set_gateway_ws_log_style(GatewayWsLogStyle style) {
    g_ws_log_style.store(style);
}

// ✅ 对齐 OpenClaw: getGatewayWsLogStyle (L9-11)
GatewayWsLogStyle get_gateway_ws_log_style() {
    return g_ws_log_style.load();
}

}  // namespace gateway
}  // namespace hunterclaw
