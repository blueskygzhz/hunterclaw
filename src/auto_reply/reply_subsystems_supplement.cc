// HunterClaw – reply_subsystems_supplement.cc
// 严格对齐 OC auto-reply/reply/ 剩余回复子系统:
//   session-fork.ts (989B) + session-fork.runtime.ts (1.75KB) — 会话分叉
//   conversation-binding-input.ts (3.86KB) — 会话绑定输入
//   reply-inline.ts (1.51KB) + reply-inline-whitespace.ts (194B) — 内联命令
//   reply-delivery.ts (5.51KB) — 回复投递路由
//   session-delivery.ts (8.74KB) — session 投递
//   block-reply-coalescer.ts (4.81KB) — 块回复合并器
// Total OC: ~27KB

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/sessions/session_manager.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/auto_reply/chunk.h"
#include "hunterclaw/config/config_system.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace hunterclaw::auto_reply {

// ═══════════════════════════════════════════════════════════════
// Session Fork (对齐 OC: session-fork.ts + session-fork.runtime.ts)
// ═══════════════════════════════════════════════════════════════

static constexpr int DEFAULT_PARENT_FORK_MAX_TOKENS = 100000;

int resolve_parent_fork_max_tokens(const std::string& config_json) {
    try {
        auto cfg = nlohmann::json::parse(config_json);
        auto val = cfg.value(nlohmann::json::json_pointer("/session/parentForkMaxTokens"), -1);
        if (val >= 0) return val;
    } catch (...) {}
    return DEFAULT_PARENT_FORK_MAX_TOKENS;
}

struct SessionForkResult {
    std::string session_id;
    std::string session_file;
    bool success = false;
};

SessionForkResult fork_session_from_parent(
    const std::string& parent_session_file,
    const std::string& agent_id,
    const std::string& sessions_dir) {
    namespace fs = std::filesystem;

    SessionForkResult result;
    if (parent_session_file.empty() || !fs::exists(parent_session_file)) {
        return result;
    }

    try {
        // Create a branched session by copying parent transcript
        result.session_id = sessions::generate_session_id();
        result.session_file = (fs::path(sessions_dir) /
            ("agent-" + agent_id) /
            ("session-" + result.session_id + ".jsonl")).string();

        // Ensure directory exists
        fs::create_directories(fs::path(result.session_file).parent_path());

        // Copy parent transcript as starting point for branched session
        fs::copy_file(parent_session_file, result.session_file,
                       fs::copy_options::overwrite_existing);
        result.success = true;
    } catch (...) {
        result.success = false;
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Conversation Binding Input (对齐 OC: conversation-binding-input.ts)
// ═══════════════════════════════════════════════════════════════

struct ConversationBindingInput {
    std::string channel;
    std::string surface;
    std::string provider;
    std::string account_id;
    std::string chat_type;
    std::string thread_id;
    std::string thread_parent_id;
    std::string sender_id;
    std::string session_key;
    std::string parent_session_key;
    std::string to;
    std::string from;
    std::string native_channel_id;
};

static std::string to_lower_trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    std::string r = s.substr(start, end - start + 1);
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string resolve_binding_channel(const ConversationBindingInput& ctx,
                                     const std::string& command_channel = "") {
    std::string raw = !ctx.channel.empty() ? ctx.channel
                    : !command_channel.empty() ? command_channel
                    : !ctx.surface.empty() ? ctx.surface
                    : ctx.provider;
    return to_lower_trim(raw);
}

struct ResolvedConversationBinding {
    std::string channel;
    std::string account_id;
    std::string sender_id;
    std::string chat_type;
    std::string thread_id;
    std::string to;
    bool is_valid = false;
};

ResolvedConversationBinding resolve_conversation_binding(
    const ConversationBindingInput& ctx,
    const std::string& command_channel = "") {
    ResolvedConversationBinding result;
    result.channel = resolve_binding_channel(ctx, command_channel);
    result.account_id = to_lower_trim(ctx.account_id);
    result.sender_id = to_lower_trim(ctx.sender_id);
    result.chat_type = to_lower_trim(ctx.chat_type);
    result.thread_id = ctx.thread_id.empty() ? ctx.thread_parent_id : ctx.thread_id;
    result.to = to_lower_trim(ctx.to.empty() ? ctx.from : ctx.to);
    result.is_valid = !result.channel.empty();
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Reply Inline (对齐 OC: reply-inline.ts + reply-inline-whitespace.ts)
// ═══════════════════════════════════════════════════════════════

// 对齐 collapseInlineHorizontalWhitespace
static std::string collapse_inline_horizontal_whitespace(const std::string& value) {
    static const std::regex ws_re("[^\\S\\n]+");
    return std::regex_replace(value, ws_re, " ");
}

struct InlineSimpleCommandResult {
    std::string command;
    std::string cleaned;
    bool found = false;
};

// 对齐 extractInlineSimpleCommand
InlineSimpleCommandResult extract_inline_simple_command(const std::string& body) {
    if (body.empty()) return {};

    static const std::regex inline_re(
        "(?:^|\\s)/(help|commands|whoami|id)(?=$|\\s|:)",
        std::regex_constants::icase);

    std::smatch match;
    if (!std::regex_search(body, match, inline_re)) return {};

    std::string alias = "/" + to_lower_trim(match[1].str());

    // Alias map
    static const std::map<std::string, std::string> aliases = {
        {"/help", "/help"}, {"/commands", "/commands"},
        {"/whoami", "/whoami"}, {"/id", "/whoami"},
    };
    auto it = aliases.find(alias);
    if (it == aliases.end()) return {};

    std::string cleaned = collapse_inline_horizontal_whitespace(
        std::regex_replace(body, inline_re, " ", std::regex_constants::format_first_only));
    // Trim
    auto s = cleaned.find_first_not_of(" \t\n\r");
    auto e = cleaned.find_last_not_of(" \t\n\r");
    cleaned = (s != std::string::npos) ? cleaned.substr(s, e - s + 1) : "";

    return {it->second, cleaned, true};
}

// 对齐 extractInlineStatusCommand
InlineSimpleCommandResult extract_inline_status_command(const std::string& body) {
    if (body.empty()) return {};

    static const std::regex status_re(
        "(?:^|\\s)/status(?=$|\\s|:)(?:\\s*:\\s*)?",
        std::regex_constants::icase);

    std::smatch match;
    if (!std::regex_search(body, match, status_re)) return {};

    std::string cleaned = collapse_inline_horizontal_whitespace(
        std::regex_replace(body, status_re, " "));
    auto s = cleaned.find_first_not_of(" \t\n\r");
    auto e = cleaned.find_last_not_of(" \t\n\r");
    cleaned = (s != std::string::npos) ? cleaned.substr(s, e - s + 1) : "";

    return {"/status", cleaned, true};
}

// ═══════════════════════════════════════════════════════════════
// Reply Delivery (对齐 OC: reply-delivery.ts:11-141)
// ═══════════════════════════════════════════════════════════════

enum class ReplyDirectiveParseMode { Always, Auto, Never };

struct NormalizedReplyPayloadResult {
    ReplyPayload payload;
    bool is_silent = false;
};

NormalizedReplyPayloadResult normalize_reply_payload_directives(
    const ReplyPayload& payload,
    const std::string& current_message_id = "",
    const std::string& silent_token = "NO_REPLY",
    ReplyDirectiveParseMode parse_mode = ReplyDirectiveParseMode::Always) {
    NormalizedReplyPayloadResult result;
    result.payload = payload;

    std::string text = payload.text.value_or("");
    if (text.empty()) {
        result.is_silent = true;
        return result;
    }

    // Check for silent reply
    if (is_silent_reply_text(text, silent_token)) {
        result.is_silent = true;
        return result;
    }

    bool should_parse = (parse_mode == ReplyDirectiveParseMode::Always) ||
        (parse_mode == ReplyDirectiveParseMode::Auto &&
         (text.find("[[") != std::string::npos ||
          text.find("MEDIA:") != std::string::npos ||
          text.find(silent_token) != std::string::npos));

    if (!should_parse) {
        return result;
    }

    // Parse [[reply_to_current]] directive
    static const std::regex reply_to_current_re("\\[\\[reply_to_current\\]\\]");
    if (std::regex_search(text, reply_to_current_re)) {
        result.payload.reply_to_current = true;
        text = std::regex_replace(text, reply_to_current_re, "");
        if (!current_message_id.empty()) {
            result.payload.reply_to_id = current_message_id;
            result.payload.reply_to_current = false;
        }
    }

    // Strip trailing silent token
    std::string stripped = strip_silent_token(text, silent_token);
    if (stripped.empty()) {
        result.is_silent = true;
        return result;
    }

    result.payload.text = stripped;
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Block Reply Coalescer (对齐 OC: block-reply-coalescer.ts)
// ═══════════════════════════════════════════════════════════════

struct BlockReplyCoalescerConfig {
    int min_chars = 200;
    int max_chars = 800;
    int idle_ms = 500;
    std::string joiner;
    bool flush_on_enqueue = false;
};

class BlockReplyCoalescer {
public:
    using FlushCallback = std::function<void(const ReplyPayload&)>;

    explicit BlockReplyCoalescer(BlockReplyCoalescerConfig config,
                                  std::function<bool()> should_abort,
                                  FlushCallback on_flush)
        : config_(std::move(config))
        , should_abort_(std::move(should_abort))
        , on_flush_(std::move(on_flush)) {}

    // 对齐 enqueue
    void enqueue(const ReplyPayload& payload) {
        std::string text = payload.text.value_or("");
        if (text.empty()) return;

        if (buffer_text_.empty()) {
            buffer_reply_to_id_ = payload.reply_to_id;
            buffer_is_reasoning_ = payload.is_reasoning;
        }

        if (!buffer_text_.empty() && !config_.joiner.empty()) {
            buffer_text_ += config_.joiner;
        }
        buffer_text_ += text;

        if (config_.flush_on_enqueue && static_cast<int>(buffer_text_.size()) >= config_.min_chars) {
            flush(false);
        } else if (static_cast<int>(buffer_text_.size()) >= config_.max_chars) {
            flush(false);
        }
    }

    // 对齐 flush
    void flush(bool force = false) {
        if (buffer_text_.empty()) return;
        if (!force && static_cast<int>(buffer_text_.size()) < config_.min_chars) return;
        if (should_abort_ && should_abort_()) return;

        ReplyPayload payload;
        payload.text = buffer_text_;
        payload.reply_to_id = buffer_reply_to_id_;
        payload.is_reasoning = buffer_is_reasoning_;

        buffer_text_.clear();
        buffer_reply_to_id_.reset();
        buffer_is_reasoning_ = false;

        if (on_flush_) on_flush_(payload);
    }

    bool has_buffered() const { return !buffer_text_.empty(); }
    void stop() { flush(true); }

private:
    BlockReplyCoalescerConfig config_;
    std::function<bool()> should_abort_;
    FlushCallback on_flush_;
    std::string buffer_text_;
    std::optional<std::string> buffer_reply_to_id_;
    bool buffer_is_reasoning_ = false;
};

// ═══════════════════════════════════════════════════════════════
// Session Delivery (对齐 OC: session-delivery.ts 核心逻辑)
// ═══════════════════════════════════════════════════════════════

struct SessionDeliveryContext {
    std::string channel;
    std::string to;
    std::string account_id;
    std::string session_key;
    bool is_internal = false;
    bool is_deliverable = false;
};

SessionDeliveryContext resolve_session_delivery_context(
    const std::string& session_key,
    const std::string& channel,
    const std::string& to,
    const std::string& account_id) {
    SessionDeliveryContext ctx;
    ctx.session_key = session_key;
    ctx.channel = to_lower_trim(channel);
    ctx.to = to;
    ctx.account_id = to_lower_trim(account_id);
    ctx.is_internal = (ctx.channel == "internal" || ctx.channel.empty());
    ctx.is_deliverable = !ctx.is_internal && !ctx.to.empty();
    return ctx;
}

}  // namespace hunterclaw::auto_reply
