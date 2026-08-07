#pragma once

// 🎯 严格对齐 OpenClaw extensions/active-memory/escalation.ts
//   判定是否需要将 memory 召回升级到 lane-2（更重的 subagent 召回）：
//     - mode="off"→ 从不升级
//     - mode="always" → 总是升级
//     - mode="escalate"→ lane-1 未强命中且消息含"回忆意图"时升级
//   has_recall_intent 覆盖英/中/日/韩/西多语言召回意图正则集。

#include <string>

namespace hunterclaw {
namespace memory {

// 对齐 ActiveMemoryMode ("escalate" | "always" | "off")。
enum class ActiveMemoryMode {
    Escalate,
    Always,
    Off,
};

// 对齐 ActiveMemoryMode 字符串解析（未知值→Escalate，与 OC 默认一致）。
ActiveMemoryMode parse_active_memory_mode(const std::string& value);

// 对齐 hasRecallIntent: 归一化空白后，命中任一英文召回意图正则，
//   或命中某语言的 intent 且（无 future 或含 retrospective）→ true。
bool has_recall_intent(const std::string& message);

// 对齐 shouldEscalateRecall。
bool should_escalate_recall(
    ActiveMemoryMode mode,
    const std::string& message,
    bool has_strong_lane_one_hit);

} // namespace memory
} // namespace hunterclaw
