// 🎯 Strict alignment: OpenClaw interactive/payload.ts
// Interactive reply message payload types and normalization —
// buttons, select, text blocks, content detection, fallback text extraction.

#include <hunterclaw/channels/plugin.h>
#include <hunterclaw/core/types.h>
#include <hunterclaw/core/json.h>
#include <hunterclaw/core/logging.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw::channels {

namespace {
auto log() { return core::get_logger("interactive-payload"); }

std::optional<std::string> normalize_optional_string(const nlohmann::json& val) {
    if (val.is_string()) {
        auto s = val.get<std::string>();
        if (s.find_first_not_of(' ') != std::string::npos) return s;
    }
    return std::nullopt;
}
} // namespace

// ─── Types (aligned: interactive/payload.ts) ─────────────────────────────

enum class InteractiveButtonStyle { kPrimary, kSecondary, kSuccess, kDanger };

struct InteractiveReplyButton {
    std::string label;
    std::string value;
    std::optional<InteractiveButtonStyle> style;
};

struct InteractiveReplyOption {
    std::string label;
    std::string value;
};

struct InteractiveReplyTextBlock {
    std::string text;
};

struct InteractiveReplyButtonsBlock {
    std::vector<InteractiveReplyButton> buttons;
};

struct InteractiveReplySelectBlock {
    std::optional<std::string> placeholder;
    std::vector<InteractiveReplyOption> options;
};

enum class InteractiveBlockType { kText, kButtons, kSelect };

struct InteractiveReplyBlock {
    InteractiveBlockType type;
    // Union-like storage
    std::optional<InteractiveReplyTextBlock> text_block;
    std::optional<InteractiveReplyButtonsBlock> buttons_block;
    std::optional<InteractiveReplySelectBlock> select_block;
};

struct InteractiveReply {
    std::vector<InteractiveReplyBlock> blocks;
};

// ─── Normalization (aligned: normalizeButtonStyle) ───────────────────────

// 🎯 对齐 OC: normalizeButtonStyle
std::optional<InteractiveButtonStyle> normalize_button_style(const nlohmann::json& val) {
    auto s = normalize_optional_string(val);
    if (!s.has_value()) return std::nullopt;
    std::string lower = *s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "primary") return InteractiveButtonStyle::kPrimary;
    if (lower == "secondary") return InteractiveButtonStyle::kSecondary;
    if (lower == "success") return InteractiveButtonStyle::kSuccess;
    if (lower == "danger") return InteractiveButtonStyle::kDanger;
    return std::nullopt;
}

// 🎯 对齐 OC: normalizeInteractiveButton
std::optional<InteractiveReplyButton> normalize_interactive_button(const nlohmann::json& raw) {
    if (!raw.is_object()) return std::nullopt;

    auto label = normalize_optional_string(raw.value("label", nlohmann::json{}));
    if (!label) label = normalize_optional_string(raw.value("text", nlohmann::json{}));

    auto value = normalize_optional_string(raw.value("value", nlohmann::json{}));
    if (!value) value = normalize_optional_string(raw.value("callbackData", nlohmann::json{}));
    if (!value) value = normalize_optional_string(raw.value("callback_data", nlohmann::json{}));

    if (!label || !value) return std::nullopt;
    return InteractiveReplyButton{*label, *value, normalize_button_style(raw.value("style", nlohmann::json{}))};
}

// 🎯 对齐 OC: normalizeInteractiveOption
std::optional<InteractiveReplyOption> normalize_interactive_option(const nlohmann::json& raw) {
    if (!raw.is_object()) return std::nullopt;
    auto label = normalize_optional_string(raw.value("label", nlohmann::json{}));
    if (!label) label = normalize_optional_string(raw.value("text", nlohmann::json{}));
    auto value = normalize_optional_string(raw.value("value", nlohmann::json{}));
    if (!label || !value) return std::nullopt;
    return InteractiveReplyOption{*label, *value};
}

// ─── Block normalization (aligned: normalizeInteractiveBlock) ────────────

// 🎯 对齐 OC: normalizeInteractiveBlock
std::optional<InteractiveReplyBlock> normalize_interactive_block(const nlohmann::json& raw) {
    if (!raw.is_object()) return std::nullopt;

    auto type_str = raw.value("type", std::string(""));

    if (type_str == "text") {
        auto text = normalize_optional_string(raw.value("text", nlohmann::json{}));
        if (!text) return std::nullopt;
        InteractiveReplyBlock block;
        block.type = InteractiveBlockType::kText;
        block.text_block = InteractiveReplyTextBlock{*text};
        return block;
    }

    if (type_str == "buttons") {
        auto buttons_raw = raw.value("buttons", nlohmann::json::array());
        if (!buttons_raw.is_array()) return std::nullopt;
        InteractiveReplyButtonsBlock bb;
        for (const auto& b : buttons_raw) {
            auto btn = normalize_interactive_button(b);
            if (btn) bb.buttons.push_back(*btn);
        }
        if (bb.buttons.empty()) return std::nullopt;
        InteractiveReplyBlock block;
        block.type = InteractiveBlockType::kButtons;
        block.buttons_block = bb;
        return block;
    }

    if (type_str == "select") {
        auto options_raw = raw.value("options", nlohmann::json::array());
        if (!options_raw.is_array()) return std::nullopt;
        InteractiveReplySelectBlock sb;
        sb.placeholder = normalize_optional_string(raw.value("placeholder", nlohmann::json{}));
        for (const auto& o : options_raw) {
            auto opt = normalize_interactive_option(o);
            if (opt) sb.options.push_back(*opt);
        }
        if (sb.options.empty()) return std::nullopt;
        InteractiveReplyBlock block;
        block.type = InteractiveBlockType::kSelect;
        block.select_block = sb;
        return block;
    }

    return std::nullopt;
}

// ─── Reply normalization (aligned: normalizeInteractiveReply) ────────────

// 🎯 对齐 OC: normalizeInteractiveReply
std::optional<InteractiveReply> normalize_interactive_reply(const nlohmann::json& raw) {
    if (!raw.is_object()) return std::nullopt;

    auto blocks_raw = raw.value("blocks", nlohmann::json::array());
    if (!blocks_raw.is_array()) return std::nullopt;

    InteractiveReply reply;
    for (const auto& b : blocks_raw) {
        auto block = normalize_interactive_block(b);
        if (block) reply.blocks.push_back(*block);
    }

    return reply.blocks.empty() ? std::nullopt : std::optional<InteractiveReply>(reply);
}

// ─── Content detection (aligned: hasInteractiveReplyBlocks) ──────────────

// 🎯 对齐 OC: hasInteractiveReplyBlocks
bool has_interactive_reply_blocks(const nlohmann::json& interactive_raw) {
    auto reply = normalize_interactive_reply(interactive_raw);
    return reply.has_value() && !reply->blocks.empty();
}

// 🎯 对齐 OC: hasReplyContent
bool has_reply_content(
        const std::optional<std::string>& text,
        const std::optional<std::string>& media_url,
        const std::vector<std::string>& media_urls,
        const nlohmann::json& interactive,
        bool has_channel_data,
        bool extra_content) {

    if (text.has_value() && !text->empty()) return true;
    if (media_url.has_value() && !media_url->empty()) return true;
    for (const auto& url : media_urls) {
        if (!url.empty()) return true;
    }
    if (has_interactive_reply_blocks(interactive)) return true;
    if (has_channel_data) return true;
    if (extra_content) return true;
    return false;
}

// ─── Fallback text (aligned: resolveInteractiveTextFallback) ─────────────

// 🎯 对齐 OC: resolveInteractiveTextFallback
std::optional<std::string> resolve_interactive_text_fallback(
        const std::optional<std::string>& text,
        const std::optional<InteractiveReply>& interactive) {

    if (text.has_value() && !text->empty()) return text;

    if (!interactive.has_value()) return text;

    std::string result;
    for (const auto& block : interactive->blocks) {
        if (block.type == InteractiveBlockType::kText && block.text_block.has_value()) {
            auto trimmed = block.text_block->text;
            // Trim
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            if (!trimmed.empty()) {
                if (!result.empty()) result += "\n\n";
                result += trimmed;
            }
        }
    }

    return result.empty() ? text : std::optional<std::string>(result);
}

} // namespace hunterclaw::channels
