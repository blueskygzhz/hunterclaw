#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <memory>

namespace hunterclaw {
namespace markdown {

// ============================================================================
// Markdown System - 严格对齐 OpenClaw src/markdown/
// ============================================================================
// 对齐文件:
// - markdown/ir.ts         (Markdown IR - 中间表示)
// - markdown/render.ts     (Markdown 渲染)
// - markdown/fences.ts     (代码块解析)
// - markdown/tables.ts     (表格转换)
// - markdown/code-spans.ts (代码片段)
// - markdown/frontmatter.ts (YAML front matter)
// - markdown/whatsapp.ts   (WhatsApp 格式化)
// ============================================================================

// ============================================================================
// 1. Markdown Style (对齐 markdown/ir.ts MarkdownStyle)
// ============================================================================

enum class MarkdownStyle {
    Bold,            // **bold**
    Italic,          // *italic*
    Strikethrough,   // ~~strikethrough~~
    Code,            // `code`
    CodeBlock,       // ```code block```
    Spoiler,         // ||spoiler||
    Blockquote       // > blockquote
};

// Style span with position
struct MarkdownStyleSpan {
    int start;
    int end;
    MarkdownStyle style;
};

// Link span
struct MarkdownLinkSpan {
    int start;
    int end;
    std::string href;
};

// ============================================================================
// 2. Markdown IR (对齐 markdown/ir.ts MarkdownIR)
// ============================================================================

// Markdown Intermediate Representation
struct MarkdownIR {
    std::string text;                      // Plain text
    std::vector<MarkdownStyleSpan> styles; // Style spans
    std::vector<MarkdownLinkSpan> links;   // Link spans
};

// Parse options
struct MarkdownParseOptions {
    bool linkify = true;
    bool enable_spoilers = false;
    std::string heading_style = "bold";  // "none" | "bold"
    std::string blockquote_prefix = "> ";
    bool autolink = true;
    std::string table_mode = "off";  // "off" | "bullets" | "code"
};

// Parse result with metadata
struct MarkdownParseResult {
    MarkdownIR ir;
    bool has_tables;
};

// Markdown parser
class MarkdownParser {
public:
    MarkdownParser();
    ~MarkdownParser();
    
    // Parse markdown to IR
    MarkdownIR parse(const std::string& markdown,
                     const MarkdownParseOptions& options = {});
    
    // Parse with metadata
    MarkdownParseResult parse_with_meta(const std::string& markdown,
                                        const MarkdownParseOptions& options = {});
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 3. Markdown Renderer (对齐 markdown/render.ts)
// ============================================================================

// Style marker (open/close tags)
struct RenderStyleMarker {
    std::string open;
    std::string close;
};

using RenderStyleMap = std::unordered_map<MarkdownStyle, RenderStyleMarker>;

// Render link
struct RenderLink {
    int start;
    int end;
    std::string open;
    std::string close;
};

// Render options
struct RenderOptions {
    RenderStyleMap style_markers;
    std::function<std::string(const std::string&)> escape_text;
    std::function<std::optional<RenderLink>(const MarkdownLinkSpan&, const std::string&)> build_link;
};

// Markdown renderer
class MarkdownRenderer {
public:
    // Render IR with markers
    static std::string render_with_markers(const MarkdownIR& ir,
                                           const RenderOptions& options);
    
    // Render to Markdown
    static std::string render_to_markdown(const MarkdownIR& ir);
    
    // Render to HTML
    static std::string render_to_html(const MarkdownIR& ir);
    
    // Render to plain text (strip all formatting)
    static std::string render_to_plain_text(const MarkdownIR& ir);
    
private:
    // Sort style spans for rendering
    static std::vector<MarkdownStyleSpan> sort_style_spans(
        const std::vector<MarkdownStyleSpan>& spans);
};

// ============================================================================
// 4. Code Fences (对齐 markdown/fences.ts)
// ============================================================================

// Fence span (code block range)
struct FenceSpan {
    int start;
    int end;
    std::string open_line;
    std::string marker;    // ``` or ~~~
    std::string indent;
};

// Fence parser
class FenceParser {
public:
    // Parse fence spans
    static std::vector<FenceSpan> parse_fence_spans(const std::string& buffer);
    
    // Find fence span at index
    static std::optional<FenceSpan> find_fence_span_at(
        const std::vector<FenceSpan>& spans,
        int index);
    
    // Check if index is inside fence
    static bool is_inside_fence(const std::vector<FenceSpan>& spans, int index);
    
    // Extract fence info (language)
    static std::string extract_fence_info(const std::string& open_line);
    
    // Extract fence content
    static std::string extract_fence_content(const std::string& buffer,
                                             const FenceSpan& span);
};

// ============================================================================
// 5. Code Spans (对齐 markdown/code-spans.ts)
// ============================================================================

// Code span (inline code range)
struct CodeSpan {
    int start;
    int end;
    int backticks;  // Number of backticks
};

// Code span parser
class CodeSpanParser {
public:
    // Parse code spans
    static std::vector<CodeSpan> parse_code_spans(const std::string& buffer);
    
    // Find code span at index
    static std::optional<CodeSpan> find_code_span_at(
        const std::vector<CodeSpan>& spans,
        int index);
    
    // Check if index is inside code span
    static bool is_inside_code_span(const std::vector<CodeSpan>& spans, int index);
    
    // Extract code span content
    static std::string extract_code_span_content(const std::string& buffer,
                                                  const CodeSpan& span);
};

// ============================================================================
// 6. Tables (对齐 markdown/tables.ts)
// ============================================================================

// Table mode
enum class MarkdownTableMode {
    Off,       // Don't process tables
    Bullets,   // Convert to bullet list
    Code       // Convert to code block
};

// Table cell
struct TableCell {
    std::string text;
    std::vector<MarkdownStyleSpan> styles;
    std::vector<MarkdownLinkSpan> links;
};

// Table structure
struct MarkdownTable {
    std::vector<TableCell> headers;
    std::vector<std::vector<TableCell>> rows;
};

// Table converter
class TableConverter {
public:
    // Convert markdown tables
    static std::string convert_tables(const std::string& markdown,
                                       MarkdownTableMode mode);
    
    // Parse table
    static std::optional<MarkdownTable> parse_table(const std::string& markdown,
                                                     int start_pos);
    
    // Convert table to bullets
    static std::string table_to_bullets(const MarkdownTable& table);
    
    // Convert table to code block
    static std::string table_to_code_block(const MarkdownTable& table);
};

// ============================================================================
// 7. Front Matter (对齐 markdown/frontmatter.ts)
// ============================================================================

// Front matter type
enum class FrontMatterType {
    None,
    YAML,
    TOML,
    JSON
};

// Front matter result
struct FrontMatter {
    FrontMatterType type;
    std::string content;
    int start;
    int end;
};

// Front matter parser
class FrontMatterParser {
public:
    // Parse front matter
    static std::optional<FrontMatter> parse(const std::string& markdown);
    
    // Strip front matter
    static std::string strip_front_matter(const std::string& markdown);
    
    // Extract front matter and content
    struct ParseResult {
        std::optional<FrontMatter> front_matter;
        std::string content;
    };
    static ParseResult parse_with_content(const std::string& markdown);
    
    // Parse YAML front matter to JSON-like structure
    static std::unordered_map<std::string, std::string> parse_yaml(
        const std::string& yaml_content);
};

// ============================================================================
// 8. WhatsApp Formatting (对齐 markdown/whatsapp.ts)
// ============================================================================

// WhatsApp style markers
struct WhatsAppStyle {
    static RenderStyleMap get_style_markers();
};

// WhatsApp formatter
class WhatsAppFormatter {
public:
    // Convert markdown to WhatsApp format
    static std::string markdown_to_whatsapp(const std::string& markdown);
    
    // Convert WhatsApp format to markdown
    static std::string whatsapp_to_markdown(const std::string& whatsapp_text);
    
    // Check if text has WhatsApp formatting
    static bool has_whatsapp_formatting(const std::string& text);
};

// ============================================================================
// 9. Markdown Utilities
// ============================================================================

class MarkdownUtils {
public:
    // Escape markdown characters
    static std::string escape_markdown(const std::string& text);
    
    // Unescape markdown characters
    static std::string unescape_markdown(const std::string& text);
    
    // Strip markdown formatting
    static std::string strip_formatting(const std::string& markdown);
    
    // Extract links from markdown
    static std::vector<std::string> extract_links(const std::string& markdown);
    
    // Extract code blocks from markdown
    static std::vector<std::string> extract_code_blocks(const std::string& markdown);
    
    // Count words in markdown (ignoring formatting)
    static int count_words(const std::string& markdown);
    
    // Truncate markdown safely (preserve formatting)
    static std::string truncate(const std::string& markdown,
                                 int max_length,
                                 const std::string& suffix = "...");
    
    // Normalize whitespace
    static std::string normalize_whitespace(const std::string& markdown);
    
    // Convert line endings
    static std::string normalize_line_endings(const std::string& markdown,
                                               const std::string& line_ending = "\n");
};

// ============================================================================
// 10. Advanced Markdown Features
// ============================================================================

// Blockquote parser
class BlockquoteParser {
public:
    struct BlockquoteSpan {
        int start;
        int end;
        int level;  // Nesting level
    };
    
    static std::vector<BlockquoteSpan> parse_blockquotes(const std::string& markdown);
    static std::string extract_blockquote_content(const std::string& markdown,
                                                   const BlockquoteSpan& span);
};

// List parser
class ListParser {
public:
    enum class ListType {
        Bullet,
        Ordered
    };
    
    struct ListItem {
        ListType type;
        int level;
        int index;  // For ordered lists
        std::string marker;
        std::string content;
        int start;
        int end;
    };
    
    static std::vector<ListItem> parse_list_items(const std::string& markdown);
    static std::string extract_list_item_content(const std::string& markdown,
                                                  const ListItem& item);
};

// Heading parser
class HeadingParser {
public:
    struct Heading {
        int level;  // 1-6
        std::string text;
        int start;
        int end;
    };
    
    static std::vector<Heading> parse_headings(const std::string& markdown);
    static std::string extract_heading_text(const std::string& markdown,
                                             const Heading& heading);
};

// ============================================================================
// 11. Markdown Converter
// ============================================================================

// Convert between formats
class MarkdownConverter {
public:
    // Markdown to HTML
    static std::string markdown_to_html(const std::string& markdown,
                                        const MarkdownParseOptions& options = {});
    
    // HTML to Markdown
    static std::string html_to_markdown(const std::string& html);
    
    // Markdown to plain text
    static std::string markdown_to_plain_text(const std::string& markdown);
    
    // Markdown to Slack format
    static std::string markdown_to_slack(const std::string& markdown);
    
    // Markdown to Discord format
    static std::string markdown_to_discord(const std::string& markdown);
    
    // Markdown to Telegram format
    static std::string markdown_to_telegram(const std::string& markdown);
};

// ============================================================================
// 12. Markdown Validator
// ============================================================================

// Validation issue
struct MarkdownValidationIssue {
    std::string code;
    std::string message;
    int line;
    int column;
};

// Markdown validator
class MarkdownValidator {
public:
    // Validate markdown
    static std::vector<MarkdownValidationIssue> validate(const std::string& markdown);
    
    // Check for unclosed formatting
    static bool has_unclosed_formatting(const std::string& markdown);
    
    // Check for broken links
    static std::vector<std::string> find_broken_links(const std::string& markdown);
    
    // Check for invalid table syntax
    static bool has_invalid_tables(const std::string& markdown);
};

}  // namespace markdown
}  // namespace hunterclaw
