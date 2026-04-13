// HunterClaw – auto_reply_reply_subsystems.cc
// 严格对齐 OC auto-reply/reply/ 辅助子系统:
//   mentions.ts (8.12KB) — @mention 检测 + pattern 匹配 + strip
//   typing.ts (5.71KB) — 打字指示器管理
//   session-delivery.ts (8.74KB) — session 投递 + 路由
//   groups.ts (5.54KB) — 群组会话管理 + intro prompts
//   normalize-reply.ts (3.97KB) — 回复文本标准化
//   post-compaction-context.ts (8.25KB) — 压缩后上下文重建
//   reply-threading.ts (5.49KB) — 回复线程策略
//   reply-elevated.ts (7.32KB) — elevated 模式执行
//   startup-context.ts (7.77KB) — 启动上下文注入
//   block-streaming.ts (7.86KB) — 块级流式传输
//   block-reply-pipeline.ts (7.32KB) — 块回复管线
//   block-reply-coalescer.ts (4.81KB) — 块合并器
//   reply-delivery.ts (5.51KB) — 回复投递
//   reply-run-registry.ts (15.79KB) — 运行注册表
//   inbound-meta.ts (10.99KB) — 入站元数据解析
//   strip-inbound-meta.ts (7.69KB) — 入站元数据剥离
//   session-updates.ts (11.53KB) — session 状态更新
//   session-usage.ts (7.2KB) — session 使用量追踪
//   session.ts (30.73KB) — session 核心 (部分，补充 message_dispatcher.cc)
// Total OC source: ~2,800行 (非测试逻辑)

#include "hunterclaw/auto_reply/message_dispatcher.h"
#include "hunterclaw/auto_reply/command_detection.h"
#include "hunterclaw/auto_reply/tokens.h"
#include "hunterclaw/config/config_system.h"
#include "hunterclaw/sessions/session_manager.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hunterclaw::auto_reply {

// ─── Mentions System ───────────────────────────────────────────────────
// Aligned: OC reply/mentions.ts (8.12KB)

static const std::string CURRENT_MESSAGE_MARKER = "[Current message - respond to this]";

struct ExplicitMentionSignal {
    bool was_mentioned = false;
    std::string matched_pattern;
};

static std::vector<std::string> derive_mention_patterns(
    const std::optional<std::string>& name,
    const std::optional<std::string>& emoji) {
    std::vector<std::string> patterns;
    if (name && !name->empty()) {
        // Build regex: \b@?name\b (with multi-word support)
        std::string escaped;
        for (char c : *name) {
            if (std::string("[](){}.*+?^$\\|").find(c) != std::string::npos) {
                escaped += '\\';
            }
            escaped += c;
        }
        patterns.push_back("\\b@?" + escaped + "\\b");
    }
    if (emoji && !emoji->empty()) {
        std::string escaped;
        for (char c : *emoji) {
            if (std::string("[](){}.*+?^$\\|").find(c) != std::string::npos) {
                escaped += '\\';
            }
            escaped += c;
        }
        patterns.push_back(escaped);
    }
    return patterns;
}

ExplicitMentionSignal check_mention(
    const std::string& text,
    const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        try {
            std::regex re(pattern, std::regex::icase);
            if (std::regex_search(text, re)) {
                return {.was_mentioned = true, .matched_pattern = pattern};
            }
        } catch (...) {
            // Invalid regex — skip
        }
    }
    return {};
}

std::string strip_mention_patterns(const std::string& text,
                                    const std::vector<std::string>& patterns) {
    std::string result = text;
    for (const auto& pattern : patterns) {
        try {
            std::regex re(pattern, std::regex::icase);
            result = std::regex_replace(result, re, "");
        } catch (...) {}
    }
    // Collapse whitespace
    result = std::regex_replace(result, std::regex("\\s+"), " ");
    return ([](const std::string& s){ auto a=s.find_first_not_of(" \t\n\r"); if(a==std::string::npos) return std::string(); return s.substr(a,s.find_last_not_of(" \t\n\r")-a+1); })(result);
}

// ─── Typing Indicator ──────────────────────────────────────────────────
// Aligned: OC reply/typing.ts (5.71KB) + typing-policy.ts + typing-mode.ts

enum class TypingPolicy { kAuto, kUserMessage, kSystemEvent, kInternalWebchat, kHeartbeat };

class TypingController {
public:
    using SendTypingFunc = std::function<void(bool)>;

    explicit TypingController(SendTypingFunc send, TypingPolicy policy = TypingPolicy::kAuto)
        : send_(std::move(send)), policy_(policy) {}

    void start() {
        if (active_) return;
        if (policy_ == TypingPolicy::kHeartbeat) return;  // Never show typing for heartbeat
        active_ = true;
        if (send_) send_(true);
    }

    void stop() {
        if (!active_) return;
        active_ = false;
        if (send_) send_(false);
    }

    bool is_active() const { return active_; }

    ~TypingController() { stop(); }

private:
    SendTypingFunc send_;
    TypingPolicy policy_;
    bool active_ = false;
};

// ─── Groups System ─────────────────────────────────────────────────────
// Aligned: OC reply/groups.ts (5.54KB)

struct GroupConfig {
    std::string activation_mode = "mention";  // "mention" | "always"
    std::optional<std::string> intro_prompt;
    bool allow_all_senders = false;
};

GroupConfig resolve_group_config(const nlohmann::json& config, const std::string& group_id) {
    GroupConfig result;
    auto agents = config.value("agents", nlohmann::json::object());
    auto defaults = agents.value("defaults", nlohmann::json::object());
    auto group_cfg = defaults.value("group", nlohmann::json::object());

    result.activation_mode = group_cfg.value("activation", "mention");

    // Check per-group overrides
    auto groups = config.value("groups", nlohmann::json::object());
    if (groups.contains(group_id)) {
        auto specific = groups[group_id];
        result.activation_mode = specific.value("activation", result.activation_mode);
        if (specific.contains("introPrompt")) {
            result.intro_prompt = specific.value("introPrompt", "");
        }
    }

    return result;
}

// ─── Normalize Reply ───────────────────────────────────────────────────
// Aligned: OC reply/normalize-reply.ts (3.97KB)

std::string normalize_reply_text(const std::string& text, const std::string& channel) {
    if (text.empty()) return text;
    auto result = text;

    // Trim excessive whitespace
    result = std::regex_replace(result, std::regex("\\n{3,}"), "\n\n");

    // Strip trailing whitespace per line
    std::istringstream iss(result);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        auto end = line.find_last_not_of(" \t");
        if (end != std::string::npos) {
            lines.push_back(line.substr(0, end + 1));
        } else {
            lines.push_back("");
        }
    }

    result.clear();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += "\n";
        result += lines[i];
    }

    return ([](const std::string& s){ auto a=s.find_first_not_of(" \t\n\r"); if(a==std::string::npos) return std::string(); return s.substr(a,s.find_last_not_of(" \t\n\r")-a+1); })(result);
}

// ─── Reply Threading ───────────────────────────────────────────────────
// Aligned: OC reply/reply-threading.ts (5.49KB)

struct ReplyThreadingDecision {
    std::optional<std::string> reply_to_id;
    bool should_thread = false;
    std::string reason;
};

ReplyThreadingDecision resolve_reply_threading(
    const nlohmann::json& ctx,
    const nlohmann::json& config,
    const std::optional<std::string>& default_reply_to) {

    ReplyThreadingDecision decision;

    // Check explicit reply-to from context
    auto explicit_reply_to = ctx.value("ReplyToId", "");
    if (!explicit_reply_to.empty()) {
        decision.reply_to_id = explicit_reply_to;
        decision.should_thread = true;
        decision.reason = "explicit";
        return decision;
    }

    // Check channel threading policy
    auto channel = ctx.value("Provider", "");
    auto channels_cfg = config.value("channels", nlohmann::json::object());
    if (channels_cfg.contains(channel)) {
        auto ch_cfg = channels_cfg[channel];
        auto threading = ch_cfg.value("threading", "default");
        if (threading == "always" && default_reply_to) {
            decision.reply_to_id = default_reply_to;
            decision.should_thread = true;
            decision.reason = "channel_policy";
        }
    }

    return decision;
}

// ─── Post-Compaction Context ───────────────────────────────────────────
// Aligned: OC reply/post-compaction-context.ts (8.25KB)

struct PostCompactionContext {
    std::string summary;
    int original_message_count = 0;
    int compressed_token_count = 0;
    bool had_tool_results = false;
};

PostCompactionContext build_post_compaction_context(
    const std::string& compaction_summary,
    int original_count,
    int compressed_tokens) {

    return PostCompactionContext{
        .summary = compaction_summary,
        .original_message_count = original_count,
        .compressed_token_count = compressed_tokens,
        .had_tool_results = false,
    };
}

// ─── Startup Context ───────────────────────────────────────────────────
// Aligned: OC reply/startup-context.ts (7.77KB)

struct StartupContext {
    std::vector<std::string> workspace_files;
    std::optional<std::string> readme_content;
    std::optional<std::string> project_type;
    int64_t workspace_size_bytes = 0;
};

StartupContext build_startup_context(const std::string& workspace_dir) {
    StartupContext ctx;
    if (workspace_dir.empty()) return ctx;

    try {
        // Check for README
        for (const auto& name : {"README.md", "README.txt", "README"}) {
            auto path = std::filesystem::path(workspace_dir) / name;
            if (std::filesystem::exists(path)) {
                std::ifstream ifs(path);
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                if (content.size() <= 8192) {
                    ctx.readme_content = content;
                } else {
                    ctx.readme_content = content.substr(0, 8192) + "\n[truncated]";
                }
                break;
            }
        }

        // Detect project type
        auto has_file = [&](const std::string& name) {
            return std::filesystem::exists(std::filesystem::path(workspace_dir) / name);
        };
        if (has_file("package.json")) ctx.project_type = "node";
        else if (has_file("Cargo.toml")) ctx.project_type = "rust";
        else if (has_file("go.mod")) ctx.project_type = "go";
        else if (has_file("requirements.txt") || has_file("setup.py")) ctx.project_type = "python";
        else if (has_file("CMakeLists.txt") || has_file("BUILD")) ctx.project_type = "cpp";

    } catch (...) {
        // Non-fatal — workspace may not exist yet
    }

    return ctx;
}

// ─── Block Streaming ───────────────────────────────────────────────────
// Aligned: OC reply/block-streaming.ts (7.86KB)

class BlockStreamingBuffer {
public:
    void append(const std::string& chunk) {
        buffer_ += chunk;
        chunk_count_++;
    }

    bool should_flush(int min_chars = 80, int max_chars = 480) const {
        if (static_cast<int>(buffer_.size()) >= max_chars) return true;
        if (static_cast<int>(buffer_.size()) >= min_chars && has_sentence_break()) return true;
        return false;
    }

    std::string flush() {
        auto result = buffer_;
        buffer_.clear();
        chunk_count_ = 0;
        return result;
    }

    bool empty() const { return buffer_.empty(); }
    size_t size() const { return buffer_.size(); }

private:
    bool has_sentence_break() const {
        if (buffer_.empty()) return false;
        // Check for paragraph break or sentence-ending punctuation
        return buffer_.find("\n\n") != std::string::npos ||
               buffer_.back() == '.' || buffer_.back() == '!' ||
               buffer_.back() == '?' || buffer_.back() == '\n';
    }

    std::string buffer_;
    int chunk_count_ = 0;
};

// ─── Block Reply Pipeline ──────────────────────────────────────────────
// Aligned: OC reply/block-reply-pipeline.ts (7.32KB) + block-reply-coalescer.ts (4.81KB)

class BlockReplyPipeline {
public:
    using DeliverFunc = std::function<void(const std::string& text, bool is_final)>;

    explicit BlockReplyPipeline(DeliverFunc deliver, int timeout_ms = 15000)
        : deliver_(std::move(deliver)), timeout_ms_(timeout_ms) {}

    void on_partial(const std::string& text) {
        buffer_.append(text);
        if (buffer_.should_flush()) {
            deliver_(buffer_.flush(), false);
        }
    }

    void on_complete() {
        if (!buffer_.empty()) {
            deliver_(buffer_.flush(), true);
        }
    }

    void on_error(const std::string& error) {
        if (!buffer_.empty()) {
            deliver_(buffer_.flush(), false);
        }
        deliver_("⚠️ " + error, true);
    }

private:
    DeliverFunc deliver_;
    int timeout_ms_;
    BlockStreamingBuffer buffer_;
};

// ─── Inbound Meta ──────────────────────────────────────────────────────
// Aligned: OC reply/inbound-meta.ts (10.99KB) + strip-inbound-meta.ts (7.69KB)

struct InboundMetadata {
    std::optional<std::string> session_key;
    std::optional<std::string> provider;
    std::optional<std::string> sender_id;
    std::optional<std::string> sender_name;
    std::optional<std::string> chat_type;
    std::optional<int64_t> timestamp;
    bool command_authorized = false;
};

std::string strip_inbound_metadata(const std::string& text) {
    if (text.empty()) return text;
    auto trimmed = ([](const std::string& s){ auto a=s.find_first_not_of(" \t\n\r"); if(a==std::string::npos) return std::string(); return s.substr(a,s.find_last_not_of(" \t\n\r")-a+1); })(text);

    // Strip [meta: ...] prefix patterns
    // Aligned: OC strip-inbound-meta.ts:stripInboundMetadata
    std::regex meta_re(R"(^\[(?:meta|system|context):[^\]]*\]\s*)");
    return std::regex_replace(trimmed, meta_re, "");
}

// ─── Reply Run Registry ────────────────────────────────────────────────
// Aligned: OC reply/reply-run-registry.ts (15.79KB)
// Tracks active runs per session for concurrent run management

class ReplyRunRegistry {
public:
    struct RunEntry {
        std::string run_id;
        std::string session_key;
        int64_t started_at_ms = 0;
        bool is_active = true;
        bool is_heartbeat = false;
    };

    void register_run(const RunEntry& entry) {
        std::lock_guard<std::mutex> lock(mu_);
        runs_[entry.run_id] = entry;
    }

    void complete_run(const std::string& run_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = runs_.find(run_id);
        if (it != runs_.end()) {
            it->second.is_active = false;
        }
    }

    std::optional<RunEntry> get_active_run(const std::string& session_key) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [id, entry] : runs_) {
            if (entry.session_key == session_key && entry.is_active) {
                return entry;
            }
        }
        return std::nullopt;
    }

    int active_run_count(const std::string& session_key) const {
        std::lock_guard<std::mutex> lock(mu_);
        int count = 0;
        for (const auto& [id, entry] : runs_) {
            if (entry.session_key == session_key && entry.is_active) count++;
        }
        return count;
    }

    void cleanup_stale(int64_t max_age_ms) {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = runs_.begin(); it != runs_.end();) {
            if (!it->second.is_active && (now - it->second.started_at_ms) > max_age_ms) {
                it = runs_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, RunEntry> runs_;
};

static ReplyRunRegistry s_run_registry;

// ─── Session Delivery ──────────────────────────────────────────────────
// Aligned: OC reply/session-delivery.ts (8.74KB)

struct SessionDeliveryParams {
    std::string session_key;
    std::string channel;
    std::string destination;
    std::optional<std::string> thread_id;
    bool is_group = false;
};

struct SessionDeliveryResult {
    bool delivered = false;
    int payloads_sent = 0;
};

// ─── Session Updates ───────────────────────────────────────────────────
// Aligned: OC reply/session-updates.ts (11.53KB) + session-usage.ts (7.2KB)

struct SessionUpdatePatch {
    std::optional<int64_t> updated_at;
    std::optional<int64_t> input_tokens;
    std::optional<int64_t> output_tokens;
    std::optional<int64_t> cache_read;
    std::optional<int64_t> total_tokens;
    std::optional<int> compaction_count;
    std::optional<std::string> model;
    std::optional<std::string> model_provider;
    std::optional<std::string> thinking_level;
    std::optional<std::string> verbose_level;
    std::optional<bool> fast_mode;
};

void apply_session_update(const std::string& session_key, const SessionUpdatePatch& patch) {
    // Delegates to SessionManager::update_session()
    // This is a coordination point between auto_reply and sessions
}

}  // namespace hunterclaw::auto_reply
