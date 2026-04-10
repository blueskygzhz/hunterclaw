#pragma once

#include <functional>
#include <string>

// 严格对齐 OpenClaw src/memory/manager-async-state.ts (31行)

namespace hunterclaw {
namespace memory {

// ============================================================================
// startAsyncSearchSync (对齐 manager-async-state.ts:1-14)
// ============================================================================
struct AsyncSearchSyncParams {
    bool enabled = false;
    bool dirty = false;
    bool sessions_dirty = false;
    std::function<void(const std::string&)> sync;
    std::function<void(const std::string&)> on_error;
};

void start_async_search_sync(const AsyncSearchSyncParams& params);

// ============================================================================
// awaitPendingManagerWork (对齐 manager-async-state.ts:16-30)
// C++ 中简化为同步等待，因为没有 Promise
// ============================================================================
void await_pending_manager_work(bool has_pending_sync, bool has_pending_provider_init);

} // namespace memory
} // namespace hunterclaw
