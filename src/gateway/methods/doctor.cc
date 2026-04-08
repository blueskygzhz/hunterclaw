// ============================================================================
// doctor.cc - Diagnostic and health introspection
// 🎯 严格对齐 OpenClaw server-methods/doctor.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t doctor_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// doctor.memory.status
core::Result<json> method_doctor_memory_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx; (void)params;

    json result;
    result["ok"] = true;
    result["ts"] = doctor_now_ms();

    // Memory info
    // In production, would read /proc/self/status or use getrusage
    result["heapUsedMB"] = 0;
    result["heapTotalMB"] = 0;
    result["rssMB"] = 0;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
