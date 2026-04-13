// ============================================================================
// canvas_render.cc
// ✅ 严格对齐 OpenClaw: src/chat/canvas-render.ts (246行)
//
// Canvas 预览解析系统：
//   - CanvasPreview: canvas 元数据结构
//   - coerceCanvasPreview: 从 JSON record 解析 canvas preview
//   - parseCanvasAttributes: 从 shortcode 属性解析
//   - extractCanvasFromText: 从 markdown fence 提取 canvas
//   - extractCanvasFromContentBlocks: 从 content blocks 提取
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <regex>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

namespace hunterclaw {
namespace channels {

namespace {
auto log() { return spdlog::get("canvas"); }
} // namespace

// ============================================================================
// 对齐 canvas-render.ts:3-15: CanvasPreview
// ============================================================================
struct CanvasPreview {
    std::string kind = "canvas";
    std::string surface;  // "assistant_message"
    std::string render;   // "url"
    std::optional<std::string> title;
    std::optional<int> preferred_height;
    std::optional<std::string> url;
    std::optional<std::string> view_id;
    std::optional<std::string> class_name;
    std::optional<std::string> style;
};

// ============================================================================
// 对齐 canvas-render.ts helpers
// ============================================================================
static std::optional<std::string> get_string_field(const json& record, const std::string& key) {
    if (record.contains(key) && record[key].is_string()) {
        std::string val = record[key];
        if (!val.empty()) return val;
    }
    return std::nullopt;
}

static std::optional<int> get_number_field(const json& record, const std::string& key) {
    if (record.contains(key) && record[key].is_number()) {
        return record[key].get<int>();
    }
    return std::nullopt;
}

static json get_nested_record(const json& record, const std::string& key) {
    if (record.contains(key) && record[key].is_object()) return record[key];
    return json();
}

// 对齐 canvas-render.ts:57-59
static std::optional<std::string> normalize_surface(const std::string& value) {
    if (value == "assistant_message") return value;
    return std::nullopt;
}

// 对齐 canvas-render.ts:61-65
static std::optional<int> normalize_preferred_height(std::optional<int> value) {
    if (!value.has_value()) return std::nullopt;
    int v = *value;
    if (v < 160) return std::nullopt;
    return std::min(v, 1200);
}

// ============================================================================
// 对齐 canvas-render.ts:67-130: coerceCanvasPreview
// ============================================================================
std::optional<CanvasPreview> coerce_canvas_preview(const json& record) {
    if (record.is_null() || !record.is_object()) return std::nullopt;

    auto kind = get_string_field(record, "kind");
    if (!kind.has_value()) return std::nullopt;
    std::string kind_lower = *kind;
    std::transform(kind_lower.begin(), kind_lower.end(), kind_lower.begin(), ::tolower);
    if (kind_lower != "canvas") return std::nullopt;

    auto presentation = get_nested_record(record, "presentation");
    auto view = get_nested_record(record, "view");
    auto source = get_nested_record(record, "source");

    auto requested_surface = get_string_field(presentation, "target");
    if (!requested_surface.has_value()) requested_surface = get_string_field(record, "target");
    std::string surface = "assistant_message";
    if (requested_surface.has_value()) {
        auto ns = normalize_surface(*requested_surface);
        if (!ns.has_value()) return std::nullopt;
        surface = *ns;
    }

    auto title = get_string_field(presentation, "title");
    if (!title.has_value()) title = get_string_field(view, "title");

    auto height = get_number_field(presentation, "preferred_height");
    if (!height.has_value()) height = get_number_field(presentation, "preferredHeight");
    if (!height.has_value()) height = get_number_field(view, "preferred_height");
    if (!height.has_value()) height = get_number_field(view, "preferredHeight");
    auto preferred_height = normalize_preferred_height(height);

    auto class_name = get_string_field(presentation, "class_name");
    if (!class_name.has_value()) class_name = get_string_field(presentation, "className");
    auto style = get_string_field(presentation, "style");

    // Check view.url or view.entryUrl
    auto view_url = get_string_field(view, "url");
    if (!view_url.has_value()) view_url = get_string_field(view, "entryUrl");
    auto view_id = get_string_field(view, "id");
    if (!view_id.has_value()) view_id = get_string_field(view, "docId");

    if (view_url.has_value()) {
        CanvasPreview preview;
        preview.surface = surface;
        preview.render = "url";
        preview.url = view_url;
        preview.view_id = view_id;
        preview.title = title;
        preview.preferred_height = preferred_height;
        preview.class_name = class_name;
        preview.style = style;
        return preview;
    }

    // Check source.type === "url"
    auto source_type = get_string_field(source, "type");
    if (source_type.has_value()) {
        std::string st = *source_type;
        std::transform(st.begin(), st.end(), st.begin(), ::tolower);
        if (st == "url") {
            auto url = get_string_field(source, "url");
            if (!url.has_value()) return std::nullopt;
            CanvasPreview preview;
            preview.surface = surface;
            preview.render = "url";
            preview.url = url;
            preview.title = title;
            preview.preferred_height = preferred_height;
            preview.class_name = class_name;
            preview.style = style;
            return preview;
        }
    }

    return std::nullopt;
}

// ============================================================================
// 对齐 canvas-render.ts:132-144: parseCanvasAttributes
// ============================================================================
std::map<std::string, std::string> parse_canvas_attributes(const std::string& raw) {
    std::map<std::string, std::string> attrs;
    // Match key="value" or key='value' attributes
    std::regex re_dq(R"__X__(([A-Za-z_][A-Za-z0-9_-]*)\s*=\s*"([^"]*)")__X__");
    std::regex re_sq(R"__X__(([A-Za-z_][A-Za-z0-9_-]*)\s*=\s*'([^']*)')__X__");
    // Pass 1: double-quoted
    {
        std::sregex_iterator it(raw.begin(), raw.end(), re_dq);
        std::sregex_iterator end_it;
        for (; it != end_it; ++it) {
            std::string key = (*it)[1];
            std::string value = (*it)[2];
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (!key.empty() && !value.empty()) attrs[key] = value;
        }
    }
    // Pass 2: single-quoted (lower priority)
    {
        std::sregex_iterator it(raw.begin(), raw.end(), re_sq);
        std::sregex_iterator end_it;
        for (; it != end_it; ++it) {
            std::string key = (*it)[1];
            std::string value = (*it)[2];
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (!key.empty() && !value.empty() && attrs.find(key) == attrs.end()) {
                attrs[key] = value;
            }
        }
    }
    return attrs;
}

// ============================================================================
// 对齐 canvas-render.ts:146-149: defaultCanvasEntryUrl
// ============================================================================
static std::string default_canvas_entry_url(const std::string& ref) {
    // Simple URL encoding (encode spaces and special chars)
    std::string encoded;
    for (char c : ref) {
        if (c == ' ') encoded += "%20";
        else if (c == '/') encoded += "%2F";
        else if (c == '?') encoded += "%3F";
        else if (c == '#') encoded += "%23";
        else encoded += c;
    }
    return "/__hunterclaw__/canvas/documents/" + encoded + "/index.html";
}

// ============================================================================
// 对齐 canvas-render.ts:151-179: previewFromShortcode
// ============================================================================
std::optional<CanvasPreview> preview_from_shortcode(
    const std::map<std::string, std::string>& attrs
) {
    auto target_it = attrs.find("target");
    if (target_it != attrs.end() && normalize_surface(target_it->second) != "assistant_message") {
        return std::nullopt;
    }

    auto title_it = attrs.find("title");
    auto height_it = attrs.find("height");
    auto class_it = attrs.find("class");
    if (class_it == attrs.end()) class_it = attrs.find("class_name");
    auto style_it = attrs.find("style");
    auto ref_it = attrs.find("ref");
    auto url_it = attrs.find("url");

    if (url_it == attrs.end() && ref_it == attrs.end()) return std::nullopt;

    CanvasPreview preview;
    preview.surface = "assistant_message";
    preview.render = "url";
    if (url_it != attrs.end()) {
        preview.url = url_it->second;
    } else {
        preview.url = default_canvas_entry_url(ref_it->second);
        preview.view_id = ref_it->second;
    }
    if (title_it != attrs.end()) preview.title = title_it->second;
    if (height_it != attrs.end()) {
        try { preview.preferred_height = normalize_preferred_height(std::stoi(height_it->second)); }
        catch (...) {}
    }
    if (class_it != attrs.end()) preview.class_name = class_it->second;
    if (style_it != attrs.end()) preview.style = style_it->second;
    return preview;
}

// ============================================================================
// 对齐 canvas-render.ts:extractCanvasFromText (simplified)
// Extracts canvas previews from assistant message text
// ============================================================================
std::vector<CanvasPreview> extract_canvas_from_text(const std::string& text) {
    std::vector<CanvasPreview> results;

    // Pattern 1: ```canvas shortcode
    std::regex fence_re(R"(```canvas\s+([^`]+)```)");
    std::sregex_iterator it(text.begin(), text.end(), fence_re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        std::string body = (*it)[1];
        // Try JSON parse first
        try {
            auto j = json::parse(body);
            auto preview = coerce_canvas_preview(j);
            if (preview.has_value()) {
                results.push_back(*preview);
                continue;
            }
        } catch (...) {}
        // Try shortcode attributes
        auto attrs = parse_canvas_attributes(body);
        auto preview = preview_from_shortcode(attrs);
        if (preview.has_value()) {
            results.push_back(*preview);
        }
    }

    return results;
}

} // namespace channels
} // namespace hunterclaw
