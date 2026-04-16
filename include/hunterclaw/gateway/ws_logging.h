// ✅ 严格对齐 OpenClaw: src/gateway/ws-logging.ts (14行)
// WebSocket 日志样式配置 + 慢 WS 阈值
#pragma once

#include <string>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Types — 对齐 OpenClaw: GatewayWsLogStyle
// ============================================================================

// ✅ 对齐 OpenClaw: GatewayWsLogStyle = "auto" | "full" | "compact"
enum class GatewayWsLogStyle {
    AUTO,     // 自动根据 TTY 选择
    FULL,     // 完整日志
    COMPACT   // 精简日志
};

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: setGatewayWsLogStyle (L5-7)
void set_gateway_ws_log_style(GatewayWsLogStyle style);

// ✅ 对齐 OpenClaw: getGatewayWsLogStyle (L9-11)
GatewayWsLogStyle get_gateway_ws_log_style();

// ✅ 对齐 OpenClaw: DEFAULT_WS_SLOW_MS = 50 (L13)
constexpr int64_t DEFAULT_WS_SLOW_MS = 50;

}  // namespace gateway
}  // namespace hunterclaw
