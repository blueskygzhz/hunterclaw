#include "hunterclaw/memory/active_memory_circuit_breaker.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

// 🎯 严格对齐 OpenClaw extensions/active-memory/recall-state.ts 的
//   timeoutCircuitBreaker 部分。

namespace hunterclaw {
namespace memory {

namespace {

std::mutex& cb_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, CircuitBreakerEntry>& cb_map() {
    static std::unordered_map<std::string, CircuitBreakerEntry> m;
    return m;
}

// 对齐 Date.now(): epoch 毫秒。
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string build_circuit_breaker_key(
    const std::string& agent_id,
    const std::optional<std::string>& provider,
    const std::optional<std::string>& model) {
    // 对齐 OC: `${agentId}:${provider ?? "unknown"}/${model ?? "unknown"}`
    return agent_id + ":" + provider.value_or("unknown") + "/" + model.value_or("unknown");
}

bool is_circuit_breaker_open(const std::string& key, int max_timeouts, int64_t cooldown_ms) {
    std::lock_guard<std::mutex> lock(cb_mutex());
    auto& map = cb_map();
    auto it = map.find(key);
    // 对齐 OC: if (!entry || entry.consecutiveTimeouts < maxTimeouts) return false;
    if (it == map.end() || it->second.consecutive_timeouts < max_timeouts) {
        return false;
    }
    // 对齐 OC: if (Date.now() - entry.lastTimeoutAt >= cooldownMs) { delete; return false; }
    if (now_ms() - it->second.last_timeout_at >= cooldown_ms) {
        map.erase(it);
        return false;
    }
    return true;
}

void record_circuit_breaker_timeout(const std::string& key) {
    std::lock_guard<std::mutex> lock(cb_mutex());
    auto& map = cb_map();
    auto it = map.find(key);
    if (it != map.end()) {
        // 对齐 OC: entry.consecutiveTimeouts++; entry.lastTimeoutAt = Date.now();
        it->second.consecutive_timeouts++;
        it->second.last_timeout_at = now_ms();
    } else {
        // 对齐 OC: set({ consecutiveTimeouts: 1, lastTimeoutAt: Date.now() })
        map[key] = CircuitBreakerEntry{1, now_ms()};
    }
}

void reset_circuit_breaker(const std::string& key) {
    std::lock_guard<std::mutex> lock(cb_mutex());
    cb_map().erase(key);
}

std::optional<CircuitBreakerEntry> get_circuit_breaker_entry(const std::string& key) {
    std::lock_guard<std::mutex> lock(cb_mutex());
    auto& map = cb_map();
    auto it = map.find(key);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

void reset_all_circuit_breakers() {
    std::lock_guard<std::mutex> lock(cb_mutex());
    cb_map().clear();
}

} // namespace memory
} // namespace hunterclaw
