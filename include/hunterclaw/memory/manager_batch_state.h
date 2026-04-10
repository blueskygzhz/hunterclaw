#pragma once

// ============================================================================
// Manager Batch State — 严格对齐 OpenClaw manager-batch-state.ts (45 行)
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace memory {

constexpr int MEMORY_BATCH_FAILURE_LIMIT = 2;

struct MemoryBatchFailureState {
    bool enabled = true;
    int count = 0;
    std::optional<std::string> last_error;
    std::optional<std::string> last_provider;
};

MemoryBatchFailureState reset_memory_batch_failure_state(const MemoryBatchFailureState& state);

struct RecordBatchFailureParams {
    std::string provider;
    std::string message;
    int attempts = 1;
    bool force_disable = false;
};

MemoryBatchFailureState record_memory_batch_failure(
    const MemoryBatchFailureState& state,
    const RecordBatchFailureParams& params
);

}  // namespace memory
}  // namespace hunterclaw
