#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ===== Web Search Tool - 100% 对齐 OpenClaw web-search.ts =====
//
// 功能对齐:
// 1. ✅ 支持 5 个搜索 Provider: Brave, Gemini, Grok, Kimi, Perplexity
// 2. ✅ 统一搜索参数: query, count, country, language, freshness, date_after, date_before
// 3. ✅ Provider 自动选择: 根据 API Key 可用性自动切换
// 4. ✅ 结果缓存: 默认 15 分钟 TTL (可配置)
// 5. ✅ 结果标准化: 统一返回格式 { results: [{title, url, excerpt}], provider, raw }
// 6. ✅ Citation 重定向: 支持 /cite/... 路径重定向
// 7. ✅ 安全包装: 外部内容标记 (untrusted)
// 8. ✅ 超时控制: 默认 30 秒 (可配置)
// 9. ✅ 错误处理: Provider fallback, graceful degradation
// 10. ✅ 高级过滤: 国家/语言/时间范围过滤
//
// Supported Providers:
// - Brave: https://api.search.brave.com (BRAVE_API_KEY)
// - Gemini: Google Gemini with Search Grounding (GEMINI_API_KEY)
// - Grok: X.AI API (XAI_API_KEY / GROK_API_KEY)
// - Kimi: Moonshot AI with $web_search (MOONSHOT_API_KEY / KIMI_API_KEY)
// - Perplexity: Perplexity API or OpenRouter (PERPLEXITY_API_KEY)
//
// OpenClaw 原始路径: src/agents/tools/web-search.ts (1,400+ 行)

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(std::shared_ptr<infra::HttpClient> http_client);
    
    std::string name() const override { return "web_search"; }
    
    std::string description() const override {
        return "Search the web for real-time information using available search providers. "
               "Returns a list of relevant results with titles, URLs, and excerpts. "
               "Supports filtering by country, language, and time range.";
    }
    
    Schema parameters() const override;
    
    // 执行搜索
    // Returns: { results: SearchResult[], provider: string, cached?: bool, raw?: json }
    ToolResult execute(const core::JsonValue& params) override;
    
private:
    std::shared_ptr<infra::HttpClient> http_client_;
    
    // ===== Provider 配置 =====
    
    // 支持的 Provider
    enum class SearchProvider {
        Brave,
        Gemini,
        Grok,
        Kimi,
        Perplexity
    };
    
    // 检查 Provider 是否可用 (API Key)
    bool is_provider_available(SearchProvider provider) const;
    
    // 选择最佳可用 Provider
    SearchProvider select_provider(const std::string& requested_provider) const;
    
    // ===== Provider 实现 =====
    
    // Brave Search
    core::Result<json> search_brave(
        const std::string& query,
        int count,
        const std::string& country,
        const std::string& language,
        const std::string& freshness,
        const std::string& date_after,
        const std::string& date_before
    );
    
    // Gemini Search (with Grounding)
    core::Result<json> search_gemini(
        const std::string& query,
        int count
    );
    
    // Grok Search (X.AI Responses API)
    core::Result<json> search_grok(
        const std::string& query,
        int count
    );
    
    // Kimi Search (Moonshot $web_search)
    core::Result<json> search_kimi(
        const std::string& query,
        int count
    );
    
    // Perplexity Search
    core::Result<json> search_perplexity(
        const std::string& query,
        int count,
        const std::string& freshness,
        const std::string& date_after,
        const std::string& date_before
    );
    
    // ===== 结果标准化 =====
    
    struct SearchResult {
        std::string title;
        std::string url;
        std::string excerpt;
        std::string published_date;  // 可选
    };
    
    // 标准化不同 Provider 的返回结果
    std::vector<SearchResult> normalize_brave_results(const json& raw_results);
    std::vector<SearchResult> normalize_gemini_results(const json& raw_results);
    std::vector<SearchResult> normalize_grok_results(const json& raw_results);
    std::vector<SearchResult> normalize_kimi_results(const json& raw_results);
    std::vector<SearchResult> normalize_perplexity_results(const json& raw_results);
    
    // ===== 缓存管理 =====
    
    // 生成缓存 Key
    std::string cache_key(
        SearchProvider provider,
        const std::string& query,
        const json& filters
    ) const;
    
    // 读取缓存
    std::optional<json> read_cache(const std::string& key) const;
    
    // 写入缓存 (默认 15 分钟 TTL)
    void write_cache(const std::string& key, const json& value, int ttl_minutes = 15);
    
    // 缓存存储 (简单内存缓存)
    struct CacheEntry {
        json value;
        std::chrono::system_clock::time_point expires_at;
    };
    mutable std::unordered_map<std::string, CacheEntry> cache_;
    
    // ===== 时间过滤辅助 =====
    
    // 将 freshness shortcut 转换为 Brave 格式 (pd, pw, pm, py)
    std::string freshness_to_brave(const std::string& freshness) const;
    
    // 将 freshness 转换为 Perplexity recency (day, week, month, year)
    std::string freshness_to_perplexity(const std::string& freshness) const;
    
    // 将日期转换为 ISO 格式 (YYYY-MM-DD)
    std::string normalize_iso_date(const std::string& date) const;
    
    // ===== 安全辅助 =====
    
    // 包装外部搜索结果 (标记为 untrusted)
    json wrap_search_results(const std::vector<SearchResult>& results) const;
    
    // 解析 Citation 重定向 URL
    std::string resolve_citation_url(const std::string& url) const;
};

// ===== Web Search Tool Factory =====

class WebSearchToolFactory {
public:
    static std::shared_ptr<Tool> create(std::shared_ptr<infra::HttpClient> http_client);
};

} // namespace tools
} // namespace hunterclaw
