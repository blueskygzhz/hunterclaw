#pragma once
// ============================================================================
// streaming_directives.h — 严格对齐 OpenClaw streaming-directives.ts (146行)
//
// Streaming 阶段的指令累积器: [[reply:ID]], [[MEDIA:URL]], silent token
// ============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace react {

// 对齐 reply-directives.ts: ReplyDirectiveParseResult
struct DirectiveParseResult {
    std::string text;
    std::vector<std::string> media_urls;
    std::optional<std::string> media_url;
    std::optional<std::string> reply_to_id;
    bool reply_to_current = false;
    bool reply_to_tag = false;
    bool audio_as_voice = false;
    bool is_silent = false;
};

// 对齐 streaming-directives.ts:81-145: createStreamingDirectiveAccumulator
class StreamingDirectiveAccumulator {
public:
    StreamingDirectiveAccumulator();

    // 对齐: consume(raw, {final?, silentToken?})
    // Returns parsed result, or nullopt if chunk has no renderable content
    std::optional<DirectiveParseResult> consume(
        const std::string& raw,
        bool is_final = false,
        const std::string& silent_token = "NO_REPLY"
    );

    // 对齐: reset()
    void reset();

private:
    std::string pending_tail_;

    struct ReplyState {
        std::optional<std::string> explicit_id;
        bool saw_current = false;
        bool has_tag = false;
    };
    ReplyState pending_reply_;
    ReplyState active_reply_;

    // 对齐: splitTrailingDirective
    struct SplitResult {
        std::string text;
        std::string tail;
    };
    static SplitResult split_trailing_directive(const std::string& text);

    // 对齐: parseChunk
    static DirectiveParseResult parse_chunk(
        const std::string& raw,
        const std::string& silent_token
    );
};

} // namespace react
} // namespace hunterclaw
