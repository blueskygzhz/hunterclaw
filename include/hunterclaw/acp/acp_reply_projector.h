#pragma once

// ============================================================================
// acp_reply_projector.h
// Aligned with openclaw: src/auto-reply/reply/acp-projector.ts (498 lines)
//
// Projects AcpRuntimeEvent stream into deliverable messages.
// Supports three delivery modes: live, block, final_only.
// ============================================================================

#include "hunterclaw/acp/runtime_types.h"
#include <string>
#include <functional>
#include <vector>
#include <map>
#include <set>
#include <optional>

namespace hunterclaw {
namespace acp {

// ── Delivery mode ────────────────────────────────────────────────────────────
enum class AcpDeliveryMode {
    Live,       // Stream realtime with idle flush
    Block,      // Chunked block delivery
    FinalOnly,  // Only deliver on completion
};

// ── Projected delivery metadata ──────────────────────────────────────────────
struct AcpProjectedDeliveryMeta {
    std::optional<AcpSessionUpdateTag> tag;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> tool_status;
    bool allow_edit = false;
};

// ── Delivery callback ────────────────────────────────────────────────────────
// kind: "block" | "tool" | "final"
using AcpDeliverFn = std::function<void(
    const std::string& kind,
    const std::string& text,
    const std::optional<AcpProjectedDeliveryMeta>& meta
)>;

// ── AcpReplyProjector ───────────────────────────────────────────────────────
// Aligned with openclaw: createAcpReplyProjector (acp-projector.ts)
class AcpReplyProjector {
public:
    struct Config {
        AcpDeliveryMode delivery_mode = AcpDeliveryMode::Block;
        int max_output_chars = 100000;
        int max_session_update_chars = 500;
        bool repeat_suppression = true;
        bool should_send_tool_summaries = true;
    };

    AcpReplyProjector(Config config, AcpDeliverFn deliver)
        : config_(std::move(config))
        , deliver_(std::move(deliver))
    {}

    // Aligned with openclaw: projector.onEvent
    void on_event(const AcpRuntimeEvent& event) {
        switch (event.type) {
            case AcpRuntimeEvent::Type::TextDelta:
                handle_text_delta(event);
                break;
            case AcpRuntimeEvent::Type::Status:
                handle_status(event);
                break;
            case AcpRuntimeEvent::Type::ToolCall:
                handle_tool_call(event);
                break;
            case AcpRuntimeEvent::Type::Done:
            case AcpRuntimeEvent::Type::Error:
                flush(true);
                reset_turn_state();
                break;
        }
    }

    // Aligned with openclaw: projector.flush
    void flush(bool force = false) {
        // Flush live buffer
        if (!live_buffer_.empty()) {
            deliver_text(live_buffer_);
            live_buffer_.clear();
        }
        // Flush pending tool deliveries (final_only mode)
        if (config_.delivery_mode == AcpDeliveryMode::FinalOnly && force) {
            for (auto& entry : pending_tool_deliveries_) {
                deliver_("tool", entry.text, entry.meta);
            }
            pending_tool_deliveries_.clear();
        }
        // Flush chunker buffer
        if (!chunk_buffer_.empty()) {
            deliver_text(chunk_buffer_);
            chunk_buffer_.clear();
        }
    }

private:
    Config config_;
    AcpDeliverFn deliver_;

    // State
    int emitted_output_chars_ = 0;
    bool truncation_notice_emitted_ = false;
    std::string live_buffer_;
    std::string chunk_buffer_;
    std::optional<std::string> last_status_hash_;
    std::optional<std::string> last_tool_hash_;

    struct PendingToolDelivery {
        std::string text;
        std::optional<AcpProjectedDeliveryMeta> meta;
    };
    std::vector<PendingToolDelivery> pending_tool_deliveries_;

    struct ToolLifecycleState {
        bool started = false;
        bool terminal = false;
        std::optional<std::string> last_rendered_hash;
    };
    std::map<std::string, ToolLifecycleState> tool_lifecycle_;
    static const std::set<std::string>& terminal_statuses() {
        static std::set<std::string> s = {"completed", "failed", "cancelled", "done", "error"};
        return s;
    }

    void handle_text_delta(const AcpRuntimeEvent& event) {
        if (event.stream && *event.stream != "output") return;
        std::string text = event.text;
        if (text.empty()) return;

        if (emitted_output_chars_ >= config_.max_output_chars) {
            emit_truncation_notice();
            return;
        }

        int remaining = config_.max_output_chars - emitted_output_chars_;
        std::string accepted = (static_cast<int>(text.size()) > remaining)
            ? text.substr(0, remaining) : text;

        emitted_output_chars_ += static_cast<int>(accepted.size());

        if (config_.delivery_mode == AcpDeliveryMode::Live) {
            live_buffer_ += accepted;
            // Boundary flush: double newline or hard limit
            if (live_buffer_.size() >= 480 ||
                (live_buffer_.size() >= 2 &&
                 live_buffer_.substr(live_buffer_.size() - 2) == "\n\n")) {
                flush(true);
            }
        } else {
            chunk_buffer_ += accepted;
            // Auto-flush on paragraph boundaries
            if (chunk_buffer_.size() >= 2 &&
                chunk_buffer_.substr(chunk_buffer_.size() - 2) == "\n\n") {
                deliver_text(chunk_buffer_);
                chunk_buffer_.clear();
            }
        }

        if (static_cast<int>(accepted.size()) < static_cast<int>(text.size())) {
            emit_truncation_notice();
        }
    }

    void handle_status(const AcpRuntimeEvent& event) {
        if (!config_.should_send_tool_summaries) return;
        std::string text = event.text;
        if (text.empty()) return;
        if (static_cast<int>(text.size()) > config_.max_session_update_chars) {
            text = text.substr(0, config_.max_session_update_chars - 1) + "\xe2\x80\xa6";
        }

        // Dedupe
        if (config_.repeat_suppression && last_status_hash_ == text) return;

        AcpProjectedDeliveryMeta meta;
        if (event.tag) meta.tag = event.tag;

        if (config_.delivery_mode == AcpDeliveryMode::FinalOnly) {
            pending_tool_deliveries_.push_back({text, meta});
        } else {
            flush(true);
            deliver_("tool", text, meta);
        }
        last_status_hash_ = text;
    }

    void handle_tool_call(const AcpRuntimeEvent& event) {
        if (!config_.should_send_tool_summaries) return;

        std::string rendered = render_tool_summary(event);
        if (rendered.empty()) return;
        if (static_cast<int>(rendered.size()) > config_.max_session_update_chars) {
            rendered = rendered.substr(0, config_.max_session_update_chars - 1) + "\xe2\x80\xa6";
        }

        std::string tool_call_id = event.tool_call_id.value_or("");
        std::string status_str = event.status.value_or("");
        bool is_terminal = terminal_statuses().count(status_str) > 0;
        bool is_start = (status_str == "in_progress" || (event.tag && *event.tag == "tool_call"));

        // Dedupe
        if (config_.repeat_suppression) {
            if (!tool_call_id.empty()) {
                auto& state = tool_lifecycle_[tool_call_id];
                if (is_terminal && state.terminal) return;
                if (is_start && state.started) return;
                if (state.last_rendered_hash == rendered) return;
                if (is_start) state.started = true;
                if (is_terminal) state.terminal = true;
                state.last_rendered_hash = rendered;
            } else if (last_tool_hash_ == rendered) {
                return;
            }
        }

        AcpProjectedDeliveryMeta meta;
        if (event.tag) meta.tag = event.tag;
        if (!tool_call_id.empty()) meta.tool_call_id = tool_call_id;
        if (!status_str.empty()) meta.tool_status = status_str;
        meta.allow_edit = !tool_call_id.empty() && event.tag && *event.tag == "tool_call_update";

        if (config_.delivery_mode == AcpDeliveryMode::FinalOnly) {
            pending_tool_deliveries_.push_back({rendered, meta});
        } else {
            flush(true);
            deliver_("tool", rendered, meta);
        }
        last_tool_hash_ = rendered;
    }

    std::string render_tool_summary(const AcpRuntimeEvent& event) {
        std::string parts;
        if (event.title && !event.title->empty()) {
            parts = *event.title;
        }
        if (event.status && !event.status->empty()) {
            if (!parts.empty()) parts += " · ";
            parts += "status=" + *event.status;
        }
        if (parts.empty() && !event.text.empty()) {
            parts = event.text;
        }
        if (parts.empty()) parts = "tool call";
        return "\xf0\x9f\x94\xa7 " + parts;  // 🔧
    }

    void emit_truncation_notice() {
        if (truncation_notice_emitted_) return;
        truncation_notice_emitted_ = true;
        AcpProjectedDeliveryMeta meta;
        meta.tag = "session_info_update";
        flush(true);
        deliver_("tool", "\xe2\x9a\xa0\xef\xb8\x8f output truncated", meta);
    }

    void deliver_text(const std::string& text) {
        if (text.empty()) return;
        deliver_("block", text, std::nullopt);
    }

    void reset_turn_state() {
        emitted_output_chars_ = 0;
        truncation_notice_emitted_ = false;
        live_buffer_.clear();
        chunk_buffer_.clear();
        last_status_hash_.reset();
        last_tool_hash_.reset();
        pending_tool_deliveries_.clear();
        tool_lifecycle_.clear();
    }
};

} // namespace acp
} // namespace hunterclaw
