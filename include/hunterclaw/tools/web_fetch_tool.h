#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/result.h"
#include "hunterclaw/infra/http_client.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <optional>

namespace hunterclaw {
namespace tools {

using json = nlohmann::json;

// ===== Web Fetch Tool - 100% 对齐 OpenClaw web-fetch.ts =====
//
// 功能对齐:
// 1. ✅ HTTP/HTTPS URL 抓取: GET 请求获取网页内容
// 2. ✅ 内容提取模式:
//    - "markdown": HTML → 可读 Markdown (Readability.js + Turndown)
//    - "text": HTML → 纯文本 (去除所有标记)
// 3. ✅ Readability 清理: 提取主要内容,去除导航/广告/页脚
// 4. ✅ 字符截断: maxChars 限制返回大小 (默认 50K)
// 5. ✅ 响应限制: maxResponseBytes 限制下载大小 (默认 2MB)
// 6. ✅ 重定向跟踪: 最多 3 次重定向 (可配置)
// 7. ✅ 缓存支持: 默认 15 分钟 TTL (可配置)
// 8. ✅ 超时控制: 默认 30 秒 (可配置)
// 9. ✅ SSRF 防护: 阻止内网 IP/localhost 访问
// 10. ✅ 安全包装: 外部内容标记 (untrusted)
// 11. ✅ User-Agent: 标准浏览器 UA
// 12. ✅ Firecrawl 集成: 可选 Firecrawl API (高级抓取)
//
// OpenClaw 原始路径: src/agents/tools/web-fetch.ts (800+ 行)

class WebFetchTool : public Tool {
public:
    explicit WebFetchTool(std::shared_ptr<infra::HttpClient> http_client);
    
    std::string name() const override { return "web_fetch"; }
    
    std::string description() const override {
        // 🎯 严格对齐 OpenClaw web-fetch.ts:956
        return "Fetch URL; extract readable markdown/text. Lightweight; no browser automation.";
    }
    
    Schema parameters() const override;

    // 执行抓取
    // Returns: { content: string, url: string, extract_mode: string, truncated?: bool }
    ToolResult execute(const core::JsonValue& params) override;
    
private:
    std::shared_ptr<infra::HttpClient> http_client_;
    
    // ===== 提取模式 =====
    
    enum class ExtractMode {
        Markdown,  // HTML → Markdown (默认)
        Text       // HTML → 纯文本
    };
    
    ExtractMode parse_extract_mode(const std::string& mode_str) const;
    
    // ===== 内容抓取 =====
    
    struct FetchResult {
        std::string content;         // HTML 原始内容
        std::string final_url;       // 最终 URL (可能重定向)
        std::string content_type;    // Content-Type
        int status_code;             // HTTP 状态码
    };
    
    // 抓取 URL (带 SSRF 防护)
    core::Result<FetchResult> fetch_url(
        const std::string& url,
        int timeout_seconds,
        int max_redirects
    );
    
    // Firecrawl 抓取 (可选)
    core::Result<FetchResult> fetch_firecrawl(
        const std::string& url,
        int timeout_seconds
    );
    
    // ===== 内容提取 =====
    
    // 提取可读内容 (Readability)
    std::string extract_readable_content(
        const std::string& html,
        const std::string& url
    ) const;
    
    // HTML → Markdown
    std::string html_to_markdown(const std::string& html) const;
    
    // Markdown → Text
    std::string markdown_to_text(const std::string& markdown) const;
    
    // HTML → Text (直接)
    std::string html_to_text(const std::string& html) const;
    
    // 截断文本
    struct TruncateResult {
        std::string text;
        bool truncated;
    };
    TruncateResult truncate_text(const std::string& text, int max_chars) const;
    
    // ===== 缓存管理 =====
    
    // 生成缓存 Key
    std::string cache_key(
        const std::string& url,
        ExtractMode mode,
        int max_chars
    ) const;
    
    // 读取缓存
    std::optional<json> read_cache(const std::string& key) const;
    
    // 写入缓存 (默认 15 分钟 TTL)
    void write_cache(const std::string& key, const json& value, int ttl_minutes = 15);
    
    // 缓存存储
    struct CacheEntry {
        json value;
        std::chrono::system_clock::time_point expires_at;
    };
    mutable std::unordered_map<std::string, CacheEntry> cache_;
    
    // ===== SSRF 防护 =====
    
    // 检查 URL 是否安全 (阻止内网)
    bool is_url_safe(const std::string& url) const;
    
    // 检查 IP 是否为内网
    bool is_private_ip(const std::string& ip) const;
    
    // ===== 安全辅助 =====
    
    // 包装外部内容 (标记为 untrusted)
    json wrap_web_content(
        const std::string& content,
        const std::string& url,
        const std::string& mode
    ) const;
    
    // ===== Firecrawl 配置 =====
    
    // Firecrawl 是否启用
    bool is_firecrawl_enabled() const;
    
    // 获取 Firecrawl API Key
    std::optional<std::string> get_firecrawl_api_key() const;
    
    // Firecrawl Base URL (默认: https://api.firecrawl.dev)
    std::string get_firecrawl_base_url() const;
};

// ===== Web Fetch Tool Factory =====

class WebFetchToolFactory {
public:
    static std::shared_ptr<Tool> create(std::shared_ptr<infra::HttpClient> http_client);
};

} // namespace tools
} // namespace hunterclaw
