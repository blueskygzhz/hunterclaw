#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * @brief Link Understanding 结果
 * 
 * Aligned with OpenClaw: extractPageContent response
 */
struct LinkUnderstandingResult {
    std::string url;                  // 原始 URL
    std::string title;                // 页面标题
    std::string content;              // Markdown 格式内容
    std::string excerpt;              // 简短摘要 (前 500 字符)
    std::vector<std::string> images;  // 图片 URL 列表
    int content_length = 0;           // 内容长度 (字符数)
};

/**
 * @brief Link Understanding Tool
 * 
 * Aligned with OpenClaw: src/utils/link-understanding.ts
 * 
 * **功能**:
 * 1. 检测消息中的 URL
 * 2. 抓取 URL 内容 (HTML)
 * 3. 提取主要内容 (Readability/Trafilatura)
 * 4. 转换为 Markdown
 * 5. 注入 prompt context
 * 
 * **实现方式**:
 * - HTTP 客户端: hunterclaw::infra::HttpClient
 * - Markdown 转换: hunterclaw::markdown::MarkdownSystem::html_to_markdown
 * - Content 提取: 基于 DOM 解析 (libxml2/gumbo-parser)
 */
class LinkUnderstanding {
public:
    using Ptr = std::shared_ptr<LinkUnderstanding>;
    
    /**
     * @brief 创建 Link Understanding 实例
     */
    static Ptr create();
    
    /**
     * @brief 从文本中提取所有 URL
     * 
     * @param text 输入文本
     * @return URL 列表
     */
    std::vector<std::string> extract_urls(const std::string& text);
    
    /**
     * @brief 抓取并提取 URL 内容
     * 
     * Aligned with OpenClaw: extractPageContent(url)
     * 
     * @param url 目标 URL
     * @param timeout_ms 超时时间 (毫秒, default 10000)
     * @return LinkUnderstandingResult 或错误
     */
    core::Result<LinkUnderstandingResult> extract_page_content(
        const std::string& url,
        int timeout_ms = 10000
    );
    
    /**
     * @brief 批量提取多个 URL 内容
     * 
     * @param urls URL 列表
     * @param timeout_ms 每个 URL 的超时时间
     * @return 成功提取的结果列表 (失败的 URL 跳过)
     */
    std::vector<LinkUnderstandingResult> extract_batch(
        const std::vector<std::string>& urls,
        int timeout_ms = 10000
    );
    
    /**
     * @brief 构建 Link Understanding prompt context
     * 
     * Aligned with OpenClaw: buildLinkContext(results)
     * 
     * @param results Link Understanding 结果列表
     * @return Markdown 格式的 context (用于注入 prompt)
     */
    std::string build_link_context(const std::vector<LinkUnderstandingResult>& results);
    
private:
    LinkUnderstanding() = default;
    
    /**
     * @brief 提取页面主要内容 (Readability-like algorithm)
     * 
     * @param html 原始 HTML
     * @return Markdown 格式内容
     */
    std::string extract_main_content(const std::string& html);
    
    /**
     * @brief 清理和规范化 Markdown
     * 
     * @param markdown 原始 Markdown
     * @return 清理后的 Markdown
     */
    std::string clean_markdown(const std::string& markdown);
};

}  // namespace tools
}  // namespace hunterclaw
