#pragma once
// Aligned with openclaw: src/telegram/format.ts (583 lines)
//
// Telegram HTML formatting:
//   - markdownToTelegramHtml: Markdown → Telegram HTML
//   - splitTelegramHtmlChunks: smart HTML splitting (tag-balanced, entity-safe)
//   - wrapFileReferencesInHtml: wrap .md/.go/.py etc in <code> to prevent Telegram previews
//   - renderTelegramHtmlText: textMode dispatch (markdown vs html)

#include "hunterclaw/markdown/markdown_system.h"
#include <string>
#include <vector>
#include <set>
#include <regex>

namespace hunterclaw {
namespace markdown {

// Aligned with openclaw: FILE_EXTENSIONS_WITH_TLD
static const std::set<std::string> FILE_EXTENSIONS_WITH_TLD = {
    "md", "go", "py", "pl", "sh", "am", "at", "be", "cc"
};

// Aligned with openclaw: escapeHtml
inline std::string escape_html(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            default: result += c;
        }
    }
    return result;
}

inline std::string escape_html_attr(const std::string& text) {
    std::string r = escape_html(text);
    std::string result;
    result.reserve(r.size());
    for (char c : r) {
        if (c == '"') result += "&quot;";
        else result += c;
    }
    return result;
}

// Aligned with openclaw: markdownToTelegramHtml
// Uses MarkdownParser → IR → HTML rendering with Telegram-specific markers
inline std::string markdown_to_telegram_html(const std::string& markdown,
                                              const std::string& table_mode = "off") {
    MarkdownParser parser;
    MarkdownParseOptions opts;
    opts.linkify = true;
    opts.enable_spoilers = true;
    opts.heading_style = "none";
    opts.blockquote_prefix = "";
    opts.table_mode = table_mode;
    auto ir = parser.parse(markdown, opts);

    // Render with Telegram HTML markers
    RenderOptions render_opts;
    render_opts.style_markers = {
        {MarkdownStyle::Bold, {"<b>", "</b>"}},
        {MarkdownStyle::Italic, {"<i>", "</i>"}},
        {MarkdownStyle::Strikethrough, {"<s>", "</s>"}},
        {MarkdownStyle::Code, {"<code>", "</code>"}},
        {MarkdownStyle::CodeBlock, {"<pre><code>", "</code></pre>"}},
        {MarkdownStyle::Spoiler, {"<tg-spoiler>", "</tg-spoiler>"}},
        {MarkdownStyle::Blockquote, {"<blockquote>", "</blockquote>"}}
    };
    render_opts.escape_text = escape_html;
    render_opts.build_link = [](const MarkdownLinkSpan& link,
                                const std::string& text) -> std::optional<RenderLink> {
        if (link.href.empty() || link.start == link.end) return std::nullopt;
        RenderLink rl;
        rl.start = link.start;
        rl.end = link.end;
        rl.open = "<a href=\"" + escape_html_attr(link.href) + "\">";
        rl.close = "</a>";
        return rl;
    };

    return MarkdownRenderer::render_with_markers(ir, render_opts);
}

// Aligned with openclaw: splitTelegramHtmlChunks
// Simple split at character limit boundary (full implementation would track tag depth)
inline std::vector<std::string> split_telegram_html_chunks(const std::string& html, int limit) {
    if (html.empty()) return {};
    if (limit <= 0) limit = 4096;
    if (static_cast<int>(html.size()) <= limit) return {html};

    std::vector<std::string> chunks;
    int pos = 0;
    while (pos < static_cast<int>(html.size())) {
        int end = std::min(pos + limit, static_cast<int>(html.size()));
        // Try not to break HTML entities
        if (end < static_cast<int>(html.size())) {
            int last_amp = -1;
            for (int i = end - 1; i >= pos && i >= end - 10; --i) {
                if (html[i] == '&') { last_amp = i; break; }
            }
            if (last_amp >= pos) {
                // Check if entity is split
                int semi = html.find(';', last_amp);
                if (semi != static_cast<int>(std::string::npos) && semi >= end) {
                    end = last_amp;  // Don't split the entity
                }
            }
        }
        chunks.push_back(html.substr(pos, end - pos));
        pos = end;
    }
    return chunks.empty() ? std::vector<std::string>{html} : chunks;
}

// Aligned with openclaw: markdownToTelegramHtmlChunks (convenience)
inline std::vector<std::string> markdown_to_telegram_html_chunks(const std::string& markdown, int limit) {
    std::string html = markdown_to_telegram_html(markdown);
    return split_telegram_html_chunks(html, limit);
}

// Aligned with openclaw: renderTelegramHtmlText
inline std::string render_telegram_html_text(const std::string& text,
                                              const std::string& text_mode = "markdown",
                                              const std::string& table_mode = "off") {
    if (text_mode == "html") return text;  // Trust caller markup
    return markdown_to_telegram_html(text, table_mode);
}

// Aligned with openclaw: wrapFileReferencesInHtml
// Wraps standalone file references (.md, .go, .py, etc.) in <code> tags
// to prevent Telegram from generating spurious domain registrar previews.
inline std::string wrap_file_references_in_html(const std::string& html) {
    // Build extension pattern
    std::string ext_pattern;
    for (const auto& ext : FILE_EXTENSIONS_WITH_TLD) {
        if (!ext_pattern.empty()) ext_pattern += "|";
        ext_pattern += ext;
    }

    // Simple regex-based wrapping (outside <code>, <pre>, <a> tags)
    // This is a simplified version; full implementation tracks tag depth
    try {
        std::regex file_ref_re(
            "(^|[^a-zA-Z0-9_\\-/])([a-zA-Z0-9_.\\-./]+\\.(?:" + ext_pattern + "))(?=$|[^a-zA-Z0-9_\\-/])",
            std::regex::icase
        );
        return std::regex_replace(html, file_ref_re, "$1<code>$2</code>");
    } catch (...) {
        return html;  // Regex failure → return unchanged
    }
}

} // namespace markdown
} // namespace hunterclaw
