// ============================================================================
// model_pricing_cache.cc
// 🎯 严格对齐 OpenClaw: src/gateway/model-pricing-cache-state.ts (111 行)
//
// 模型定价缓存系统
// - 模块级单例 Map 存储模型定价
// - 支持 provider/model key 格式的直接查找和标准化查找
// - 支持 wrapper providers (openrouter, cloudflare 等) 的别名解析
// ============================================================================

#include "hunterclaw/gateway/gateway.h"
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <algorithm>
#include <chrono>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ── Types ────────────────────────────────────────────────────────────────────

// 🎯 对齐: CachedModelPricing
struct CachedModelPricing {
    double input = 0;       // price per million input tokens
    double output = 0;      // price per million output tokens
    double cache_read = 0;  // price per million cache read tokens
    double cache_write = 0; // price per million cache write tokens
};

// ── Module-level cache state ────────────────────────────────────────────────
// 🎯 对齐: let cachedPricing, let cachedAt — 全局可变缓存

namespace {

std::mutex g_pricing_mutex;
std::map<std::string, CachedModelPricing> g_cached_pricing;
int64_t g_cached_at = 0;

// 🎯 对齐: WRAPPER_PROVIDERS
const std::set<std::string> WRAPPER_PROVIDERS = {
    "cloudflare-ai-gateway",
    "kilocode",
    "openrouter",
    "vercel-ai-gateway",
};

} // anonymous namespace

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string normalize_lowercase(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    size_t start = r.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = r.find_last_not_of(" \t\n\r");
    return r.substr(start, end - start + 1);
}

// 🎯 对齐: modelPricingCacheKey(provider, model)
// Key format: "providerId/modelId"
// If model already starts with "providerId/", don't double-prefix
static std::string model_pricing_cache_key(
    const std::string& provider, const std::string& model) {

    std::string provider_id = normalize_lowercase(provider);
    std::string model_id = model;
    // Trim model
    size_t ms = model_id.find_first_not_of(" \t");
    if (ms == std::string::npos) return "";
    model_id = model_id.substr(ms);
    size_t me = model_id.find_last_not_of(" \t");
    model_id = model_id.substr(0, me + 1);

    if (provider_id.empty() || model_id.empty()) return "";

    std::string model_lower = normalize_lowercase(model_id);
    std::string prefix = provider_id + "/";
    if (model_lower.rfind(prefix, 0) == 0) {
        return model_id;
    }
    return provider_id + "/" + model_id;
}

// 🎯 对齐: shouldNormalizeCachedPricingLookup(provider)
static bool should_normalize_cached_pricing_lookup(const std::string& provider) {
    std::string normalized = normalize_lowercase(provider);
    return normalized == "anthropic" ||
           normalized == "openrouter" ||
           normalized == "xai" ||
           WRAPPER_PROVIDERS.count(normalized) > 0;
}

// ── replaceGatewayModelPricingCache ─────────────────────────────────────────
// 🎯 对齐: replaceGatewayModelPricingCache(nextPricing, nextCachedAt)

void replace_gateway_model_pricing_cache(
    const std::map<std::string, CachedModelPricing>& next_pricing,
    int64_t next_cached_at) {
    std::lock_guard<std::mutex> lock(g_pricing_mutex);
    g_cached_pricing = next_pricing;
    g_cached_at = next_cached_at > 0 ? next_cached_at :
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── clearGatewayModelPricingCacheState ──────────────────────────────────────
// 🎯 对齐: clearGatewayModelPricingCacheState()

void clear_gateway_model_pricing_cache_state() {
    std::lock_guard<std::mutex> lock(g_pricing_mutex);
    g_cached_pricing.clear();
    g_cached_at = 0;
}

// ── getCachedGatewayModelPricing ────────────────────────────────────────────
// 🎯 对齐: getCachedGatewayModelPricing({ provider, model })
// 先直接匹配，失败后对特定 provider 尝试标准化查找

std::optional<CachedModelPricing> get_cached_gateway_model_pricing(
    const std::string& provider, const std::string& model) {

    std::string p = provider;
    std::string m = model;
    // Trim
    size_t ps = p.find_first_not_of(" \t");
    if (ps == std::string::npos) return std::nullopt;
    p = p.substr(ps);
    size_t pe = p.find_last_not_of(" \t");
    p = p.substr(0, pe + 1);

    size_t ms = m.find_first_not_of(" \t");
    if (ms == std::string::npos) return std::nullopt;
    m = m.substr(ms);
    size_t me = m.find_last_not_of(" \t");
    m = m.substr(0, me + 1);

    if (p.empty() || m.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(g_pricing_mutex);

    // Direct lookup
    std::string key = model_pricing_cache_key(p, m);
    if (!key.empty()) {
        auto it = g_cached_pricing.find(key);
        if (it != g_cached_pricing.end()) {
            return it->second;
        }
    }

    // Normalized lookup for known providers
    if (!should_normalize_cached_pricing_lookup(p)) {
        return std::nullopt;
    }

    // Try with normalized provider/model
    std::string norm_p = normalize_lowercase(p);
    std::string norm_m = normalize_lowercase(m);
    std::string norm_key = model_pricing_cache_key(norm_p, norm_m);
    if (!norm_key.empty()) {
        auto it = g_cached_pricing.find(norm_key);
        if (it != g_cached_pricing.end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

// ── getGatewayModelPricingCacheMeta ─────────────────────────────────────────
// 🎯 对齐: getGatewayModelPricingCacheMeta()

json get_gateway_model_pricing_cache_meta() {
    std::lock_guard<std::mutex> lock(g_pricing_mutex);
    json j;
    j["cachedAt"] = g_cached_at;
    j["ttlMs"] = 0;
    j["size"] = static_cast<int>(g_cached_pricing.size());
    return j;
}

// ── JSON serialization ──────────────────────────────────────────────────────

json cached_model_pricing_to_json(const CachedModelPricing& p) {
    json j;
    j["input"] = p.input;
    j["output"] = p.output;
    j["cacheRead"] = p.cache_read;
    j["cacheWrite"] = p.cache_write;
    return j;
}

CachedModelPricing cached_model_pricing_from_json(const json& j) {
    CachedModelPricing p;
    p.input = j.value("input", 0.0);
    p.output = j.value("output", 0.0);
    p.cache_read = j.value("cacheRead", 0.0);
    p.cache_write = j.value("cacheWrite", 0.0);
    return p;
}

}  // namespace gateway
}  // namespace hunterclaw
