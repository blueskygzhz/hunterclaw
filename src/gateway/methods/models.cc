// ============================================================================
// models.cc - Model catalog management
// 🎯 严格对齐 OpenClaw server-methods/models.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

// models.list → return available model catalog
// 🎯 OpenClaw 对齐: models.ts L12-39
core::Result<json> method_models_list(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto cfg = server->config();
    json models = json::array();

    // Load model catalog from config
    if (cfg) {
        std::string catalog_file = cfg->config_dir + "/model-catalog.json";
        try {
            if (fs::exists(catalog_file)) {
                std::ifstream f(catalog_file);
                json catalog;
                if (f.is_open()) {
                    f >> catalog;
                    if (catalog.is_array()) models = catalog;
                }
            }
        } catch (...) {}
    }

    // If no catalog found, return built-in defaults
    if (models.empty()) {
        models = json::array({
            json::object({{"id", "claude-3-5-sonnet"}, {"provider", "anthropic"}, {"label", "Claude 3.5 Sonnet"}}),
            json::object({{"id", "gpt-4o"}, {"provider", "openai"}, {"label", "GPT-4o"}}),
            json::object({{"id", "deepseek-chat"}, {"provider", "deepseek"}, {"label", "DeepSeek Chat"}}),
        });
    }

    json result;
    result["models"] = models;
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
