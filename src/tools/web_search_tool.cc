#include "hunterclaw/tools/web_search_tool.h"
#include "hunterclaw/core/config.h"
#include <spdlog/spdlog.h>
#include <regex>

namespace hunterclaw {
namespace tools {

// ===== Constructor =====

WebSearchTool::WebSearchTool(std::shared_ptr<core::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

// ===== Schema =====

json WebSearchTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"query", {
                {"type", "string"},
                {"description", "Search query string"}
            }},
            {"count", {
                {"type", "number"},
                {"description", "Number of results to return (1-10, default: 5)"},
                {"minimum", 1},
                {"maximum", 10}
            }},
            {"provider", {
                {"type", "string"},
                {"description", "Search provider (brave/gemini/grok/kimi/perplexity, auto-select if not specified)"},
                {"enum", {"brave", "gemini", "grok", "kimi", "perplexity"}}
            }},
            {"country", {
                {"type", "string"},
                {"description", "2-letter country code (e.g., 'US', 'DE', 'ALL')"}
            }},
            {"language", {
                {"type", "string"},
                {"description", "ISO 639-1 language code (e.g., 'en', 'de', 'zh')"}
            }},
            {"freshness", {
                {"type", "string"},
                {"description", "Time filter: 'day' (24h), 'week', 'month', 'year'"},
                {"enum", {"day", "week", "month", "year"}}
            }},
            {"date_after", {
                {"type", "string"},
                {"description", "Only results after this date (YYYY-MM-DD)"}
            }},
            {"date_before", {
                {"type", "string"},
                {"description", "Only results before this date (YYYY-MM-DD)"}
            }}
        }},
        {"required", {"query"}}
    };
}

// ===== Execute =====

core::Result<json> WebSearchTool::execute(const json& params) {
    // 1. 解析参数
    if (!params.contains("query") || !params["query"].is_string()) {
        return core::Result<json>::failure("Missing required parameter: query");
    }
    
    std::string query = params["query"].get<std::string>();
    int count = params.value("count", 5);
    std::string provider_str = params.value("provider", std::string("auto"));
    std::string country = params.value("country", std::string("US"));
    std::string language = params.value("language", std::string("en"));
    std::string freshness = params.value("freshness", std::string(""));
    std::string date_after = params.value("date_after", std::string(""));
    std::string date_before = params.value("date_before", std::string(""));
    
    // 2. 选择 Provider
    SearchProvider provider;
    if (provider_str == "brave") provider = SearchProvider::Brave;
    else if (provider_str == "gemini") provider = SearchProvider::Gemini;
    else if (provider_str == "grok") provider = SearchProvider::Grok;
    else if (provider_str == "kimi") provider = SearchProvider::Kimi;
    else if (provider_str == "perplexity") provider = SearchProvider::Perplexity;
    else provider = select_provider(provider_str);
    
    // 3. 检查缓存
    json filters = {
        {"country", country},
        {"language", language},
        {"freshness", freshness},
        {"date_after", date_after},
        {"date_before", date_before},
        {"count", count}
    };
    std::string key = cache_key(provider, query, filters);
    auto cached = read_cache(key);
    if (cached) {
        spdlog::debug("[WebSearch] Cache hit: {}", query);
        json result = *cached;
        result["cached"] = true;
        return core::Result<json>::success(result);
    }
    
    // 4. 执行搜索
    core::Result<json> search_result;
    
    switch (provider) {
        case SearchProvider::Brave:
            search_result = search_brave(query, count, country, language, 
                                        freshness, date_after, date_before);
            break;
        case SearchProvider::Gemini:
            search_result = search_gemini(query, count);
            break;
        case SearchProvider::Grok:
            search_result = search_grok(query, count);
            break;
        case SearchProvider::Kimi:
            search_result = search_kimi(query, count);
            break;
        case SearchProvider::Perplexity:
            search_result = search_perplexity(query, count, freshness, 
                                             date_after, date_before);
            break;
    }
    
    if (!search_result.is_success()) {
        return search_result;
    }
    
    // 5. 写入缓存
    write_cache(key, search_result.value(), 15);
    
    return search_result;
}

// ===== Provider 可用性检查 =====

bool WebSearchTool::is_provider_available(SearchProvider provider) const {
    auto& config = core::Config::instance();
    
    switch (provider) {
        case SearchProvider::Brave:
            return config.has("BRAVE_API_KEY");
        case SearchProvider::Gemini:
            return config.has("GEMINI_API_KEY");
        case SearchProvider::Grok:
            return config.has("XAI_API_KEY") || config.has("GROK_API_KEY");
        case SearchProvider::Kimi:
            return config.has("MOONSHOT_API_KEY") || config.has("KIMI_API_KEY");
        case SearchProvider::Perplexity:
            return config.has("PERPLEXITY_API_KEY");
    }
    return false;
}

WebSearchTool::SearchProvider WebSearchTool::select_provider(const std::string& requested) const {
    // 优先级: Brave > Gemini > Grok > Perplexity > Kimi
    if (is_provider_available(SearchProvider::Brave)) return SearchProvider::Brave;
    if (is_provider_available(SearchProvider::Gemini)) return SearchProvider::Gemini;
    if (is_provider_available(SearchProvider::Grok)) return SearchProvider::Grok;
    if (is_provider_available(SearchProvider::Perplexity)) return SearchProvider::Perplexity;
    if (is_provider_available(SearchProvider::Kimi)) return SearchProvider::Kimi;
    
    // 默认 Brave
    return SearchProvider::Brave;
}

// ===== Brave Search =====

core::Result<json> WebSearchTool::search_brave(
    const std::string& query,
    int count,
    const std::string& country,
    const std::string& language,
    const std::string& freshness,
    const std::string& date_after,
    const std::string& date_before
) {
    auto& config = core::Config::instance();
    auto api_key = config.get("BRAVE_API_KEY");
    
    if (!api_key) {
        return core::Result<json>::failure("BRAVE_API_KEY not configured");
    }
    
    // 构建请求 URL
    std::string url = "https://api.search.brave.com/res/v1/web/search";
    url += "?q=" + http_client_->url_encode(query);
    url += "&count=" + std::to_string(count);
    url += "&country=" + country;
    
    if (!language.empty()) {
        url += "&search_lang=" + language;
    }
    
    if (!freshness.empty()) {
        std::string brave_freshness = freshness_to_brave(freshness);
        url += "&freshness=" + brave_freshness;
    }
    
    // Brave API 请求
    core::HttpRequest request;
    request.url = url;
    request.method = "GET";
    request.headers["X-Subscription-Token"] = *api_key;
    request.headers["Accept"] = "application/json";
    request.timeout_seconds = 30;
    
    auto response = http_client_->request(request);
    if (!response.is_success()) {
        return core::Result<json>::failure("Brave API request failed: " + response.error());
    }
    
    // 解析响应
    json raw_result;
    try {
        raw_result = json::parse(response.value().body);
    } catch (const std::exception& e) {
        return core::Result<json>::failure("Failed to parse Brave response: " + std::string(e.what()));
    }
    
    // 提取结果
    auto results = normalize_brave_results(raw_result);
    auto wrapped = wrap_search_results(results);
    
    json output = {
        {"results", wrapped},
        {"provider", "brave"},
        {"raw", raw_result}
    };
    
    spdlog::info("[WebSearch] Brave: {} results for '{}'", results.size(), query);
    
    return core::Result<json>::success(output);
}

// ===== 其他 Provider (占位实现) =====

core::Result<json> WebSearchTool::search_gemini(const std::string& query, int count) {
    // TODO: 实现 Gemini Search (with Grounding)
    return core::Result<json>::failure("Gemini search not yet implemented");
}

core::Result<json> WebSearchTool::search_grok(const std::string& query, int count) {
    // TODO: 实现 Grok Search
    return core::Result<json>::failure("Grok search not yet implemented");
}

core::Result<json> WebSearchTool::search_kimi(const std::string& query, int count) {
    // TODO: 实现 Kimi Search
    return core::Result<json>::failure("Kimi search not yet implemented");
}

core::Result<json> WebSearchTool::search_perplexity(
    const std::string& query,
    int count,
    const std::string& freshness,
    const std::string& date_after,
    const std::string& date_before
) {
    // TODO: 实现 Perplexity Search
    return core::Result<json>::failure("Perplexity search not yet implemented");
}

// ===== 结果标准化 =====

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_brave_results(const json& raw_results) {
    std::vector<SearchResult> results;
    
    if (!raw_results.contains("web") || !raw_results["web"].contains("results")) {
        return results;
    }
    
    for (const auto& item : raw_results["web"]["results"]) {
        SearchResult result;
        result.title = item.value("title", std::string(""));
        result.url = item.value("url", std::string(""));
        result.excerpt = item.value("description", std::string(""));
        result.published_date = item.value("age", std::string(""));
        
        results.push_back(result);
    }
    
    return results;
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_gemini_results(const json& raw) {
    // TODO: 实现 Gemini 结果标准化
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_grok_results(const json& raw) {
    // TODO: 实现 Grok 结果标准化
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_kimi_results(const json& raw) {
    // TODO: 实现 Kimi 结果标准化
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_perplexity_results(const json& raw) {
    // TODO: 实现 Perplexity 结果标准化
    return {};
}

// ===== 缓存管理 =====

std::string WebSearchTool::cache_key(
    SearchProvider provider,
    const std::string& query,
    const json& filters
) const {
    std::string provider_str;
    switch (provider) {
        case SearchProvider::Brave: provider_str = "brave"; break;
        case SearchProvider::Gemini: provider_str = "gemini"; break;
        case SearchProvider::Grok: provider_str = "grok"; break;
        case SearchProvider::Kimi: provider_str = "kimi"; break;
        case SearchProvider::Perplexity: provider_str = "perplexity"; break;
    }
    
    return "web_search:" + provider_str + ":" + query + ":" + filters.dump();
}

std::optional<json> WebSearchTool::read_cache(const std::string& key) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return std::nullopt;
    }
    
    if (std::chrono::system_clock::now() > it->second.expires_at) {
        cache_.erase(it);
        return std::nullopt;
    }
    
    return it->second.value;
}

void WebSearchTool::write_cache(const std::string& key, const json& value, int ttl_minutes) {
    CacheEntry entry;
    entry.value = value;
    entry.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(ttl_minutes);
    cache_[key] = entry;
}

// ===== 时间过滤辅助 =====

std::string WebSearchTool::freshness_to_brave(const std::string& freshness) const {
    if (freshness == "day") return "pd";
    if (freshness == "week") return "pw";
    if (freshness == "month") return "pm";
    if (freshness == "year") return "py";
    return "";
}

std::string WebSearchTool::freshness_to_perplexity(const std::string& freshness) const {
    // Perplexity 直接使用 day/week/month/year
    return freshness;
}

std::string WebSearchTool::normalize_iso_date(const std::string& date) const {
    // 简单验证 YYYY-MM-DD 格式
    std::regex iso_pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (std::regex_match(date, iso_pattern)) {
        return date;
    }
    return "";
}

// ===== 安全辅助 =====

json WebSearchTool::wrap_search_results(const std::vector<SearchResult>& results) const {
    json wrapped = json::array();
    
    for (const auto& result : results) {
        json item = {
            {"title", result.title},
            {"url", result.url},
            {"excerpt", result.excerpt}
        };
        
        if (!result.published_date.empty()) {
            item["published_date"] = result.published_date;
        }
        
        // 标记为外部内容
        item["untrusted"] = true;
        item["source"] = "web_search";
        
        wrapped.push_back(item);
    }
    
    return wrapped;
}

std::string WebSearchTool::resolve_citation_url(const std::string& url) const {
    // TODO: 实现 Citation 重定向逻辑
    return url;
}

// ===== Factory =====

std::shared_ptr<Tool> WebSearchToolFactory::create(std::shared_ptr<core::HttpClient> http_client) {
    return std::make_shared<WebSearchTool>(std::move(http_client));
}

} // namespace tools
} // namespace hunterclaw
