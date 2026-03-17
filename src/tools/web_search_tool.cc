#include "hunterclaw/tools/web_search_tool.h"
#include <regex>
#include <cstdio>
#include <algorithm>

namespace hunterclaw {
namespace tools {

// ===== 常量定义 (对齐 OpenClaw) =====

// Provider 列表
static const std::vector<std::string> SEARCH_PROVIDERS = {
    "brave", "gemini", "grok", "kimi", "perplexity"
};

// 默认值
static const int DEFAULT_SEARCH_COUNT = 5;
static const int MAX_SEARCH_COUNT = 10;
static const int DEFAULT_TIMEOUT_SECONDS = 30;
static const int DEFAULT_CACHE_TTL_MINUTES = 15;

// API Endpoints
static const std::string BRAVE_SEARCH_ENDPOINT = "https://api.search.brave.com/res/v1/web/search";
static const std::string BRAVE_LLM_CONTEXT_ENDPOINT = "https://api.search.brave.com/res/v1/llm/context";
static const std::string GEMINI_API_BASE = "https://generativelanguage.googleapis.com/v1beta";
static const std::string DEFAULT_GEMINI_MODEL = "gemini-2.5-flash";
static const std::string XAI_API_ENDPOINT = "https://api.x.ai/v1/responses";
static const std::string DEFAULT_GROK_MODEL = "grok-4-1-fast";
static const std::string DEFAULT_KIMI_BASE_URL = "https://api.moonshot.ai/v1";
static const std::string DEFAULT_KIMI_MODEL = "moonshot-v1-128k";
static const std::string PERPLEXITY_DIRECT_BASE_URL = "https://api.perplexity.ai";
static const std::string PERPLEXITY_SEARCH_ENDPOINT = "https://api.perplexity.ai/search";
static const std::string DEFAULT_PERPLEXITY_BASE_URL = "https://openrouter.ai/api/v1";
static const std::string DEFAULT_PERPLEXITY_MODEL = "perplexity/sonar-pro";

// Freshness 映射 (对齐 OpenClaw 的 FRESHNESS_TO_RECENCY)
static const std::unordered_map<std::string, std::string> FRESHNESS_TO_RECENCY = {
    {"pd", "day"},
    {"pw", "week"},
    {"pm", "month"},
    {"py", "year"}
};

static const std::unordered_map<std::string, std::string> RECENCY_TO_FRESHNESS = {
    {"day", "pd"},
    {"week", "pw"},
    {"month", "pm"},
    {"year", "py"}
};

// ===== 构造函数 =====

WebSearchTool::WebSearchTool(std::shared_ptr<infra::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

// ===== Schema =====

Tool::Schema WebSearchTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.required.push_back("query");
    
    // 简化 schema,真实参数解析在 execute 中处理
    // 对齐 OpenClaw 的 createWebSearchSchema 函数
    return schema;
}

// ===== Execute =====

ToolResult WebSearchTool::execute(const core::JsonValue& params) {
    // 1. 解析参数 (对齐 OpenClaw execute 函数 line 1938-2111)
    if (!params.is_object() || !params.contains("query") || !params["query"].is_string()) {
        ToolResult result;
        result.success = false;
        result.error = "Missing required parameter: query";
        return result;
    }
    
    std::string query = params["query"].as_string();
    int count = params.contains("count") && params["count"].is_number() ? 
                static_cast<int>(params["count"].as_number()) : DEFAULT_SEARCH_COUNT;
    count = std::max(1, std::min(MAX_SEARCH_COUNT, count));
    
    std::string provider_str = params.contains("provider") && params["provider"].is_string() ? 
                               params["provider"].as_string() : "auto";
    std::string country = params.contains("country") && params["country"].is_string() ? 
                         params["country"].as_string() : "US";
    std::string language = params.contains("language") && params["language"].is_string() ? 
                          params["language"].as_string() : "en";
    std::string freshness = params.contains("freshness") && params["freshness"].is_string() ? 
                           params["freshness"].as_string() : "";
    std::string date_after = params.contains("date_after") && params["date_after"].is_string() ? 
                            params["date_after"].as_string() : "";
    std::string date_before = params.contains("date_before") && params["date_before"].is_string() ? 
                             params["date_before"].as_string() : "";
    
    // 2. 选择 Provider (对齐 OpenClaw resolveSearchProvider line 604-670)
    SearchProvider provider;
    if (provider_str == "brave") provider = SearchProvider::Brave;
    else if (provider_str == "gemini") provider = SearchProvider::Gemini;
    else if (provider_str == "grok") provider = SearchProvider::Grok;
    else if (provider_str == "kimi") provider = SearchProvider::Kimi;
    else if (provider_str == "perplexity") provider = SearchProvider::Perplexity;
    else provider = select_provider(provider_str);
    
    // 3. 检查缓存 (对齐 OpenClaw runWebSearch line 1618-1626)
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
        fprintf(stderr, "[WebSearch] Cache hit: %s\n", query.c_str());
        json result = *cached;
        result["cached"] = true;
        
        // 转换为 ToolResult
        ToolResult tool_result;
        tool_result.success = true;
        ToolResultContent content;
        content.type = ToolResultContent::Type::Json;
        content.text = result.dump();
        tool_result.content.push_back(content);
        return tool_result;
    }
    
    // 4. 执行搜索 (对齐 OpenClaw runWebSearch line 1628-1889)
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
    
    if (!search_result.is_ok()) {
        ToolResult tool_result;
        tool_result.success = false;
        tool_result.error = search_result.error;
        return tool_result;
    }
    
    // 5. 写入缓存 (对齐 OpenClaw line 1655/1687/1715/1742/1768/1887)
    json result_value = search_result.unwrap();
    write_cache(key, result_value, DEFAULT_CACHE_TTL_MINUTES);
    
    // 6. 转换为 ToolResult
    ToolResult tool_result;
    tool_result.success = true;
    
    ToolResultContent content;
    content.type = ToolResultContent::Type::Json;
    content.text = result_value.dump();
    tool_result.content.push_back(content);
    
    return tool_result;
}

// ===== Provider 可用性检查 (对齐 OpenClaw resolveSearchApiKey etc.) =====

bool WebSearchTool::is_provider_available(SearchProvider provider) const {
    switch (provider) {
        case SearchProvider::Brave:
            return std::getenv("BRAVE_API_KEY") != nullptr;
        case SearchProvider::Gemini:
            return std::getenv("GEMINI_API_KEY") != nullptr;
        case SearchProvider::Grok:
            return std::getenv("XAI_API_KEY") != nullptr || std::getenv("GROK_API_KEY") != nullptr;
        case SearchProvider::Kimi:
            return std::getenv("MOONSHOT_API_KEY") != nullptr || std::getenv("KIMI_API_KEY") != nullptr;
        case SearchProvider::Perplexity:
            return std::getenv("PERPLEXITY_API_KEY") != nullptr || 
                   std::getenv("OPENROUTER_API_KEY") != nullptr;
    }
    return false;
}

WebSearchTool::SearchProvider WebSearchTool::select_provider(const std::string& requested) const {
    // 优先级: Brave > Gemini > Grok > Kimi > Perplexity
    // 对齐 OpenClaw resolveSearchProvider line 625-667
    if (is_provider_available(SearchProvider::Brave)) return SearchProvider::Brave;
    if (is_provider_available(SearchProvider::Gemini)) return SearchProvider::Gemini;
    if (is_provider_available(SearchProvider::Grok)) return SearchProvider::Grok;
    if (is_provider_available(SearchProvider::Kimi)) return SearchProvider::Kimi;
    if (is_provider_available(SearchProvider::Perplexity)) return SearchProvider::Perplexity;
    
    // 默认 Brave (即使没有 API Key 也返回,后续会报错)
    return SearchProvider::Brave;
}

// ===== Brave Search (对齐 OpenClaw line 1812-1888) =====

core::Result<json> WebSearchTool::search_brave(
    const std::string& query,
    int count,
    const std::string& country,
    const std::string& language,
    const std::string& freshness,
    const std::string& date_after,
    const std::string& date_before
) {
    const char* api_key_env = std::getenv("BRAVE_API_KEY");
    if (!api_key_env) {
        return core::Result<json>::Err("BRAVE_API_KEY not configured. Set it in the environment.");
    }
    std::string api_key = api_key_env;
    
    // 构建请求 URL (对齐 OpenClaw line 1812-1835)
    std::string url = BRAVE_SEARCH_ENDPOINT;
    url += "?q=" + query;  // TODO: URL encode
    url += "&count=" + std::to_string(count);
    url += "&country=" + country;
    
    if (!language.empty()) {
        url += "&search_lang=" + language;
    }
    
    if (!freshness.empty()) {
        std::string brave_freshness = freshness_to_brave(freshness);
        if (!brave_freshness.empty()) {
            url += "&freshness=" + brave_freshness;
        }
    } else if (!date_after.empty() && !date_before.empty()) {
        url += "&freshness=" + date_after + "to" + date_before;
    } else if (!date_after.empty()) {
        // 使用当前日期作为结束
        url += "&freshness=" + date_after + "to2030-12-31";
    } else if (!date_before.empty()) {
        url += "&freshness=1970-01-01to" + date_before;
    }
    
    // Brave API 请求 (对齐 OpenClaw line 1837-1872)
    infra::HttpRequestOptions options;
    options.headers["X-Subscription-Token"] = api_key;
    options.headers["Accept"] = "application/json";
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    
    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->get(url, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Brave API request failed: " + response.error);
    }
    
    // 解析响应 (对齐 OpenClaw line 1850-1872)
    json raw_result;
    try {
        raw_result = json::parse(response.unwrap().body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Failed to parse Brave response: " + std::string(e.what()));
    }
    
    // 提取并标准化结果 (对齐 OpenClaw line 1857-1872)
    auto results = normalize_brave_results(raw_result);
    auto wrapped = wrap_search_results(results);
    
    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // 构建返回值 (对齐 OpenClaw line 1874-1888)
    json output = {
        {"query", query},
        {"provider", "brave"},
        {"count", results.size()},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "brave"},
            {"wrapped", true}
        }},
        {"results", wrapped}
    };
    
    fprintf(stderr, "[WebSearch] Brave: %zu results for '%s' in %ld ms\n", 
            results.size(), query.c_str(), took_ms);
    
    return core::Result<json>::Ok(output);
}

// ===== Gemini Search (对齐 OpenClaw runGeminiSearch line 936-1022) =====

core::Result<json> WebSearchTool::search_gemini(const std::string& query, int count) {
    const char* api_key_env = std::getenv("GEMINI_API_KEY");
    if (!api_key_env) {
        return core::Result<json>::Err("GEMINI_API_KEY not configured. Set it in the environment.");
    }
    std::string api_key = api_key_env;
    
    // 构建请求 (对齐 OpenClaw line 942-962)
    std::string endpoint = GEMINI_API_BASE + "/models/" + DEFAULT_GEMINI_MODEL + ":generateContent";
    
    json request_body = {
        {"contents", json::array({
            {{"parts", json::array({
                {{"text", query}}
            })}}
        })},
        {"tools", json::array({
            {{"google_search", json::object()}}
        })}
    };
    
    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["x-goog-api-key"] = api_key;
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = request_body.dump();
    
    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(endpoint, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Gemini API request failed: " + response.error);
    }
    
    // 解析响应 (对齐 OpenClaw line 975-1019)
    json data;
    try {
        data = json::parse(response.unwrap().body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Failed to parse Gemini response: " + std::string(e.what()));
    }
    
    if (data.contains("error")) {
        std::string error_msg = data["error"].value("message", "Unknown error");
        return core::Result<json>::Err("Gemini API error: " + error_msg);
    }
    
    // 提取内容和 citations
    std::string content = "No response";
    if (data.contains("candidates") && data["candidates"].is_array() && !data["candidates"].empty()) {
        auto& candidate = data["candidates"][0];
        if (candidate.contains("content") && candidate["content"].contains("parts")) {
            for (auto& part : candidate["content"]["parts"]) {
                if (part.contains("text")) {
                    content = part["text"].get<std::string>();
                    break;
                }
            }
        }
    }
    
    json citations = json::array();
    if (data.contains("candidates") && data["candidates"].is_array() && !data["candidates"].empty()) {
        auto& candidate = data["candidates"][0];
        if (candidate.contains("groundingMetadata") && 
            candidate["groundingMetadata"].contains("groundingChunks")) {
            for (auto& chunk : candidate["groundingMetadata"]["groundingChunks"]) {
                if (chunk.contains("web") && chunk["web"].contains("uri")) {
                    json citation = {
                        {"url", chunk["web"]["uri"]},
                        {"title", chunk["web"].value("title", "")}
                    };
                    citations.push_back(citation);
                }
            }
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // 构建返回值 (对齐 OpenClaw line 1754-1769)
    json output = {
        {"query", query},
        {"provider", "gemini"},
        {"model", DEFAULT_GEMINI_MODEL},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "gemini"},
            {"wrapped", true}
        }},
        {"content", content},
        {"citations", citations}
    };
    
    fprintf(stderr, "[WebSearch] Gemini: %zu citations for '%s' in %ld ms\n", 
            citations.size(), query.c_str(), took_ms);
    
    return core::Result<json>::Ok(output);
}

// ===== Grok Search (对齐 OpenClaw runGrokSearch line 1305-1360) =====

core::Result<json> WebSearchTool::search_grok(const std::string& query, int count) {
    const char* api_key_env = std::getenv("XAI_API_KEY");
    if (!api_key_env) {
        api_key_env = std::getenv("GROK_API_KEY");
    }
    if (!api_key_env) {
        return core::Result<json>::Err("XAI_API_KEY or GROK_API_KEY not configured.");
    }
    std::string api_key = api_key_env;
    
    // 构建请求 (对齐 OpenClaw line 1316-1325)
    json request_body = {
        {"model", DEFAULT_GROK_MODEL},
        {"input", json::array({
            {{"role", "user"}, {"content", query}}
        })},
        {"tools", json::array({
            {{"type", "web_search"}}
        })}
    };
    
    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["Authorization"] = "Bearer " + api_key;
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = request_body.dump();
    
    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(XAI_API_ENDPOINT, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("xAI API request failed: " + response.error);
    }
    
    // 解析响应 (对齐 OpenClaw extractGrokContent line 459-496)
    json data;
    try {
        data = json::parse(response.unwrap().body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Failed to parse Grok response: " + std::string(e.what()));
    }
    
    std::string content = "No response";
    json citations = json::array();
    
    // 提取 output_text 和 citations
    if (data.contains("output") && data["output"].is_array()) {
        for (auto& output : data["output"]) {
            if (output.value("type", "") == "message" && output.contains("content")) {
                for (auto& block : output["content"]) {
                    if (block.value("type", "") == "output_text" && block.contains("text")) {
                        content = block["text"].get<std::string>();
                        break;
                    }
                }
            }
        }
    }
    
    if (data.contains("citations") && data["citations"].is_array()) {
        citations = data["citations"];
    }
    
    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // 构建返回值 (对齐 OpenClaw line 1700-1716)
    json output = {
        {"query", query},
        {"provider", "grok"},
        {"model", DEFAULT_GROK_MODEL},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "grok"},
            {"wrapped", true}
        }},
        {"content", content},
        {"citations", citations}
    };
    
    fprintf(stderr, "[WebSearch] Grok: %zu citations for '%s' in %ld ms\n", 
            citations.size(), query.c_str(), took_ms);
    
    return core::Result<json>::Ok(output);
}

// ===== Kimi Search (对齐 OpenClaw runKimiSearch line 1412-1511) =====

core::Result<json> WebSearchTool::search_kimi(const std::string& query, int count) {
    const char* api_key_env = std::getenv("KIMI_API_KEY");
    if (!api_key_env) {
        api_key_env = std::getenv("MOONSHOT_API_KEY");
    }
    if (!api_key_env) {
        return core::Result<json>::Err("KIMI_API_KEY or MOONSHOT_API_KEY not configured.");
    }
    std::string api_key = api_key_env;
    
    std::string endpoint = DEFAULT_KIMI_BASE_URL + "/chat/completions";
    
    // 构建请求 (对齐 OpenClaw line 1421-1445)
    json messages = json::array({
        {{"role", "user"}, {"content", query}}
    });
    
    json request_body = {
        {"model", DEFAULT_KIMI_MODEL},
        {"messages", messages},
        {"tools", json::array({
            {{"type", "builtin_function"}, 
             {"function", {{"name", "$web_search"}}}}
        })}
    };
    
    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["Authorization"] = "Bearer " + api_key;
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = request_body.dump();
    
    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(endpoint, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Kimi API request failed: " + response.error);
    }
    
    // 解析响应 (对齐 OpenClaw extractKimiCitations line 1371-1400)
    json data;
    try {
        data = json::parse(response.unwrap().body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Failed to parse Kimi response: " + std::string(e.what()));
    }
    
    std::string content = "No response";
    if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
        auto& choice = data["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
            content = choice["message"]["content"].get<std::string>();
        }
    }
    
    json citations = json::array();
    if (data.contains("search_results") && data["search_results"].is_array()) {
        for (auto& result : data["search_results"]) {
            if (result.contains("url")) {
                citations.push_back(result["url"]);
            }
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // 构建返回值 (对齐 OpenClaw line 1728-1743)
    json output = {
        {"query", query},
        {"provider", "kimi"},
        {"model", DEFAULT_KIMI_MODEL},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "kimi"},
            {"wrapped", true}
        }},
        {"content", content},
        {"citations", citations}
    };
    
    fprintf(stderr, "[WebSearch] Kimi: %zu citations for '%s' in %ld ms\n", 
            citations.size(), query.c_str(), took_ms);
    
    return core::Result<json>::Ok(output);
}

// ===== Perplexity Search (对齐 OpenClaw runPerplexitySearch line 1249-1303) =====

core::Result<json> WebSearchTool::search_perplexity(
    const std::string& query,
    int count,
    const std::string& freshness,
    const std::string& date_after,
    const std::string& date_before
) {
    const char* api_key_env = std::getenv("PERPLEXITY_API_KEY");
    if (!api_key_env) {
        api_key_env = std::getenv("OPENROUTER_API_KEY");
    }
    if (!api_key_env) {
        return core::Result<json>::Err("PERPLEXITY_API_KEY or OPENROUTER_API_KEY not configured.");
    }
    std::string api_key = api_key_env;
    
    // 自动检测使用哪个 endpoint
    std::string base_url = DEFAULT_PERPLEXITY_BASE_URL;
    std::string api_key_lower = api_key;
    std::transform(api_key_lower.begin(), api_key_lower.end(), api_key_lower.begin(), ::tolower);
    if (api_key_lower.find("pplx-") == 0) {
        base_url = PERPLEXITY_DIRECT_BASE_URL;
    }
    
    std::string endpoint = base_url + "/chat/completions";
    std::string model = DEFAULT_PERPLEXITY_MODEL;
    if (base_url == PERPLEXITY_DIRECT_BASE_URL && model.find("perplexity/") == 0) {
        model = model.substr(11);  // 去掉 "perplexity/" 前缀
    }
    
    // 构建请求 (对齐 OpenClaw line 1261-1273)
    json request_body = {
        {"model", model},
        {"messages", json::array({
            {{"role", "user"}, {"content", query}}
        })}
    };
    
    if (!freshness.empty()) {
        std::string recency = freshness_to_perplexity(freshness);
        if (!recency.empty()) {
            request_body["search_recency_filter"] = recency;
        }
    }
    
    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["Authorization"] = "Bearer " + api_key;
    options.headers["HTTP-Referer"] = "https://hunterclaw.ai";
    options.headers["X-Title"] = "HunterClaw Web Search";
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = request_body.dump();
    
    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(endpoint, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Perplexity API request failed: " + response.error);
    }
    
    // 解析响应 (对齐 OpenClaw extractPerplexityCitations line 427-457)
    json data;
    try {
        data = json::parse(response.unwrap().body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Failed to parse Perplexity response: " + std::string(e.what()));
    }
    
    std::string content = "No response";
    if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
        auto& choice = data["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
            content = choice["message"]["content"].get<std::string>();
        }
    }
    
    json citations = json::array();
    if (data.contains("citations") && data["citations"].is_array()) {
        citations = data["citations"];
    }
    
    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // 构建返回值 (对齐 OpenClaw line 1641-1656)
    json output = {
        {"query", query},
        {"provider", "perplexity"},
        {"model", model},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "perplexity"},
            {"wrapped", true}
        }},
        {"content", content},
        {"citations", citations}
    };
    
    fprintf(stderr, "[WebSearch] Perplexity: %zu citations for '%s' in %ld ms\n", 
            citations.size(), query.c_str(), took_ms);
    
    return core::Result<json>::Ok(output);
}

// ===== 结果标准化 (对齐 OpenClaw line 276-314) =====

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_brave_results(const json& raw_results) {
    std::vector<SearchResult> results;
    
    // 对齐 OpenClaw mapBraveLlmContextResults & line 1857-1870
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
    // Gemini 返回格式不同,使用 content + citations
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_grok_results(const json& raw) {
    // Grok 返回格式不同,使用 content + citations
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_kimi_results(const json& raw) {
    // Kimi 返回格式不同,使用 content + citations
    return {};
}

std::vector<WebSearchTool::SearchResult> WebSearchTool::normalize_perplexity_results(const json& raw) {
    // Perplexity 返回格式不同,使用 content + citations
    return {};
}

// ===== 缓存管理 (对齐 OpenClaw line 318-354) =====

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
    
    // 对齐 OpenClaw normalizeCacheKey (web-shared.ts)
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

// ===== 时间过滤辅助 (对齐 OpenClaw line 358-379) =====

std::string WebSearchTool::freshness_to_brave(const std::string& freshness) const {
    // 对齐 OpenClaw normalizeFreshness line 1095-1129
    if (freshness == "day") return "pd";
    if (freshness == "week") return "pw";
    if (freshness == "month") return "pm";
    if (freshness == "year") return "py";
    // 已经是 Brave 格式
    if (freshness == "pd" || freshness == "pw" || freshness == "pm" || freshness == "py") {
        return freshness;
    }
    return "";
}

std::string WebSearchTool::freshness_to_perplexity(const std::string& freshness) const {
    // Perplexity 使用 day/week/month/year
    // 对齐 OpenClaw FRESHNESS_TO_RECENCY line 114-119
    auto it = FRESHNESS_TO_RECENCY.find(freshness);
    if (it != FRESHNESS_TO_RECENCY.end()) {
        return it->second;
    }
    // 已经是 Perplexity 格式
    if (freshness == "day" || freshness == "week" || freshness == "month" || freshness == "year") {
        return freshness;
    }
    return "";
}

std::string WebSearchTool::normalize_iso_date(const std::string& date) const {
    // 对齐 OpenClaw isValidIsoDate line 1131-1144
    std::regex iso_pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (std::regex_match(date, iso_pattern)) {
        return date;
    }
    return "";
}

// ===== 安全辅助 (对齐 OpenClaw wrapWebContent & line 382-404) =====

json WebSearchTool::wrap_search_results(const std::vector<SearchResult>& results) const {
    json wrapped = json::array();
    
    for (const auto& result : results) {
        json item = {
            {"title", result.title},
            {"url", result.url},
            {"description", result.excerpt}
        };
        
        if (!result.published_date.empty()) {
            item["published"] = result.published_date;
        }
        
        // 标记为外部内容 (对齐 OpenClaw wrapWebContent)
        item["untrusted"] = true;
        item["source"] = "web_search";
        
        wrapped.push_back(item);
    }
    
    return wrapped;
}

std::string WebSearchTool::resolve_citation_url(const std::string& url) const {
    // 对齐 OpenClaw resolveCitationRedirectUrl (web-search-citation-redirect.ts)
    // TODO: 实现 Citation 重定向逻辑
    return url;
}

// ===== Factory =====

std::shared_ptr<Tool> WebSearchToolFactory::create(std::shared_ptr<infra::HttpClient> http_client) {
    return std::make_shared<WebSearchTool>(std::move(http_client));
}

} // namespace tools
} // namespace hunterclaw
