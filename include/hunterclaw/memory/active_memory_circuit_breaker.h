#pragma once

// 🎯 严格对齐 OpenClaw extensions/active-memory/recall-state.ts 的
//   timeoutCircuitBreaker 部分 + config.ts 默认常量。
//   记录每个 "{agentId}:{provider}/{model}" 键的连续超时次数；连续超时达阈值
//   且未过冷却期→ 熔断打开（跳过 lane-1 召回）；冷却期过则重置放行一次。

#include <cstdint>
#include <optional>
#include <string>

namespace hunterclaw {
namespace memory {

//对齐 config.ts 默认与 clamp 边界。
constexpr int     DEFAULT_CIRCUIT_BREAKER_MAX_TIMEOUTS = 3;        // clamp [1,20]
constexpr int64_t DEFAULT_CIRCUIT_BREAKER_COOLDOWN_MS  = 60000;    // clamp [5000,600000]

// 对齐 CircuitBreakerEntry (types.ts)。
struct CircuitBreakerEntry {
    int consecutive_timeouts = 0;
    int64_t last_timeout_at = 0;  // epoch ms
};

// 对齐 buildCircuitBreakerKey: "{agentId}:{provider??unknown}/{model??unknown}"。
std::string build_circuit_breaker_key(
    const std::string& agent_id,
    const std::optional<std::string>& provider = std::nullopt,
    const std::optional<std::string>& model = std::nullopt);

// 对齐 isCircuitBreakerOpen:
//   无记录或连续超时 < max_timeouts → false；
//   已过 cooldown → 删除记录并 false（放行一次）；
//   否则 true（熔断）。
bool is_circuit_breaker_open(const std::string& key, int max_timeouts, int64_t cooldown_ms);

// 对齐 recordCircuitBreakerTimeout: 递增连续超时并刷新 last_timeout_at。
void record_circuit_breaker_timeout(const std::string& key);

// 对齐 resetCircuitBreaker: 删除该键记录（成功召回后调用）。
void reset_circuit_breaker(const std::string& key);

// 对齐 getCircuitBreakerEntry: 读取当前记录（用于观测/测试）。
std::optional<CircuitBreakerEntry> get_circuit_breaker_entry(const std::string& key);

// 对齐 resetActiveRecallStateForTests 中的 timeoutCircuitBreaker.clear()。
void reset_all_circuit_breakers();

} // namespace memory
} // namespace hunterclaw
