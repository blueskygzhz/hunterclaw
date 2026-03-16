#include "hunterclaw/gateway/auth_rate_limiter.h"
#include <spdlog/spdlog.h>
#include <regex>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * 检查是否是本地回环地址
 */
static bool is_loopback_ip(const std::string& ip) {
    if (ip.empty()) return false;
    
    // IPv4 loopback: 127.0.0.0/8
    if (ip.find("127.") == 0) return true;
    
    // IPv6 loopback: ::1
    if (ip == "::1" || ip == "::ffff:127.0.0.1") return true;
    
    // localhost
    if (ip == "localhost") return true;
    
    return false;
}

/**
 * 生成限制键 (ip:scope)
 */
static std::string make_key(const std::string& ip, const std::string& scope) {
    return ip + ":" + scope;
}

// ============================================================================
// 速率限制条目
// ============================================================================

struct RateLimitEntry {
    /** 失败尝试时间戳列表 (毫秒) */
    std::vector<int64_t> attempts;
    
    /** 锁定结束时间 (毫秒, 0 表示未锁定) */
    int64_t locked_until = 0;
};

// ============================================================================
// AuthRateLimiter::Impl
// ============================================================================

struct AuthRateLimiter::Impl {
    AuthRateLimiterConfig config;
    
    /** 限制条目映射: key -> entry */
    std::map<std::string, RateLimitEntry> entries;
    
    /** 日志记录器 */
    std::shared_ptr<spdlog::logger> logger;
    
    Impl(const AuthRateLimiterConfig& cfg)
        : config(cfg)
        , logger(spdlog::get("hunterclaw") ? spdlog::get("hunterclaw") : spdlog::default_logger())
    {
        logger->debug("[AuthRateLimiter] Initialized with max_attempts={}, window_ms={}, lockout_ms={}",
                     config.max_attempts, config.window_ms, config.lockout_ms);
    }
    
    /**
     * 滑动窗口: 移除窗口外的旧尝试
     */
    void slide_window(RateLimitEntry& entry, int64_t now) {
        int64_t window_start = now - config.window_ms;
        
        // 移除窗口外的尝试
        entry.attempts.erase(
            std::remove_if(
                entry.attempts.begin(),
                entry.attempts.end(),
                [window_start](int64_t attempt_time) {
                    return attempt_time < window_start;
                }
            ),
            entry.attempts.end()
        );
    }
    
    /**
     * 检查 IP 是否豁免
     */
    bool is_exempt(const std::string& ip) const {
        if (!config.exempt_loopback) return false;
        return is_loopback_ip(ip);
    }
    
    /**
     * 获取当前时间 (毫秒)
     */
    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

// ============================================================================
// AuthRateLimiter Implementation
// ============================================================================

AuthRateLimiter::AuthRateLimiter(const AuthRateLimiterConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
}

AuthRateLimiter::~AuthRateLimiter() = default;

RateLimitCheckResult AuthRateLimiter::check(
    const std::string& ip,
    const std::string& scope
) {
    // 豁免检查
    if (impl_->is_exempt(ip)) {
        return RateLimitCheckResult{
            .allowed = true,
            .remaining = impl_->config.max_attempts,
            .retry_after_ms = 0
        };
    }
    
    const std::string key = make_key(ip, scope);
    const int64_t now = Impl::now_ms();
    
    // 没有记录 -> 允许
    auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) {
        return RateLimitCheckResult{
            .allowed = true,
            .remaining = impl_->config.max_attempts,
            .retry_after_ms = 0
        };
    }
    
    auto& entry = it->second;
    
    // 检查是否仍在锁定期
    if (entry.locked_until > 0 && now < entry.locked_until) {
        impl_->logger->warn("[AuthRateLimiter] IP {} (scope: {}) is locked, retry after {} ms",
                           ip, scope, entry.locked_until - now);
        
        return RateLimitCheckResult{
            .allowed = false,
            .remaining = 0,
            .retry_after_ms = entry.locked_until - now
        };
    }
    
    // 滑动窗口: 移除过期尝试
    impl_->slide_window(entry, now);
    
    // 计算剩余尝试次数
    int remaining = std::max(0, impl_->config.max_attempts - static_cast<int>(entry.attempts.size()));
    bool allowed = remaining > 0;
    
    if (!allowed) {
        impl_->logger->warn("[AuthRateLimiter] IP {} (scope: {}) exceeded max attempts ({})",
                           ip, scope, impl_->config.max_attempts);
    }
    
    return RateLimitCheckResult{
        .allowed = allowed,
        .remaining = remaining,
        .retry_after_ms = 0
    };
}

void AuthRateLimiter::record_failure(
    const std::string& ip,
    const std::string& scope
) {
    // 豁免检查
    if (impl_->is_exempt(ip)) {
        return;
    }
    
    const std::string key = make_key(ip, scope);
    const int64_t now = Impl::now_ms();
    
    // 获取或创建条目
    auto& entry = impl_->entries[key];
    
    // 滑动窗口
    impl_->slide_window(entry, now);
    
    // 添加失败记录
    entry.attempts.push_back(now);
    
    impl_->logger->debug("[AuthRateLimiter] Recorded failure for IP {} (scope: {}), total attempts: {}",
                        ip, scope, entry.attempts.size());
    
    // 达到限制 -> 设置锁定时间
    if (static_cast<int>(entry.attempts.size()) >= impl_->config.max_attempts) {
        entry.locked_until = now + impl_->config.lockout_ms;
        
        impl_->logger->warn("[AuthRateLimiter] IP {} (scope: {}) locked until {} (lockout_ms: {})",
                           ip, scope, entry.locked_until, impl_->config.lockout_ms);
    }
}

void AuthRateLimiter::reset(
    const std::string& ip,
    const std::string& scope
) {
    const std::string key = make_key(ip, scope);
    
    auto it = impl_->entries.find(key);
    if (it != impl_->entries.end()) {
        impl_->logger->debug("[AuthRateLimiter] Reset rate limit for IP {} (scope: {})", ip, scope);
        impl_->entries.erase(it);
    }
}

size_t AuthRateLimiter::size() const {
    return impl_->entries.size();
}

size_t AuthRateLimiter::prune() {
    const int64_t now = Impl::now_ms();
    size_t pruned = 0;
    
    for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ) {
        auto& entry = it->second;
        
        // 滑动窗口
        impl_->slide_window(entry, now);
        
        // 条目已空且未锁定 -> 删除
        bool is_locked = entry.locked_until > 0 && entry.locked_until >= now;
        if (entry.attempts.empty() && !is_locked) {
            it = impl_->entries.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    
    if (pruned > 0) {
        impl_->logger->debug("[AuthRateLimiter] Pruned {} expired entries", pruned);
    }
    
    return pruned;
}

void AuthRateLimiter::dispose() {
    impl_->logger->debug("[AuthRateLimiter] Disposing all entries (count: {})", impl_->entries.size());
    impl_->entries.clear();
}

} // namespace gateway
} // namespace hunterclaw
