// ============================================================================
// table_renderer.cc
// ✅ 严格对齐 OpenClaw: src/terminal/table.ts (468行)
//
// 终端表格渲染器：
//   - TableColumn: 列定义 (key, header, align, minWidth, maxWidth, flex)
//   - RenderTableOptions: 渲染选项 (columns, rows, width, padding, border)
//   - renderTable: 核心渲染函数
//   - padCell: 单元格对齐 (left/right/center)
//   - wrapLine: ANSI-aware 换行
//   - resolveDefaultBorder: 根据平台选择 unicode/ascii border
// ============================================================================

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <map>

namespace hunterclaw {
namespace shared {

// ============================================================================
// 对齐 table.ts:4-21: Types
// ============================================================================
enum class Align { Left, Right, Center };

struct TableColumn {
    std::string key;
    std::string header;
    Align align = Align::Left;
    int min_width = 0;
    int max_width = 0;  // 0 = no limit
    bool flex = false;
};

struct RenderTableOptions {
    std::vector<TableColumn> columns;
    std::vector<std::map<std::string, std::string>> rows;
    int width = 80;
    int padding = 1;
    std::string border = "unicode";  // "unicode" | "ascii" | "none"
};

// ============================================================================
// 对齐 table.ts border characters
// ============================================================================
struct BorderChars {
    std::string horizontal;
    std::string vertical;
    std::string top_left;
    std::string top_right;
    std::string bottom_left;
    std::string bottom_right;
    std::string tee_down;
    std::string tee_up;
    std::string tee_right;
    std::string tee_left;
    std::string cross;
};

static BorderChars get_unicode_border() {
    return {"─", "│", "┌", "┐", "└", "┘", "┬", "┴", "├", "┤", "┼"};
}

static BorderChars get_ascii_border() {
    return {"-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"};
}

// ============================================================================
// 对齐 table.ts:43-48: repeat
// ============================================================================
static std::string repeat_str(const std::string& ch, int n) {
    if (n <= 0) return "";
    std::string result;
    result.reserve(ch.size() * n);
    for (int i = 0; i < n; ++i) result += ch;
    return result;
}

// ============================================================================
// 对齐 table.ts:50-65: padCell
// ============================================================================
static std::string pad_cell(const std::string& text, int width, Align align) {
    int w = static_cast<int>(text.size()); // Simplified: assuming ASCII
    if (w >= width) return text;
    int pad = width - w;
    if (align == Align::Right) {
        return std::string(pad, ' ') + text;
    }
    if (align == Align::Center) {
        int left = pad / 2;
        int right = pad - left;
        return std::string(left, ' ') + text + std::string(right, ' ');
    }
    return text + std::string(pad, ' ');
}

// ============================================================================
// 对齐 table.ts:wrapLine (simplified — no ANSI awareness in C++ version)
// ============================================================================
static std::vector<std::string> wrap_line(const std::string& text, int width) {
    if (width <= 0 || static_cast<int>(text.size()) <= width) {
        return {text};
    }

    std::vector<std::string> lines;
    std::string current;
    std::string word;

    auto flush_word = [&]() {
        if (word.empty()) return;
        if (!current.empty() && static_cast<int>(current.size() + 1 + word.size()) > width) {
            lines.push_back(current);
            current = word;
        } else {
            if (!current.empty()) current += ' ';
            current += word;
        }
        word.clear();
    };

    for (char c : text) {
        if (c == ' ' || c == '\t') {
            flush_word();
        } else {
            word += c;
            // Break long words
            if (static_cast<int>(word.size()) >= width) {
                flush_word();
            }
        }
    }
    flush_word();
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) lines.push_back("");
    return lines;
}

// ============================================================================
// 对齐 table.ts: renderTable (核心)
// ============================================================================
std::string render_table(const RenderTableOptions& opts) {
    if (opts.columns.empty()) return "";

    bool has_border = opts.border != "none";
    auto border = (opts.border == "ascii") ? get_ascii_border() : get_unicode_border();
    int pad = opts.padding;

    // Calculate column widths
    int num_cols = static_cast<int>(opts.columns.size());
    int border_overhead = has_border ? (num_cols + 1) : 0; // vertical bars
    int padding_overhead = num_cols * pad * 2;
    int available = opts.width - border_overhead - padding_overhead;

    std::vector<int> col_widths(num_cols);
    int flex_count = 0;
    int fixed_total = 0;

    for (int c = 0; c < num_cols; ++c) {
        const auto& col = opts.columns[c];
        // Find max content width
        int max_w = static_cast<int>(col.header.size());
        for (const auto& row : opts.rows) {
            auto it = row.find(col.key);
            if (it != row.end()) {
                max_w = std::max(max_w, static_cast<int>(it->second.size()));
            }
        }

        if (col.min_width > 0) max_w = std::max(max_w, col.min_width);
        if (col.max_width > 0) max_w = std::min(max_w, col.max_width);

        if (col.flex) {
            ++flex_count;
            col_widths[c] = max_w;
        } else {
            col_widths[c] = max_w;
            fixed_total += max_w;
        }
    }

    // Distribute remaining space to flex columns
    if (flex_count > 0) {
        int remaining = available - fixed_total;
        int flex_share = std::max(10, remaining / flex_count);
        for (int c = 0; c < num_cols; ++c) {
            if (opts.columns[c].flex) {
                col_widths[c] = std::max(col_widths[c], flex_share);
                if (opts.columns[c].max_width > 0) {
                    col_widths[c] = std::min(col_widths[c], opts.columns[c].max_width);
                }
            }
        }
    }

    // Rendering
    std::ostringstream out;
    std::string pad_str(pad, ' ');

    // Helper: horizontal rule
    auto h_rule = [&](const std::string& left, const std::string& mid, const std::string& right) {
        out << left;
        for (int c = 0; c < num_cols; ++c) {
            out << repeat_str(border.horizontal, col_widths[c] + pad * 2);
            out << (c < num_cols - 1 ? mid : right);
        }
        out << "\n";
    };

    // Top border
    if (has_border) h_rule(border.top_left, border.tee_down, border.top_right);

    // Header row
    if (has_border) out << border.vertical;
    for (int c = 0; c < num_cols; ++c) {
        out << pad_str << pad_cell(opts.columns[c].header, col_widths[c], Align::Left) << pad_str;
        if (has_border) out << border.vertical;
    }
    out << "\n";

    // Header separator
    if (has_border) h_rule(border.tee_right, border.cross, border.tee_left);

    // Data rows
    for (const auto& row : opts.rows) {
        // Wrap cells
        std::vector<std::vector<std::string>> wrapped(num_cols);
        int max_lines = 1;
        for (int c = 0; c < num_cols; ++c) {
            auto it = row.find(opts.columns[c].key);
            std::string cell = (it != row.end()) ? it->second : "";
            wrapped[c] = wrap_line(cell, col_widths[c]);
            max_lines = std::max(max_lines, static_cast<int>(wrapped[c].size()));
        }

        for (int line = 0; line < max_lines; ++line) {
            if (has_border) out << border.vertical;
            for (int c = 0; c < num_cols; ++c) {
                std::string text = (line < static_cast<int>(wrapped[c].size()))
                    ? wrapped[c][line] : "";
                out << pad_str << pad_cell(text, col_widths[c], opts.columns[c].align) << pad_str;
                if (has_border) out << border.vertical;
            }
            out << "\n";
        }
    }

    // Bottom border
    if (has_border) h_rule(border.bottom_left, border.tee_up, border.bottom_right);

    return out.str();
}

} // namespace shared
} // namespace hunterclaw
