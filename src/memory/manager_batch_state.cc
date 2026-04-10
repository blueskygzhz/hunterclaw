#include "hunterclaw/memory/manager_batch_state.h"
#include <algorithm>

// 严格对齐 OpenClaw memory/manager-batch-state.ts (45 行)

namespace hunterclaw {
namespace memory {

MemoryBatchFailureState reset_memory_batch_failure_state(const MemoryBatchFailureState& state) {
    return {state.enabled, 0, std::nullopt, std::nullopt};
}

MemoryBatchFailureState record_memory_batch_failure(
    const MemoryBatchFailureState& state,
    const RecordBatchFailureParams& params
) {
    if (!state.enabled) return state;
    int increment = params.force_disable
        ? MEMORY_BATCH_FAILURE_LIMIT
        : std::max(1, params.attempts);
    int count = state.count + increment;
    bool enabled = !(params.force_disable || count >= MEMORY_BATCH_FAILURE_LIMIT);
    return {enabled, count, params.message, params.provider};
}

}  // namespace memory
}  // namespace hunterclaw
