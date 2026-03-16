#pragma once

#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <memory>

namespace hunterclaw {
namespace gateway {

/**
 * 🎯 100% 对齐 OpenClaw: Auth Rate Limiter
 * 
 * 基于滑动窗口的速率限制器,用于防止鉴权暴力破解攻击。
 * 
 * OpenClaw 参考: src/gateway/auth-rate-limit.ts
 * 
 * 特性:
 * - 滑动窗口算法 (Sliding Window)
 * - 基于 IP + Scope 的限制
 * - 自动锁定和解锁
 * - 本地回环豁免
 * - 自动清理过期条目
 * 
 * 使用示例:
 * ```cpp
 * auto limiter = std::make_shared<AuthRateLimiter>(
 *     AuthRateLimiterConfig{
 *         .max_attempts = 10,
 *         .window_ms = 60000,      // 1 分钟
 *         .lockout_ms = 300000,    // 锁定 5 分钟
 *         .exempt_loopback = true
 *     }
 * );
 * 
 * // 检查是否允许
 * auto result = limiter->check("192.168.1.100", "default");
 * if (!result.allowed) {
 *     return error("rate_limited", result.retry_after_ms);
 * }
 * 
 * // 记录失败
 * limiter->record_failure("192.168.1.100", "default");
 * 
 * // 成功后重置
 * limiter->reset("192.168.1.100", "default");
 * ```
 */

/**
 * 速率限制配置
 */
struct AuthRateLimiterConfig {
    /** 最大失败尝试次数 (默认: 10) */
    int max_attempts = 10;
    
    /** 滑动窗口时长 (毫秒, 默认: 60000 = 1分钟) */
    int64_t window_ms = 60000;
    
    /** 锁定时长 (毫秒, 默认: 300000 = 5分钟) */
    int64_t lockout_ms = 300000;
    
    /** 是否豁免本地回环地址 (默认: true) */
    bool exempt_loopback = true;
};

/**
 * 检查结果
 */
struct RateLimitCheckResult {
    /** 是否允许 */
    bool allowed = true;
    
    /** 剩余尝试次数 */
    int remaining = 0;
    
    /** 重试等待时间 (毫秒, 0 表示未锁定) */
    int64_t retry_after_ms = 0;
};

/**
 * 速率限制器
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export function createAuthRateLimiter(config?: RateLimitConfig): AuthRateLimiter {
 *   const maxAttempts = config?.maxAttempts ?? 10;
 *   const windowMs = config?.windowMs ?? 60000;
 *   const lockoutMs = config?.lockoutMs ?? 300000;
 *   const exemptLoopback = config?.exemptLoopback ?? true;
 *   
 *   function check(rawIp: string, scope?: string): RateLimitCheckResult {
 *     // 检查是否在锁定期
 *     // 滑动窗口检查
 *   }
 *   
 *   function recordFailure(rawIp: string, scope?: string): void {
 *     // 记录失败尝试
 *     // 达到限制时设置锁定时间
 *   }
 *   
 *   function reset(rawIp: string, scope?: string): void {
 *     // 清除失败记录
 *   }
 * }
 * ```
 */
class AuthRateLimiter {
public:
    /**
     * 构造函数
     * @param config 配置对象
     */
    explicit AuthRateLimiter(const AuthRateLimiterConfig& config = {});
    ~AuthRateLimiter();
    
    /**
     * 检查是否允许访问
     * 
     * @param ip 客户端 IP 地址
     * @param scope 限制作用域 (可选, 用于区分不同类型的鉴权)
     * @return 检查结果
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function check(rawIp: string | undefined, rawScope?: string): RateLimitCheckResult {
     *   const { key, ip } = resolveKey(rawIp, rawScope);
     *   if (isExempt(ip)) {
     *     return { allowed: true, remaining: maxAttempts, retryAfterMs: 0 };
     *   }
     *   
     *   const entry = entries.get(key);
     *   if (!entry) {
     *     return { allowed: true, remaining: maxAttempts, retryAfterMs: 0 };
     *   }
     *   
     *   // 检查是否仍在锁定期
     *   if (entry.lockedUntil && now < entry.lockedUntil) {
     *     return {
     *       allowed: false,
     *       remaining: 0,
     *       retryAfterMs: entry.lockedUntil - now,
     *     };
     *   }
     *   
     *   slideWindow(entry, now);
     *   const remaining = Math.max(0, maxAttempts - entry.attempts.length);
     *   return { allowed: remaining > 0, remaining, retryAfterMs: 0 };
     * }
     * ```
     */
    RateLimitCheckResult check(
        const std::string& ip,
        const std::string& scope = "default"
    );
    
    /**
     * 记录失败尝试
     * 
     * @param ip 客户端 IP 地址
     * @param scope 限制作用域
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function recordFailure(rawIp: string | undefined, rawScope?: string): void {
     *   const { key, ip } = resolveKey(rawIp, rawScope);
     *   if (isExempt(ip)) return;
     *   
     *   const now = Date.now();
     *   let entry = entries.get(key);
     *   
     *   if (!entry) {
     *     entry = { attempts: [] };
     *     entries.set(key, entry);
     *   }
     *   
     *   slideWindow(entry, now);
     *   entry.attempts.push(now);
     *   
     *   // 达到限制时设置锁定时间
     *   if (entry.attempts.length >= maxAttempts) {
     *     entry.lockedUntil = now + lockoutMs;
     *   }
     * }
     * ```
     */
    void record_failure(
        const std::string& ip,
        const std::string& scope = "default"
    );
    
    /**
     * 重置 (鉴权成功后调用)
     * 
     * @param ip 客户端 IP 地址
     * @param scope 限制作用域
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function reset(rawIp: string | undefined, rawScope?: string): void {
     *   const { key } = resolveKey(rawIp, rawScope);
     *   entries.delete(key);
     * }
     * ```
     */
    void reset(
        const std::string& ip,
        const std::string& scope = "default"
    );
    
    /**
     * 获取当前条目数量
     * @return 条目数
     */
    size_t size() const;
    
    /**
     * 清理过期条目
     * @return 清理的条目数
     * 
     * OpenClaw 对齐:
     * ```typescript
     * function prune(): number {
     *   let pruned = 0;
     *   const now = Date.now();
     *   for (const [key, entry] of entries) {
     *     slideWindow(entry, now);
     *     if (entry.attempts.length === 0 && (!entry.lockedUntil || entry.lockedUntil < now)) {
     *       entries.delete(key);
     *       pruned++;
     *     }
     *   }
     *   return pruned;
     * }
     * ```
     */
    size_t prune();
    
    /**
     * 清空所有条目
     */
    void dispose();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// 速率限制作用域常量 (对齐 OpenClaw)
// ============================================================================

namespace rate_limit_scopes {
    /** 默认作用域 */
    constexpr const char* DEFAULT = "default";
    
    /** 共享密钥鉴权 */
    constexpr const char* SHARED_SECRET = "shared-secret";
    
    /** 设备令牌鉴权 */
    constexpr const char* DEVICE_TOKEN = "device-token";
    
    /** Hook 鉴权 */
    constexpr const char* HOOK_AUTH = "hook-auth";
}

} // namespace gateway
} // namespace hunterclaw
