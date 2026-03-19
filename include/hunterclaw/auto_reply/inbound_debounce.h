// Aligned with openclaw: src/auto-reply/inbound-debounce.ts
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <optional>
#include <map>
#include <chrono>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// InboundDebouncer
// Aligned with openclaw: createInboundDebouncer (inbound-debounce.ts)
//
// Buffers inbound items by key and flushes them after a debounce window.
// If debounceMs == 0 or shouldDebounce returns false, items are flushed immediately.
// ============================================================================

template <typename T>
class InboundDebouncer {
public:
    struct Params {
        int debounce_ms = 0;

        // Returns the debounce key for an item (null/empty → no debounce)
        std::function<std::string(const T&)> build_key;

        // Optional: per-item debounce override
        std::function<std::optional<int>(const T&)> resolve_debounce_ms;

        // Optional: whether this item should be debounced (default: true)
        std::function<bool(const T&)> should_debounce;

        // Called when a buffer is flushed (with all buffered items)
        std::function<void(const std::vector<T>&)> on_flush;

        // Called on flush error
        std::function<void(const std::string& error, const std::vector<T>&)> on_error;
    };

    explicit InboundDebouncer(Params params)
        : params_(std::move(params)) {}

    // Aligned with openclaw: enqueue
    void enqueue(const T& item) {
        const std::string key = params_.build_key ? params_.build_key(item) : "";
        const int debounce_ms = resolve_debounce_ms_for(item);
        const bool can_debounce = debounce_ms > 0 &&
            (!params_.should_debounce || params_.should_debounce(item));

        if (!can_debounce || key.empty()) {
            // Flush any existing buffer for this key first
            if (!key.empty()) {
                flush_key(key);
            }
            try {
                if (params_.on_flush) params_.on_flush({item});
            } catch (const std::exception& e) {
                if (params_.on_error) params_.on_error(e.what(), {item});
            }
            return;
        }

        auto& buf = buffers_[key];
        buf.items.push_back(item);
        buf.debounce_ms = debounce_ms;
        buf.scheduled_at = std::chrono::steady_clock::now();
    }

    // Aligned with openclaw: flushKey
    void flush_key(const std::string& key) {
        auto it = buffers_.find(key);
        if (it == buffers_.end()) return;
        auto items = std::move(it->second.items);
        buffers_.erase(it);
        if (items.empty()) return;
        try {
            if (params_.on_flush) params_.on_flush(items);
        } catch (const std::exception& e) {
            if (params_.on_error) params_.on_error(e.what(), items);
        }
    }

    // Flush all buffers whose debounce window has expired.
    // Call this periodically (e.g. from a timer thread).
    void flush_expired() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> expired_keys;
        for (const auto& [key, buf] : buffers_) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - buf.scheduled_at
            ).count();
            if (elapsed_ms >= buf.debounce_ms) {
                expired_keys.push_back(key);
            }
        }
        for (const auto& key : expired_keys) {
            flush_key(key);
        }
    }

private:
    struct Buffer {
        std::vector<T> items;
        int debounce_ms = 0;
        std::chrono::steady_clock::time_point scheduled_at;
    };

    int resolve_debounce_ms_for(const T& item) const {
        if (params_.resolve_debounce_ms) {
            auto opt = params_.resolve_debounce_ms(item);
            if (opt.has_value() && *opt >= 0) return *opt;
        }
        return std::max(0, params_.debounce_ms);
    }

    Params params_;
    std::map<std::string, Buffer> buffers_;
};

// ============================================================================
// resolveInboundDebounceMs
// Aligned with openclaw: resolveInboundDebounceMs (inbound-debounce.ts)
// ============================================================================

struct InboundDebounceConfig {
    int debounce_ms = 0;
    std::map<std::string, int> by_channel;  // per-channel overrides
};

inline int resolve_inbound_debounce_ms(
    const InboundDebounceConfig& cfg,
    const std::string& channel,
    std::optional<int> override_ms = std::nullopt
) {
    if (override_ms.has_value() && *override_ms >= 0) return *override_ms;

    auto it = cfg.by_channel.find(channel);
    if (it != cfg.by_channel.end() && it->second >= 0) return it->second;

    return std::max(0, cfg.debounce_ms);
}

} // namespace auto_reply
} // namespace hunterclaw
