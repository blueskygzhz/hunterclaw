#pragma once

// ============================================================================
// Manager Sync Control — 严格对齐 OpenClaw manager-sync-control.ts (194 行)
//
// 同步控制逻辑：readonly recovery、targeted session sync queue。
// ============================================================================

#include "hunterclaw/memory/types.h"
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <mutex>
#include <atomic>

namespace hunterclaw {
namespace memory {

/**
 * Check if an error is a SQLite readonly database error.
 * (对齐 manager-sync-control.ts:37-68 isMemoryReadonlyDbError)
 */
bool is_memory_readonly_db_error(const std::string& error_message);

/**
 * Extract a human-readable reason from an error.
 * (对齐 manager-sync-control.ts:70-84 extractMemoryErrorReason)
 */
std::string extract_memory_error_reason(const std::exception& err);

// ============================================================================
// Readonly Recovery State (对齐 manager-sync-control.ts:11-35)
// ============================================================================
struct ReadonlyRecoveryStats {
    std::atomic<int> attempts{0};
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::string last_error;
    std::mutex mutex;
};

/**
 * Targeted session sync queue.
 * (对齐 manager-sync-control.ts:126-172 enqueueMemoryTargetedSessionSync)
 */
class MemoryTargetedSessionSyncQueue {
public:
    void enqueue(const std::vector<std::string>& session_files);
    std::vector<std::string> drain();
    bool empty() const;
    void clear();

private:
    std::set<std::string> queued_;
    mutable std::mutex mutex_;
};

}  // namespace memory
}  // namespace hunterclaw
