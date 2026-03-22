#pragma once

// ============================================================================
// Agent Limits (严格对齐 OpenClaw src/config/agent-limits.ts)
// ============================================================================
// 对齐文件: config/agent-limits.ts
//   - DEFAULT_AGENT_MAX_CONCURRENT = 4
//   - DEFAULT_SUBAGENT_MAX_CONCURRENT = 8
//   - DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH = 1
//   - resolveAgentMaxConcurrent(cfg?)
//   - resolveSubagentMaxConcurrent(cfg?)
// ============================================================================

#include <algorithm>
#include <cmath>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace config {

// 对齐 OpenClaw: DEFAULT_AGENT_MAX_CONCURRENT = 4
constexpr int DEFAULT_AGENT_MAX_CONCURRENT = 4;

// 对齐 OpenClaw: DEFAULT_SUBAGENT_MAX_CONCURRENT = 8
constexpr int DEFAULT_SUBAGENT_MAX_CONCURRENT = 8;

// 对齐 OpenClaw: DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH = 1
// Keep depth-1 subagents as leaves unless config explicitly opts into nesting.
constexpr int DEFAULT_SUBAGENT_MAX_SPAWN_DEPTH = 1;

// 对齐 OpenClaw: resolveAgentMaxConcurrent(cfg?)
// 从 cfg.agents.defaults.maxConcurrent 读取，不合法则返回默认值 4
inline int resolve_agent_max_concurrent(const nlohmann::json& cfg) {
    try {
        if (cfg.contains("agents") && cfg["agents"].contains("defaults")) {
            const auto& defaults = cfg["agents"]["defaults"];
            if (defaults.contains("maxConcurrent") && defaults["maxConcurrent"].is_number()) {
                double raw = defaults["maxConcurrent"].get<double>();
                if (std::isfinite(raw)) {
                    return std::max(1, static_cast<int>(std::floor(raw)));
                }
            }
        }
    } catch (...) {}
    return DEFAULT_AGENT_MAX_CONCURRENT;
}

// 对齐 OpenClaw: resolveSubagentMaxConcurrent(cfg?)
// 从 cfg.agents.defaults.subagents.maxConcurrent 读取，不合法则返回默认值 8
inline int resolve_subagent_max_concurrent(const nlohmann::json& cfg) {
    try {
        if (cfg.contains("agents") && cfg["agents"].contains("defaults")) {
            const auto& defaults = cfg["agents"]["defaults"];
            if (defaults.contains("subagents") && defaults["subagents"].contains("maxConcurrent")) {
                const auto& raw_val = defaults["subagents"]["maxConcurrent"];
                if (raw_val.is_number()) {
                    double raw = raw_val.get<double>();
                    if (std::isfinite(raw)) {
                        return std::max(1, static_cast<int>(std::floor(raw)));
                    }
                }
            }
        }
    } catch (...) {}
    return DEFAULT_SUBAGENT_MAX_CONCURRENT;
}

}  // namespace config
}  // namespace hunterclaw
