#pragma once

// ============================================================================
// runtime_cache.h
// Aligned with openclaw: src/acp/control-plane/runtime-cache.ts
//
// In-memory cache of active AcpRuntime + AcpRuntimeHandle pairs per session.
// Supports TTL-based idle eviction.
// ============================================================================

#include "hunterclaw/acp/runtime_types.h"
#include <string>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <memory>

namespace hunterclaw {
namespace acp {

// Aligned with openclaw: CachedRuntimeState
struct CachedRuntimeState {
    std::shared_ptr<AcpRuntime> runtime;
    AcpRuntimeHandle handle;
    std::string backend;
    std::string agent;
    AcpRuntimeSessionMode mode = AcpRuntimeSessionMode::Persistent;
    std::optional<std::string> cwd;
    std::optional<std::string> applied_control_signature;
};

// Aligned with openclaw: CachedRuntimeSnapshot
struct CachedRuntimeSnapshot {
    std::string actor_key;
    CachedRuntimeState state;
    int64_t last_touched_at;
    int64_t idle_ms;
};

// Aligned with openclaw: RuntimeCache
class RuntimeCache {
public:
    int size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(cache_.size());
    }

    bool has(const std::string& actor_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.count(actor_key) > 0;
    }

    // Aligned with openclaw: get (with touch)
    CachedRuntimeState* get(const std::string& actor_key, bool touch = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(actor_key);
        if (it == cache_.end()) return nullptr;
        if (touch) {
            it->second.last_touched_at = now_ms();
        }
        return &it->second.state;
    }

    // Aligned with openclaw: peek (no touch)
    CachedRuntimeState* peek(const std::string& actor_key) {
        return get(actor_key, false);
    }

    std::optional<int64_t> get_last_touched_at(const std::string& actor_key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(actor_key);
        if (it == cache_.end()) return std::nullopt;
        return it->second.last_touched_at;
    }

    void set(const std::string& actor_key, const CachedRuntimeState& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[actor_key] = {state, now_ms()};
    }

    void clear(const std::string& actor_key) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(actor_key);
    }

    // Aligned with openclaw: collectIdleCandidates
    std::vector<CachedRuntimeSnapshot> collect_idle_candidates(int64_t max_idle_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t now = now_ms();
        std::vector<CachedRuntimeSnapshot> result;
        for (auto& [key, entry] : cache_) {
            int64_t idle = now - entry.last_touched_at;
            if (idle >= max_idle_ms) {
                result.push_back({key, entry.state, entry.last_touched_at, idle});
            }
        }
        return result;
    }

private:
    struct CacheEntry {
        CachedRuntimeState state;
        int64_t last_touched_at = 0;
    };

    mutable std::mutex mutex_;
    std::map<std::string, CacheEntry> cache_;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

} // namespace acp
} // namespace hunterclaw
