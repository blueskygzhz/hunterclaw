#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>

namespace hunterclaw {
namespace memory {

using json = nlohmann::json;

// ============================================================================
// Memory Host Events — 严格对齐 OpenClaw memory-host-sdk/events.ts
//
// OpenClaw 的记忆事件系统通过 JSONL 日志文件记录，而非 WebSocket 实时推送。
// 事件类型:
//   - memory.recall.recorded: 搜索召回信号
//   - memory.promotion.applied: 短期记忆晋升
//   - memory.dreaming.phase: 梦境阶段完成
//
// OpenClaw 中没有:
//   - memory.add / memory.update / memory.delete (无 CRUD 事件)
//   - EventEmittingMemoryProvider (无此装饰器)
//   - MemoryProvider 基类 (唯一接口是 MemorySearchManager)
// ============================================================================

// Event log relative path (对齐 events.ts:5)
inline constexpr const char* MEMORY_HOST_EVENT_LOG_RELATIVE_PATH = "memory/.dreams/events.jsonl";

// ============================================================================
// Event types (对齐 events.ts:7-68)
// ============================================================================

struct MemoryHostRecallRecordedEvent {
    std::string type = "memory.recall.recorded";
    std::string timestamp;
    std::string query;
    int result_count = 0;
    struct ResultEntry {
        std::string path;
        int start_line = 0;
        int end_line = 0;
        double score = 0.0;
    };
    std::vector<ResultEntry> results;
};

struct MemoryHostPromotionAppliedEvent {
    std::string type = "memory.promotion.applied";
    std::string timestamp;
    std::string memory_path;
    int applied = 0;
    struct CandidateEntry {
        std::string key;
        std::string path;
        int start_line = 0;
        int end_line = 0;
        double score = 0.0;
    };
    std::vector<CandidateEntry> candidates;
};

struct MemoryHostDreamingPhaseEvent {
    std::string type = "memory.dreaming.phase";
    std::string timestamp;
    std::string phase;  // "light" | "rem" | "deep"
    std::string workspace_dir;
    int processed = 0;
    int promoted = 0;
};

// Union of all event types
using MemoryHostEvent = std::variant<
    MemoryHostRecallRecordedEvent,
    MemoryHostPromotionAppliedEvent,
    MemoryHostDreamingPhaseEvent
>;

// ============================================================================
// Event log I/O (对齐 events.ts:70-94)
// ============================================================================

/**
 * Append a memory host event to the JSONL event log.
 * (对齐 events.ts:appendMemoryHostEvent)
 */
core::Result<void> append_memory_host_event(
    const std::string& workspace_dir,
    const json& event_json
);

/**
 * Read all memory host events from the JSONL event log.
 * (对齐 events.ts:readMemoryHostEvents)
 */
core::Result<std::vector<json>> read_memory_host_events(
    const std::string& workspace_dir
);

}  // namespace memory
}  // namespace hunterclaw
