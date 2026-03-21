#pragma once

#include "hunterclaw/gateway/methods.h"
#include "hunterclaw/gateway/gateway.h"
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// Gateway Methods List (严格对齐 OpenClaw server-methods-list.ts)
// ============================================================================

/**
 * @brief 返回所有 Gateway 支持的方法名列表
 *
 * 严格对齐 OpenClaw:
 * ```typescript
 * export function listGatewayMethods(): string[]
 * ```
 */
inline std::vector<std::string> list_gateway_methods() {
    return {
        // Health
        "health",
        "doctor.memory.status",
        "logs.tail",
        "channels.status",
        "channels.logout",
        "status",
        // Usage
        "usage.status",
        "usage.cost",
        // TTS
        "tts.status",
        "tts.providers",
        "tts.enable",
        "tts.disable",
        "tts.convert",
        "tts.setProvider",
        // Config
        "config.get",
        "config.set",
        "config.apply",
        "config.patch",
        "config.schema",
        "config.schema.lookup",
        // Exec Approvals
        "exec.approvals.get",
        "exec.approvals.set",
        "exec.approvals.node.get",
        "exec.approvals.node.set",
        "exec.approval.request",
        "exec.approval.waitDecision",
        "exec.approval.resolve",
        // Wizard
        "wizard.start",
        "wizard.next",
        "wizard.cancel",
        "wizard.status",
        // Talk
        "talk.config",
        "talk.mode",
        // Models
        "models.list",
        // Tools
        "tools.catalog",
        // Agents
        "agents.list",
        "agents.create",
        "agents.update",
        "agents.delete",
        "agents.files.list",
        "agents.files.get",
        "agents.files.set",
        // Skills
        "skills.status",
        "skills.bins",
        "skills.install",
        "skills.update",
        // Update
        "update.run",
        // VoiceWake
        "voicewake.get",
        "voicewake.set",
        // Secrets
        "secrets.reload",
        "secrets.resolve",
        // Sessions
        "sessions.list",
        "sessions.preview",
        "sessions.patch",
        "sessions.reset",
        "sessions.delete",
        "sessions.compact",
        // Heartbeat
        "last-heartbeat",
        "set-heartbeats",
        // Cron
        "wake",
        // Node pairing
        "node.pair.request",
        "node.pair.list",
        "node.pair.approve",
        "node.pair.reject",
        "node.pair.verify",
        // Device pairing
        "device.pair.list",
        "device.pair.approve",
        "device.pair.reject",
        "device.pair.remove",
        "device.token.rotate",
        "device.token.revoke",
        // Node
        "node.rename",
        "node.list",
        "node.describe",
        "node.pending.drain",
        "node.pending.enqueue",
        "node.invoke",
        "node.pending.pull",
        "node.pending.ack",
        "node.invoke.result",
        "node.event",
        "node.canvas.capability.refresh",
        // Cron
        "cron.list",
        "cron.status",
        "cron.add",
        "cron.update",
        "cron.remove",
        "cron.run",
        "cron.runs",
        // System
        "gateway.identity.get",
        "system-presence",
        "system-event",
        // Send / Agent
        "send",
        "agent",
        "agent.identity.get",
        "agent.wait",
        // Browser
        "browser.request",
        // Chat (WebSocket-native)
        "chat.history",
        "chat.abort",
        "chat.send",
    };
}

// ============================================================================
// Gateway Events (严格对齐 OpenClaw server-methods-list.ts GATEWAY_EVENTS)
// ============================================================================

/**
 * @brief 返回所有 Gateway 支持的事件名列表
 *
 * 严格对齐 OpenClaw:
 * ```typescript
 * export const GATEWAY_EVENTS = [...]
 * ```
 */
inline std::vector<std::string> list_gateway_events() {
    return {
        "connect.challenge",
        "agent",
        "chat",
        "presence",
        "tick",
        "talk.mode",
        "shutdown",
        "health",
        "heartbeat",
        "cron",
        "node.pair.requested",
        "node.pair.resolved",
        "node.invoke.request",
        "device.pair.requested",
        "device.pair.resolved",
        "voicewake.changed",
        "exec.approval.requested",
        "exec.approval.resolved",
        "update.available",
    };
}

} // namespace gateway
} // namespace hunterclaw
