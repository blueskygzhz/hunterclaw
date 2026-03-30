// web_fetch_tool.cc
// Strictly aligned with OpenClaw: src/agents/tools/web-fetch.ts
//
// Tool: web_fetch
// Description: Fetch and extract content from a web page.
//              Returns readable markdown or plain text.

#include "hunterclaw/tools/web_fetch_tool.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <string>
#include <regex>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <optional>
#include <cstdlib>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ============================================================================
// Constants (aligned with OpenClaw web-fetch.ts)
// ============================================================================

// 🎯 对齐 OpenClaw: DEFAULT_MAX_CHARS = 50_000
static constexpr int DEFAULT_MAX_CHARS = 50000;
// 🎯 对齐 OpenClaw: DEFAULT_MAX_RESPONSE_BYTES = 2 * 1024 * 1024
static constexpr int DEFAULT_MAX_RESPONSE_BYTES = 2 * 1024 * 1024;
// 🎯 对齐 OpenClaw: DEFAULT_TIMEOUT_SECONDS = 30
static constexpr int DEFAULT_TIMEOUT_SECONDS = 30;
// 🎯 对齐 OpenClaw: DEFAULT_MAX_REDIRECTS = 3
static constexpr int DEFAULT_MAX_REDIRECTS = 3;
// 🎯 对齐 OpenClaw: DEFAULT_CACHE_TTL_MINUTES = 15
static constexpr int DEFAULT_CACHE_TTL_MINUTES = 15;

// 🎯 对齐 OpenClaw: User-Agent
static const std::string USER_AGENT =
    "Mozilla/5.0 (compatible; OpenClaw/1.0; +https://openclaw.ai)";

// ============================================================================
// Helpers
// ============================================================================

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ============================================================================
// WebFetchTool implementation
// ============================================================================

WebFetchTool::WebFetchTool(std::shared_ptr<infra::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

Tool::Schema WebFetchTool::parameters() const {
    Tool::Schema schema;
    schema.type = "object";
    schema.description = "Fetch and extract content from a web page";
    schema.properties["url"] = core::JsonValue(json{{"type","string"},{"description","URL to fetch"}});
    schema.properties["extractMode"] = core::JsonValue(json{{"type","string"},{"enum",json::array({"markdown","text"})},{"description","Content extraction mode: markdown (default) or text"}});
    schema.properties["maxChars"] = core::JsonValue(json{{"type","number"},{"description","Max chars to return (default 50000)"}});
    schema.properties["maxResponseBytes"] = core::JsonValue(json{{"type","number"},{"description","Max response bytes (default 2MB)"}});
    schema.properties["timeoutSeconds"] = core::JsonValue(json{{"type","number"},{"description","Request timeout in seconds (default 30)"}});
    schema.properties["maxRedirects"] = core::JsonValue(json{{"type","number"},{"description","Max redirects (default 3)"}});
    schema.properties["useFirecrawl"] = core::JsonValue(json{{"type","boolean"},{"description","Use Firecrawl API if available"}});
    schema.required = {"url"};
    return schema;
}

ToolResult WebFetchTool::execute(const core::JsonValue& params_val) {
    json params = params_val.to_json();
    // 🎯 对齐 OpenClaw: readStringParam(params, "url", { required: true })
    if (!params.contains("url") || !params["url"].is_string())
        return tool_result::error("url is required");
    std::string url = trim(params["url"].get<std::string>());
    if (url.empty())
        return tool_result::error("url must not be empty");

    // 🎯 对齐 OpenClaw: SSRF protection
    if (!is_url_safe(url))
        return tool_result::error("URL is not allowed (private/internal addresses blocked)");

    // Parse params
    std::string mode_str = "markdown";
    if (params.contains("extractMode") && params["extractMode"].is_string())
        mode_str = to_lower(trim(params["extractMode"].get<std::string>()));
    ExtractMode mode = parse_extract_mode(mode_str);

    int max_chars = DEFAULT_MAX_CHARS;
    if (params.contains("maxChars") && params["maxChars"].is_number())
        max_chars = std::max(1, (int)params["maxChars"].get<double>());

    int max_response_bytes = DEFAULT_MAX_RESPONSE_BYTES;
    if (params.contains("maxResponseBytes") && params["maxResponseBytes"].is_number())
        max_response_bytes = std::max(1, (int)params["maxResponseBytes"].get<double>());
    (void)max_response_bytes;

    int timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
    if (params.contains("timeoutSeconds") && params["timeoutSeconds"].is_number())
        timeout_seconds = std::max(1, (int)params["timeoutSeconds"].get<double>());

    int max_redirects = DEFAULT_MAX_REDIRECTS;
    if (params.contains("maxRedirects") && params["maxRedirects"].is_number())
        max_redirects = std::max(0, (int)params["maxRedirects"].get<double>());

    bool use_firecrawl = params.value("useFirecrawl", false);

    // 🎯 对齐 OpenClaw: check cache
    std::string ck = cache_key(url, mode, max_chars);
    auto cached = read_cache(ck);
    if (cached.has_value()) {
        return tool_result::json(core::JsonValue(cached.value()));
    }

    // 🎯 对齐 OpenClaw: fetch URL
    core::Result<FetchResult> fetch_result;
    if (use_firecrawl && is_firecrawl_enabled()) {
        fetch_result = fetch_firecrawl(url, timeout_seconds);
    } else {
        fetch_result = fetch_url(url, timeout_seconds, max_redirects);
    }

    if (!fetch_result.ok)
        return tool_result::error(fetch_result.error);

    const auto& fetched = fetch_result.get();

    // 🎯 对齐 OpenClaw: extract content
    std::string content;
    if (mode == ExtractMode::Markdown) {
        std::string readable = extract_readable_content(fetched.content, fetched.final_url);
        content = html_to_markdown(readable);
    } else {
        content = html_to_text(fetched.content);
    }

    // 🎯 对齐 OpenClaw: truncate
    auto truncated = truncate_text(content, max_chars);

    // 🎯 对齐 OpenClaw: wrap_web_content (mark as untrusted)
    json result_json = wrap_web_content(truncated.text, fetched.final_url, mode_str);
    if (truncated.truncated) result_json["truncated"] = true;
    result_json["url"] = fetched.final_url;
    result_json["extractMode"] = mode_str;
    result_json["statusCode"] = fetched.status_code;

    // 🎯 对齐 OpenClaw: write cache
    write_cache(ck, result_json, DEFAULT_CACHE_TTL_MINUTES);

    return tool_result::json(core::JsonValue(result_json));
}

// ============================================================================
// ExtractMode parsing
// ============================================================================

WebFetchTool::ExtractMode WebFetchTool::parse_extract_mode(const std::string& mode_str) const {
    if (mode_str == "text") return ExtractMode::Text;
    return ExtractMode::Markdown;
}

// ============================================================================
// URL fetching
// ============================================================================

core::Result<WebFetchTool::FetchResult> WebFetchTool::fetch_url(
    const std::string& url,
    int timeout_seconds,
    int max_redirects
) {
    if (!http_client_)
        return core::Result<FetchResult>::Err("HTTP client not available");

    infra::HttpRequestOptions opts;
    opts.headers = {
        {"User-Agent",      USER_AGENT},
        {"Accept",          "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-US,en;q=0.5"},
    };
    opts.timeout_ms = timeout_seconds * 1000;
    opts.max_redirects = max_redirects;

    auto resp_result = http_client_->get(url, opts);
    if (!resp_result.ok)
        return core::Result<FetchResult>::Err("HTTP fetch failed: " + resp_result.error);

    auto& response = resp_result.get();
    if (!response.ok)
        return core::Result<FetchResult>::Err("HTTP fetch failed: status " + std::to_string(response.status_code));

    std::string body = response.body;
    // Check response size
    if ((int)body.size() > DEFAULT_MAX_RESPONSE_BYTES) {
        // Truncate to max
        body = body.substr(0, DEFAULT_MAX_RESPONSE_BYTES);
    }

    // Extract content-type from headers
    std::string content_type;
    auto ct_it = response.headers.find("content-type");
    if (ct_it == response.headers.end())
        ct_it = response.headers.find("Content-Type");
    if (ct_it != response.headers.end())
        content_type = ct_it->second;

    FetchResult result;
    result.content      = body;
    result.final_url    = url;
    result.content_type = content_type;
    result.status_code  = response.status_code;
    return core::Result<FetchResult>::Ok(result);
}

core::Result<WebFetchTool::FetchResult> WebFetchTool::fetch_firecrawl(
    const std::string& url,
    int timeout_seconds
) {
    auto api_key = get_firecrawl_api_key();
    if (!api_key.has_value())
        return core::Result<FetchResult>::Err("Firecrawl API key not found");

    std::string base_url = get_firecrawl_base_url();
    json body = {
        {"url",     url},
        {"formats", json::array({"markdown"})},
    };

    if (!http_client_)
        return core::Result<FetchResult>::Err("HTTP client not available");

    infra::HttpRequestOptions fc_opts;
    fc_opts.headers = {
        {"Authorization", "Bearer " + api_key.value()},
        {"Content-Type",  "application/json"},
    };
    fc_opts.body = body.dump();
    fc_opts.timeout_ms = timeout_seconds * 1000;

    auto resp_result = http_client_->post(base_url + "/v1/scrape", fc_opts);
    if (!resp_result.ok)
        return core::Result<FetchResult>::Err("Firecrawl failed: " + resp_result.error);

    auto& response = resp_result.get();
    if (!response.ok)
        return core::Result<FetchResult>::Err("Firecrawl failed: status " + std::to_string(response.status_code));
    // Parse Firecrawl response
    try {
        json resp = json::parse(response.body);
        std::string markdown;
        if (resp.contains("data") && resp["data"].contains("markdown"))
            markdown = resp["data"]["markdown"].get<std::string>();
        else if (resp.contains("markdown"))
            markdown = resp["markdown"].get<std::string>();

        FetchResult result;
        result.content      = markdown;
        result.final_url    = url;
        result.content_type = "text/markdown";
        result.status_code  = 200;
        return core::Result<FetchResult>::Ok(result);
    } catch (const std::exception& e) {
        return core::Result<FetchResult>::Err(
            std::string("Firecrawl response parse failed: ") + e.what());
    }
}

// ============================================================================
// Content extraction
// ============================================================================

std::string WebFetchTool::extract_readable_content(
    const std::string& html,
    const std::string& /*url*/
) const {
    // 🎯 对齐 OpenClaw: Readability.js equivalent
    // Simple heuristic: extract <main>, <article>, or <body> content
    // Remove scripts, styles, nav, footer, header

    std::string result = html;

    // Remove script tags
    static const std::regex script_re{R"(<script[^>]*>[\s\S]*?</script>)",
                                       std::regex_constants::icase};
    result = std::regex_replace(result, script_re, "");

    // Remove style tags
    static const std::regex style_re{R"(<style[^>]*>[\s\S]*?</style>)",
                                      std::regex_constants::icase};
    result = std::regex_replace(result, style_re, "");

    // Remove nav, header, footer
    static const std::regex nav_re{R"(<(nav|header|footer|aside)[^>]*>[\s\S]*?</\1>)",
                                    std::regex_constants::icase};
    result = std::regex_replace(result, nav_re, "");

    // Try to extract <main> or <article>
    static const std::regex main_re{R"(<(main|article)[^>]*>([\s\S]*?)</(main|article)>)",
                                     std::regex_constants::icase};
    std::smatch m;
    if (std::regex_search(result, m, main_re)) {
        result = m[2].str();
    }

    return result;
}

std::string WebFetchTool::html_to_markdown(const std::string& html) const {
    // 🎯 对齐 OpenClaw: Turndown equivalent
    // Convert common HTML elements to Markdown

    std::string result = html;

    // Headers
    for (int i = 6; i >= 1; --i) {
        std::string tag = "h" + std::to_string(i);
        std::string prefix(i, '#');
        std::regex h_re{"<" + tag + "[^>]*>(.*?)</" + tag + ">",
                        std::regex_constants::icase};
        result = std::regex_replace(result, h_re, prefix + " $1\n\n");
    }

    // Bold
    static const std::regex bold_re{R"(<(strong|b)[^>]*>(.*?)</(strong|b)>)",
                                     std::regex_constants::icase};
    result = std::regex_replace(result, bold_re, "**$2**");

    // Italic
    static const std::regex italic_re{R"(<(em|i)[^>]*>(.*?)</(em|i)>)",
                                       std::regex_constants::icase};
    result = std::regex_replace(result, italic_re, "_$2_");

    // Links
    static const std::regex link_re{R"re(<a[^>]*href="([^"]*)"[^>]*>(.*?)</a>)re",
                                     std::regex_constants::icase};
    result = std::regex_replace(result, link_re, "[$2]($1)");

    // Images
    static const std::regex img_re{R"re(<img[^>]*alt="([^"]*)"[^>]*src="([^"]*)"[^>]*>)re",
                                    std::regex_constants::icase};
    result = std::regex_replace(result, img_re, "![$1]($2)");
    // Code blocks
    static const std::regex pre_re{R"(<pre[^>]*><code[^>]*>([\s\S]*?)</code></pre>)",
                                    std::regex_constants::icase};
    result = std::regex_replace(result, pre_re, "```\n$1\n```\n");

    // Inline code
    static const std::regex code_re{R"(<code[^>]*>(.*?)</code>)",
                                     std::regex_constants::icase};
    result = std::regex_replace(result, code_re, "`$1`");

    // Paragraphs
    static const std::regex p_re{R"(<p[^>]*>([\s\S]*?)</p>)",
                                  std::regex_constants::icase};
    result = std::regex_replace(result, p_re, "$1\n\n");

    // Line breaks
    static const std::regex br_re{R"(<br\s*/?>)", std::regex_constants::icase};
    result = std::regex_replace(result, br_re, "\n");

    // List items
    static const std::regex li_re{R"(<li[^>]*>([\s\S]*?)</li>)",
                                   std::regex_constants::icase};
    result = std::regex_replace(result, li_re, "- $1\n");

    // Remove remaining HTML tags
    static const std::regex tag_re{R"(<[^>]+>)"};
    result = std::regex_replace(result, tag_re, "");

    // Decode HTML entities
    static const std::vector<std::pair<std::string, std::string>> entities = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "},
        {"&mdash;", "—"}, {"&ndash;", "–"}, {"&hellip;", "…"},
    };
    for (const auto& [entity, ch] : entities) {
        size_t pos = 0;
        while ((pos = result.find(entity, pos)) != std::string::npos) {
            result.replace(pos, entity.size(), ch);
            pos += ch.size();
        }
    }

    // Collapse multiple blank lines
    static const std::regex multi_blank{R"(\n{3,})"};
    result = std::regex_replace(result, multi_blank, "\n\n");

    return trim(result);
}

std::string WebFetchTool::html_to_text(const std::string& html) const {
    // Remove all HTML tags
    static const std::regex tag_re{R"(<[^>]+>)"};
    std::string result = std::regex_replace(html, tag_re, " ");

    // Decode HTML entities
    static const std::vector<std::pair<std::string, std::string>> entities = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "},
    };
    for (const auto& [entity, ch] : entities) {
        size_t pos = 0;
        while ((pos = result.find(entity, pos)) != std::string::npos) {
            result.replace(pos, entity.size(), ch);
            pos += ch.size();
        }
    }

    // Collapse whitespace
    static const std::regex ws_re{R"(\s{2,})"};
    result = std::regex_replace(result, ws_re, " ");

    return trim(result);
}

std::string WebFetchTool::markdown_to_text(const std::string& markdown) const {
    // Remove Markdown formatting
    std::string result = markdown;
    static const std::regex md_re{R"(\*\*([^*]+)\*\*|__([^_]+)__|_([^_]+)_|\*([^*]+)\*|`([^`]+)`)"};
    result = std::regex_replace(result, md_re, "$1$2$3$4$5");
    static const std::regex link_re{R"re(\[([^\]]+)\]\([^)]+\))re"};
    result = std::regex_replace(result, link_re, "$1");
    return trim(result);
}

// ============================================================================
// Truncation
// ============================================================================

WebFetchTool::TruncateResult WebFetchTool::truncate_text(
    const std::string& text, int max_chars
) const {
    if ((int)text.size() <= max_chars)
        return {text, false};
    return {text.substr(0, max_chars) + "\n…(truncated)…", true};
}

// ============================================================================
// Cache
// ============================================================================

std::string WebFetchTool::cache_key(
    const std::string& url,
    ExtractMode mode,
    int max_chars
) const {
    return url + "|" + (mode == ExtractMode::Markdown ? "md" : "txt") +
           "|" + std::to_string(max_chars);
}

std::optional<json> WebFetchTool::read_cache(const std::string& key) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;
    auto now = std::chrono::system_clock::now();
    if (now > it->second.expires_at) {
        cache_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

void WebFetchTool::write_cache(const std::string& key, const json& value, int ttl_minutes) {
    CacheEntry entry;
    entry.value = value;
    entry.expires_at = std::chrono::system_clock::now() +
                       std::chrono::minutes(ttl_minutes);
    cache_[key] = entry;
}

// ============================================================================
// SSRF protection
// ============================================================================

bool WebFetchTool::is_url_safe(const std::string& url) const {
    // 🎯 对齐 OpenClaw: SSRF protection
    std::string lower = to_lower(url);

    // Block non-http(s) schemes
    if (lower.substr(0, 7) != "http://" && lower.substr(0, 8) != "https://")
        return false;

    // Block localhost
    if (lower.find("localhost") != std::string::npos ||
        lower.find("127.0.0.1") != std::string::npos ||
        lower.find("::1") != std::string::npos)
        return false;

    // Block private IP ranges
    static const std::vector<std::string> private_prefixes = {
        "http://10.", "https://10.",
        "http://192.168.", "https://192.168.",
        "http://172.16.", "https://172.16.",
        "http://172.17.", "https://172.17.",
        "http://172.18.", "https://172.18.",
        "http://172.19.", "https://172.19.",
        "http://172.2",   "https://172.2",
        "http://172.3",   "https://172.3",
        "http://169.254.", "https://169.254.",
    };
    for (const auto& prefix : private_prefixes) {
        if (lower.substr(0, prefix.size()) == prefix) return false;
    }

    return true;
}

bool WebFetchTool::is_private_ip(const std::string& ip) const {
    return ip.substr(0, 3) == "10." ||
           ip.substr(0, 8) == "192.168." ||
           ip.substr(0, 7) == "172.16." ||
           ip == "127.0.0.1" ||
           ip == "::1";
}

// ============================================================================
// Security wrapping
// ============================================================================

json WebFetchTool::wrap_web_content(
    const std::string& content,
    const std::string& url,
    const std::string& mode
) const {
    // 🎯 对齐 OpenClaw: wrap external content as untrusted
    return json{
        {"content",     content},
        {"url",         url},
        {"extractMode", mode},
        {"untrusted",   true},
    };
}

// ============================================================================
// Firecrawl
// ============================================================================

bool WebFetchTool::is_firecrawl_enabled() const {
    return get_firecrawl_api_key().has_value();
}

std::optional<std::string> WebFetchTool::get_firecrawl_api_key() const {
    const char* val = std::getenv("FIRECRAWL_API_KEY");
    if (!val) return std::nullopt;
    std::string s = trim(std::string(val));
    if (s.empty()) return std::nullopt;
    return s;
}

std::string WebFetchTool::get_firecrawl_base_url() const {
    const char* val = std::getenv("FIRECRAWL_BASE_URL");
    if (val) {
        std::string s = trim(std::string(val));
        if (!s.empty()) return s;
    }
    return "https://api.firecrawl.dev";
}

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> WebFetchToolFactory::create(
    std::shared_ptr<infra::HttpClient> http_client
) {
    return std::make_shared<WebFetchTool>(std::move(http_client));
}

} // namespace tools
} // namespace hunterclaw
