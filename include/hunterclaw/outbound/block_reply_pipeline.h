#pragma once
// Aligned with openclaw: src/auto-reply/reply/block-reply-pipeline.ts (262 lines)
//
// BlockReplyPipeline: ordered, deduplicated, coalesced block reply delivery.
//   - Sequential send chain (Promise chain → mutex + queue in C++)
//   - Three-level dedup: seenKeys / sentKeys / pendingKeys
//   - Content-key dedup for final-payload suppression after block streaming
//   - Timeout + abort support
//   - Optional coalescer integration
//   - Optional buffer (e.g., audioAsVoice)

#include "hunterclaw/outbound/block_reply_coalescer.h"
#include "hunterclaw/queue/delivery_queue.h"
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

namespace hunterclaw {
namespace outbound {

class BlockReplyPipeline {
public:
    using SendCallback = std::function<void(const queue::MessagePayload&)>;

    struct Config {
        SendCallback on_block_reply;
        int timeout_ms = 30000;
        std::optional<BlockStreamingCoalescing> coalescing;
    };

    explicit BlockReplyPipeline(const Config& config)
        : on_block_reply_(config.on_block_reply)
        , timeout_ms_(config.timeout_ms)
    {
        if (config.coalescing.has_value()) {
            coalescer_ = std::make_unique<BlockReplyCoalescer>(
                *config.coalescing,
                [this]() { return aborted_.load(); },
                [this](const queue::MessagePayload& payload) {
                    std::lock_guard<std::mutex> lock(buffered_keys_mu_);
                    buffered_keys_.clear();
                    send_payload(payload, true);
                }
            );
        }
    }

    ~BlockReplyPipeline() { stop(); }

    void enqueue(const queue::MessagePayload& payload) {
        if (aborted_.load()) return;

        bool has_media = !payload.media_url.empty() || !payload.media_urls.empty();
        if (has_media) {
            if (coalescer_) coalescer_->flush(true);
            send_payload(payload, false);
            return;
        }
        if (coalescer_) {
            std::string key = create_payload_key(payload);
            std::lock_guard<std::mutex> lock(keys_mu_);
            if (seen_keys_.count(key) || pending_keys_.count(key) || buffered_keys_.count(key)) return;
            seen_keys_.insert(key);
            {
                std::lock_guard<std::mutex> bk(buffered_keys_mu_);
                buffered_keys_.insert(key);
            }
            coalescer_->enqueue(payload);
            return;
        }
        send_payload(payload, false);
    }

    void flush(bool force = false) {
        if (coalescer_) coalescer_->flush(force);
        // Wait for send chain to complete
        std::lock_guard<std::mutex> lock(send_mu_);
    }

    void stop() {
        if (coalescer_) coalescer_->stop();
    }

    bool has_buffered() const {
        return (coalescer_ && coalescer_->has_buffered());
    }

    bool did_stream() const { return did_stream_.load(); }
    bool is_aborted() const { return aborted_.load(); }

    bool has_sent_payload(const queue::MessagePayload& payload) const {
        std::string key = create_content_key(payload);
        std::lock_guard<std::mutex> lock(keys_mu_);
        return sent_content_keys_.count(key) > 0;
    }

private:
    void send_payload(const queue::MessagePayload& payload, bool bypass_seen_check) {
        if (aborted_.load()) return;

        std::string payload_key = create_payload_key(payload);
        std::string content_key = create_content_key(payload);

        {
            std::lock_guard<std::mutex> lock(keys_mu_);
            if (!bypass_seen_check) {
                if (seen_keys_.count(payload_key)) return;
                seen_keys_.insert(payload_key);
            }
            if (sent_keys_.count(payload_key) || pending_keys_.count(payload_key)) return;
            pending_keys_.insert(payload_key);
        }

        // Sequential send (serialize via send_mu_)
        std::lock_guard<std::mutex> send_lock(send_mu_);
        if (aborted_.load()) {
            std::lock_guard<std::mutex> lock(keys_mu_);
            pending_keys_.erase(payload_key);
            return;
        }

        try {
            // Timeout enforcement
            auto start = std::chrono::steady_clock::now();
            on_block_reply_(payload);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();

            if (timeout_ms_ > 0 && elapsed > timeout_ms_) {
                aborted_.store(true);
            }

            {
                std::lock_guard<std::mutex> lock(keys_mu_);
                sent_keys_.insert(payload_key);
                sent_content_keys_.insert(content_key);
                pending_keys_.erase(payload_key);
            }
            did_stream_.store(true);
        } catch (...) {
            std::lock_guard<std::mutex> lock(keys_mu_);
            pending_keys_.erase(payload_key);
        }
    }

    static std::string create_payload_key(const queue::MessagePayload& payload) {
        // Includes replyToId for thread-aware dedup
        std::string text = payload.text;
        // Trim
        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s != std::string::npos && e != std::string::npos) text = text.substr(s, e - s + 1);
        else text = "";

        std::string key = "t:" + text + "|m:";
        for (const auto& u : payload.media_urls) key += u + ",";
        if (!payload.media_url.empty()) key += payload.media_url;
        key += "|r:" + payload.reply_to_id;
        return key;
    }

    static std::string create_content_key(const queue::MessagePayload& payload) {
        // Ignores replyToId for final-payload suppression
        std::string text = payload.text;
        auto s = text.find_first_not_of(" \t\n\r");
        auto e = text.find_last_not_of(" \t\n\r");
        if (s != std::string::npos && e != std::string::npos) text = text.substr(s, e - s + 1);
        else text = "";

        std::string key = "t:" + text + "|m:";
        for (const auto& u : payload.media_urls) key += u + ",";
        if (!payload.media_url.empty()) key += payload.media_url;
        return key;
    }

    SendCallback on_block_reply_;
    int timeout_ms_;
    std::unique_ptr<BlockReplyCoalescer> coalescer_;

    mutable std::mutex keys_mu_;
    std::set<std::string> seen_keys_;
    std::set<std::string> sent_keys_;
    std::set<std::string> sent_content_keys_;
    std::set<std::string> pending_keys_;

    std::mutex buffered_keys_mu_;
    std::set<std::string> buffered_keys_;

    std::mutex send_mu_;  // serializes sends
    std::atomic<bool> aborted_{false};
    std::atomic<bool> did_stream_{false};
};

} // namespace outbound
} // namespace hunterclaw
