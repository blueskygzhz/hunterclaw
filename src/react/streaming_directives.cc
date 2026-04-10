#include "hunterclaw/react/streaming_directives.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace react {

namespace {
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

bool is_silent_reply_text(const std::string& text, const std::string& token) {
    return trim(text) == token;
}

bool is_silent_reply_prefix_text(const std::string& text, const std::string& token) {
    auto trimmed = trim(text);
    if (trimmed.empty()) return false;
    if (trimmed.find(token) == 0) {
        return trim(trimmed.substr(token.size())).empty();
    }
    return false;
}

bool starts_with_silent_token(const std::string& text, const std::string& token) {
    return text.find(token) == 0;
}

std::string strip_leading_silent(const std::string& text, const std::string& token) {
    auto pos = text.find(token);
    if (pos == std::string::npos) return text;
    return text.substr(pos + token.size());
}

bool has_renderable_content(const DirectiveParseResult& r) {
    if (!trim(r.text).empty()) return true;
    if (r.media_url.has_value()) return true;
    if (!r.media_urls.empty()) return true;
    if (r.audio_as_voice) return true;
    return false;
}
} // anon

StreamingDirectiveAccumulator::StreamingDirectiveAccumulator() {
    reset();
}

void StreamingDirectiveAccumulator::reset() {
    pending_tail_.clear();
    pending_reply_ = {};
    active_reply_ = {};
}

// 对齐 streaming-directives.ts:28-41
StreamingDirectiveAccumulator::SplitResult
StreamingDirectiveAccumulator::split_trailing_directive(const std::string& text) {
    auto open_idx = text.rfind("[[");
    if (open_idx == std::string::npos) {
        return {text, ""};
    }
    auto close_idx = text.find("]]", open_idx + 2);
    if (close_idx != std::string::npos) {
        return {text, ""};  // Complete directive — no split needed
    }
    return {text.substr(0, open_idx), text.substr(open_idx)};
}

// 对齐 streaming-directives.ts:43-76
DirectiveParseResult StreamingDirectiveAccumulator::parse_chunk(
    const std::string& raw,
    const std::string& silent_token
) {
    DirectiveParseResult result;
    std::string text = raw;

    // Parse [[reply:ID]] and [[reply:current]] tags
    static const std::regex reply_tag_re(R"(\[\[reply:([^\]]+)\]\])");
    static const std::regex reply_current_re(R"(\[\[reply:current\]\])", std::regex_constants::icase);
    static const std::regex media_re(R"(\[\[MEDIA:([^\]]+)\]\])");
    static const std::regex audio_re(R"(\[\[AUDIO_AS_VOICE\]\])", std::regex_constants::icase);

    // Extract reply tags
    std::smatch reply_match;
    if (std::regex_search(text, reply_match, reply_current_re)) {
        result.reply_to_current = true;
        result.reply_to_tag = true;
        text = std::regex_replace(text, reply_current_re, "");
    }
    if (std::regex_search(text, reply_match, reply_tag_re)) {
        result.reply_to_id = reply_match[1].str();
        result.reply_to_tag = true;
        text = std::regex_replace(text, reply_tag_re, "");
    }

    // Extract MEDIA directives
    std::sregex_iterator media_it(text.begin(), text.end(), media_re);
    std::sregex_iterator end;
    for (; media_it != end; ++media_it) {
        auto url = (*media_it)[1].str();
        result.media_urls.push_back(url);
        if (!result.media_url.has_value()) result.media_url = url;
    }
    text = std::regex_replace(text, media_re, "");

    // Extract audio tag
    if (std::regex_search(text, audio_re)) {
        result.audio_as_voice = true;
        text = std::regex_replace(text, audio_re, "");
    }

    // Silent token handling
    if (is_silent_reply_text(text, silent_token) ||
        is_silent_reply_prefix_text(text, silent_token)) {
        result.is_silent = true;
        text = "";
    } else if (starts_with_silent_token(text, silent_token)) {
        text = strip_leading_silent(text, silent_token);
    }

    result.text = text;
    return result;
}

// 对齐 streaming-directives.ts:92-138
std::optional<DirectiveParseResult> StreamingDirectiveAccumulator::consume(
    const std::string& raw,
    bool is_final,
    const std::string& silent_token
) {
    std::string combined = pending_tail_ + raw;
    pending_tail_.clear();

    if (!is_final) {
        auto split = split_trailing_directive(combined);
        combined = split.text;
        pending_tail_ = split.tail;
    }

    if (combined.empty()) return std::nullopt;

    auto parsed = parse_chunk(combined, silent_token);

    // Merge reply state (sticky across chunks)
    bool has_tag = active_reply_.has_tag || pending_reply_.has_tag || parsed.reply_to_tag;
    bool saw_current = active_reply_.saw_current || pending_reply_.saw_current || parsed.reply_to_current;
    std::optional<std::string> explicit_id =
        parsed.reply_to_id.has_value() ? parsed.reply_to_id :
        pending_reply_.explicit_id.has_value() ? pending_reply_.explicit_id :
        active_reply_.explicit_id;

    DirectiveParseResult combined_result = parsed;
    combined_result.reply_to_id = explicit_id;
    combined_result.reply_to_current = saw_current;
    combined_result.reply_to_tag = has_tag;

    if (!has_renderable_content(combined_result)) {
        if (has_tag) {
            pending_reply_ = {explicit_id, saw_current, has_tag};
        }
        return std::nullopt;
    }

    // Keep reply context sticky
    active_reply_ = {explicit_id, saw_current, has_tag};
    pending_reply_ = {};
    return combined_result;
}

} // namespace react
} // namespace hunterclaw
