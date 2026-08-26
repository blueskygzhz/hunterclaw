// ============================================================================
// Agent Lanes / Channel 常量实现 — 严格对齐 OpenClaw
//   agents/lanes.ts + process/lanes.ts + utils/message-channel-constants.ts
// ============================================================================

#include "hunterclaw/agents/agent_lanes.h"

namespace hunterclaw {
namespace agents {

namespace {

bool is_space_ch(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// 対齐 JS String.prototype.trim()
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space_ch(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_space_ch(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // namespace

// 対齐 utils/message-channel-constants.ts:4
const std::string INTERNAL_MESSAGE_CHANNEL = "webchat";
// 対齐 process/lanes.ts:5,6,14,15
const std::string AGENT_LANE_CRON        = "cron";
const std::string AGENT_LANE_CRON_NESTED = "cron-nested";
const std::string AGENT_LANE_SUBAGENT    = "subagent";
const std::string AGENT_LANE_NESTED      = "nested";
// 対齐 agents/lanes.ts:12
const std::string NESTED_LANE_PREFIX     = "nested:";

std::string resolve_nested_agent_lane_for_session(const std::string& session_key) {
    // 対齐 agents/lanes.ts:27-31
    const std::string trimmed = trim(session_key);
    if (trimmed.empty()) return AGENT_LANE_NESTED;
    return NESTED_LANE_PREFIX + trimmed;
}

std::string resolve_cron_agent_lane(const std::string& lane) {
    // 対齐 agents/lanes.ts:16-22 —— 空或恰为外层 cron 泳道时改用 cron-nested，
    // 否则沿用调用方指定的泳道。
    const std::string trimmed = trim(lane);
    if (trimmed.empty() || trimmed == AGENT_LANE_CRON) return AGENT_LANE_CRON_NESTED;
    return trimmed;
}

bool is_nested_agent_lane(const std::string& lane) {
    // 対齐 agents/lanes.ts:36-39 —— 恰为 "nested" 或以 "nested:" 开头
    if (lane.empty()) return false;
    if (lane == AGENT_LANE_NESTED) return true;
    return lane.size() >= NESTED_LANE_PREFIX.size() &&
           lane.rfind(NESTED_LANE_PREFIX, 0) == 0;
}

}  // namespace agents
}  // namespace hunterclaw
