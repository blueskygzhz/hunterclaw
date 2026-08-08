// 🎯 严格对齐 OpenClaw: src/plugin-sdk/provider-catalog-live-runtime.ts
//                + provider-catalog-live-normalize.internal.ts
// Live Model Catalog 发现器实现。

#include "hunterclaw/llm/model_discovery.h"

#include "hunterclaw/security/ssrf_guard.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace hunterclaw {
namespace llm {

using json = nlohmann::json;

namespace {

auto log() {
    static auto logger = spdlog::get("model-discovery");
    if (!logger) logger = spdlog::stdout_logger_mt("model-discovery");
    return logger;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ── TTL 缓存 (对齐 getCachedLiveProviderModelRows) ───────────────────────────
struct CacheEntry {
    int64_t expires_at_ms = 0;
    std::vector<context::ModelEntry> models;
};
std::mutex g_cache_mutex;
std::unordered_map<std::string, CacheEntry> g_cache;

// 🎯 对齐 isSafeLiveModelId: 拒绝可疑/危险的 model id
bool is_safe_model_id(const std::string& id) {
    if (id.empty() || id.size() > 256) return false;
    for (char c : id) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) return false;  // 控制字符
        if (c == '/' || c == '\\' || c == '?' || c == '#' || c == ' ') return false;
    }
    return true;
}

// 🎯 对齐 readLiveModelPositiveInteger: 从若干候选 key 读第一个正整数
std::optional<int> read_positive_int(const json& record,
                                     const std::vector<std::string>& keys) {
    if (!record.is_object()) return std::nullopt;
    for (const auto& key : keys) {
        auto it = record.find(key);
        if (it == record.end()) continue;
        if (it->is_number_integer() || it->is_number_unsigned()) {
            int v = it->get<int>();
            if (v > 0) return v;
        } else if (it->is_number_float()) {
            double d = it->get<double>();
            if (d > 0) return static_cast<int>(d);
        } else if (it->is_string()) {
            try {
                int v = std::stoi(it->get<std::string>());
                if (v > 0) return v;
            } catch (...) {}
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_string(const json& record,
                                       const std::vector<std::string>& keys) {
    if (!record.is_object()) return std::nullopt;
    for (const auto& key : keys) {
        auto it = record.find(key);
        if (it != record.end() && it->is_string()) {
            std::string s = it->get<std::string>();
            // trim
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            s = s.substr(a, b - a + 1);
            if (!s.empty()) return s;
        }
    }
    return std::nullopt;
}

// 🎯 对齐 readDefaultLiveModelCatalogRows: 支持 [...] 或 { data: [...] } 或 { models: [...] }
const json* read_rows(const json& body, LiveCatalogProviderKind kind) {
    if (body.is_array()) return &body;
    if (body.is_object()) {
        if (kind == LiveCatalogProviderKind::Google) {
            auto it = body.find("models");
            if (it != body.end() && it->is_array()) return &(*it);
        }
        auto it = body.find("data");
        if (it != body.end() && it->is_array()) return &(*it);
        it = body.find("models");
        if (it != body.end() && it->is_array()) return &(*it);
    }
    return nullptr;
}

// 从嵌套记录数组里查正整数 (对齐 [record, topProvider, capabilities, modelInfo])
std::optional<int> read_positive_int_nested(const std::vector<const json*>& records,
                                            const std::vector<std::string>& keys) {
    for (const json* r : records) {
        if (!r) continue;
        auto v = read_positive_int(*r, keys);
        if (v) return v;
    }
    return std::nullopt;
}

const json* find_object(const json& record, const std::string& key) {
    if (!record.is_object()) return nullptr;
    auto it = record.find(key);
    if (it != record.end() && it->is_object()) return &(*it);
    return nullptr;
}

// 🎯 对齐 buildOpenAICompatibleLiveModel / OpenRouter / Google的 contextWindow 解析
std::optional<context::ModelEntry> parse_row(const json& row,
                                             LiveCatalogProviderKind kind) {
    if (!row.is_object()) return std::nullopt;

    // model id: Google 用 "name" (models/xxx), 其余用 id/model/model_name
    std::string id;
    if (kind == LiveCatalogProviderKind::Google) {
        auto n = read_string(row, {"name", "baseModelId", "displayName"});
        if (!n) return std::nullopt;
        // strip "models/" 前缀
        std::string s = *n;
        auto slash = s.rfind('/');
        if (slash != std::string::npos) s = s.substr(slash + 1);
        id = s;
    } else {
        auto i = read_string(row, {"id", "model", "model_name", "modelName"});
        if (!i) return std::nullopt;
        id = *i;
    }
    if (!is_safe_model_id(id)) return std::nullopt;

    // active/enabled==false 或 archived/deprecated==true → 跳过
    if (row.contains("active") && row["active"].is_boolean() && !row["active"].get<bool>())
        return std::nullopt;
    if (row.contains("deprecated") && row["deprecated"].is_boolean() && row["deprecated"].get<bool>())
        return std::nullopt;

    std::optional<int> context_window;

    if (kind == LiveCatalogProviderKind::Google) {
        // Gemini: inputTokenLimit
        context_window = read_positive_int(row, {"inputTokenLimit", "input_token_limit"});
    } else if (kind == LiveCatalogProviderKind::OpenRouter) {
        // OpenRouter: 优先 top_provider.context_length, 回退 context_length
        const json* top = find_object(row, "top_provider");
        if (top) context_window = read_positive_int(*top, {"context_length", "max_context_length"});
        if (!context_window)
            context_window = read_positive_int(row, {"context_length", "context_window"});
    } else {
        // OpenAI-compatible / Anthropic: 多字段候选
        const json* capabilities = find_object(row, "capabilities");
        const json* top_provider = find_object(row, "top_provider");
        const json* model_info = find_object(row, "model_info");
        context_window = read_positive_int_nested(
            {&row, top_provider, capabilities, model_info},
            {
                "context_window", "contextWindow",
                "context_length", "contextLength",
                "context_size", "contextSize",
                "max_context_length", "maxModelLen", "max_model_len",
                // Anthropic 用输入侧命名, 放最后
                "max_input_tokens", "maxInputTokens",
            });
    }

    if (!context_window || *context_window <= 0) {
        // 无context 元数据的行不产生发现值(保留静态兜底)
        return std::nullopt;
    }

    context::ModelEntry entry;
    entry.id = id;
    entry.context_window = context_window;
    return entry;
}

std::string trim_trailing_slash(const std::string& s) {
    std::string out = s;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}
std::string trim_leading_slash(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == '/') ++i;
    return s.substr(i);
}

std::string resolve_endpoint(const std::string& base_url, const std::string& path) {
    return trim_trailing_slash(base_url) + "/" + trim_leading_slash(path);
}

std::string cache_key(const LiveModelDiscoveryParams& p) {
    // key = provider + endpoint + auth (对齐 cacheKeyParts)
    std::string auth = !p.discovery_api_key.empty() ? p.discovery_api_key : p.api_key;
    return p.provider_id + "|" + resolve_endpoint(p.base_url, p.endpoint_path) + "|" + auth;
}

}  // namespace

ModelDiscovery::ModelDiscovery(std::shared_ptr<infra::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

void ModelDiscovery::clear_cache_for_test() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.clear();
}

LiveModelDiscoveryResult ModelDiscovery::discover(const LiveModelDiscoveryParams& params) {
    LiveModelDiscoveryResult result;

    const std::string endpoint = resolve_endpoint(params.base_url, params.endpoint_path);
    const std::string key = cache_key(params);

    // ── TTL 缓存命中 ─────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.expires_at_ms > now_ms()) {
            result.ok = true;
            result.from_cache = true;
            result.models = it->second.models;
            return result;
        }
    }

    if (!http_client_) {
        result.error = "no http client";
        return result;
    }

    // ── SSRF 校验 (对齐 fetchWithSsrfGuard) ─────────────────────────────────
    auto ssrf = security::get_global_ssrf_guard().check_url(endpoint);
    if (!ssrf.allowed) {
        result.error = "SSRF check blocked discovery endpoint: " + ssrf.reason;
        log()->warn("[ModelDiscovery] {} blocked: {}", endpoint, ssrf.reason);
        return result;
    }

    // require_https: 拒绝非 https (对齐 requireHttps)
    if (params.require_https && endpoint.rfind("https://", 0) != 0) {
        result.error = "discovery endpoint must be https";
        return result;
    }

    // ── HTTP GET (带 TLS 校验 + Bearer + Accept) ─────────────────────────────
    infra::HttpRequestOptions opts;
    opts.timeout_ms = params.timeout_ms;
    opts.ssl_verify = true;
    opts.headers["Accept"] = "application/json";
    std::string auth = !params.discovery_api_key.empty() ? params.discovery_api_key
                                                          : params.api_key;
    if (!auth.empty()) {
        opts.headers["Authorization"] = "Bearer " + auth;
    }

    auto resp = http_client_->get(endpoint, opts);
    if (resp.is_err()) {
        result.error = "http error: " + resp.error;
        return result;
    }
    const auto& http = resp.get();
    if (!http.ok) {
        result.error = params.provider_id + " model discovery failed: HTTP " +
                       std::to_string(http.status_code);
        return result;
    }

    // 🎯 4MB body 上限 (对齐 LIVE_MODEL_CATALOG_BODY_MAX_BYTES)
    if (static_cast<int>(http.body.size()) > LIVE_MODEL_CATALOG_BODY_MAX_BYTES) {
        result.error = "live model catalog response exceeded 4MB";
        return result;
    }

    json body;
    try {
        body = json::parse(http.body);
    } catch (const std::exception& e) {
        result.error = std::string("json parse error: ") + e.what();
        return result;
    }

    const json* rows = read_rows(body, params.kind);
    if (!rows) {
        result.error = "response must be an array or { data: [] } / { models: [] }";
        return result;
    }

    // ── 解析每行, 去重 ───────────────────────────────────────────────────────
    std::vector<context::ModelEntry> models;
    std::unordered_map<std::string, bool> seen;
    for (const auto& row : *rows) {
        auto entry = parse_row(row, params.kind);
        if (!entry) continue;
        if (seen.count(entry->id)) continue;
        seen[entry->id] = true;
        models.push_back(*entry);
    }

    result.ok = true;
    result.models = models;

    // ── 写TTL 缓存 (仅在有结果时缓存, 对齐 shouldCache: length>0) ────────────
    if (!models.empty()) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[key] = CacheEntry{now_ms() + params.ttl_ms, models};
    }

    log()->info("[ModelDiscovery] {} discovered {} models with context windows",
                params.provider_id, models.size());
    return result;
}

int ModelDiscovery::discover_and_apply(const LiveModelDiscoveryParams& params) {
    auto result = discover(params);
    if (!result.ok || result.models.empty()) {
        return 0;
    }
    // 🎯灌入ContextTokenCache (保守最小值 + provider 独立由该函数保证)
    context::apply_discovered_context_windows(result.models);
    return static_cast<int>(result.models.size());
}

// 🎯 便捷入口: provider_id → kind + endpoint_path 映射
int discover_and_apply_provider_context_windows(
    std::shared_ptr<infra::HttpClient> http_client,
    const std::string& provider_id,
    const std::string& base_url,
    const std::string& api_key) {
    if (!http_client || base_url.empty()) return 0;

    LiveModelDiscoveryParams params;
    params.provider_id = provider_id;
    params.base_url = base_url;
    params.api_key = api_key;

    std::string pid = provider_id;
    std::transform(pid.begin(), pid.end(), pid.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (pid == "google" || pid == "gemini") {
        params.kind = LiveCatalogProviderKind::Google;
        params.endpoint_path = "v1beta/models";
    } else if (pid == "openrouter") {
        params.kind = LiveCatalogProviderKind::OpenRouter;
        params.endpoint_path = "models";
    } else if (pid == "anthropic") {
        params.kind = LiveCatalogProviderKind::Anthropic;
        params.endpoint_path = "models";
    } else {
        // openai 及所有 OpenAI 兼容 provider (deepseek/groq/...)
        params.kind = LiveCatalogProviderKind::OpenAICompatible;
        params.endpoint_path = "models";
    }

    ModelDiscovery discovery(std::move(http_client));
    return discovery.discover_and_apply(params);
}

}  // namespace llm
}  // namespace hunterclaw
