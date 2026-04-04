// Aligned with openclaw:
//   src/utils/link-understanding.ts (extractPageContent, buildLinkContext)
//   Uses Readability-like algorithm for content extraction

#include "hunterclaw/tools/link_understanding.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/markdown/markdown_system.h"
#include "hunterclaw/core/logging.h"
#include <regex>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace tools {

// ============================================================================
// LinkUnderstanding Implementation
// ============================================================================

LinkUnderstanding::Ptr LinkUnderstanding::create() {
    return std::shared_ptr<LinkUnderstanding>(new LinkUnderstanding());
}

std::vector<std::string> LinkUnderstanding::extract_urls(const std::string& text) {
    // Aligned with openclaw: URL regex pattern
    static const std::regex url_re(
        R"(https?://[^\s<>"{}|\\^`\[\]]+)",
        std::regex_constants::icase
    );
    
    std::vector<std::string> urls;
    auto begin = std::sregex_iterator(text.begin(), text.end(), url_re);
    auto end = std::sregex_iterator();
    
    for (auto it = begin; it != end; ++it) {
        urls.push_back((*it)[0].str());
    }
    
    return urls;
}

core::Result<LinkUnderstandingResult> LinkUnderstanding::extract_page_content(
    const std::string& url,
    int timeout_ms
) {
    auto logger = core::get_logger("link_understanding");
    
    // Step 1: Fetch URL content
    auto http_client = infra::HttpClient::create();
    infra::HttpRequestOptions options;
    options.timeout_ms = timeout_ms;
    options.headers["User-Agent"] = "Mozilla/5.0 (compatible; HunterClaw/1.0)";
    options.follow_redirects = true;
    options.max_redirects = 5;
    
    logger->debug("Fetching URL: {}", url);
    
    auto response = http_client->get(url, options);
    if (!response.is_ok()) {
        return core::Result<LinkUnderstandingResult>::Err(
            "Failed to fetch URL: " + response.error
        );
    }
    
    const auto& resp = response.get();
    if (!resp.ok) {
        return core::Result<LinkUnderstandingResult>::Err(
            "HTTP error: " + std::to_string(resp.status_code)
        );
    }
    
    // Step 2: Check Content-Type
    auto content_type_it = resp.headers.find("content-type");
    if (content_type_it != resp.headers.end()) {
        const std::string& ct = content_type_it->second;
        if (ct.find("text/html") == std::string::npos &&
            ct.find("text/plain") == std::string::npos) {
            return core::Result<LinkUnderstandingResult>::Err(
                "Unsupported content type: " + ct
            );
        }
    }
    
    // Step 3: Extract main content (Readability-like)
    std::string markdown_content = extract_main_content(resp.body);
    
    // Step 4: Extract title (from <title> tag)
    std::string title = "Untitled";
    static const std::regex title_re(R"(<title>(.*?)</title>)", std::regex_constants::icase);
    std::smatch title_match;
    if (std::regex_search(resp.body, title_match, title_re)) {
        title = title_match[1].str();
        // Decode HTML entities
        title = std::regex_replace(title, std::regex("&lt;"), "<");
        title = std::regex_replace(title, std::regex("&gt;"), ">");
        title = std::regex_replace(title, std::regex("&amp;"), "&");
        title = std::regex_replace(title, std::regex("&quot;"), "\"");
    }
    
    // Step 5: Build result
    LinkUnderstandingResult result;
    result.url = url;
    result.title = title;
    result.content = markdown_content;
    result.excerpt = markdown_content.substr(0, std::min<size_t>(500, markdown_content.size()));
    result.content_length = markdown_content.size();
    
    logger->info("Extracted page content: url={}, title={}, content_length={}",
                 url, title, result.content_length);
    
    return core::Result<LinkUnderstandingResult>::Ok(result);
}

std::vector<LinkUnderstandingResult> LinkUnderstanding::extract_batch(
    const std::vector<std::string>& urls,
    int timeout_ms
) {
    std::vector<LinkUnderstandingResult> results;
    
    for (const auto& url : urls) {
        auto result = extract_page_content(url, timeout_ms);
        if (result.is_ok()) {
            results.push_back(result.get());
        } else {
            auto logger = core::get_logger("link_understanding");
            logger->warn("Failed to extract URL {}: {}", url, result.error);
        }
    }
    
    return results;
}

std::string LinkUnderstanding::build_link_context(
    const std::vector<LinkUnderstandingResult>& results
) {
    if (results.empty()) return "";
    
    std::ostringstream oss;
    oss << "\n\n--- Link Context ---\n";
    
    for (const auto& result : results) {
        oss << "\n**[" << result.title << "](" << result.url << ")**\n\n";
        
        // Limit content to 2000 characters per URL (aligned with OpenClaw)
        const std::string limited_content = result.content.substr(
            0, std::min<size_t>(2000, result.content.size())
        );
        
        oss << limited_content;
        
        if (result.content.size() > 2000) {
            oss << "\n\n_[Content truncated...]_";
        }
        
        oss << "\n\n---\n";
    }
    
    return oss.str();
}

std::string LinkUnderstanding::extract_main_content(const std::string& html) {
    // Aligned with openclaw: Readability-like algorithm
    // 
    // Simplified implementation:
    // 1. Remove script/style tags
    // 2. Extract text from <body>
    // 3. Convert to Markdown
    
    std::string cleaned = html;
    
    // Step 1: Remove <script> and <style> tags
    cleaned = std::regex_replace(cleaned, std::regex(R"(<script[^>]*>.*?</script>)", std::regex_constants::icase), "");
    cleaned = std::regex_replace(cleaned, std::regex(R"(<style[^>]*>.*?</style>)", std::regex_constants::icase), "");
    
    // Step 2: Remove HTML comments
    cleaned = std::regex_replace(cleaned, std::regex(R"(<!--.*?-->)"), "");
    
    // Step 3: Convert HTML to Markdown (using MarkdownSystem)
    // Note: MarkdownSystem::html_to_markdown is a TODO, so we do basic conversion
    std::string markdown = clean_markdown(cleaned);
    
    return markdown;
}

std::string LinkUnderstanding::clean_markdown(const std::string& markdown) {
    std::string result = markdown;
    
    // Remove HTML tags (basic strip)
    result = std::regex_replace(result, std::regex("<[^>]+>"), "");
    
    // Decode HTML entities
    result = std::regex_replace(result, std::regex("&lt;"), "<");
    result = std::regex_replace(result, std::regex("&gt;"), ">");
    result = std::regex_replace(result, std::regex("&amp;"), "&");
    result = std::regex_replace(result, std::regex("&quot;"), "\"");
    result = std::regex_replace(result, std::regex("&nbsp;"), " ");
    result = std::regex_replace(result, std::regex("&#39;"), "'");
    
    // Collapse multiple newlines
    result = std::regex_replace(result, std::regex("\n{3,}"), "\n\n");
    
    // Trim whitespace
    result = std::regex_replace(result, std::regex("^\\s+"), "");
    result = std::regex_replace(result, std::regex("\\s+$"), "");
    
    return result;
}

}  // namespace tools
}  // namespace hunterclaw
