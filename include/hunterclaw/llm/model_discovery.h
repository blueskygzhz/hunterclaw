#pragma once

// 🎯 严格对齐 OpenClaw: src/plugin-sdk/provider-catalog-live-runtime.ts
//                + provider-catalog-live-normalize.internal.ts
//
// Live Model Catalog 发现器: 运行时从各 provider 的 /models 端点动态拉取模型
// 上下文窗口 (context window / context_length / inputTokenLimit 等), 解析为
// ModelEntry 后灌入ContextTokenCache (apply_discovered_context_windows),
// 使不同模型的压缩窗口能"从服务器按 model 动态返回", 静态 catalog 仅作兜底。
//
// 复用 infra::HttpClient (含 TLS 校验) + security::SsrfGuard (防SSRF)。

#include "hunterclaw/context/context_window.h"
#include "hunterclaw/infra/http_client.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace llm {

// 🎯 对齐 OpenClaw body/page上限
inline constexpr int LIVE_MODEL_CATALOG_BODY_MAX_BYTES = 4 * 1024 * 1024;  // 4MB
inline constexpr int LIVE_MODEL_CATALOG_MAX_PAGES = 50;
inline constexpr int LIVE_MODEL_CATALOG_DEFAULT_TIMEOUT_MS = 5000;
inline constexpr int64_t LIVE_MODEL_CATALOG_DEFAULT_TTL_MS = 60000;  // 60s

// provider 家族: 决定 endpoint 路径与响应解析器 (对齐 openclaw 各 provider plugin)
enum class LiveCatalogProviderKind {
    OpenAICompatible,  // GET {baseUrl}/models → { data: [...] } / [...]
    OpenRouter,        // GET {baseUrl}/models → data[].top_provider.context_length
    Google,            // GET {baseUrl}/models → models[].inputTokenLimit
    Anthropic,// GET {baseUrl}/models → data[].max_input_tokens
};

struct LiveModelDiscoveryParams {
    std::string provider_id;   // e.g. "openai" / "openrouter" / "google" / "anthropic"
    std::string base_url;      // provider 推理 base url
    std::string api_key;       // 可选; 空则匿名
    std::string discovery_api_key;  // 可选; 优先于 api_key (对齐 discoveryApiKey)
    LiveCatalogProviderKind kind = LiveCatalogProviderKind::OpenAICompatible;
    std::string endpoint_path = "models";  // 相对路径 (默认 "models")
    int timeout_ms = LIVE_MODEL_CATALOG_DEFAULT_TIMEOUT_MS;
    int64_t ttl_ms = LIVE_MODEL_CATALOG_DEFAULT_TTL_MS;
    bool require_https = true;
};

struct LiveModelDiscoveryResult {
    bool ok = false;
    std::string error;                    // 失败原因 (advisory: 失败不影响静态兜底)
    std::vector<context::ModelEntry> models;  // 解析出的模型 (含 context_window)
    bool from_cache = false;
};

// 🎯 对齐 OpenClaw fetchLiveProviderModelRows + buildOpenAICompatibleLiveModels:
// 拉取并解析 provider 的 live model catalog。带 SSRF 校验、4MB 上限、分页(<=50)、
// TTL 缓存。失败返回 ok=false (advisory, 调用方保留静态 catalog)。
class ModelDiscovery {
 public:
    explicit ModelDiscovery(std::shared_ptr<infra::HttpClient> http_client);

    LiveModelDiscoveryResult discover(const LiveModelDiscoveryParams& params);

    // 🎯 对齐 applyDiscoveredContextWindows: 拉取后直接灌入 ContextTokenCache。
    // 返回灌入的模型数; 失败返回 0。
    int discover_and_apply(const LiveModelDiscoveryParams& params);

    // 测试用: 清空 TTL 缓存 (对齐 clearLiveCatalogCacheForTests)
    static void clear_cache_for_test();

 private:
    std::shared_ptr<infra::HttpClient> http_client_;
};

// 🎯 便捷入口: 按provider_id 推断 kind + endpoint_path, 拉取并灌入缓存。
// 供启动/按需接线调用 (advisory: 失败静默, 保留静态 catalog)。
// provider_id 支持: openai / anthropic / gemini(google) / openrouter /
// deepseek / groq / 及其它 OpenAI 兼容 provider (默认走 OpenAICompatible)。
int discover_and_apply_provider_context_windows(
    std::shared_ptr<infra::HttpClient> http_client,
    const std::string& provider_id,
    const std::string& base_url,
    const std::string& api_key);

}  // namespace llm
}  // namespace hunterclaw
