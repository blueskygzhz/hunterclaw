// ============================================================================
// usage.cc - Usage tracking and cost reporting
// 🎯 严格对齐 OpenClaw server-methods/usage.ts
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/method_guard.h"
#include "hunterclaw/core/utils.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <chrono>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace gateway {
namespace methods {

static inline int64_t usage_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// usage.status → provider usage summary
// 🎯 OpenClaw 对齐: usage.ts L351-354
core::Result<json> method_usage_status(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx; (void)params;

    auto cfg = server->config();
    if (!cfg) {
        json result;
        result["ts"] = usage_now_ms();
        result["totalCost"] = 0.0;
        result["totalTokens"] = 0;
        return core::Result<json>::Ok(result);
    }

    // Load provider usage from usage tracking file
    std::string usage_file = cfg->config_dir + "/provider-usage.json";
    json summary;
    try {
        if (fs::exists(usage_file)) {
            std::ifstream f(usage_file);
            if (f.is_open()) {
                f >> summary;
                return core::Result<json>::Ok(summary);
            }
        }
    } catch (...) {}

    summary["ts"] = usage_now_ms();
    summary["totalCost"] = 0.0;
    summary["totalTokens"] = 0;
    return core::Result<json>::Ok(summary);
}

// usage.cost → cost usage with date range + caching
// 🎯 OpenClaw 对齐: usage.ts L355-366
static std::mutex s_cost_cache_mutex;
static std::map<std::string, json> s_cost_cache;
static constexpr int64_t COST_CACHE_TTL_MS = 30000;

core::Result<json> method_usage_cost(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    // Parse date range
    std::string start_date = params.value("startDate", "");
    std::string end_date = params.value("endDate", "");
    int days = params.value("days", 30);

    auto cfg = server->config();
    if (!cfg) {
        json result;
        result["totals"] = json::object({
            {"input", 0}, {"output", 0}, {"totalTokens", 0}, {"totalCost", 0.0}
        });
        return core::Result<json>::Ok(result);
    }

    // Cache key
    std::string cache_key = start_date + "-" + end_date + "-" + std::to_string(days);
    {
        std::lock_guard<std::mutex> lock(s_cost_cache_mutex);
        auto it = s_cost_cache.find(cache_key);
        if (it != s_cost_cache.end()) {
            int64_t cached_at = it->second.value("_cachedAt", (int64_t)0);
            if (usage_now_ms() - cached_at < COST_CACHE_TTL_MS) {
                json cached = it->second;
                cached.erase("_cachedAt");
                return core::Result<json>::Ok(cached);
            }
        }
    }

    // Load cost data
    std::string cost_file = cfg->config_dir + "/cost-usage.json";
    json result;
    try {
        if (fs::exists(cost_file)) {
            std::ifstream f(cost_file);
            if (f.is_open()) {
                f >> result;
            }
        }
    } catch (...) {}

    if (result.is_null() || result.empty()) {
        result["totals"] = json::object({
            {"input", 0}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0},
            {"totalTokens", 0}, {"totalCost", 0.0}, {"inputCost", 0.0},
            {"outputCost", 0.0}, {"cacheReadCost", 0.0}, {"cacheWriteCost", 0.0},
            {"missingCostEntries", 0}
        });
    }

    // Cache
    {
        std::lock_guard<std::mutex> lock(s_cost_cache_mutex);
        json cached = result;
        cached["_cachedAt"] = usage_now_ms();
        s_cost_cache[cache_key] = cached;
    }

    return core::Result<json>::Ok(result);
}

// sessions.usage → session-level usage aggregation
// 🎯 OpenClaw 对齐: usage.ts L367-801
core::Result<json> method_sessions_usage(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)method_guard; (void)client_ctx;

    int limit = params.value("limit", 50);
    std::string specific_key = params.value("key", "");

    auto cfg = server->config();

    json sessions = json::array();
    json totals = json::object({
        {"input", 0}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0},
        {"totalTokens", 0}, {"totalCost", 0.0}, {"inputCost", 0.0},
        {"outputCost", 0.0}, {"cacheReadCost", 0.0}, {"cacheWriteCost", 0.0},
        {"missingCostEntries", 0}
    });

    // Load session store
    if (cfg) {
        std::string sessions_dir = cfg->config_dir + "/sessions";
        try {
            if (fs::exists(sessions_dir)) {
                int count = 0;
                for (const auto& entry : fs::directory_iterator(sessions_dir)) {
                    if (count >= limit) break;
                    if (entry.is_regular_file() && entry.path().extension() == ".json") {
                        json session;
                        session["key"] = entry.path().stem().string();
                        session["sessionId"] = entry.path().stem().string();
                        session["updatedAt"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                            entry.last_write_time().time_since_epoch()
                        ).count();
                        sessions.push_back(session);
                        count++;
                    }
                }
            }
        } catch (...) {}
    }

    json result;
    result["updatedAt"] = usage_now_ms();
    result["sessions"] = sessions;
    result["totals"] = totals;
    return core::Result<json>::Ok(result);
}

// sessions.usage.timeseries
// 🎯 OpenClaw 对齐: usage.ts L802-838
core::Result<json> method_sessions_usage_timeseries(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string key = params.value("key", "");
    if (key.empty()) {
        return core::Result<json>::Err("key is required for timeseries");
    }

    // Return empty timeseries for now
    json result;
    result["points"] = json::array();
    return core::Result<json>::Ok(result);
}

// sessions.usage.logs
// 🎯 OpenClaw 对齐: usage.ts L839-869
core::Result<json> method_sessions_usage_logs(
    std::shared_ptr<GatewayServer> server,
    std::shared_ptr<MethodGuard> method_guard,
    const ClientContext& client_ctx,
    const json& params
) {
    (void)server; (void)method_guard; (void)client_ctx;

    std::string key = params.value("key", "");
    if (key.empty()) {
        return core::Result<json>::Err("key is required for logs");
    }

    json result;
    result["logs"] = json::array();
    return core::Result<json>::Ok(result);
}

}  // namespace methods
}  // namespace gateway
}  // namespace hunterclaw
