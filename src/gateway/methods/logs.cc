// ============================================================================
// logs.cc - Log tailing
// 🎯 严格对齐 OpenClaw server-methods/logs.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t logs_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// logs.tail
core::Result<json> method_logs_tail(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    int limit = params.value("limit", 100);
    std::string filter = params.value("filter", "");

    auto cfg = server->config();
    json lines = json::array();

    if (cfg) {
        std::string log_file = cfg->config_dir + "/gateway.log";
        try {
            if (fs::exists(log_file)) {
                std::ifstream f(log_file);
                std::vector<std::string> all_lines;
                std::string line;
                while (std::getline(f, line)) {
                    if (!filter.empty() && line.find(filter) == std::string::npos) continue;
                    all_lines.push_back(line);
                }
                // Take last N lines
                int start = std::max(0, (int)all_lines.size() - limit);
                for (int i = start; i < (int)all_lines.size(); i++) {
                    lines.push_back(all_lines[i]);
                }
            }
        } catch (...) {}
    }

    json result;
    result["lines"] = lines;
    result["ts"] = logs_now_ms();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
