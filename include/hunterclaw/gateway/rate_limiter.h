#pragma once

#include "hunterclaw/core/result.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// 限流策略 (Rate Limit Strategy)
// ============================================================================

enum class RateLimitStrategy {
    FixedWindow,     // 固定窗口计数器
    SlidingWindow,   // 滑动窗口日志
    TokenBucket,     // 令牌桶
    LeakyBucket      // 漏桶
};

// ============================================================================
// 固定窗口限流器 (Fixed Window Rate Limiter)
// ============================================================================

struct FixedWindowConfig {
    size_t max_requests = 100;                  // 窗口内最大请求数
    std::chrono::milliseconds window_ms{60000}; // 窗口大小 (1分钟)
    bool exempt_loopback = true;                // 本地请求豁免
};

class FixedWindowRateLimiter {
public:
    explicit FixedWindowRateLimiter(const FixedWindowConfig& config);
    
    // 检查是否超限
    bool check_limit(const std::string& identifier);
    
    // 重置计数器
    void reset(const std::string& identifier);
    void reset_all();
    
    // 查询状态
    size_t get_count(const std::string& identifier) const;
    std::chrono::system_clock::time_point get_window_start(const std::string& identifier) const;
    
private:
    struct WindowData {
        size_t count = 0;
        std::chrono::system_clock::time_point window_start;
    };
    
    FixedWindowConfig config_;
    std::map<std::string, WindowData> windows_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 滑动窗口限流器 (Sliding Window Log Rate Limiter)
// ============================================================================

struct SlidingWindowConfig {
    size_t max_requests = 100;                  // 窗口内最大请求数
    std::chrono::milliseconds window_ms{60000}; // 窗口大小 (1分钟)
    bool exempt_loopback = true;
};

class SlidingWindowRateLimiter {
public:
    explicit SlidingWindowRateLimiter(const SlidingWindowConfig& config);
    
    bool check_limit(const std::string& identifier);
    void reset(const std::string& identifier);
    void reset_all();
    
    size_t get_count(const std::string& identifier) const;
    
private:
    void cleanup_expired_entries(std::vector<std::chrono::system_clock::time_point>& log);
    
    SlidingWindowConfig config_;
    std::map<std::string, std::vector<std::chrono::system_clock::time_point>> logs_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 令牌桶限流器 (Token Bucket Rate Limiter)
// ============================================================================

struct TokenBucketConfig {
    size_t capacity = 100;                      // 桶容量
    size_t refill_rate = 10;                    // 每秒补充令牌数
    size_t initial_tokens = 100;                // 初始令牌数
    bool exempt_loopback = true;
};

class TokenBucketRateLimiter {
public:
    explicit TokenBucketRateLimiter(const TokenBucketConfig& config);
    
    // 尝试消费 tokens 个令牌
    bool try_consume(const std::string& identifier, size_t tokens = 1);
    
    // 查询状态
    size_t available_tokens(const std::string& identifier) const;
    
    void reset(const std::string& identifier);
    void reset_all();
    
private:
    struct BucketData {
        size_t tokens;
        std::chrono::steady_clock::time_point last_refill_time;
    };
    
    void refill_tokens(BucketData& bucket);
    
    TokenBucketConfig config_;
    std::map<std::string, BucketData> buckets_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 漏桶限流器 (Leaky Bucket Rate Limiter)
// ============================================================================

struct LeakyBucketConfig {
    size_t capacity = 100;                      // 桶容量
    size_t leak_rate = 10;                      // 每秒漏出请求数
    bool exempt_loopback = true;
};

class LeakyBucketRateLimiter {
public:
    explicit LeakyBucketRateLimiter(const LeakyBucketConfig& config);
    
    bool try_add(const std::string& identifier);
    size_t queue_size(const std::string& identifier) const;
    
    void reset(const std::string& identifier);
    void reset_all();
    
private:
    struct BucketData {
        size_t queue_size;
        std::chrono::steady_clock::time_point last_leak_time;
    };
    
    void leak_requests(BucketData& bucket);
    
    LeakyBucketConfig config_;
    std::map<std::string, BucketData> buckets_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 熔断器 (Circuit Breaker)
// ============================================================================

enum class CircuitState {
    Closed,      // 关闭 (正常)
    Open,        // 打开 (熔断)
    HalfOpen     // 半开 (尝试恢复)
};

struct CircuitBreakerConfig {
    size_t failure_threshold = 5;                      // 失败阈值
    std::chrono::milliseconds timeout_ms{30000};       // 熔断超时 (30秒)
    size_t success_threshold = 2;                      // 半开状态成功阈值
    std::chrono::milliseconds monitoring_window_ms{60000};  // 监控窗口 (1分钟)
};

class CircuitBreaker {
public:
    explicit CircuitBreaker(const CircuitBreakerConfig& config);
    
    // 检查熔断器状态
    bool is_request_allowed();
    
    // 记录请求结果
    void record_success();
    void record_failure();
    
    // 查询状态
    CircuitState state() const { return state_; }
    size_t failure_count() const { return failure_count_; }
    size_t success_count() const { return success_count_; }
    
    // 手动控制
    void reset();
    void force_open();
    void force_close();
    
private:
    void transition_to_open();
    void transition_to_half_open();
    void transition_to_closed();
    
    CircuitBreakerConfig config_;
    std::atomic<CircuitState> state_{CircuitState::Closed};
    std::atomic<size_t> failure_count_{0};
    std::atomic<size_t> success_count_{0};
    std::chrono::system_clock::time_point last_failure_time_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 全局限流器 (Global Rate Limiter)
// ============================================================================

struct GlobalRateLimitConfig {
    RateLimitStrategy strategy = RateLimitStrategy::SlidingWindow;
    
    // 策略配置 (根据 strategy 选择)
    std::optional<FixedWindowConfig> fixed_window;
    std::optional<SlidingWindowConfig> sliding_window;
    std::optional<TokenBucketConfig> token_bucket;
    std::optional<LeakyBucketConfig> leaky_bucket;
    
    // 熔断器配置 (可选)
    std::optional<CircuitBreakerConfig> circuit_breaker;
    
    core::JsonValue to_json() const;
    static core::Result<GlobalRateLimitConfig> from_json(const core::JsonValue& json);
};

class GlobalRateLimiter {
public:
    explicit GlobalRateLimiter(const GlobalRateLimitConfig& config);
    
    // 检查限流 + 熔断
    bool check_limit(const std::string& identifier);
    
    // 记录请求结果 (用于熔断器)
    void record_success(const std::string& identifier);
    void record_failure(const std::string& identifier);
    
    // 查询状态
    RateLimitStrategy strategy() const { return config_.strategy; }
    std::optional<CircuitState> circuit_state(const std::string& identifier) const;
    
    // 重置
    void reset(const std::string& identifier);
    void reset_all();
    
private:
    GlobalRateLimitConfig config_;
    
    // 策略实现 (只会初始化一个)
    std::unique_ptr<FixedWindowRateLimiter> fixed_window_limiter_;
    std::unique_ptr<SlidingWindowRateLimiter> sliding_window_limiter_;
    std::unique_ptr<TokenBucketRateLimiter> token_bucket_limiter_;
    std::unique_ptr<LeakyBucketRateLimiter> leaky_bucket_limiter_;
    
    // 熔断器 (每个 identifier 一个)
    std::map<std::string, std::unique_ptr<CircuitBreaker>> circuit_breakers_;
    mutable std::mutex mutex_;
};

// ============================================================================
// Gateway 限流中间件 (Rate Limit Middleware)
// ============================================================================

struct RateLimitResult {
    bool allowed;
    std::optional<std::string> reason;          // 拒绝原因
    std::optional<size_t> retry_after_ms;       // 建议重试时间 (ms)
    
    // 速率限制头 (X-RateLimit-*)
    std::optional<size_t> limit;                // X-RateLimit-Limit
    std::optional<size_t> remaining;            // X-RateLimit-Remaining
    std::optional<std::chrono::system_clock::time_point> reset;  // X-RateLimit-Reset
};

class GatewayRateLimitMiddleware {
public:
    explicit GatewayRateLimitMiddleware(std::shared_ptr<GlobalRateLimiter> limiter);
    
    // 检查请求是否允许
    RateLimitResult check_request(
        const std::string& identifier,
        const std::string& path,
        const std::string& method
    );
    
    // 注册路径限流规则 (覆盖全局规则)
    void register_path_rule(const std::string& path, const GlobalRateLimitConfig& config);
    
    // 白名单管理
    void add_to_whitelist(const std::string& identifier);
    void remove_from_whitelist(const std::string& identifier);
    bool is_whitelisted(const std::string& identifier) const;
    
private:
    std::shared_ptr<GlobalRateLimiter> global_limiter_;
    std::map<std::string, std::shared_ptr<GlobalRateLimiter>> path_limiters_;
    std::set<std::string> whitelist_;
    mutable std::mutex mutex_;
};

// ============================================================================
// 工具函数
// ============================================================================

// 从 IP 地址提取标识符
std::string extract_identifier_from_ip(const std::string& ip);

// 从用户 ID 提取标识符
std::string extract_identifier_from_user(const std::string& user_id);

// 组合标识符 (例如: "user:123", "ip:192.168.1.1")
std::string combine_identifier(const std::string& type, const std::string& value);

}  // namespace gateway
}  // namespace hunterclaw
