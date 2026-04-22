// Aligned with openclaw: src/discord/monitor/provider.lifecycle.ts
// runDiscordGatewayLifecycle: manages the Discord Gateway WebSocket lifecycle,
// including reconnect loop, stall watchdog, hello timeout, and status updates.
#pragma once

#include "hunterclaw/channels/discord_gateway_client.h"
#include "hunterclaw/channels/discord_listener.h"
#include "hunterclaw/channels/plugin.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DiscordGatewayLifecycleParams
// Aligned with openclaw: runDiscordGatewayLifecycle params
// ============================================================================
struct DiscordGatewayLifecycleParams {
    std::string account_id;
    std::string token;
    int intents = 0;

    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<std::atomic<bool>> abort_signal;

    // Aligned with openclaw: statusSink → ctx.set_status
    std::function<void(bool connected,
                       std::optional<int64_t> last_connected_at,
                       std::optional<int64_t> last_event_at,
                       std::optional<nlohmann::json> last_disconnect,
                       std::optional<std::string> last_error)> set_status;

    // Aligned with openclaw: listeners registered on client.listeners
    // Each listener is called for every DISPATCH event.
    std::vector<std::shared_ptr<DiscordListenerBase>> listeners;

    // Aligned with openclaw: isDisallowedIntentsError
    std::function<bool(const std::string& err)> is_disallowed_intents_error;

    // ════════════════════════════════════════════════════════════════════
    // 严格对齐 OC: WhatsApp monitor.ts L297-314
    //   void drainPendingDeliveries({ drainKey, selectEntry, ... })
    //
    // 在 gateway **reconnect 成功**（READY/RESUMED 且 reconnect_attempts > 0）
    // 后调用一次。外层注入该 callback 来衔接
    // outbound::drain_pending_deliveries()，排空属于本 channel+account 的
    // 积压消息。
    //
    // 未注入时跳过（向后兼容）。首次连接（非 reconnect）不调用。
    // ════════════════════════════════════════════════════════════════════
    std::function<void()> on_reconnect_connected;
};

// ============================================================================
// run_discord_gateway_lifecycle
// Aligned with openclaw: runDiscordGatewayLifecycle (provider.lifecycle.ts)
//
// Blocks until abort_signal fires or a fatal gateway error occurs.
// Manages:
//   - WebSocket connect/reconnect loop
//   - HELLO timeout watchdog (30s)
//   - Reconnect stall watchdog (5min)
//   - Status updates (connected/disconnected/lastEventAt)
//   - Dispatch events to registered listeners
// ============================================================================
void run_discord_gateway_lifecycle(DiscordGatewayLifecycleParams params);

// ============================================================================
// dispatch_discord_event
// Aligned with openclaw: client.listeners → listener.handle(data, client)
// Dispatches a DISPATCH event to all registered listeners.
// ============================================================================
void dispatch_discord_event(
    const std::vector<std::shared_ptr<DiscordListenerBase>>& listeners,
    const std::string& event_name,
    const nlohmann::json& data,
    const std::string& account_id);

} // namespace channels
} // namespace hunterclaw
