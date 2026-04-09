#include "hunterclaw/markdown/markdown_system.h"
#include <regex>
#include <algorithm>
#include <sstream>

namespace hunterclaw {
namespace markdown {

// ============================================================================
// 1. Markdown Parser Implementation
// ============================================================================

struct MarkdownParser::Impl {
    // Parser state (simplified)
};

MarkdownParser::MarkdownParser() : impl_(std::make_unique<Impl>()) {}
MarkdownParser::~MarkdownParser() = default;

MarkdownIR MarkdownParser::parse(const std::string& markdown,
                                  const MarkdownParseOptions& options) {
    MarkdownIR ir;
    ir.text = markdown;
    
    // Simple implementation (full implementation would use markdown-it or similar)
    // For now, just basic bold/italic/code detection
    
    // Bold: **text**
    std::regex bold_regex(R"(\*\*([^*]+)\*\*)");
    std::smatch match;
    std::string::const_iterator search_start(markdown.cbegin());
    while (std::regex_search(search_start, markdown.cend(), match, bold_regex)) {
        MarkdownStyleSpan span;
        span.start = match.position(1) + std::distance(markdown.cbegin(), search_start);
        span.end = span.start + match.str(1).length();
        span.style = MarkdownStyle::Bold;
        ir.styles.push_back(span);
        search_start = match.suffix().first;
    }
    
    // Italic: *text* or _text_
    std::regex italic_regex(R"(\*([^*]+)\*|_([^_]+)_)");
    search_start = markdown.cbegin();
    while (std::regex_search(search_start, markdown.cend(), match, italic_regex)) {
        MarkdownStyleSpan span;
        span.start = (match[1].matched ? match.position(1) : match.position(2)) + 
                     std::distance(markdown.cbegin(), search_start);
        span.end = span.start + (match[1].matched ? match.str(1).length() : match.str(2).length());
        span.style = MarkdownStyle::Italic;
        ir.styles.push_back(span);
        search_start = match.suffix().first;
    }
    
    // Code: `text`
    std::regex code_regex(R"(`([^`]+)`)");
    search_start = markdown.cbegin();
    while (std::regex_search(search_start, markdown.cend(), match, code_regex)) {
        MarkdownStyleSpan span;
        span.start = match.position(1) + std::distance(markdown.cbegin(), search_start);
        span.end = span.start + match.str(1).length();
        span.style = MarkdownStyle::Code;
        ir.styles.push_back(span);
        search_start = match.suffix().first;
    }
    
    // Links: [text](url)
    std::regex link_regex(R"(\[([^\]]+)\]\(([^)]+)\))");
    search_start = markdown.cbegin();
    while (std::regex_search(search_start, markdown.cend(), match, link_regex)) {
        MarkdownLinkSpan link;
        link.start = match.position(1) + std::distance(markdown.cbegin(), search_start);
        link.end = link.start + match.str(1).length();
        link.href = match.str(2);
        ir.links.push_back(link);
        search_start = match.suffix().first;
    }
    
    return ir;
}

MarkdownParseResult MarkdownParser::parse_with_meta(const std::string& markdown,
                                                     const MarkdownParseOptions& options) {
    MarkdownParseResult result;
    result.ir = parse(markdown, options);
    result.has_tables = markdown.find('|') != std::string::npos;  // Simple check
    return result;
}

// ============================================================================
// 2. Markdown Renderer Implementation
// ============================================================================

std::vector<MarkdownStyleSpan> MarkdownRenderer::sort_style_spans(
    const std::vector<MarkdownStyleSpan>& spans) {
    std::vector<MarkdownStyleSpan> sorted = spans;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end > b.end;
        return false;
    });
    return sorted;
}

std::string MarkdownRenderer::render_with_markers(const MarkdownIR& ir,
                                                   const RenderOptions& options) {
    std::string result = ir.text;
    
    // Sort spans
    auto sorted_styles = sort_style_spans(ir.styles);
    
    // Apply style markers (from end to start to preserve positions)
    for (auto it = sorted_styles.rbegin(); it != sorted_styles.rend(); ++it) {
        auto style_it = options.style_markers.find(it->style);
        if (style_it != options.style_markers.end()) {
            const auto& marker = style_it->second;
            result.insert(it->end, marker.close);
            result.insert(it->start, marker.open);
        }
    }
    
    // Apply links (from end to start)
    for (auto it = ir.links.rbegin(); it != ir.links.rend(); ++it) {
        if (options.build_link) {
            auto link = options.build_link(*it, ir.text);
            if (link) {
                result.insert(link->end, link->close);
                result.insert(link->start, link->open);
            }
        }
    }
    
    // Escape text if needed
    if (options.escape_text) {
        result = options.escape_text(result);
    }
    
    return result;
}

std::string MarkdownRenderer::render_to_markdown(const MarkdownIR& ir) {
    RenderOptions options;
    options.style_markers = {
        {MarkdownStyle::Bold, {"**", "**"}},
        {MarkdownStyle::Italic, {"*", "*"}},
        {MarkdownStyle::Code, {"`", "`"}},
        {MarkdownStyle::Strikethrough, {"~~", "~~"}},
        {MarkdownStyle::CodeBlock, {"```\n", "\n```"}},
        {MarkdownStyle::Blockquote, {"> ", ""}}
    };
    options.build_link = [](const MarkdownLinkSpan& link, const std::string& text) -> std::optional<RenderLink> {
        RenderLink render_link;
        render_link.start = link.start;
        render_link.end = link.end;
        render_link.open = "[";
        render_link.close = "](" + link.href + ")";
        return render_link;
    };
    return render_with_markers(ir, options);
}

std::string MarkdownRenderer::render_to_html(const MarkdownIR& ir) {
    RenderOptions options;
    options.style_markers = {
        {MarkdownStyle::Bold, {"<strong>", "</strong>"}},
        {MarkdownStyle::Italic, {"<em>", "</em>"}},
        {MarkdownStyle::Code, {"<code>", "</code>"}},
        {MarkdownStyle::Strikethrough, {"<s>", "</s>"}},
        {MarkdownStyle::CodeBlock, {"<pre><code>", "</code></pre>"}},
        {MarkdownStyle::Blockquote, {"<blockquote>", "</blockquote>"}}
    };
    options.build_link = [](const MarkdownLinkSpan& link, const std::string& text) -> std::optional<RenderLink> {
        RenderLink render_link;
        render_link.start = link.start;
        render_link.end = link.end;
        render_link.open = "<a href=\"" + link.href + "\">";
        render_link.close = "</a>";
        return render_link;
    };
    return render_with_markers(ir, options);
}

std::string MarkdownRenderer::render_to_plain_text(const MarkdownIR& ir) {
    return ir.text;
}

// ============================================================================
// 3. Fence Parser Implementation
// ============================================================================

std::vector<FenceSpan> FenceParser::parse_fence_spans(const std::string& buffer) {
    std::vector<FenceSpan> spans;
    
    // C++17 doesn't support std::regex::multiline, need to match ^ at each line start manually
    std::regex fence_regex(R"(^( {0,3})(`{3,}|~{3,}).*$)");
    std::smatch match;
    
    std::optional<FenceSpan> open_fence;
    std::string::const_iterator search_start(buffer.cbegin());
    
    while (std::regex_search(search_start, buffer.cend(), match, fence_regex)) {
        int pos = match.position() + std::distance(buffer.cbegin(), search_start);
        std::string indent = match.str(1);
        std::string marker = match.str(2);
        char marker_char = marker[0];
        int marker_len = marker.length();
        
        if (!open_fence) {
            // Opening fence
            FenceSpan fence;
            fence.start = pos;
            fence.open_line = match.str();
            fence.marker = marker;
            fence.indent = indent;
            open_fence = fence;
        } else if (open_fence->marker[0] == marker_char && marker_len >= static_cast<int>(open_fence->marker.length())) {
            // Closing fence
            open_fence->end = pos + match.str().length();
            spans.push_back(*open_fence);
            open_fence = std::nullopt;
        }
        
        search_start = match.suffix().first;
    }
    
    // If no closing fence found, fence extends to end
    if (open_fence) {
        open_fence->end = buffer.length();
        spans.push_back(*open_fence);
    }
    
    return spans;
}

std::optional<FenceSpan> FenceParser::find_fence_span_at(
    const std::vector<FenceSpan>& spans,
    int index) {
    for (const auto& span : spans) {
        if (index >= span.start && index < span.end) {
            return span;
        }
    }
    return std::nullopt;
}

bool FenceParser::is_inside_fence(const std::vector<FenceSpan>& spans, int index) {
    return find_fence_span_at(spans, index).has_value();
}

std::string FenceParser::extract_fence_info(const std::string& open_line) {
    std::regex info_regex(R"(^[ ]{0,3}[`~]{3,}([^ ]*))");
    std::smatch match;
    if (std::regex_search(open_line, match, info_regex)) {
        return match.str(1);
    }
    return "";
}

std::string FenceParser::extract_fence_content(const std::string& buffer,
                                               const FenceSpan& span) {
    if (span.start >= span.end) {
        return "";
    }
    
    // Find first newline after opening fence
    int content_start = buffer.find('\n', span.start);
    if (content_start == std::string::npos || content_start >= span.end) {
        return "";
    }
    content_start++;  // Skip newline
    
    // Find last newline before closing fence
    int content_end = buffer.rfind('\n', span.end - 1);
    if (content_end == std::string::npos || content_end <= content_start) {
        return "";
    }
    
    return buffer.substr(content_start, content_end - content_start);
}

// ============================================================================
// 4. Code Span Parser Implementation
// ============================================================================

std::vector<CodeSpan> CodeSpanParser::parse_code_spans(const std::string& buffer) {
    std::vector<CodeSpan> spans;
    
    // Simple implementation: single backticks only
    std::regex code_regex(R"((`+)([^`]|`(?!`))+\1)");
    std::smatch match;
    std::string::const_iterator search_start(buffer.cbegin());
    
    while (std::regex_search(search_start, buffer.cend(), match, code_regex)) {
        CodeSpan span;
        span.start = match.position() + std::distance(buffer.cbegin(), search_start);
        span.end = span.start + match.str().length();
        span.backticks = match.str(1).length();
        spans.push_back(span);
        search_start = match.suffix().first;
    }
    
    return spans;
}

std::optional<CodeSpan> CodeSpanParser::find_code_span_at(
    const std::vector<CodeSpan>& spans,
    int index) {
    for (const auto& span : spans) {
        if (index >= span.start && index < span.end) {
            return span;
        }
    }
    return std::nullopt;
}

bool CodeSpanParser::is_inside_code_span(const std::vector<CodeSpan>& spans, int index) {
    return find_code_span_at(spans, index).has_value();
}

std::string CodeSpanParser::extract_code_span_content(const std::string& buffer,
                                                      const CodeSpan& span) {
    if (span.start >= span.end) {
        return "";
    }
    return buffer.substr(span.start + span.backticks,
                         span.end - span.start - 2 * span.backticks);
}

// ============================================================================
// 5. Table Converter Implementation
// ============================================================================

std::string TableConverter::convert_tables(const std::string& markdown,
                                           MarkdownTableMode mode) {
    if (mode == MarkdownTableMode::Off) {
        return markdown;
    }
    
    // Simple implementation: convert first table found
    auto table = parse_table(markdown, 0);
    if (!table) {
        return markdown;
    }
    
    if (mode == MarkdownTableMode::Bullets) {
        return table_to_bullets(*table);
    } else {
        return table_to_code_block(*table);
    }
}

std::optional<MarkdownTable> TableConverter::parse_table(const std::string& markdown,
                                                          int start_pos) {
    // 🎯 对齐 OpenClaw: parseMarkdownTable — 解析 | 分隔的表格
    // Look for | column | column | pattern
    std::istringstream stream(markdown.substr(start_pos));
    std::string line;
    std::vector<std::string> lines;

    while (std::getline(stream, line)) {
        if (line.find('|') == std::string::npos) break;
        lines.push_back(line);
    }

    if (lines.size() < 2) return std::nullopt;  // Need at least header + separator

    // Parse cells from a row
    auto parse_row = [](const std::string& row) -> std::vector<std::string> {
        std::vector<std::string> cells;
        std::istringstream ss(row);
        std::string cell;
        // Split by |
        while (std::getline(ss, cell, '|')) {
            // Trim whitespace
            auto start = cell.find_first_not_of(" \t");
            auto end = cell.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                cells.push_back(cell.substr(start, end - start + 1));
            } else if (!cell.empty()) {
                cells.push_back("");
            }
        }
        return cells;
    };

    // Check if second line is separator (---|---|---)
    bool has_separator = false;
    std::regex sep_re(R"(^[\s|:-]+$)");
    if (lines.size() >= 2 && std::regex_match(lines[1], sep_re) &&
        lines[1].find('-') != std::string::npos) {
        has_separator = true;
    }

    if (!has_separator) return std::nullopt;

    MarkdownTable table;

    // Parse headers
    auto header_cells = parse_row(lines[0]);
    for (const auto& cell : header_cells) {
        TableCell hc;
        hc.text = cell;
        table.headers.push_back(hc);
    }

    // Parse data rows (skip separator at index 1)
    for (size_t i = 2; i < lines.size(); ++i) {
        auto row_cells = parse_row(lines[i]);
        std::vector<TableCell> row;
        for (const auto& cell : row_cells) {
            TableCell rc;
            rc.text = cell;
            row.push_back(rc);
        }
        table.rows.push_back(row);
    }

    return table;
}

std::string TableConverter::table_to_bullets(const MarkdownTable& table) {
    std::ostringstream oss;
    
    // Headers as bold
    for (const auto& header : table.headers) {
        oss << "- **" << header.text << "**\n";
    }
    
    // Rows as bullet points
    for (const auto& row : table.rows) {
        for (const auto& cell : row) {
            oss << "  - " << cell.text << "\n";
        }
    }
    
    return oss.str();
}

std::string TableConverter::table_to_code_block(const MarkdownTable& table) {
    // 🎯 对齐 OpenClaw: Format as ASCII table
    std::ostringstream oss;
    oss << "```\n";
    
    // Calculate column widths
    std::vector<size_t> widths;
    for (const auto& header : table.headers) {
        widths.push_back(header.text.size());
    }
    for (const auto& row : table.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].text.size());
        }
    }

    // Helper to pad string
    auto pad = [](const std::string& s, size_t w) -> std::string {
        if (s.size() >= w) return s;
        return s + std::string(w - s.size(), ' ');
    };

    // Header row
    for (size_t i = 0; i < table.headers.size(); ++i) {
        oss << " " << pad(table.headers[i].text, widths[i]) << " |";
    }
    oss << "\n";

    // Separator row
    for (size_t i = 0; i < widths.size(); ++i) {
        oss << "-" << std::string(widths[i], '-') << "-+";
    }
    oss << "\n";

    // Data rows
    for (const auto& row : table.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            oss << " " << pad(row[i].text, widths[i]) << " |";
        }
        oss << "\n";
    }
    
    oss << "```\n";
    return oss.str();
}

// ============================================================================
// 6. Front Matter Parser Implementation
// ============================================================================

std::optional<FrontMatter> FrontMatterParser::parse(const std::string& markdown) {
    // YAML front matter: --- ... ---
    std::regex yaml_regex(R"(^---\r?\n([\s\S]*?)\r?\n---\r?\n)");
    std::smatch match;
    
    if (std::regex_search(markdown, match, yaml_regex)) {
        FrontMatter fm;
        fm.type = FrontMatterType::YAML;
        fm.content = match.str(1);
        fm.start = 0;
        fm.end = match.str().length();
        return fm;
    }
    
    return std::nullopt;
}

std::string FrontMatterParser::strip_front_matter(const std::string& markdown) {
    auto fm = parse(markdown);
    if (fm) {
        return markdown.substr(fm->end);
    }
    return markdown;
}

FrontMatterParser::ParseResult FrontMatterParser::parse_with_content(const std::string& markdown) {
    ParseResult result;
    result.front_matter = parse(markdown);
    result.content = result.front_matter ? markdown.substr(result.front_matter->end) : markdown;
    return result;
}

std::unordered_map<std::string, std::string> FrontMatterParser::parse_yaml(
    const std::string& yaml_content) {
    std::unordered_map<std::string, std::string> result;
    
    // Simple key: value parsing
    std::regex kv_regex(R"(([^:]+):\s*(.+))");
    std::istringstream iss(yaml_content);
    std::string line;
    
    while (std::getline(iss, line)) {
        std::smatch match;
        if (std::regex_search(line, match, kv_regex)) {
            std::string key = match.str(1);
            std::string value = match.str(2);
            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            result[key] = value;
        }
    }
    
    return result;
}

// ============================================================================
// 7. WhatsApp Formatter Implementation
// ============================================================================

RenderStyleMap WhatsAppStyle::get_style_markers() {
    return {
        {MarkdownStyle::Bold, {"*", "*"}},
        {MarkdownStyle::Italic, {"_", "_"}},
        {MarkdownStyle::Strikethrough, {"~", "~"}},
        {MarkdownStyle::Code, {"```", "```"}}
    };
}

// Aligned with openclaw: markdown/whatsapp.ts markdownToWhatsApp (lines 38-77)
// Order of operations:
//   1. Protect fenced code blocks (```...```) — already WhatsApp-compatible
//   2. Protect inline code (`...`) — leave as-is
//   3. Convert **bold** → *bold* and __bold__ → *bold*
//   4. Convert ~~strike~~ → ~strike~
//   5. Restore protected spans
std::string WhatsAppFormatter::markdown_to_whatsapp(const std::string& text) {
    if (text.empty()) return text;

    // 1. Protect fenced code blocks
    static const std::string FENCE_PLACEHOLDER = "\x00FENCE";
    static const std::string CODE_PLACEHOLDER = "\x00CODE";

    std::vector<std::string> fences;
    std::string result = text;

    // Extract fenced code blocks
    std::regex fence_re("```[\\s\\S]*?```");
    {
        std::string tmp;
        std::sregex_iterator it(result.begin(), result.end(), fence_re);
        std::sregex_iterator end;
        size_t last_pos = 0;
        for (; it != end; ++it) {
            tmp += result.substr(last_pos, it->position() - last_pos);
            fences.push_back(it->str());
            tmp += FENCE_PLACEHOLDER + std::to_string(fences.size() - 1);
            last_pos = it->position() + it->length();
        }
        tmp += result.substr(last_pos);
        result = tmp;
    }

    // 2. Protect inline code
    std::vector<std::string> inline_codes;
    {
        std::regex code_re("`[^`\\n]+`");
        std::string tmp;
        std::sregex_iterator it(result.begin(), result.end(), code_re);
        std::sregex_iterator end;
        size_t last_pos = 0;
        for (; it != end; ++it) {
            tmp += result.substr(last_pos, it->position() - last_pos);
            inline_codes.push_back(it->str());
            tmp += CODE_PLACEHOLDER + std::to_string(inline_codes.size() - 1);
            last_pos = it->position() + it->length();
        }
        tmp += result.substr(last_pos);
        result = tmp;
    }

    // 3. Convert **bold** → *bold* and __bold__ → *bold*
    result = std::regex_replace(result, std::regex("\\*\\*(.+?)\\*\\*"), "*$1*");
    result = std::regex_replace(result, std::regex("__(.+?)__"), "*$1*");

    // 4. Convert ~~strikethrough~~ → ~strikethrough~
    result = std::regex_replace(result, std::regex("~~(.+?)~~"), "~$1~");

    // 5. Restore inline code
    for (size_t i = 0; i < inline_codes.size(); ++i) {
        std::string placeholder = CODE_PLACEHOLDER + std::to_string(i);
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), inline_codes[i]);
        }
    }

    // 6. Restore fenced code blocks
    for (size_t i = 0; i < fences.size(); ++i) {
        std::string placeholder = FENCE_PLACEHOLDER + std::to_string(i);
        auto pos = result.find(placeholder);
        if (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), fences[i]);
        }
    }

    return result;
}

std::string WhatsAppFormatter::whatsapp_to_markdown(const std::string& whatsapp_text) {
    // WhatsApp uses same markers as markdown (mostly)
    return whatsapp_text;
}

bool WhatsAppFormatter::has_whatsapp_formatting(const std::string& text) {
    return text.find('*') != std::string::npos ||
           text.find('_') != std::string::npos ||
           text.find('~') != std::string::npos;
}

// ============================================================================
// 8. Markdown Utilities Implementation
// ============================================================================

std::string MarkdownUtils::escape_markdown(const std::string& text) {
    std::string result = text;
    const std::vector<char> special_chars = {'\\', '*', '_', '`', '[', ']', '(', ')', '#', '+', '-', '.', '!', '|'};
    
    for (char ch : special_chars) {
        std::string from(1, ch);
        std::string to = "\\" + from;
        size_t pos = 0;
        while ((pos = result.find(from, pos)) != std::string::npos) {
            result.replace(pos, 1, to);
            pos += to.length();
        }
    }
    
    return result;
}

std::string MarkdownUtils::unescape_markdown(const std::string& text) {
    std::string result = text;
    std::regex escape_regex(R"(\\([\\*_`\[\]()#+\-.!|]))");
    result = std::regex_replace(result, escape_regex, "$1");
    return result;
}

std::string MarkdownUtils::strip_formatting(const std::string& markdown) {
    MarkdownParser parser;
    auto ir = parser.parse(markdown);
    return ir.text;
}

std::vector<std::string> MarkdownUtils::extract_links(const std::string& markdown) {
    std::vector<std::string> links;
    std::regex link_regex(R"(\[([^\]]+)\]\(([^)]+)\))");
    std::smatch match;
    std::string::const_iterator search_start(markdown.cbegin());
    
    while (std::regex_search(search_start, markdown.cend(), match, link_regex)) {
        links.push_back(match.str(2));
        search_start = match.suffix().first;
    }
    
    return links;
}

std::vector<std::string> MarkdownUtils::extract_code_blocks(const std::string& markdown) {
    std::vector<std::string> code_blocks;
    auto fences = FenceParser::parse_fence_spans(markdown);
    
    for (const auto& fence : fences) {
        code_blocks.push_back(FenceParser::extract_fence_content(markdown, fence));
    }
    
    return code_blocks;
}

int MarkdownUtils::count_words(const std::string& markdown) {
    std::string text = strip_formatting(markdown);
    std::regex word_regex(R"(\S+)");
    auto words_begin = std::sregex_iterator(text.begin(), text.end(), word_regex);
    auto words_end = std::sregex_iterator();
    return std::distance(words_begin, words_end);
}

std::string MarkdownUtils::truncate(const std::string& markdown,
                                     int max_length,
                                     const std::string& suffix) {
    if (markdown.length() <= static_cast<size_t>(max_length)) {
        return markdown;
    }
    return markdown.substr(0, max_length - suffix.length()) + suffix;
}

std::string MarkdownUtils::normalize_whitespace(const std::string& markdown) {
    std::string result = markdown;
    std::regex multi_space(R"(\s+)");
    result = std::regex_replace(result, multi_space, " ");
    return result;
}

std::string MarkdownUtils::normalize_line_endings(const std::string& markdown,
                                                   const std::string& line_ending) {
    std::string result = markdown;
    std::regex crlf(R"(\r\n)");
    result = std::regex_replace(result, crlf, "\n");
    std::regex cr(R"(\r)");
    result = std::regex_replace(result, cr, "\n");
    
    if (line_ending != "\n") {
        std::regex lf(R"(\n)");
        result = std::regex_replace(result, lf, line_ending);
    }
    
    return result;
}

// ============================================================================
// 9. Markdown Converter Implementation (stubs)
// ============================================================================

std::string MarkdownConverter::markdown_to_html(const std::string& markdown,
                                                const MarkdownParseOptions& options) {
    MarkdownParser parser;
    auto ir = parser.parse(markdown, options);
    return MarkdownRenderer::render_to_html(ir);
}

std::string MarkdownConverter::html_to_markdown(const std::string& html) {
    // 🎯 对齐 OpenClaw: html-to-markdown conversion
    std::string result = html;
    // Strip HTML tags and convert common elements
    // <br> / <br/> → \n
    result = std::regex_replace(result, std::regex(R"(<br\s*/?>)"), "\n");
    // <p>...</p> → \n\n...\n\n
    result = std::regex_replace(result, std::regex(R"(<p[^>]*>)"), "\n\n");
    result = std::regex_replace(result, std::regex(R"(</p>)"), "\n\n");
    // <strong>/<b> → **
    result = std::regex_replace(result, std::regex(R"(</?(?:strong|b)>)"), "**");
    // <em>/<i> → *
    result = std::regex_replace(result, std::regex(R"(</?(?:em|i)>)"), "*");
    // <code> → `
    result = std::regex_replace(result, std::regex(R"(</?code>)"), "`");
    // <a href="url">text</a> → [text](url)
    {
        std::regex a_re(R"xx(<a\s+href="([^"]*)"[^>]*>([^<]*)</a>)xx");
        result = std::regex_replace(result, a_re, "[$2]($1)");
    }
    // <h1>...<h6> → # ... ######
    for (int i = 6; i >= 1; --i) {
        std::string hashes(i, '#');
        result = std::regex_replace(result, std::regex("<h" + std::to_string(i) + "[^>]*>"), "\n" + hashes + " ");
        result = std::regex_replace(result, std::regex("</h" + std::to_string(i) + ">"), "\n");
    }
    // <li> → - 
    result = std::regex_replace(result, std::regex(R"(<li[^>]*>)"), "- ");
    result = std::regex_replace(result, std::regex(R"(</li>)"), "\n");
    // Strip remaining tags
    result = std::regex_replace(result, std::regex(R"(<[^>]+>)"), "");
    // Decode common entities
    size_t pos;
    while ((pos = result.find("&amp;")) != std::string::npos) result.replace(pos, 5, "&");
    while ((pos = result.find("&lt;")) != std::string::npos) result.replace(pos, 4, "<");
    while ((pos = result.find("&gt;")) != std::string::npos) result.replace(pos, 4, ">");
    while ((pos = result.find("&quot;")) != std::string::npos) result.replace(pos, 6, "\"");
    while ((pos = result.find("&#39;")) != std::string::npos) result.replace(pos, 5, "'");
    while ((pos = result.find("&nbsp;")) != std::string::npos) result.replace(pos, 6, " ");
    // Collapse excessive newlines
    result = std::regex_replace(result, std::regex(R"(\n{4,})"), "\n\n\n");
    return result;
}

std::string MarkdownConverter::markdown_to_plain_text(const std::string& markdown) {
    return MarkdownUtils::strip_formatting(markdown);
}

std::string MarkdownConverter::markdown_to_slack(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: markdown → Slack mrkdwn 格式
    std::string result = markdown;

    // Convert **bold** → *bold*
    result = std::regex_replace(result, std::regex(R"(\*\*(.+?)\*\*)"), "*$1*");
    // Convert *italic* → _italic_
    result = std::regex_replace(result, std::regex(R"((?<!\*)\*([^*]+?)\*(?!\*))"), "_$1_");
    // Convert ~~strikethrough~~ → ~strikethrough~
    result = std::regex_replace(result, std::regex(R"(~~(.+?)~~)"), "~$1~");
    // Convert [text](url) → <url|text>
    result = std::regex_replace(result, std::regex(R"(\[([^\]]+)\]\(([^)]+)\))"), "<$2|$1>");

    return result;
}

std::string MarkdownConverter::markdown_to_discord(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: Discord 原生支持大部分 Markdown
    // 主要差异: 标题用 **加粗** 替代（Discord 不支持 # 标题）
    std::string result = markdown;

    // Convert # headings to **bold** (Discord doesn't render # as headings in messages)
    // C++17 doesn't have std::regex::multiline, process line by line
    {
        std::istringstream ss(result);
        std::string line, out;
        std::regex heading_re(R"(^#{1,6}\s+(.+)$)");
        while (std::getline(ss, line)) {
            out += std::regex_replace(line, heading_re, "**$1**") + "\n";
        }
        if (!out.empty() && out.back() == '\n') out.pop_back();
        result = out;
    }

    return result;
}

std::string MarkdownConverter::markdown_to_telegram(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: markdown → Telegram MarkdownV2 格式
    // Telegram MarkdownV2 需要转义特殊字符: _*[]()~`>#+-=|{}.!
    std::string result = markdown;

    // Escape special chars that aren't part of formatting
    // (保留 **bold**, *italic*, `code`, ```block```, [link](url))
    // 简化版: 转义 . ! - ( ) 等
    auto escape_telegram = [](const std::string& text) -> std::string {
        std::string out;
        out.reserve(text.size() * 1.2);
        for (char c : text) {
            switch (c) {
                case '.': case '!': case '-': case '(': case ')':
                case '{': case '}': case '=': case '+': case '>':
                case '#': case '|':
                    out += '\\';
                    out += c;
                    break;
                default:
                    out += c;
                    break;
            }
        }
        return out;
    };

    // Protect code blocks and inline code first
    // (simplified — full implementation would use state machine)
    result = escape_telegram(result);

    return result;
}

// ============================================================================
// 10. Markdown Validator Implementation (stubs)
// ============================================================================

std::vector<MarkdownValidationIssue> MarkdownValidator::validate(const std::string& markdown) {
    std::vector<MarkdownValidationIssue> issues;
    
    // Check for unclosed formatting
    if (has_unclosed_formatting(markdown)) {
        MarkdownValidationIssue issue;
        issue.code = "unclosed_formatting";
        issue.message = "Unclosed formatting detected";
        issue.line = 0;
        issue.column = 0;
        issues.push_back(issue);
    }
    
    return issues;
}

bool MarkdownValidator::has_unclosed_formatting(const std::string& markdown) {
    // Count asterisks, underscores, backticks
    int asterisks = std::count(markdown.begin(), markdown.end(), '*');
    int underscores = std::count(markdown.begin(), markdown.end(), '_');
    int backticks = std::count(markdown.begin(), markdown.end(), '`');
    
    return (asterisks % 2 != 0) || (underscores % 2 != 0) || (backticks % 2 != 0);
}

std::vector<std::string> MarkdownValidator::find_broken_links(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: findBrokenLinks — 查找格式错误的 markdown 链接
    std::vector<std::string> broken;

    // Find all [text](url) patterns
    std::regex link_re(R"(\[([^\]]*)\]\(([^)]*)\))");
    auto begin = std::sregex_iterator(markdown.begin(), markdown.end(), link_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string url = (*it)[2].str();
        // Check for obviously broken URLs
        if (url.empty()) {
            broken.push_back("[" + (*it)[1].str() + "]() — empty URL");
        } else if (url.find(' ') != std::string::npos && url.find("http") == 0) {
            broken.push_back(url + " — contains spaces");
        } else if (url.find("http") == 0 && url.find("://") == std::string::npos) {
            broken.push_back(url + " — missing ://");
        }
    }

    // Find unclosed links: [text]( without closing )
    std::regex unclosed_re(R"(\[[^\]]*\]\([^)]*$)");
    auto ub = std::sregex_iterator(markdown.begin(), markdown.end(), unclosed_re);
    for (auto it = ub; it != end; ++it) {
        broken.push_back((*it)[0].str() + " — unclosed link");
    }

    return broken;
}

bool MarkdownValidator::has_invalid_tables(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: hasInvalidTables — 检测格式错误的表格
    // Valid markdown table needs:
    // 1. Header row with |
    // 2. Separator row with |---|
    // 3. At least one data row

    std::regex table_start_re(R"(^\|.+\|$)");
    std::regex separator_re(R"(^\|[\s\-:|]+\|$)");

    // Check line-by-line since C++ regex doesn't support multiline
    bool has_table_rows = false;
    bool has_separator = false;
    {
        std::istringstream ss(markdown);
        std::string line;
        while (std::getline(ss, line)) {
            if (std::regex_match(line, table_start_re)) has_table_rows = true;
            if (std::regex_match(line, separator_re) && line.find('-') != std::string::npos) has_separator = true;
        }
    }
    if (!has_table_rows) return false;  // No tables at all

    if (!has_separator) return true;  // Has table-like rows but no separator

    // Check column count consistency
    std::istringstream stream(markdown);
    std::string line;
    int prev_col_count = -1;
    bool in_table = false;

    while (std::getline(stream, line)) {
        if (line.empty() || line.find('|') == std::string::npos) {
            in_table = false;
            prev_col_count = -1;
            continue;
        }
        if (line.front() == '|') {
            int col_count = static_cast<int>(std::count(line.begin(), line.end(), '|')) - 1;
            if (in_table && prev_col_count != -1 && col_count != prev_col_count) {
                return true;  // Inconsistent column count
            }
            in_table = true;
            prev_col_count = col_count;
        }
    }

    return false;
}

// ============================================================================
// 11. Advanced Parsers (stubs)
// ============================================================================

std::vector<BlockquoteParser::BlockquoteSpan> BlockquoteParser::parse_blockquotes(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: parseBlockquotes — 解析 > 引用块
    std::vector<BlockquoteSpan> spans;

    std::istringstream stream(markdown);
    std::string line;
    int pos = 0;
    int span_start = -1;
    int depth = 0;

    while (std::getline(stream, line)) {
        int line_start = pos;
        pos += static_cast<int>(line.size()) + 1;  // +1 for newline

        // Count leading > characters
        int line_depth = 0;
        size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '>') {
                line_depth++;
                i++;
                if (i < line.size() && line[i] == ' ') i++;  // Skip optional space
            } else if (line[i] == ' ' || line[i] == '\t') {
                i++;
            } else {
                break;
            }
        }

        if (line_depth > 0) {
            if (span_start == -1) {
                span_start = line_start;
                depth = line_depth;
            }
        } else {
            if (span_start != -1) {
                BlockquoteSpan span;
                span.start = span_start;
                span.end = line_start;
                span.level = depth;
                spans.push_back(span);
                span_start = -1;
            }
        }
    }

    // Close last span if open
    if (span_start != -1) {
        BlockquoteSpan span;
        span.start = span_start;
        span.end = static_cast<int>(markdown.size());
        span.level = depth;
        spans.push_back(span);
    }

    return spans;
}

std::string BlockquoteParser::extract_blockquote_content(const std::string& markdown,
                                                         const BlockquoteSpan& span) {
    return markdown.substr(span.start, span.end - span.start);
}

std::vector<ListParser::ListItem> ListParser::parse_list_items(const std::string& markdown) {
    // 🎯 对齐 OpenClaw: parseListItems — 解析有序/无序列表
    std::vector<ListItem> items;

    std::istringstream stream(markdown);
    std::string line;
    int pos = 0;

    // Patterns: "- item", "* item", "+ item", "1. item", "1) item"
    std::regex unordered_re(R"(^(\s*)([-*+])\s+(.+)$)");
    std::regex ordered_re(R"(^(\s*)(\d+)[.)]\s+(.+)$)");

    while (std::getline(stream, line)) {
        int line_start = pos;
        pos += static_cast<int>(line.size()) + 1;

        std::smatch match;
        if (std::regex_match(line, match, unordered_re)) {
            ListItem item;
            item.start = line_start;
            item.end = pos - 1;
            item.level = static_cast<int>(match[1].str().size()) / 2;
            item.type = ListType::Bullet;
            item.marker = match[2].str();
            item.index = 0;
            item.content = match[3].str();
            items.push_back(item);
        } else if (std::regex_match(line, match, ordered_re)) {
            ListItem item;
            item.start = line_start;
            item.end = pos - 1;
            item.level = static_cast<int>(match[1].str().size()) / 2;
            item.type = ListType::Ordered;
            item.index = std::stoi(match[2].str());
            item.marker = match[2].str() + ".";
            item.content = match[3].str();
            items.push_back(item);
        }
    }

    return items;
}

std::string ListParser::extract_list_item_content(const std::string& markdown,
                                                  const ListItem& item) {
    return markdown.substr(item.start, item.end - item.start);
}

std::vector<HeadingParser::Heading> HeadingParser::parse_headings(const std::string& markdown) {
    std::vector<Heading> headings;
    
    // C++17 doesn't support multiline flag, process line by line instead
    std::regex heading_regex(R"(^(#{1,6})\s+(.+)$)");
    std::smatch match;
    std::string::const_iterator search_start(markdown.cbegin());
    
    while (std::regex_search(search_start, markdown.cend(), match, heading_regex)) {
        Heading heading;
        heading.level = match.str(1).length();
        heading.text = match.str(2);
        heading.start = match.position() + std::distance(markdown.cbegin(), search_start);
        heading.end = heading.start + match.str().length();
        headings.push_back(heading);
        search_start = match.suffix().first;
    }
    
    return headings;
}

std::string HeadingParser::extract_heading_text(const std::string& markdown,
                                                const Heading& heading) {
    return heading.text;
}

}  // namespace markdown
}  // namespace hunterclaw
