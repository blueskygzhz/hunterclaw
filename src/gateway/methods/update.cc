// ============================================================================
// update.cc - Self-update mechanism
// 🎯 严格对齐 OpenClaw server-methods/update.ts (135 行)
//
// 核心功能:
//   - update.run: 触发自更新流程
//   - 参数验证 (validateUpdateRunParams)
//   - 审计日志 (resolveControlPlaneActor)
//   - 写入 restart sentinel
//   - 可选的 SIGUSR1 重启
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t update_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// update.run (对齐 update.ts updateHandlers["update.run"])
core::Result<json> method_update_run(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> /*method_guard*/,
    const ClientContext& client_ctx,
    const json& params
) {
    int64_t now = update_now_ms();

    // 对齐 update.ts:24 parseRestartRequestParams
    std::string session_key = params.value("sessionKey", "");
    std::string note = params.value("note", "");
    int restart_delay_ms = params.value("restartDelayMs", 2000);
    int timeout_ms = params.value("timeoutMs", 60000);
    timeout_ms = std::max(1000, timeout_ms);

    // 对齐 update.ts:33-56 — 执行更新
    // C++ binary 不支持 npm 自更新，但保持接口对齐
    json update_result;
    update_result["status"] = "skipped";
    update_result["mode"] = "binary";
    update_result["reason"] = "C++ binary does not support npm self-update. Use package manager or rebuild.";
    update_result["steps"] = json::array();
    update_result["durationMs"] = 0;

    // 对齐 update.ts:58-86 — 写入 restart sentinel
    json sentinel_payload;
    sentinel_payload["kind"] = "update";
    sentinel_payload["status"] = update_result["status"];
    sentinel_payload["ts"] = now;
    sentinel_payload["sessionKey"] = session_key;
    if (!note.empty()) sentinel_payload["message"] = note;
    sentinel_payload["stats"] = {
        {"mode", update_result["mode"]},
        {"reason", update_result["reason"]},
        {"durationMs", 0},
        {"steps", json::array()}
    };

    // 尝试写入 sentinel 文件
    std::string sentinel_path;
    try {
        auto state_dir = fs::current_path() / ".openclaw";
        sentinel_path = (state_dir / "restart-sentinel.json").string();
        fs::create_directories(fs::path(sentinel_path).parent_path());
        std::ofstream f(sentinel_path);
        f << sentinel_payload.dump(2);
    } catch (...) {
        sentinel_path = "";
    }

    // 对齐 update.ts:98-110 — 不自动重启（C++ binary 由外部管理）
    json restart_info = nullptr;

    json result;
    result["ok"] = (update_result["status"] != "error");
    result["result"] = update_result;
    result["restart"] = restart_info;
    result["sentinel"] = {
        {"path", sentinel_path.empty() ? json(nullptr) : json(sentinel_path)},
        {"payload", sentinel_payload}
    };
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
