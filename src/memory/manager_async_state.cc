#include "hunterclaw/memory/manager_async_state.h"

namespace hunterclaw {
namespace memory {

// ============================================================================
// startAsyncSearchSync (对齐 manager-async-state.ts:1-14)
// ============================================================================
void start_async_search_sync(const AsyncSearchSyncParams& params) {
    if (!params.enabled || (!params.dirty && !params.sessions_dirty)) {
        return;
    }
    // 对齐 openclaw: void params.sync({ reason: "search" }).catch(...)
    // C++ 中同步调用，错误由调用方处理
    try {
        if (params.sync) {
            params.sync("search");
        }
    } catch (const std::exception& e) {
        if (params.on_error) {
            params.on_error(e.what());
        }
    }
}

// ============================================================================
// awaitPendingManagerWork (对齐 manager-async-state.ts:16-30)
// ============================================================================
void await_pending_manager_work(bool /*has_pending_sync*/, bool /*has_pending_provider_init*/) {
    // C++ 同步模型中无需等待 — pending work 在调用线程中已完成
    // 保留接口对齐 OpenClaw
}

} // namespace memory
} // namespace hunterclaw
