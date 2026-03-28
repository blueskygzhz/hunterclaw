#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ===== Web Search Tool - 严格对齐 OpenClaw web-search.ts =====
//
// 功能对齐:
// 1. ✅ 支持 5 个搜索 Provider: Brave (web + llm-context), Gemini, Grok, Kimi, Perplexity
// 2. ✅ 统一搜索参数: query, count, country, language, search_lang, ui_lang,
//                    freshness, date_after, date_before, domain_filter,
//                    max_tokens, max_tokens_per_page
// 3. ✅ Provider 自动选择: 根据 API Key 可用性自动切换 (alphabetical order)
// 4. ✅ 结果缓存: 默认 15 分钟 TTL
// 5. ✅ 参数验证: 完整的 provider 支持检查 + 结构化错误响应
// 6. ✅ Brave 语言规范化: normalizeBraveSearchLang / normalizeBraveUiLang / normalizeBraveLanguageParams
// 7. ✅ Brave LLM Context 模式: runBraveLlmContextSearch
// 8. ✅ Perplexity Transport: search_api (native) vs chat_completions (OpenRouter)
// 9. ✅ Perplexity 日期转换: isoToPerplexityDate
// 10. ✅ Grok: extractGrokContent (message wrapper + top-level output_text + deprecated output_text)
// 11. ✅ Grok: inline_citations 支持
// 12. ✅ Kimi: 多轮对话 (MAX_ROUNDS=3) + extractKimiCitations (tool_calls 解析)
// 13. ✅ freshness 规范化: Brave (pd/pw/pm/py + date range) + Perplexity (day/week/month/year)
// 14. ✅ resolveSiteName: 从 URL 提取 hostname
// 15. ✅ missingSearchKeyPayload: 结构化缺少 API Key 错误
// 16. ✅ wrapWebContent: 外部内容标记
//
// OpenClaw 原始路径: src/agents/tools/web-search.ts (2223 行)

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(std::shared_ptr<infra::HttpClient> http_client);

    std::string name() const override { return "web_search"; }

    std::string description() const override {
        return "Search the web using Brave Search API. Supports region-specific and localized search "
               "via country and language parameters. Returns titles, URLs, and snippets for fast research.";
    }

    Schema parameters() const override;

    ToolResult execute(const core::JsonValue& params) override;

private:
    std::shared_ptr<infra::HttpClient> http_client_;

    // ============================================================
    // Perplexity Transport (对齐 OpenClaw resolvePerplexityTransport)
    // ============================================================
    struct PerplexityTransport {
        std::string api_key;
        std::string base_url;
        std::string model;
        std::string transport;  // "search_api" | "chat_completions"
    };

    // ============================================================
    // Provider 选择 (对齐 OpenClaw resolveSearchProvider)
    // ============================================================
    std::string select_provider_str(const std::string& requested) const;
    std::string resolve_api_key(const std::string& provider) const;

    // ============================================================
    // Config resolution helpers
    // ============================================================
    PerplexityTransport resolve_perplexity_transport(const std::string& api_key) const;
    std::string resolve_brave_mode() const;
    std::string resolve_grok_model() const;
    bool resolve_grok_inline_citations() const;
    std::string resolve_gemini_model() const;
    std::string resolve_kimi_base_url() const;
    std::string resolve_kimi_model() const;

    // ============================================================
    // Provider 实现
    // ============================================================

    // Brave Web Search (对齐 OpenClaw runWebSearch brave/web branch)
    core::Result<json> run_brave_web_search(
        const std::string& query,
        const std::string& api_key,
        int count,
        const std::string& country,
        const std::string& search_lang,
        const std::string& ui_lang,
        const std::string& freshness,
        const std::string& date_after,
        const std::string& date_before
    );

    // Brave LLM Context Search (对齐 OpenClaw runBraveLlmContextSearch)
    core::Result<json> run_brave_llm_context_search(
        const std::string& query,
        const std::string& api_key,
        const std::string& country,
        const std::string& search_lang,
        const std::string& freshness
    );

    // Gemini Search (对齐 OpenClaw runGeminiSearch)
    core::Result<json> run_gemini_search(
        const std::string& query,
        const std::string& api_key,
        const std::string& model
    );

    // Grok Search (对齐 OpenClaw runGrokSearch + extractGrokContent)
    core::Result<json> run_grok_search(
        const std::string& query,
        const std::string& api_key,
        const std::string& model,
        bool inline_citations
    );

    // Kimi Search (对齐 OpenClaw runKimiSearch - multi-round MAX_ROUNDS=3)
    core::Result<json> run_kimi_search(
        const std::string& query,
        const std::string& api_key,
        const std::string& base_url,
        const std::string& model
    );

    // Perplexity Chat Completions (对齐 OpenClaw runPerplexitySearch)
    core::Result<json> run_perplexity_search(
        const std::string& query,
        const std::string& api_key,
        const std::string& base_url,
        const std::string& model,
        const std::string& freshness
    );

    // Perplexity Search API (对齐 OpenClaw runPerplexitySearchApi)
    core::Result<json> run_perplexity_search_api(
        const std::string& query,
        const std::string& api_key,
        int count,
        const std::string& country,
        const std::vector<std::string>& domain_filter,
        const std::string& freshness,
        const std::string& language,
        const std::string& date_after,
        const std::string& date_before,
        int max_tokens,
        int max_tokens_per_page
    );

    // ============================================================
    // 缓存管理 (对齐 OpenClaw web-shared.ts)
    // ============================================================
    struct CacheEntry {
        json value;
        std::chrono::system_clock::time_point expires_at;
    };
    mutable std::unordered_map<std::string, CacheEntry> cache_;

    std::optional<json> read_cache(const std::string& key) const;
    void write_cache(const std::string& key, const json& value, int ttl_minutes = 15);

    // ============================================================
    // ToolResult 构建辅助
    // ============================================================
    ToolResult make_json_result(const json& value) const;
};

// ===== Web Search Tool Factory =====

class WebSearchToolFactory {
public:
    static std::shared_ptr<Tool> create(std::shared_ptr<infra::HttpClient> http_client);
};

} // namespace tools
} // namespace hunterclaw
