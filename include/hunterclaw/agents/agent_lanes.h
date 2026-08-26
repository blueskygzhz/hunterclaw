#pragma once
// ============================================================================
// Agent Lanes / Channel 常量 — 严格对齐 OpenClaw
//   agents/lanes.ts + process/lanes.ts + utils/message-channel-constants.ts
// ============================================================================
// 刻意与 agent_step 分离（真源里它们本就是独立模块）：
// 这样纯函数部分可被测试与其它子系统直接引用，不必拖入 gateway/websocket。
// ============================================================================

#include <string>

namespace hunterclaw {
namespace agents {

/// 対齐 utils/message-channel-constants.ts:4 INTERNAL_MESSAGE_CHANNEL
/// ⚠ 真源取值是 "webchat"，**不是** "internal"。
/// 写错会让 gateway 按未知 channel 处理，渠道策略与回复投递全部走偏。
extern const std::string INTERNAL_MESSAGE_CHANNEL;

/// 対齐 process/lanes.ts:15 CommandLane.Nested
extern const std::string AGENT_LANE_NESTED;

/// 対齐 process/lanes.ts:6 CommandLane.CronNested
extern const std::string AGENT_LANE_CRON_NESTED;

/// 対齐 process/lanes.ts:5 CommandLane.Cron
extern const std::string AGENT_LANE_CRON;

/// 対齐 process/lanes.ts:14 CommandLane.Subagent
extern const std::string AGENT_LANE_SUBAGENT;

/// 対齐 agents/lanes.ts:12 NESTED_LANE_PREFIX
extern const std::string NESTED_LANE_PREFIX;

/// 対齐 agents/lanes.ts:26-32 resolveNestedAgentLaneForSession(sessionKey)
///
/// 返回 "nested:{sessionKey}" —— **per-session** 泳道，而非全局 "nested"。
/// 这一点很关键：全局单泳道会把所有会话的嵌套 agent 工作串行化，
/// A2A ping-pong 里两个会话互相等待就会**自我死锁**。
/// sessionKey trim 后为空时退回全局 "nested"（対齐:28-30）。
std::string resolve_nested_agent_lane_for_session(const std::string& session_key);

/// 対齐 agents/lanes.ts:15-23 resolveCronAgentLane(lane)
/// cron 任务已占用外层 cron 泳道，其内部 agent 工作需要独立泳道以免自我死锁。
std::string resolve_cron_agent_lane(const std::string& lane);

/// 対齐 agents/lanes.ts:35-40 isNestedAgentLane(lane)
bool is_nested_agent_lane(const std::string& lane);

}  // namespace agents
}  // namespace hunterclaw
