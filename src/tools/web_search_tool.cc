// 🎯 严格对齐 OpenClaw web-search.ts
// Reference: openclaw-analysis/src/agents/tools/web-search.ts
#include "hunterclaw/tools/web_search_tool.h"
#include <regex>
#include <cstdio>
#include <algorithm>
#include <set>
#include <chrono>
#include <sstream>

namespace hunterclaw {
namespace tools {

// ============================================================================
// 常量 (对齐 OpenClaw web-search.ts)
// ============================================================================

static const std::vector<std::string> SEARCH_PROVIDERS = {
    "brave", "gemini", "grok", "kimi", "perplexity"
};

// 🎯 对齐 OpenClaw: const DEFAULT_SEARCH_COUNT = 5
static constexpr int DEFAULT_SEARCH_COUNT = 5;
// 🎯 对齐 OpenClaw: const MAX_SEARCH_COUNT = 10
static constexpr int MAX_SEARCH_COUNT = 10;
// 🎯 对齐 OpenClaw: DEFAULT_TIMEOUT_SECONDS (from web-shared.ts)
static constexpr int DEFAULT_TIMEOUT_SECONDS = 30;
// 🎯 对齐 OpenClaw: DEFAULT_CACHE_TTL_MINUTES (from web-shared.ts)
static constexpr int DEFAULT_CACHE_TTL_MINUTES = 15;

// 🎯 对齐 OpenClaw: const BRAVE_SEARCH_ENDPOINT
static const std::string BRAVE_SEARCH_ENDPOINT = "https://api.search.brave.com/res/v1/web/search";
// 🎯 对齐 OpenClaw: const BRAVE_LLM_CONTEXT_ENDPOINT
static const std::string BRAVE_LLM_CONTEXT_ENDPOINT = "https://api.search.brave.com/res/v1/llm/context";
// 🎯 对齐 OpenClaw: const DEFAULT_PERPLEXITY_BASE_URL
static const std::string DEFAULT_PERPLEXITY_BASE_URL = "https://openrouter.ai/api/v1";
// 🎯 对齐 OpenClaw: const PERPLEXITY_DIRECT_BASE_URL
static const std::string PERPLEXITY_DIRECT_BASE_URL = "https://api.perplexity.ai";
// 🎯 对齐 OpenClaw: const PERPLEXITY_SEARCH_ENDPOINT
static const std::string PERPLEXITY_SEARCH_ENDPOINT = "https://api.perplexity.ai/search";
// 🎯 对齐 OpenClaw: const DEFAULT_PERPLEXITY_MODEL
static const std::string DEFAULT_PERPLEXITY_MODEL = "perplexity/sonar-pro";
// 🎯 对齐 OpenClaw: const PERPLEXITY_KEY_PREFIXES = ["pplx-"]
static const std::vector<std::string> PERPLEXITY_KEY_PREFIXES = {"pplx-"};
// 🎯 对齐 OpenClaw: const OPENROUTER_KEY_PREFIXES = ["sk-or-"]
static const std::vector<std::string> OPENROUTER_KEY_PREFIXES = {"sk-or-"};

// 🎯 对齐 OpenClaw: const XAI_API_ENDPOINT
static const std::string XAI_API_ENDPOINT = "https://api.x.ai/v1/responses";
// 🎯 对齐 OpenClaw: const DEFAULT_GROK_MODEL
static const std::string DEFAULT_GROK_MODEL = "grok-4-1-fast";

// 🎯 对齐 OpenClaw: const DEFAULT_KIMI_BASE_URL
static const std::string DEFAULT_KIMI_BASE_URL = "https://api.moonshot.ai/v1";
// 🎯 对齐 OpenClaw: const DEFAULT_KIMI_MODEL
static const std::string DEFAULT_KIMI_MODEL = "moonshot-v1-128k";
// 🎯 对齐 OpenClaw: const KIMI_WEB_SEARCH_TOOL
static const json KIMI_WEB_SEARCH_TOOL = {
    {"type", "builtin_function"},
    {"function", {{"name", "$web_search"}}}
};

// 🎯 对齐 OpenClaw: const DEFAULT_GEMINI_MODEL
static const std::string DEFAULT_GEMINI_MODEL = "gemini-2.5-flash";
// 🎯 对齐 OpenClaw: const GEMINI_API_BASE
static const std::string GEMINI_API_BASE = "https://generativelanguage.googleapis.com/v1beta";

// 🎯 对齐 OpenClaw: const BRAVE_FRESHNESS_SHORTCUTS = new Set(["pd", "pw", "pm", "py"])
static const std::set<std::string> BRAVE_FRESHNESS_SHORTCUTS = {"pd", "pw", "pm", "py"};

// 🎯 对齐 OpenClaw: const PERPLEXITY_RECENCY_VALUES = new Set(["day", "week", "month", "year"])
static const std::set<std::string> PERPLEXITY_RECENCY_VALUES = {"day", "week", "month", "year"};

// 🎯 对齐 OpenClaw: const FRESHNESS_TO_RECENCY
static const std::unordered_map<std::string, std::string> FRESHNESS_TO_RECENCY = {
    {"pd", "day"}, {"pw", "week"}, {"pm", "month"}, {"py", "year"}
};
// 🎯 对齐 OpenClaw: const RECENCY_TO_FRESHNESS
static const std::unordered_map<std::string, std::string> RECENCY_TO_FRESHNESS = {
    {"day", "pd"}, {"week", "pw"}, {"month", "pm"}, {"year", "py"}
};

// 🎯 对齐 OpenClaw: const BRAVE_SEARCH_LANG_CODES
static const std::set<std::string> BRAVE_SEARCH_LANG_CODES = {
    "ar","eu","bn","bg","ca","zh-hans","zh-hant","hr","cs","da","nl",
    "en","en-gb","et","fi","fr","gl","de","el","gu","he","hi","hu","is",
    "it","jp","kn","ko","lv","lt","ms","ml","mr","nb","pl","pt-br","pt-pt",
    "pa","ro","ru","sr","sk","sl","es","sv","ta","te","th","tr","uk","vi"
};

// 🎯 对齐 OpenClaw: const BRAVE_SEARCH_LANG_ALIASES
static const std::unordered_map<std::string, std::string> BRAVE_SEARCH_LANG_ALIASES = {
    {"ja", "jp"}, {"zh", "zh-hans"}, {"zh-cn", "zh-hans"},
    {"zh-hk", "zh-hant"}, {"zh-sg", "zh-hans"}, {"zh-tw", "zh-hant"}
};

// ============================================================================
// 辅助函数
// ============================================================================

// 🎯 对齐 OpenClaw: function resolveSiteName(url)
static std::string resolve_site_name(const std::string& url) {
    if (url.empty()) return "";
    // 简单提取 hostname
    std::string s = url;
    // 去掉协议
    auto pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    // 去掉路径
    pos = s.find('/');
    if (pos != std::string::npos) s = s.substr(0, pos);
    // 去掉端口
    pos = s.find(':');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

// 🎯 对齐 OpenClaw: function isValidIsoDate(value)
static bool is_valid_iso_date(const std::string& value) {
    std::regex pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (!std::regex_match(value, pattern)) return false;
    int year = std::stoi(value.substr(0, 4));
    int month = std::stoi(value.substr(5, 2));
    int day = std::stoi(value.substr(8, 2));
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    // 简单校验
    static const int days_in_month[] = {0,31,29,31,30,31,30,31,31,30,31,30,31};
    if (day > days_in_month[month]) return false;
    (void)year;
    return true;
}

// 🎯 对齐 OpenClaw: function normalizeToIsoDate(value)
// Accepts YYYY-MM-DD or M/D/YYYY (Perplexity format)
static std::string normalize_to_iso_date(const std::string& value) {
    std::string trimmed = value;
    // trim
    auto s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    auto e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);

    // ISO format: YYYY-MM-DD
    std::regex iso_pattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (std::regex_match(trimmed, iso_pattern)) {
        return is_valid_iso_date(trimmed) ? trimmed : "";
    }

    // Perplexity format: M/D/YYYY
    std::regex pplx_pattern(R"(^(\d{1,2})/(\d{1,2})/(\d{4})$)");
    std::smatch m;
    if (std::regex_match(trimmed, m, pplx_pattern)) {
        std::string month = m[1].str();
        std::string day = m[2].str();
        std::string year = m[3].str();
        if (month.size() < 2) month = "0" + month;
        if (day.size() < 2) day = "0" + day;
        std::string iso = year + "-" + month + "-" + day;
        return is_valid_iso_date(iso) ? iso : "";
    }
    return "";
}

// 🎯 对齐 OpenClaw: function isoToPerplexityDate(iso)
static std::string iso_to_perplexity_date(const std::string& iso) {
    std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})$)");
    std::smatch m;
    if (!std::regex_match(iso, m, pattern)) return "";
    int year = std::stoi(m[1].str());
    int month = std::stoi(m[2].str());
    int day = std::stoi(m[3].str());
    return std::to_string(month) + "/" + std::to_string(day) + "/" + std::to_string(year);
}

// 🎯 对齐 OpenClaw: function normalizeFreshness(value, provider)
// Accepts both Brave format (pd/pw/pm/py) and Perplexity format (day/week/month/year)
// For Brave, also accepts date ranges (YYYY-MM-DDtoYYYY-MM-DD)
static std::string normalize_freshness(const std::string& value, const std::string& provider) {
    if (value.empty()) return "";
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Brave shortcut: pd/pw/pm/py
    if (BRAVE_FRESHNESS_SHORTCUTS.count(lower)) {
        if (provider == "brave") return lower;
        auto it = FRESHNESS_TO_RECENCY.find(lower);
        return it != FRESHNESS_TO_RECENCY.end() ? it->second : "";
    }

    // Perplexity recency: day/week/month/year
    if (PERPLEXITY_RECENCY_VALUES.count(lower)) {
        if (provider == "perplexity") return lower;
        auto it = RECENCY_TO_FRESHNESS.find(lower);
        return it != RECENCY_TO_FRESHNESS.end() ? it->second : "";
    }

    // Brave date range: YYYY-MM-DDtoYYYY-MM-DD
    if (provider == "brave") {
        std::regex range_pattern(R"(^(\d{4}-\d{2}-\d{2})to(\d{4}-\d{2}-\d{2})$)");
        std::smatch m;
        if (std::regex_match(trimmed, m, range_pattern)) {
            std::string start = m[1].str();
            std::string end = m[2].str();
            if (is_valid_iso_date(start) && is_valid_iso_date(end) && start <= end) {
                return start + "to" + end;
            }
        }
    }

    return "";
}

// 🎯 对齐 OpenClaw: function normalizeBraveSearchLang(value)
static std::string normalize_brave_search_lang(const std::string& value) {
    if (value.empty()) return "";
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Check aliases
    auto alias_it = BRAVE_SEARCH_LANG_ALIASES.find(lower);
    std::string canonical = (alias_it != BRAVE_SEARCH_LANG_ALIASES.end()) ? alias_it->second : lower;

    if (BRAVE_SEARCH_LANG_CODES.count(canonical)) return canonical;
    return "";
}

// 🎯 对齐 OpenClaw: function normalizeBraveUiLang(value)
// Must be language-region format: en-US, de-DE, etc.
static std::string normalize_brave_ui_lang(const std::string& value) {
    if (value.empty()) return "";
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);

    std::regex locale_pattern(R"(^([a-zA-Z]{2})-([a-zA-Z]{2})$)");
    std::smatch m;
    if (!std::regex_match(trimmed, m, locale_pattern)) return "";
    std::string lang = m[1].str();
    std::string region = m[2].str();
    std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
    std::transform(region.begin(), region.end(), region.begin(), ::toupper);
    return lang + "-" + region;
}

// 🎯 对齐 OpenClaw: function normalizeBraveLanguageParams(params)
struct BraveLanguageParams {
    std::string search_lang;  // empty = undefined
    std::string ui_lang;      // empty = undefined
    std::string invalid_field; // "search_lang" | "ui_lang" | ""
};

static BraveLanguageParams normalize_brave_language_params(
    const std::string& raw_search_lang,
    const std::string& raw_ui_lang
) {
    BraveLanguageParams result;
    std::string search_lang_candidate = raw_search_lang;
    std::string ui_lang_candidate = raw_ui_lang;

    // Recover common LLM mix-up: locale in search_lang + short code in ui_lang
    if (!normalize_brave_ui_lang(raw_search_lang).empty() &&
        !normalize_brave_search_lang(raw_ui_lang).empty()) {
        search_lang_candidate = raw_ui_lang;
        ui_lang_candidate = raw_search_lang;
    }

    if (!search_lang_candidate.empty()) {
        result.search_lang = normalize_brave_search_lang(search_lang_candidate);
        if (result.search_lang.empty()) {
            result.invalid_field = "search_lang";
            return result;
        }
    }

    if (!ui_lang_candidate.empty()) {
        result.ui_lang = normalize_brave_ui_lang(ui_lang_candidate);
        if (result.ui_lang.empty()) {
            result.invalid_field = "ui_lang";
            return result;
        }
    }

    return result;
}

// 🎯 对齐 OpenClaw: function resolveSearchCount(value, fallback)
static int resolve_search_count(int value, int fallback) {
    int parsed = (value > 0) ? value : fallback;
    return std::max(1, std::min(MAX_SEARCH_COUNT, parsed));
}

// 🎯 对齐 OpenClaw: function normalizeApiKey(key)
static std::string normalize_api_key(const char* key) {
    if (!key) return "";
    std::string s = key;
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 🎯 对齐 OpenClaw: function inferPerplexityBaseUrlFromApiKey(apiKey)
// Returns "direct" | "openrouter" | ""
static std::string infer_perplexity_base_url_from_api_key(const std::string& api_key) {
    if (api_key.empty()) return "";
    std::string lower = api_key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& prefix : PERPLEXITY_KEY_PREFIXES) {
        if (lower.substr(0, prefix.size()) == prefix) return "direct";
    }
    for (const auto& prefix : OPENROUTER_KEY_PREFIXES) {
        if (lower.substr(0, prefix.size()) == prefix) return "openrouter";
    }
    return "";
}

// 🎯 对齐 OpenClaw: function isDirectPerplexityBaseUrl(baseUrl)
static bool is_direct_perplexity_base_url(const std::string& base_url) {
    std::string trimmed = base_url;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    // Check if hostname is api.perplexity.ai
    auto pos = trimmed.find("://");
    std::string rest = (pos != std::string::npos) ? trimmed.substr(pos + 3) : trimmed;
    auto slash = rest.find('/');
    std::string hostname = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    std::transform(hostname.begin(), hostname.end(), hostname.begin(), ::tolower);
    return hostname == "api.perplexity.ai";
}

// 🎯 对齐 OpenClaw: function resolvePerplexityRequestModel(baseUrl, model)
static std::string resolve_perplexity_request_model(const std::string& base_url, const std::string& model) {
    if (!is_direct_perplexity_base_url(base_url)) return model;
    if (model.substr(0, 11) == "perplexity/") return model.substr(11);
    return model;
}

// 🎯 对齐 OpenClaw: missingSearchKeyPayload(provider)
static json missing_search_key_payload(const std::string& provider) {
    if (provider == "brave") {
        return {
            {"error", "missing_brave_api_key"},
            {"message", "web_search (brave) needs a Brave Search API key. Set BRAVE_API_KEY in the environment."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        };
    }
    if (provider == "gemini") {
        return {
            {"error", "missing_gemini_api_key"},
            {"message", "web_search (gemini) needs an API key. Set GEMINI_API_KEY in the Gateway environment, or configure tools.web.search.gemini.apiKey."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        };
    }
    if (provider == "grok") {
        return {
            {"error", "missing_xai_api_key"},
            {"message", "web_search (grok) needs an xAI API key. Set XAI_API_KEY in the Gateway environment, or configure tools.web.search.grok.apiKey."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        };
    }
    if (provider == "kimi") {
        return {
            {"error", "missing_kimi_api_key"},
            {"message", "web_search (kimi) needs a Moonshot API key. Set KIMI_API_KEY or MOONSHOT_API_KEY in the Gateway environment, or configure tools.web.search.kimi.apiKey."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        };
    }
    return {
        {"error", "missing_perplexity_api_key"},
        {"message", "web_search (perplexity) needs an API key. Set PERPLEXITY_API_KEY or OPENROUTER_API_KEY in the Gateway environment, or configure tools.web.search.perplexity.apiKey."},
        {"docs", "https://docs.openclaw.ai/tools/web"}
    };
}

// 🎯 对齐 OpenClaw: wrapWebContent(content, "web_search")
// In C++ we just return the content as-is (security tagging is done at the payload level)
static std::string wrap_web_content(const std::string& content) {
    return content;
}

// 🎯 对齐 OpenClaw: normalizeCacheKey(key)
static std::string normalize_cache_key(const std::string& key) {
    // Simple normalization: lowercase
    std::string result = key;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// ============================================================================
// 构造函数
// ============================================================================

WebSearchTool::WebSearchTool(std::shared_ptr<infra::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

// ============================================================================
// Schema
// ============================================================================

Tool::Schema WebSearchTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.required.push_back("query");
    return schema;
}

// ============================================================================
// Execute (对齐 OpenClaw createWebSearchTool execute)
// ============================================================================

ToolResult WebSearchTool::execute(const core::JsonValue& params) {
    if (!params.is_object() || !params.contains("query") || !params["query"].is_string()) {
        ToolResult result;
        result.success = false;
        result.error = "Missing required parameter: query";
        return result;
    }

    std::string query = params["query"].as_string();
    // trim query
    {
        auto s = query.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) {
            ToolResult r; r.success = false; r.error = "query must not be empty"; return r;
        }
        auto e = query.find_last_not_of(" \t\r\n");
        query = query.substr(s, e - s + 1);
    }

    // 🎯 对齐 OpenClaw: readNumberParam(params, "count")
    int count_raw = 0;
    if (params.contains("count") && params["count"].is_number()) {
        count_raw = static_cast<int>(params["count"].as_number());
    }

    // 🎯 对齐 OpenClaw: provider from args (runtime override)
    std::string provider_str = "";
    if (params.contains("provider") && params["provider"].is_string()) {
        provider_str = params["provider"].as_string();
        auto s = provider_str.find_first_not_of(" \t");
        if (s == std::string::npos) provider_str = "";
        else {
            auto e = provider_str.find_last_not_of(" \t");
            provider_str = provider_str.substr(s, e - s + 1);
            std::transform(provider_str.begin(), provider_str.end(), provider_str.begin(), ::tolower);
        }
    }

    // 🎯 对齐 OpenClaw: resolveSearchProvider
    std::string provider = select_provider_str(provider_str);

    // 🎯 对齐 OpenClaw: resolve API key per provider
    std::string api_key = resolve_api_key(provider);
    if (api_key.empty()) {
        return make_json_result(missing_search_key_payload(provider));
    }

    // 🎯 对齐 OpenClaw: country param
    std::string country = "";
    if (params.contains("country") && params["country"].is_string()) {
        country = params["country"].as_string();
        auto s = country.find_first_not_of(" \t");
        if (s == std::string::npos) country = "";
        else { auto e = country.find_last_not_of(" \t"); country = country.substr(s, e - s + 1); }
    }
    // 🎯 对齐 OpenClaw: country only supported by brave and perplexity search_api
    if (!country.empty() && provider != "brave" && provider != "perplexity") {
        return make_json_result({
            {"error", "unsupported_country"},
            {"message", "country filtering is not supported by the " + provider + " provider. Only Brave and Perplexity support country filtering."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }

    // 🎯 对齐 OpenClaw: language param
    std::string language = "";
    if (params.contains("language") && params["language"].is_string()) {
        language = params["language"].as_string();
        auto s = language.find_first_not_of(" \t");
        if (s == std::string::npos) language = "";
        else { auto e = language.find_last_not_of(" \t"); language = language.substr(s, e - s + 1); }
    }
    if (!language.empty() && provider != "brave" && provider != "perplexity") {
        return make_json_result({
            {"error", "unsupported_language"},
            {"message", "language filtering is not supported by the " + provider + " provider. Only Brave and Perplexity support language filtering."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    // 🎯 对齐 OpenClaw: perplexity language must be 2-letter ISO 639-1
    if (!language.empty() && provider == "perplexity") {
        std::regex lang_pattern(R"(^[a-zA-Z]{2}$)");
        if (!std::regex_match(language, lang_pattern)) {
            return make_json_result({
                {"error", "invalid_language"},
                {"message", "language must be a 2-letter ISO 639-1 code like 'en', 'de', or 'fr'."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
    }

    // 🎯 对齐 OpenClaw: search_lang / ui_lang (Brave-specific)
    std::string search_lang_raw = "";
    std::string ui_lang_raw = "";
    if (params.contains("search_lang") && params["search_lang"].is_string()) {
        search_lang_raw = params["search_lang"].as_string();
    }
    if (params.contains("ui_lang") && params["ui_lang"].is_string()) {
        ui_lang_raw = params["ui_lang"].as_string();
    }

    // 🎯 对齐 OpenClaw: normalizeBraveLanguageParams
    std::string resolved_search_lang = "";
    std::string resolved_ui_lang = "";
    if (provider == "brave") {
        // For Brave, accept both `language` (unified) and `search_lang`
        auto brave_lang = normalize_brave_language_params(
            !search_lang_raw.empty() ? search_lang_raw : language,
            ui_lang_raw
        );
        if (brave_lang.invalid_field == "search_lang") {
            return make_json_result({
                {"error", "invalid_search_lang"},
                {"message", "search_lang must be a Brave-supported language code like 'en', 'en-gb', 'zh-hans', or 'zh-hant'."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
        if (brave_lang.invalid_field == "ui_lang") {
            return make_json_result({
                {"error", "invalid_ui_lang"},
                {"message", "ui_lang must be a language-region locale like 'en-US'."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
        resolved_search_lang = brave_lang.search_lang;
        resolved_ui_lang = brave_lang.ui_lang;
    } else {
        resolved_search_lang = language;
    }

    // 🎯 对齐 OpenClaw: braveMode
    std::string brave_mode = resolve_brave_mode();
    if (!resolved_ui_lang.empty() && provider == "brave" && brave_mode == "llm-context") {
        return make_json_result({
            {"error", "unsupported_ui_lang"},
            {"message", "ui_lang is not supported by Brave llm-context mode. Remove ui_lang or use Brave web mode for locale-based UI hints."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }

    // 🎯 对齐 OpenClaw: freshness param
    std::string raw_freshness = "";
    if (params.contains("freshness") && params["freshness"].is_string()) {
        raw_freshness = params["freshness"].as_string();
        auto s = raw_freshness.find_first_not_of(" \t");
        if (s == std::string::npos) raw_freshness = "";
        else { auto e = raw_freshness.find_last_not_of(" \t"); raw_freshness = raw_freshness.substr(s, e - s + 1); }
    }
    if (!raw_freshness.empty() && provider != "brave" && provider != "perplexity") {
        return make_json_result({
            {"error", "unsupported_freshness"},
            {"message", "freshness filtering is not supported by the " + provider + " provider. Only Brave and Perplexity support freshness."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if (!raw_freshness.empty() && provider == "brave" && brave_mode == "llm-context") {
        return make_json_result({
            {"error", "unsupported_freshness"},
            {"message", "freshness filtering is not supported by Brave llm-context mode. Remove freshness or use Brave web mode."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    std::string freshness = "";
    if (!raw_freshness.empty()) {
        freshness = normalize_freshness(raw_freshness, provider);
        if (freshness.empty()) {
            return make_json_result({
                {"error", "invalid_freshness"},
                {"message", "freshness must be day, week, month, or year."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
    }

    // 🎯 对齐 OpenClaw: date_after / date_before
    std::string raw_date_after = "";
    std::string raw_date_before = "";
    if (params.contains("date_after") && params["date_after"].is_string()) {
        raw_date_after = params["date_after"].as_string();
    }
    if (params.contains("date_before") && params["date_before"].is_string()) {
        raw_date_before = params["date_before"].as_string();
    }

    // 🎯 对齐 OpenClaw: freshness and date_after/date_before cannot be used together
    if (!raw_freshness.empty() && (!raw_date_after.empty() || !raw_date_before.empty())) {
        return make_json_result({
            {"error", "conflicting_time_filters"},
            {"message", "freshness and date_after/date_before cannot be used together. Use either freshness (day/week/month/year) or a date range (date_after/date_before), not both."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if ((!raw_date_after.empty() || !raw_date_before.empty()) && provider != "brave" && provider != "perplexity") {
        return make_json_result({
            {"error", "unsupported_date_filter"},
            {"message", "date_after/date_before filtering is not supported by the " + provider + " provider. Only Brave and Perplexity support date filtering."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if ((!raw_date_after.empty() || !raw_date_before.empty()) && provider == "brave" && brave_mode == "llm-context") {
        return make_json_result({
            {"error", "unsupported_date_filter"},
            {"message", "date_after/date_before filtering is not supported by Brave llm-context mode. Use Brave web mode for date filters."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }

    std::string date_after = "";
    if (!raw_date_after.empty()) {
        date_after = normalize_to_iso_date(raw_date_after);
        if (date_after.empty()) {
            return make_json_result({
                {"error", "invalid_date"},
                {"message", "date_after must be YYYY-MM-DD format."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
    }
    std::string date_before = "";
    if (!raw_date_before.empty()) {
        date_before = normalize_to_iso_date(raw_date_before);
        if (date_before.empty()) {
            return make_json_result({
                {"error", "invalid_date"},
                {"message", "date_before must be YYYY-MM-DD format."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
    }
    if (!date_after.empty() && !date_before.empty() && date_after > date_before) {
        return make_json_result({
            {"error", "invalid_date_range"},
            {"message", "date_after must be before date_before."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }

    // 🎯 对齐 OpenClaw: domain_filter (Perplexity search_api only)
    std::vector<std::string> domain_filter;
    if (params.contains("domain_filter") && params["domain_filter"].is_array()) {
        for (size_t i = 0; i < params["domain_filter"].size(); ++i) {
            if (params["domain_filter"][i].is_string()) {
                domain_filter.push_back(params["domain_filter"][i].as_string());
            }
        }
    }
    if (!domain_filter.empty() && provider != "perplexity") {
        return make_json_result({
            {"error", "unsupported_domain_filter"},
            {"message", "domain_filter is not supported by the " + provider + " provider. Only Perplexity supports domain filtering."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if (!domain_filter.empty()) {
        bool has_denylist = false, has_allowlist = false;
        for (const auto& d : domain_filter) {
            if (!d.empty() && d[0] == '-') has_denylist = true;
            else has_allowlist = true;
        }
        if (has_denylist && has_allowlist) {
            return make_json_result({
                {"error", "invalid_domain_filter"},
                {"message", "domain_filter cannot mix allowlist and denylist entries. Use either all positive entries (allowlist) or all entries prefixed with '-' (denylist)."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
        if (domain_filter.size() > 20) {
            return make_json_result({
                {"error", "invalid_domain_filter"},
                {"message", "domain_filter supports a maximum of 20 domains."},
                {"docs", "https://docs.openclaw.ai/tools/web"}
            });
        }
    }

    // 🎯 对齐 OpenClaw: max_tokens / max_tokens_per_page (Perplexity search_api only)
    int max_tokens = -1;
    int max_tokens_per_page = -1;
    if (params.contains("max_tokens") && params["max_tokens"].is_number()) {
        max_tokens = static_cast<int>(params["max_tokens"].as_number());
    }
    if (params.contains("max_tokens_per_page") && params["max_tokens_per_page"].is_number()) {
        max_tokens_per_page = static_cast<int>(params["max_tokens_per_page"].as_number());
    }

    // 🎯 对齐 OpenClaw: Perplexity transport resolution
    std::string perplexity_base_url = "";
    std::string perplexity_model = "";
    std::string perplexity_transport = "search_api"; // "search_api" | "chat_completions"
    if (provider == "perplexity") {
        auto transport = resolve_perplexity_transport(api_key);
        perplexity_base_url = transport.base_url;
        perplexity_model = transport.model;
        perplexity_transport = transport.transport;
    }

    bool supports_structured_perplexity_filters = (provider == "perplexity" && perplexity_transport == "search_api");

    // Re-check country/language for perplexity chat_completions
    if (!country.empty() && provider == "perplexity" && !supports_structured_perplexity_filters) {
        return make_json_result({
            {"error", "unsupported_country"},
            {"message", "country filtering is only supported by the native Perplexity Search API path. Remove Perplexity baseUrl/model overrides or use a direct PERPLEXITY_API_KEY to enable it."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if (!language.empty() && provider == "perplexity" && !supports_structured_perplexity_filters) {
        return make_json_result({
            {"error", "unsupported_language"},
            {"message", "language filtering is only supported by the native Perplexity Search API path. Remove Perplexity baseUrl/model overrides or use a direct PERPLEXITY_API_KEY to enable it."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if ((!raw_date_after.empty() || !raw_date_before.empty()) && provider == "perplexity" && !supports_structured_perplexity_filters) {
        return make_json_result({
            {"error", "unsupported_date_filter"},
            {"message", "date_after/date_before are only supported by the native Perplexity Search API path. Remove Perplexity baseUrl/model overrides or use a direct PERPLEXITY_API_KEY to enable them."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if (!domain_filter.empty() && provider == "perplexity" && !supports_structured_perplexity_filters) {
        return make_json_result({
            {"error", "unsupported_domain_filter"},
            {"message", "domain_filter is only supported by the native Perplexity Search API path. Remove Perplexity baseUrl/model overrides or use a direct PERPLEXITY_API_KEY to enable it."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }
    if (provider == "perplexity" && perplexity_transport == "chat_completions" &&
        (max_tokens >= 0 || max_tokens_per_page >= 0)) {
        return make_json_result({
            {"error", "unsupported_content_budget"},
            {"message", "max_tokens and max_tokens_per_page are only supported by the native Perplexity Search API path. Remove Perplexity baseUrl/model overrides or use a direct PERPLEXITY_API_KEY to enable them."},
            {"docs", "https://docs.openclaw.ai/tools/web"}
        });
    }

    // 🎯 对齐 OpenClaw: resolve grok/gemini/kimi config
    std::string grok_model = resolve_grok_model();
    bool grok_inline_citations = resolve_grok_inline_citations();
    std::string gemini_model = resolve_gemini_model();
    std::string kimi_base_url = resolve_kimi_base_url();
    std::string kimi_model = resolve_kimi_model();

    // 🎯 对齐 OpenClaw: build cache key
    std::string provider_specific_key = "";
    if (provider == "perplexity") {
        provider_specific_key = perplexity_transport + ":" + perplexity_base_url + ":" + perplexity_model;
    } else if (provider == "grok") {
        provider_specific_key = grok_model + ":" + (grok_inline_citations ? "true" : "false");
    } else if (provider == "gemini") {
        provider_specific_key = gemini_model;
    } else if (provider == "kimi") {
        provider_specific_key = kimi_base_url + ":" + kimi_model;
    }

    std::string cache_key_str;
    if (provider == "brave" && brave_mode == "llm-context") {
        cache_key_str = normalize_cache_key(
            provider + ":llm-context:" + query + ":" +
            (country.empty() ? "default" : country) + ":" +
            (resolved_search_lang.empty() ? (language.empty() ? "default" : language) : resolved_search_lang) + ":" +
            (freshness.empty() ? "default" : freshness)
        );
    } else {
        std::string df_str = "";
        for (const auto& d : domain_filter) df_str += d + ",";
        if (!df_str.empty()) df_str.pop_back();
        cache_key_str = normalize_cache_key(
            provider + ":" + brave_mode + ":" + query + ":" +
            std::to_string(resolve_search_count(count_raw, DEFAULT_SEARCH_COUNT)) + ":" +
            (country.empty() ? "default" : country) + ":" +
            (resolved_search_lang.empty() ? (language.empty() ? "default" : language) : resolved_search_lang) + ":" +
            (resolved_ui_lang.empty() ? "default" : resolved_ui_lang) + ":" +
            (freshness.empty() ? "default" : freshness) + ":" +
            (date_after.empty() ? "default" : date_after) + ":" +
            (date_before.empty() ? "default" : date_before) + ":" +
            (df_str.empty() ? "default" : df_str) + ":" +
            (max_tokens < 0 ? "default" : std::to_string(max_tokens)) + ":" +
            (max_tokens_per_page < 0 ? "default" : std::to_string(max_tokens_per_page)) + ":" +
            provider_specific_key
        );
    }

    // 🎯 对齐 OpenClaw: readCache
    auto cached = read_cache(cache_key_str);
    if (cached) {
        json result = *cached;
        result["cached"] = true;
        return make_json_result(result);
    }

    // 🎯 对齐 OpenClaw: runWebSearch dispatch
    int count = resolve_search_count(count_raw, DEFAULT_SEARCH_COUNT);
    auto start_time = std::chrono::steady_clock::now();

    core::Result<json> search_result;

    if (provider == "perplexity") {
        if (perplexity_transport == "chat_completions") {
            search_result = run_perplexity_search(query, api_key, perplexity_base_url, perplexity_model, freshness);
        } else {
            search_result = run_perplexity_search_api(query, api_key, count, country, domain_filter,
                freshness, language, date_after, date_before, max_tokens, max_tokens_per_page);
        }
    } else if (provider == "grok") {
        search_result = run_grok_search(query, api_key, grok_model, grok_inline_citations);
    } else if (provider == "kimi") {
        search_result = run_kimi_search(query, api_key, kimi_base_url, kimi_model);
    } else if (provider == "gemini") {
        search_result = run_gemini_search(query, api_key, gemini_model);
    } else {
        // brave
        if (brave_mode == "llm-context") {
            search_result = run_brave_llm_context_search(query, api_key, country, resolved_search_lang, freshness);
        } else {
            search_result = run_brave_web_search(query, api_key, count, country, resolved_search_lang, resolved_ui_lang, freshness, date_after, date_before);
        }
    }

    if (!search_result.is_ok()) {
        ToolResult tool_result;
        tool_result.success = false;
        tool_result.error = search_result.error;
        return tool_result;
    }

    json payload = search_result.unwrap();

    // 🎯 对齐 OpenClaw: writeCache
    write_cache(cache_key_str, payload, DEFAULT_CACHE_TTL_MINUTES);

    return make_json_result(payload);
}

// ============================================================================
// Provider 选择 (对齐 OpenClaw resolveSearchProvider)
// ============================================================================

std::string WebSearchTool::select_provider_str(const std::string& requested) const {
    if (requested == "brave") return "brave";
    if (requested == "gemini") return "gemini";
    if (requested == "grok") return "grok";
    if (requested == "kimi") return "kimi";
    if (requested == "perplexity") return "perplexity";

    // Auto-detect from available API keys (alphabetical order: brave > gemini > grok > kimi > perplexity)
    if (!normalize_api_key(std::getenv("BRAVE_API_KEY")).empty()) return "brave";
    if (!normalize_api_key(std::getenv("GEMINI_API_KEY")).empty()) return "gemini";
    if (!normalize_api_key(std::getenv("XAI_API_KEY")).empty()) return "grok";
    if (!normalize_api_key(std::getenv("KIMI_API_KEY")).empty()) return "kimi";
    if (!normalize_api_key(std::getenv("MOONSHOT_API_KEY")).empty()) return "kimi";
    if (!normalize_api_key(std::getenv("PERPLEXITY_API_KEY")).empty()) return "perplexity";
    if (!normalize_api_key(std::getenv("OPENROUTER_API_KEY")).empty()) return "perplexity";

    return "brave";
}

std::string WebSearchTool::resolve_api_key(const std::string& provider) const {
    if (provider == "brave") {
        return normalize_api_key(std::getenv("BRAVE_API_KEY"));
    }
    if (provider == "gemini") {
        return normalize_api_key(std::getenv("GEMINI_API_KEY"));
    }
    if (provider == "grok") {
        std::string k = normalize_api_key(std::getenv("XAI_API_KEY"));
        if (!k.empty()) return k;
        return normalize_api_key(std::getenv("GROK_API_KEY"));
    }
    if (provider == "kimi") {
        std::string k = normalize_api_key(std::getenv("KIMI_API_KEY"));
        if (!k.empty()) return k;
        return normalize_api_key(std::getenv("MOONSHOT_API_KEY"));
    }
    if (provider == "perplexity") {
        std::string k = normalize_api_key(std::getenv("PERPLEXITY_API_KEY"));
        if (!k.empty()) return k;
        return normalize_api_key(std::getenv("OPENROUTER_API_KEY"));
    }
    return "";
}

// ============================================================================
// Perplexity Transport Resolution (对齐 OpenClaw resolvePerplexityTransport)
// ============================================================================

WebSearchTool::PerplexityTransport WebSearchTool::resolve_perplexity_transport(const std::string& api_key) const {
    PerplexityTransport result;
    result.model = DEFAULT_PERPLEXITY_MODEL;

    // 🎯 对齐 OpenClaw: resolvePerplexityBaseUrl
    // source: perplexity_env → direct, openrouter_env → openrouter, config → infer from key
    std::string hint = infer_perplexity_base_url_from_api_key(api_key);
    if (hint == "direct") {
        result.base_url = PERPLEXITY_DIRECT_BASE_URL;
    } else if (hint == "openrouter") {
        result.base_url = DEFAULT_PERPLEXITY_BASE_URL;
    } else {
        result.base_url = DEFAULT_PERPLEXITY_BASE_URL;
    }

    // 🎯 对齐 OpenClaw: transport = hasLegacyOverride || !isDirectPerplexityBaseUrl ? "chat_completions" : "search_api"
    bool has_legacy_override = false; // no config override in env-only mode
    result.transport = (has_legacy_override || !is_direct_perplexity_base_url(result.base_url))
        ? "chat_completions" : "search_api";

    return result;
}

// ============================================================================
// Brave Mode Resolution
// ============================================================================

std::string WebSearchTool::resolve_brave_mode() const {
    // Default: "web". Can be overridden via config (not implemented in env-only mode)
    return "web";
}

std::string WebSearchTool::resolve_grok_model() const { return DEFAULT_GROK_MODEL; }
bool WebSearchTool::resolve_grok_inline_citations() const { return false; }
std::string WebSearchTool::resolve_gemini_model() const { return DEFAULT_GEMINI_MODEL; }
std::string WebSearchTool::resolve_kimi_base_url() const { return DEFAULT_KIMI_BASE_URL; }
std::string WebSearchTool::resolve_kimi_model() const { return DEFAULT_KIMI_MODEL; }

// ============================================================================
// Brave Web Search (对齐 OpenClaw runWebSearch brave branch)
// ============================================================================

core::Result<json> WebSearchTool::run_brave_web_search(
    const std::string& query,
    const std::string& api_key,
    int count,
    const std::string& country,
    const std::string& search_lang,
    const std::string& ui_lang,
    const std::string& freshness,
    const std::string& date_after,
    const std::string& date_before
) {
    // 🎯 对齐 OpenClaw: use URL object with searchParams
    std::string url = BRAVE_SEARCH_ENDPOINT + "?";
    auto url_encode = [](const std::string& s) -> std::string {
        std::string result;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
    };

    url += "q=" + url_encode(query);
    url += "&count=" + std::to_string(count);
    if (!country.empty()) url += "&country=" + url_encode(country);
    // 🎯 对齐 OpenClaw: search_lang || language
    if (!search_lang.empty()) url += "&search_lang=" + url_encode(search_lang);
    if (!ui_lang.empty()) url += "&ui_lang=" + url_encode(ui_lang);
    if (!freshness.empty()) {
        url += "&freshness=" + url_encode(freshness);
    } else if (!date_after.empty() && !date_before.empty()) {
        url += "&freshness=" + url_encode(date_after + "to" + date_before);
    } else if (!date_after.empty()) {
        // 🎯 对齐 OpenClaw: dateAfter only → use today as end
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[16];
        struct tm* tm_info = gmtime(&t);
        strftime(buf, sizeof(buf), "%Y-%m-%d", tm_info);
        url += "&freshness=" + url_encode(date_after + "to" + std::string(buf));
    } else if (!date_before.empty()) {
        url += "&freshness=" + url_encode("1970-01-01to" + date_before);
    }

    infra::HttpRequestOptions options;
    options.headers["Accept"] = "application/json";
    options.headers["X-Subscription-Token"] = api_key;
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;

    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->get(url, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Brave Search API request failed: " + response.error);
    }

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        return core::Result<json>::Err(
            "Brave Search API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Brave API returned invalid JSON: " + std::string(e.what()));
    }

    // 🎯 对齐 OpenClaw: map results
    json results = json::array();
    if (data.contains("web") && data["web"].contains("results") && data["web"]["results"].is_array()) {
        for (const auto& entry : data["web"]["results"]) {
            std::string title = entry.value("title", std::string(""));
            std::string entry_url = entry.value("url", std::string(""));
            std::string description = entry.value("description", std::string(""));
            std::string age = entry.value("age", std::string(""));
            std::string site_name = resolve_site_name(entry_url);
            json item = {
                {"title", title.empty() ? "" : wrap_web_content(title)},
                {"url", entry_url},
                {"description", description.empty() ? "" : wrap_web_content(description)}
            };
            if (!age.empty()) item["published"] = age;
            if (!site_name.empty()) item["siteName"] = site_name;
            results.push_back(item);
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
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
        {"results", results}
    };

    fprintf(stderr, "[WebSearch] Brave web: %zu results for '%s' in %ld ms\n",
            results.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Brave LLM Context Search (对齐 OpenClaw runBraveLlmContextSearch)
// ============================================================================

core::Result<json> WebSearchTool::run_brave_llm_context_search(
    const std::string& query,
    const std::string& api_key,
    const std::string& country,
    const std::string& search_lang,
    const std::string& freshness
) {
    auto url_encode = [](const std::string& s) -> std::string {
        std::string result;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                result += buf;
            }
        }
        return result;
    };

    std::string url = BRAVE_LLM_CONTEXT_ENDPOINT + "?q=" + url_encode(query);
    if (!country.empty()) url += "&country=" + url_encode(country);
    if (!search_lang.empty()) url += "&search_lang=" + url_encode(search_lang);
    if (!freshness.empty()) url += "&freshness=" + url_encode(freshness);

    infra::HttpRequestOptions options;
    options.headers["Accept"] = "application/json";
    options.headers["X-Subscription-Token"] = api_key;
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;

    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->get(url, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Brave LLM Context API request failed: " + response.error);
    }

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        return core::Result<json>::Err(
            "Brave LLM Context API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Brave LLM Context API returned invalid JSON: " + std::string(e.what()));
    }

    // 🎯 对齐 OpenClaw: mapBraveLlmContextResults
    json results = json::array();
    if (data.contains("grounding") && data["grounding"].contains("generic") &&
        data["grounding"]["generic"].is_array()) {
        for (const auto& entry : data["grounding"]["generic"]) {
            std::string entry_url = entry.value("url", std::string(""));
            std::string title = entry.value("title", std::string(""));
            json snippets = json::array();
            if (entry.contains("snippets") && entry["snippets"].is_array()) {
                for (const auto& s : entry["snippets"]) {
                    if (s.is_string() && !s.get<std::string>().empty()) {
                        snippets.push_back(wrap_web_content(s.get<std::string>()));
                    }
                }
            }
            std::string site_name = resolve_site_name(entry_url);
            json item = {
                {"title", title.empty() ? "" : wrap_web_content(title)},
                {"url", entry_url},
                {"snippets", snippets}
            };
            if (!site_name.empty()) item["siteName"] = site_name;
            results.push_back(item);
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
        {"query", query},
        {"provider", "brave"},
        {"mode", "llm-context"},
        {"count", results.size()},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "brave"},
            {"wrapped", true}
        }},
        {"results", results}
    };

    // 🎯 对齐 OpenClaw: include sources if present
    if (data.contains("sources")) {
        payload["sources"] = data["sources"];
    }

    fprintf(stderr, "[WebSearch] Brave llm-context: %zu results for '%s' in %ld ms\n",
            results.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Gemini Search (对齐 OpenClaw runGeminiSearch)
// ============================================================================

core::Result<json> WebSearchTool::run_gemini_search(
    const std::string& query,
    const std::string& api_key,
    const std::string& model
) {
    std::string endpoint = GEMINI_API_BASE + "/models/" + model + ":generateContent";

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

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        // 🎯 对齐 OpenClaw: strip API key from error detail
        std::string detail = http_resp.body;
        std::regex key_pattern(R"(key=[^&\s]+)");
        detail = std::regex_replace(detail, key_pattern, "key=***");
        return core::Result<json>::Err(
            "Gemini API error (" + std::to_string(http_resp.status_code) + "): " + detail
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        std::string safe_err = std::string(e.what());
        std::regex key_pattern(R"(key=[^&\s]+)");
        safe_err = std::regex_replace(safe_err, key_pattern, "key=***");
        return core::Result<json>::Err("Gemini API returned invalid JSON: " + safe_err);
    }

    if (data.contains("error")) {
        std::string raw_msg = data["error"].value("message", data["error"].value("status", std::string("unknown")));
        std::regex key_pattern(R"(key=[^&\s]+)");
        raw_msg = std::regex_replace(raw_msg, key_pattern, "key=***");
        int code = data["error"].value("code", 0);
        return core::Result<json>::Err("Gemini API error (" + std::to_string(code) + "): " + raw_msg);
    }

    // 🎯 对齐 OpenClaw: extract content from candidates[0].content.parts
    std::string content = "No response";
    if (data.contains("candidates") && data["candidates"].is_array() && !data["candidates"].empty()) {
        auto& candidate = data["candidates"][0];
        if (candidate.contains("content") && candidate["content"].contains("parts") &&
            candidate["content"]["parts"].is_array()) {
            std::string combined = "";
            for (const auto& part : candidate["content"]["parts"]) {
                if (part.contains("text") && part["text"].is_string()) {
                    if (!combined.empty()) combined += "\n";
                    combined += part["text"].get<std::string>();
                }
            }
            if (!combined.empty()) content = combined;
        }
    }

    // 🎯 对齐 OpenClaw: extract citations from groundingChunks
    // Note: In OpenClaw, citation URLs are resolved via resolveCitationRedirectUrl
    // In C++ we skip the async redirect resolution and use raw URLs
    json citations = json::array();
    if (data.contains("candidates") && data["candidates"].is_array() && !data["candidates"].empty()) {
        auto& candidate = data["candidates"][0];
        if (candidate.contains("groundingMetadata") &&
            candidate["groundingMetadata"].contains("groundingChunks") &&
            candidate["groundingMetadata"]["groundingChunks"].is_array()) {
            for (const auto& chunk : candidate["groundingMetadata"]["groundingChunks"]) {
                if (chunk.contains("web") && chunk["web"].contains("uri") &&
                    chunk["web"]["uri"].is_string()) {
                    std::string uri = chunk["web"]["uri"].get<std::string>();
                    if (!uri.empty()) {
                        json citation = {{"url", uri}};
                        if (chunk["web"].contains("title") && chunk["web"]["title"].is_string()) {
                            citation["title"] = chunk["web"]["title"].get<std::string>();
                        }
                        citations.push_back(citation);
                    }
                }
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
        {"query", query},
        {"provider", "gemini"},
        {"model", model},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "gemini"},
            {"wrapped", true}
        }},
        {"content", wrap_web_content(content)},
        {"citations", citations}
    };

    fprintf(stderr, "[WebSearch] Gemini: %zu citations for '%s' in %ld ms\n",
            citations.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Grok Search (对齐 OpenClaw runGrokSearch + extractGrokContent)
// ============================================================================

core::Result<json> WebSearchTool::run_grok_search(
    const std::string& query,
    const std::string& api_key,
    const std::string& model,
    bool inline_citations
) {
    // 🎯 对齐 OpenClaw: body without `include` param (xAI /v1/responses doesn't support it)
    json request_body = {
        {"model", model},
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

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        return core::Result<json>::Err(
            "xAI API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("xAI API returned invalid JSON: " + std::string(e.what()));
    }

    // 🎯 对齐 OpenClaw: extractGrokContent
    std::string content = "No response";
    json annotation_citations = json::array();

    if (data.contains("output") && data["output"].is_array()) {
        for (const auto& output : data["output"]) {
            std::string output_type = output.value("type", std::string(""));

            // 🎯 对齐 OpenClaw: type === "message" → look in content array
            if (output_type == "message" && output.contains("content") && output["content"].is_array()) {
                for (const auto& block : output["content"]) {
                    if (block.value("type", std::string("")) == "output_text" &&
                        block.contains("text") && block["text"].is_string() &&
                        !block["text"].get<std::string>().empty()) {
                        content = block["text"].get<std::string>();
                        // Extract annotation citations
                        if (block.contains("annotations") && block["annotations"].is_array()) {
                            for (const auto& a : block["annotations"]) {
                                if (a.value("type", std::string("")) == "url_citation" &&
                                    a.contains("url") && a["url"].is_string()) {
                                    annotation_citations.push_back(a["url"].get<std::string>());
                                }
                            }
                        }
                        goto grok_content_found;
                    }
                }
            }

            // 🎯 对齐 OpenClaw: top-level output_text blocks (without message wrapper)
            if (output_type == "output_text" && output.contains("text") &&
                output["text"].is_string() && !output["text"].get<std::string>().empty()) {
                content = output["text"].get<std::string>();
                if (output.contains("annotations") && output["annotations"].is_array()) {
                    for (const auto& a : output["annotations"]) {
                        if (a.value("type", std::string("")) == "url_citation" &&
                            a.contains("url") && a["url"].is_string()) {
                            annotation_citations.push_back(a["url"].get<std::string>());
                        }
                    }
                }
                goto grok_content_found;
            }
        }
    }

    // 🎯 对齐 OpenClaw: fallback to deprecated output_text field
    if (data.contains("output_text") && data["output_text"].is_string()) {
        content = data["output_text"].get<std::string>();
    }

grok_content_found:;

    // 🎯 对齐 OpenClaw: prefer top-level citations; fall back to annotation-derived ones
    json citations = json::array();
    if (data.contains("citations") && data["citations"].is_array() && !data["citations"].empty()) {
        citations = data["citations"];
    } else {
        // Deduplicate annotation citations
        std::set<std::string> seen;
        for (const auto& c : annotation_citations) {
            if (c.is_string() && seen.insert(c.get<std::string>()).second) {
                citations.push_back(c);
            }
        }
    }

    // 🎯 对齐 OpenClaw: inline_citations
    json inline_cit = json();
    if (data.contains("inline_citations")) {
        inline_cit = data["inline_citations"];
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
        {"query", query},
        {"provider", "grok"},
        {"model", model},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "grok"},
            {"wrapped", true}
        }},
        {"content", wrap_web_content(content)},
        {"citations", citations}
    };
    // 🎯 对齐 OpenClaw: include inlineCitations if present
    if (!inline_cit.is_null()) {
        payload["inlineCitations"] = inline_cit;
    }

    fprintf(stderr, "[WebSearch] Grok: %zu citations for '%s' in %ld ms\n",
            citations.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Kimi Search (对齐 OpenClaw runKimiSearch - multi-round)
// ============================================================================

core::Result<json> WebSearchTool::run_kimi_search(
    const std::string& query,
    const std::string& api_key,
    const std::string& base_url,
    const std::string& model
) {
    std::string trimmed_base = base_url;
    // trim trailing slash
    while (!trimmed_base.empty() && trimmed_base.back() == '/') trimmed_base.pop_back();
    std::string endpoint = trimmed_base + "/chat/completions";

    // 🎯 对齐 OpenClaw: multi-round conversation (MAX_ROUNDS = 3)
    static constexpr int MAX_ROUNDS = 3;
    json messages = json::array({
        {{"role", "user"}, {"content", query}}
    });
    std::set<std::string> collected_citations;
    auto kimi_start = std::chrono::steady_clock::now();

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        json request_body = {
            {"model", model},
            {"messages", messages},
            {"tools", json::array({KIMI_WEB_SEARCH_TOOL})}
        };

        infra::HttpRequestOptions options;
        options.headers["Content-Type"] = "application/json";
        options.headers["Authorization"] = "Bearer " + api_key;
        options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
        options.body = request_body.dump();

        auto response = http_client_->post(endpoint, options);
        if (!response.is_ok()) {
            return core::Result<json>::Err("Kimi API request failed: " + response.error);
        }

        auto http_resp = response.unwrap();
        if (http_resp.status_code != 200) {
            return core::Result<json>::Err(
                "Kimi API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
            );
        }

        json data;
        try {
            data = json::parse(http_resp.body);
        } catch (const std::exception& e) {
            return core::Result<json>::Err("Kimi API returned invalid JSON: " + std::string(e.what()));
        }

        // 🎯 对齐 OpenClaw: extractKimiCitations
        // From search_results
        if (data.contains("search_results") && data["search_results"].is_array()) {
            for (const auto& entry : data["search_results"]) {
                if (entry.contains("url") && entry["url"].is_string()) {
                    std::string u = entry["url"].get<std::string>();
                    auto s = u.find_first_not_of(" \t");
                    if (s != std::string::npos) {
                        auto e = u.find_last_not_of(" \t");
                        u = u.substr(s, e - s + 1);
                        if (!u.empty()) collected_citations.insert(u);
                    }
                }
            }
        }

        auto& choice = data["choices"][0];
        auto& message = choice["message"];

        // 🎯 对齐 OpenClaw: extractKimiCitations from tool_calls
        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
            for (const auto& tool_call : message["tool_calls"]) {
                if (tool_call.contains("function") && tool_call["function"].contains("arguments")) {
                    std::string raw_args = tool_call["function"]["arguments"].get<std::string>();
                    try {
                        json parsed = json::parse(raw_args);
                        if (parsed.contains("url") && parsed["url"].is_string()) {
                            std::string u = parsed["url"].get<std::string>();
                            auto s = u.find_first_not_of(" \t");
                            if (s != std::string::npos) {
                                auto e = u.find_last_not_of(" \t");
                                u = u.substr(s, e - s + 1);
                                if (!u.empty()) collected_citations.insert(u);
                            }
                        }
                        if (parsed.contains("search_results") && parsed["search_results"].is_array()) {
                            for (const auto& r : parsed["search_results"]) {
                                if (r.contains("url") && r["url"].is_string()) {
                                    std::string u = r["url"].get<std::string>();
                                    auto s = u.find_first_not_of(" \t");
                                    if (s != std::string::npos) {
                                        auto e = u.find_last_not_of(" \t");
                                        u = u.substr(s, e - s + 1);
                                        if (!u.empty()) collected_citations.insert(u);
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        // ignore malformed tool arguments
                    }
                }
            }
        }

        // 🎯 对齐 OpenClaw: extractKimiMessageText
        std::string text = "";
        if (message.contains("content") && message["content"].is_string()) {
            std::string c = message["content"].get<std::string>();
            auto s = c.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) {
                auto e = c.find_last_not_of(" \t\r\n");
                text = c.substr(s, e - s + 1);
            }
        }
        if (text.empty() && message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
            std::string r = message["reasoning_content"].get<std::string>();
            auto s = r.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) {
                auto e = r.find_last_not_of(" \t\r\n");
                text = r.substr(s, e - s + 1);
            }
        }

        std::string finish_reason = choice.value("finish_reason", std::string(""));
        json tool_calls = message.contains("tool_calls") ? message["tool_calls"] : json::array();

        // 🎯 对齐 OpenClaw: if finish_reason !== "tool_calls" or no tool_calls → done
        if (finish_reason != "tool_calls" || tool_calls.empty()) {
            json citations_arr = json::array();
            for (const auto& c : collected_citations) citations_arr.push_back(c);

            auto kimi_end = std::chrono::steady_clock::now();
            auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kimi_end - kimi_start).count();

            json payload = {
                {"query", query},
                {"provider", "kimi"},
                {"model", model},
                {"tookMs", took_ms},
                {"externalContent", {
                    {"untrusted", true},
                    {"source", "web_search"},
                    {"provider", "kimi"},
                    {"wrapped", true}
                }},
                {"content", wrap_web_content(text.empty() ? "No response" : text)},
                {"citations", citations_arr}
            };
            fprintf(stderr, "[WebSearch] Kimi: %zu citations for '%s'\n",
                    collected_citations.size(), query.c_str());
            return core::Result<json>::Ok(payload);
        }

        // 🎯 对齐 OpenClaw: push assistant message and tool results
        json assistant_msg = {
            {"role", "assistant"},
            {"content", message.value("content", std::string(""))}
        };
        if (message.contains("reasoning_content")) {
            assistant_msg["reasoning_content"] = message["reasoning_content"];
        }
        assistant_msg["tool_calls"] = tool_calls;
        messages.push_back(assistant_msg);

        // 🎯 对齐 OpenClaw: buildKimiToolResultContent
        json tool_result_content = {
            {"search_results", json::array()}
        };
        if (data.contains("search_results") && data["search_results"].is_array()) {
            for (const auto& entry : data["search_results"]) {
                tool_result_content["search_results"].push_back({
                    {"title", entry.value("title", std::string(""))},
                    {"url", entry.value("url", std::string(""))},
                    {"content", entry.value("content", std::string(""))}
                });
            }
        }
        std::string tool_content_str = tool_result_content.dump();

        bool pushed_tool_result = false;
        for (const auto& tool_call : tool_calls) {
            std::string tool_call_id = "";
            if (tool_call.contains("id") && tool_call["id"].is_string()) {
                tool_call_id = tool_call["id"].get<std::string>();
                auto s = tool_call_id.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    auto e = tool_call_id.find_last_not_of(" \t");
                    tool_call_id = tool_call_id.substr(s, e - s + 1);
                } else {
                    tool_call_id = "";
                }
            }
            if (tool_call_id.empty()) continue;
            pushed_tool_result = true;
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", tool_call_id},
                {"content", tool_content_str}
            });
        }

        if (!pushed_tool_result) {
            // No valid tool call IDs → done
            json citations_arr = json::array();
            for (const auto& c : collected_citations) citations_arr.push_back(c);
            auto kimi_end2 = std::chrono::steady_clock::now();
            auto took_ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(kimi_end2 - kimi_start).count();
            json payload = {
                {"query", query},
                {"provider", "kimi"},
                {"model", model},
                {"tookMs", took_ms2},
                {"externalContent", {
                    {"untrusted", true},
                    {"source", "web_search"},
                    {"provider", "kimi"},
                    {"wrapped", true}
                }},
                {"content", wrap_web_content(text.empty() ? "No response" : text)},
                {"citations", citations_arr}
            };
            return core::Result<json>::Ok(payload);
        }
    }

    // 🎯 对齐 OpenClaw: MAX_ROUNDS exhausted
    json citations_arr = json::array();
    for (const auto& c : collected_citations) citations_arr.push_back(c);
    auto kimi_end3 = std::chrono::steady_clock::now();
    auto took_ms3 = std::chrono::duration_cast<std::chrono::milliseconds>(kimi_end3 - kimi_start).count();
    json payload = {
        {"query", query},
        {"provider", "kimi"},
        {"model", model},
        {"tookMs", took_ms3},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "kimi"},
            {"wrapped", true}
        }},
        {"content", "Search completed but no final answer was produced."},
        {"citations", citations_arr}
    };
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Perplexity Chat Completions Search (对齐 OpenClaw runPerplexitySearch)
// ============================================================================

core::Result<json> WebSearchTool::run_perplexity_search(
    const std::string& query,
    const std::string& api_key,
    const std::string& base_url,
    const std::string& model,
    const std::string& freshness
) {
    std::string trimmed_base = base_url;
    while (!trimmed_base.empty() && trimmed_base.back() == '/') trimmed_base.pop_back();
    std::string endpoint = trimmed_base + "/chat/completions";
    std::string request_model = resolve_perplexity_request_model(trimmed_base, model);

    json body = {
        {"model", request_model},
        {"messages", json::array({
            {{"role", "user"}, {"content", query}}
        })}
    };
    if (!freshness.empty()) {
        body["search_recency_filter"] = freshness;
    }

    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["Authorization"] = "Bearer " + api_key;
    options.headers["HTTP-Referer"] = "https://openclaw.ai";
    options.headers["X-Title"] = "OpenClaw Web Search";
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = body.dump();

    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(endpoint, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Perplexity API request failed: " + response.error);
    }

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        return core::Result<json>::Err(
            "Perplexity API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Perplexity API returned invalid JSON: " + std::string(e.what()));
    }

    std::string content = "No response";
    if (data.contains("choices") && data["choices"].is_array() && !data["choices"].empty()) {
        auto& choice = data["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content") &&
            choice["message"]["content"].is_string()) {
            content = choice["message"]["content"].get<std::string>();
        }
    }

    // 🎯 对齐 OpenClaw: extractPerplexityCitations
    // Prefer top-level citations; fall back to OpenRouter-style message annotations
    json citations = json::array();
    std::set<std::string> seen_urls;

    if (data.contains("citations") && data["citations"].is_array() && !data["citations"].empty()) {
        for (const auto& c : data["citations"]) {
            if (c.is_string()) {
                std::string u = c.get<std::string>();
                auto s = u.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    auto e = u.find_last_not_of(" \t");
                    u = u.substr(s, e - s + 1);
                    if (!u.empty() && seen_urls.insert(u).second) citations.push_back(u);
                }
            }
        }
    } else {
        // Fall back to annotation-derived citations
        for (const auto& choice : data.value("choices", json::array())) {
            auto& msg = choice["message"];
            if (!msg.contains("annotations") || !msg["annotations"].is_array()) continue;
            for (const auto& annotation : msg["annotations"]) {
                if (annotation.value("type", std::string("")) != "url_citation") continue;
                std::string url = "";
                if (annotation.contains("url_citation") && annotation["url_citation"].contains("url")) {
                    url = annotation["url_citation"]["url"].get<std::string>();
                } else if (annotation.contains("url") && annotation["url"].is_string()) {
                    url = annotation["url"].get<std::string>();
                }
                auto s = url.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    auto e = url.find_last_not_of(" \t");
                    url = url.substr(s, e - s + 1);
                    if (!url.empty() && seen_urls.insert(url).second) citations.push_back(url);
                }
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
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
        {"content", wrap_web_content(content)},
        {"citations", citations}
    };

    fprintf(stderr, "[WebSearch] Perplexity chat: %zu citations for '%s' in %ld ms\n",
            citations.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// Perplexity Search API (对齐 OpenClaw runPerplexitySearchApi)
// ============================================================================

core::Result<json> WebSearchTool::run_perplexity_search_api(
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
) {
    json body = {
        {"query", query},
        {"max_results", count}
    };

    if (!country.empty()) body["country"] = country;
    if (!domain_filter.empty()) {
        json df = json::array();
        for (const auto& d : domain_filter) df.push_back(d);
        body["search_domain_filter"] = df;
    }
    if (!freshness.empty()) body["search_recency_filter"] = freshness;
    if (!language.empty()) {
        body["search_language_filter"] = json::array({language});
    }
    // 🎯 对齐 OpenClaw: isoToPerplexityDate conversion
    if (!date_after.empty()) {
        std::string pplx_date = iso_to_perplexity_date(date_after);
        if (!pplx_date.empty()) body["search_after_date"] = pplx_date;
    }
    if (!date_before.empty()) {
        std::string pplx_date = iso_to_perplexity_date(date_before);
        if (!pplx_date.empty()) body["search_before_date"] = pplx_date;
    }
    if (max_tokens >= 0) body["max_tokens"] = max_tokens;
    if (max_tokens_per_page >= 0) body["max_tokens_per_page"] = max_tokens_per_page;

    infra::HttpRequestOptions options;
    options.headers["Content-Type"] = "application/json";
    options.headers["Accept"] = "application/json";
    options.headers["Authorization"] = "Bearer " + api_key;
    options.headers["HTTP-Referer"] = "https://openclaw.ai";
    options.headers["X-Title"] = "OpenClaw Web Search";
    options.timeout_ms = DEFAULT_TIMEOUT_SECONDS * 1000;
    options.body = body.dump();

    auto start = std::chrono::steady_clock::now();
    auto response = http_client_->post(PERPLEXITY_SEARCH_ENDPOINT, options);
    if (!response.is_ok()) {
        return core::Result<json>::Err("Perplexity Search API request failed: " + response.error);
    }

    auto http_resp = response.unwrap();
    if (http_resp.status_code != 200) {
        return core::Result<json>::Err(
            "Perplexity Search API error (" + std::to_string(http_resp.status_code) + "): " + http_resp.body
        );
    }

    json data;
    try {
        data = json::parse(http_resp.body);
    } catch (const std::exception& e) {
        return core::Result<json>::Err("Perplexity Search API returned invalid JSON: " + std::string(e.what()));
    }

    // 🎯 对齐 OpenClaw: map results with wrapWebContent
    json results = json::array();
    if (data.contains("results") && data["results"].is_array()) {
        for (const auto& entry : data["results"]) {
            std::string title = entry.value("title", std::string(""));
            std::string entry_url = entry.value("url", std::string(""));
            std::string snippet = entry.value("snippet", std::string(""));
            std::string date = entry.value("date", std::string(""));
            std::string site_name = resolve_site_name(entry_url);
            json item = {
                {"title", title.empty() ? "" : wrap_web_content(title)},
                {"url", entry_url},
                {"description", snippet.empty() ? "" : wrap_web_content(snippet)}
            };
            if (!date.empty()) item["published"] = date;
            if (!site_name.empty()) item["siteName"] = site_name;
            results.push_back(item);
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    json payload = {
        {"query", query},
        {"provider", "perplexity"},
        {"count", results.size()},
        {"tookMs", took_ms},
        {"externalContent", {
            {"untrusted", true},
            {"source", "web_search"},
            {"provider", "perplexity"},
            {"wrapped", true}
        }},
        {"results", results}
    };

    fprintf(stderr, "[WebSearch] Perplexity search_api: %zu results for '%s' in %ld ms\n",
            results.size(), query.c_str(), (long)took_ms);
    return core::Result<json>::Ok(payload);
}

// ============================================================================
// 缓存管理 (对齐 OpenClaw web-shared.ts)
// ============================================================================

std::optional<json> WebSearchTool::read_cache(const std::string& key) const {
    auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;
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

// ============================================================================
// ToolResult 构建辅助
// ============================================================================

ToolResult WebSearchTool::make_json_result(const json& value) const {
    ToolResult tool_result;
    tool_result.success = true;
    ToolResultContent content;
    content.type = ToolResultContent::Type::Json;
    content.text = value.dump();
    tool_result.content.push_back(content);
    return tool_result;
}

// ============================================================================
// Factory
// ============================================================================

std::shared_ptr<Tool> WebSearchToolFactory::create(std::shared_ptr<infra::HttpClient> http_client) {
    return std::make_shared<WebSearchTool>(std::move(http_client));
}

} // namespace tools
} // namespace hunterclaw
