#pragma once

// ============================================================================
// Gateway Lane Concurrency (严格对齐 OpenClaw src/gateway/server-lanes.ts)
// ============================================================================
// 对齐文件: gateway/server-lanes.ts
//   applyGatewayLaneConcurrency(cfg):
//     setCommandLaneConcurrency(CommandLane.Cron,     cfg.cron?.maxConcurrentRuns ?? 1)
//     setCommandLaneConcurrency(CommandLane.Main,     resolveAgentMaxConcurrent(cfg))
//     setCommandLaneConcurrency(CommandLane.Subagent, resolveSubagentMaxConcurrent(cfg))
// ============================================================================

#include "hunterclaw/config/agent_limits.h"
#include "hunterclaw/queue/command_queue.h"
#include "hunterclaw/queue/lanes.h"
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

// 对齐 OpenClaw: applyGatewayLaneConcurrency(cfg)
// 在 gateway 启动时调用，从配置文件读取并设置各 lane 的并发数
inline void apply_gateway_lane_concurrency(const nlohmann::json& cfg) {
    auto& queue = queue::CommandQueue::Instance();

    // 对齐 OpenClaw: setCommandLaneConcurrency(CommandLane.Cron, cfg.cron?.maxConcurrentRuns ?? 1)
    int cron_concurrent = 1;
    try {
        if (cfg.contains("cron") && cfg["cron"].contains("maxConcurrentRuns")) {
            const auto& val = cfg["cron"]["maxConcurrentRuns"];
            if (val.is_number()) {
                cron_concurrent = std::max(1, val.get<int>());
            }
        }
    } catch (...) {}
    queue.set_command_lane_concurrency(queue::CommandLane::Cron, static_cast<size_t>(cron_concurrent));

    // 对齐 OpenClaw: setCommandLaneConcurrency(CommandLane.Main, resolveAgentMaxConcurrent(cfg))
    int main_concurrent = config::resolve_agent_max_concurrent(cfg);
    queue.set_command_lane_concurrency(queue::CommandLane::Main, static_cast<size_t>(main_concurrent));

    // 对齐 OpenClaw: setCommandLaneConcurrency(CommandLane.Subagent, resolveSubagentMaxConcurrent(cfg))
    int subagent_concurrent = config::resolve_subagent_max_concurrent(cfg);
    queue.set_command_lane_concurrency(queue::CommandLane::Subagent, static_cast<size_t>(subagent_concurrent));
}

}  // namespace gateway
}  // namespace hunterclaw
