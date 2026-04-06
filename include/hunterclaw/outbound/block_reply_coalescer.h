#pragma once
// Aligned with openclaw: src/auto-reply/reply/block-reply-coalescer.ts (149 lines)
//
// BlockReplyCoalescer: merges small text payloads into larger chunks before delivery.
//   - Accumulates text in a buffer until minChars is reached
//   - Flushes when buffer exceeds maxChars
//   - Idle timer triggers flush after idleMs of inactivity
//   - flushOnEnqueue mode: each enqueue is treated as a paragraph boundary
//   - replyToId conflict detection: different threads force a flush
//   - Media payloads always bypass coalescing (direct passthrough)

#include "hunterclaw/outbound/block_streaming.h"
#include "hunterclaw/queue/delivery_queue.h"
#include <string>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>

namespace hunterclaw {
namespace outbound {

class BlockReplyCoalescer {
public:
    using FlushCallback = std::function<void(const queue::MessagePayload&)>;

    explicit BlockReplyCoalescer(const BlockStreamingCoalescing& config,
                                  std::function<bool()> should_abort,
                                  FlushCallback on_flush)
        : min_chars_(std::max(1, config.min_chars))
        , max_chars_(std::max(min_chars_, config.max_chars))
        , idle_ms_(std::max(0, config.idle_ms))
        , joiner_(config.joiner)
        , flush_on_enqueue_(config.flush_on_enqueue)
        , should_abort_(std::move(should_abort))
        , on_flush_(std::move(on_flush))
    {}

    ~BlockReplyCoalescer() { stop(); }

    void enqueue(const queue::MessagePayload& payload) {
        std::lock_guard<std::mutex> lock(mu_);
        if (should_abort_ && should_abort_()) return;

        bool has_media = !payload.media_url.empty() || !payload.media_urls.empty();
        const std::string& text = payload.text;
        bool has_text = false;
        for (char c : text) { if (c != ' ' && c != '\t' && c != '\n' && c != '\r') { has_text = true; break; } }

        if (has_media) {
            flush_locked(true);
            on_flush_(payload);
            return;
        }
        if (!has_text) return;

        // flushOnEnqueue: paragraph-boundary mode
        if (flush_on_enqueue_) {
            if (!buffer_text_.empty()) flush_locked(true);
            buffer_reply_to_id_ = payload.reply_to_id;
            buffer_text_ = text;
            flush_locked(true);
            return;
        }

        // replyToId conflict detection
        bool reply_conflict = !buffer_text_.empty() && !payload.reply_to_id.empty() &&
            (buffer_reply_to_id_.empty() || buffer_reply_to_id_ != payload.reply_to_id);
        if (!buffer_text_.empty() && reply_conflict) {
            flush_locked(true);
        }

        if (buffer_text_.empty()) {
            buffer_reply_to_id_ = payload.reply_to_id;
        }

        std::string next = buffer_text_.empty() ? text : (buffer_text_ + joiner_ + text);
        if (static_cast<int>(next.size()) > max_chars_) {
            if (!buffer_text_.empty()) {
                flush_locked(true);
                buffer_reply_to_id_ = payload.reply_to_id;
                if (static_cast<int>(text.size()) >= max_chars_) {
                    queue::MessagePayload p = payload;
                    on_flush_(p);
                    return;
                }
                buffer_text_ = text;
                schedule_idle_flush_locked();
                return;
            }
            on_flush_(payload);
            return;
        }

        buffer_text_ = next;
        if (static_cast<int>(buffer_text_.size()) >= max_chars_) {
            flush_locked(true);
            return;
        }
        schedule_idle_flush_locked();
    }

    void flush(bool force = false) {
        std::lock_guard<std::mutex> lock(mu_);
        flush_locked(force);
    }

    bool has_buffered() const {
        std::lock_guard<std::mutex> lock(mu_);
        return !buffer_text_.empty();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            cancel_idle_timer_locked();
        }
    }

private:
    void flush_locked(bool force) {
        cancel_idle_timer_locked();
        if (should_abort_ && should_abort_()) { reset_buffer_locked(); return; }
        if (buffer_text_.empty()) return;
        if (!force && !flush_on_enqueue_ && static_cast<int>(buffer_text_.size()) < min_chars_) {
            schedule_idle_flush_locked();
            return;
        }
        queue::MessagePayload payload;
        payload.text = buffer_text_;
        payload.reply_to_id = buffer_reply_to_id_;
        reset_buffer_locked();
        on_flush_(payload);
    }

    void reset_buffer_locked() {
        buffer_text_.clear();
        buffer_reply_to_id_.clear();
    }

    void cancel_idle_timer_locked() {
        idle_timer_gen_++;
    }

    void schedule_idle_flush_locked() {
        if (idle_ms_ <= 0) return;
        cancel_idle_timer_locked();
        int gen = idle_timer_gen_;
        std::thread([this, gen]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms_));
            std::lock_guard<std::mutex> lock(mu_);
            if (gen != idle_timer_gen_) return;  // cancelled
            flush_locked(false);
        }).detach();
    }

    int min_chars_;
    int max_chars_;
    int idle_ms_;
    std::string joiner_;
    bool flush_on_enqueue_;
    std::function<bool()> should_abort_;
    FlushCallback on_flush_;

    mutable std::mutex mu_;
    std::string buffer_text_;
    std::string buffer_reply_to_id_;
    int idle_timer_gen_ = 0;
};

} // namespace outbound
} // namespace hunterclaw
