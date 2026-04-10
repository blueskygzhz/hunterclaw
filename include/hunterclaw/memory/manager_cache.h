#pragma once

// ============================================================================
// Manager Cache — 严格对齐 OpenClaw manager-cache.ts (78 行)
//
// 通用 Managed Cache 模式 — 按 key 缓存实例, 支持 pending/create/close。
// ============================================================================

#include <string>
#include <map>
#include <mutex>
#include <functional>

namespace hunterclaw {
namespace memory {

template <typename T>
class ManagedCache {
public:
    bool has(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.count(key) > 0;
    }

    T* get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        return (it != cache_.end()) ? &it->second : nullptr;
    }

    T& get_or_create(const std::string& key, std::function<T()> create) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        cache_[key] = create();
        return cache_[key];
    }

    void set(const std::string& key, T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[key] = std::move(value);
    }

    void remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.erase(key);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

    std::map<std::string, T> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_;
    }

private:
    std::map<std::string, T> cache_;
    mutable std::mutex mutex_;
};

}  // namespace memory
}  // namespace hunterclaw
