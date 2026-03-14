#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <functional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace security {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 1. 认证模式枚举
// ============================================================================

enum class AuthMode {
    None,           // 无认证 (仅用于开发环境)
    Token,          // 长期令牌认证 (推荐生产环境)
    Password,       // 密码认证
    TrustedProxy,   // 反向代理认证 (Nginx/Caddy)
    DeviceToken     // 设备配对认证
};

// ============================================================================
// 2. 认证配置
// ============================================================================

struct TokenAuthConfig {
    std::string token;                              // SHA-256 令牌
    bool allow_tailscale = false;                   // 允许 Tailscale VPN 零配置 SSO
    std::vector<std::string> tailscale_users;       // 白名单用户 (可选)
};

struct PasswordAuthConfig {
    std::string password_hash;                      // bcrypt 哈希
    int work_factor = 12;                           // bcrypt 工作因子
};

struct TrustedProxyConfig {
    std::string user_header = "X-Auth-User";        // 用户标识头
    std::vector<std::string> required_headers;      // 必需的代理头 (X-Forwarded-For)
    std::vector<std::string> allow_users;           // 白名单用户列表
    std::vector<std::string> trusted_proxies;       // 可信代理 IP/CIDR
};

struct DeviceTokenConfig {
    std::string pairing_secret;                     // 配对密钥
    std::chrono::seconds token_ttl{86400 * 30};    // Token 有效期 (30天)
};

struct AuthConfig {
    AuthMode mode = AuthMode::Token;
    TokenAuthConfig token;
    PasswordAuthConfig password;
    TrustedProxyConfig trusted_proxy;
    DeviceTokenConfig device_token;
    
    // 速率限制配置
    struct RateLimitConfig {
        int max_attempts = 10;                      // 最大失败次数
        std::chrono::milliseconds window_ms{60000}; // 时间窗口 (1分钟)
        std::chrono::milliseconds lockout_ms{300000}; // 锁定时间 (5分钟)
        bool exempt_loopback = true;                // 本地环回地址豁免
    } rate_limit;
};

// ============================================================================
// 3. 认证结果
// ============================================================================

struct AuthResult {
    bool ok = false;
    std::string user_id;
    std::string session_id;
    std::unordered_map<std::string, std::string> metadata;
    
    // 错误信息
    bool rate_limited = false;
    std::optional<std::chrono::milliseconds> retry_after_ms;
    std::string error_message;
};

// ============================================================================
// 4. 滑动窗口速率限制器
// ============================================================================

class SlidingWindowRateLimiter {
public:
    explicit SlidingWindowRateLimiter(const AuthConfig::RateLimitConfig& config)
        : config_(config) {}
    
    struct CheckResult {
        bool allowed = true;
        int remaining_attempts = 0;
        std::optional<std::chrono::milliseconds> retry_after_ms;
    };
    
    CheckResult check(const std::string& client_ip, const std::string& scope) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto now = Clock::now();
        auto key = client_ip + ":" + scope;
        
        // 清理过期条目
        cleanup_expired(now);
        
        auto& entry = entries_[key];
        
        // 移除窗口外的失败记录
        auto window_start = now - config_.window_ms;
        entry.failures.erase(
            std::remove_if(entry.failures.begin(), entry.failures.end(),
                [&](const Clock::time_point& t) { return t < window_start; }),
            entry.failures.end()
        );
        
        // 检查是否处于锁定状态
        if (entry.locked_until && now < *entry.locked_until) {
            auto retry_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                *entry.locked_until - now);
            return CheckResult{false, 0, retry_ms};
        }
        
        // 检查失败次数
        if (entry.failures.size() >= static_cast<size_t>(config_.max_attempts)) {
            // 触发锁定
            entry.locked_until = now + config_.lockout_ms;
            return CheckResult{false, 0, config_.lockout_ms};
        }
        
        int remaining = config_.max_attempts - static_cast<int>(entry.failures.size());
        return CheckResult{true, remaining, std::nullopt};
    }
    
    void record_failure(const std::string& client_ip, const std::string& scope) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = client_ip + ":" + scope;
        entries_[key].failures.push_back(Clock::now());
    }
    
    void reset(const std::string& client_ip, const std::string& scope) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = client_ip + ":" + scope;
        entries_.erase(key);
    }

private:
    struct Entry {
        std::vector<Clock::time_point> failures;
        std::optional<Clock::time_point> locked_until;
    };
    
    void cleanup_expired(Clock::time_point now) {
        std::vector<std::string> to_remove;
        for (auto& [key, entry] : entries_) {
            if (entry.failures.empty() && 
                (!entry.locked_until || now >= *entry.locked_until)) {
                to_remove.push_back(key);
            }
        }
        for (const auto& key : to_remove) {
            entries_.erase(key);
        }
    }
    
    AuthConfig::RateLimitConfig config_;
    std::unordered_map<std::string, Entry> entries_;
    std::mutex mutex_;
};

// ============================================================================
// 5. 认证器接口
// ============================================================================

class Authenticator {
public:
    virtual ~Authenticator() = default;
    
    virtual AuthResult authenticate(
        const std::string& client_ip,
        const std::unordered_map<std::string, std::string>& headers,
        const std::optional<std::string>& credentials
    ) = 0;
};

// ============================================================================
// 6. Token 认证器
// ============================================================================

class TokenAuthenticator : public Authenticator {
public:
    TokenAuthenticator(const TokenAuthConfig& config, 
                      std::shared_ptr<SlidingWindowRateLimiter> rate_limiter)
        : config_(config), rate_limiter_(rate_limiter) {}
    
    AuthResult authenticate(
        const std::string& client_ip,
        const std::unordered_map<std::string, std::string>& headers,
        const std::optional<std::string>& credentials
    ) override {
        // 检查速率限制
        auto rate_result = rate_limiter_->check(client_ip, "token_auth");
        if (!rate_result.allowed) {
            return AuthResult{
                .ok = false,
                .rate_limited = true,
                .retry_after_ms = rate_result.retry_after_ms,
                .error_message = "Rate limit exceeded"
            };
        }
        
        // 验证 Token
        if (!credentials || credentials->empty()) {
            rate_limiter_->record_failure(client_ip, "token_auth");
            return AuthResult{.ok = false, .error_message = "Missing token"};
        }
        
        if (*credentials != config_.token) {
            rate_limiter_->record_failure(client_ip, "token_auth");
            return AuthResult{.ok = false, .error_message = "Invalid token"};
        }
        
        // 成功认证
        rate_limiter_->reset(client_ip, "token_auth");
        return AuthResult{
            .ok = true,
            .user_id = "token_user",
            .session_id = generate_session_id()
        };
    }

private:
    std::string generate_session_id() {
        // 生成随机 Session ID (实现略)
        return "session-" + std::to_string(Clock::now().time_since_epoch().count());
    }
    
    TokenAuthConfig config_;
    std::shared_ptr<SlidingWindowRateLimiter> rate_limiter_;
};

// ============================================================================
// 7. 反向代理认证器
// ============================================================================

class TrustedProxyAuthenticator : public Authenticator {
public:
    TrustedProxyAuthenticator(const TrustedProxyConfig& config)
        : config_(config) {}
    
    AuthResult authenticate(
        const std::string& client_ip,
        const std::unordered_map<std::string, std::string>& headers,
        const std::optional<std::string>& credentials
    ) override {
        // 验证必需的代理头
        for (const auto& header : config_.required_headers) {
            if (headers.find(header) == headers.end()) {
                return AuthResult{
                    .ok = false,
                    .error_message = "Missing required header: " + header
                };
            }
        }
        
        // 提取用户标识
        auto user_it = headers.find(config_.user_header);
        if (user_it == headers.end()) {
            return AuthResult{
                .ok = false,
                .error_message = "Missing user header: " + config_.user_header
            };
        }
        
        const std::string& user_id = user_it->second;
        
        // 检查白名单
        if (!config_.allow_users.empty()) {
            bool allowed = false;
            for (const auto& allowed_user : config_.allow_users) {
                if (user_id == allowed_user) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return AuthResult{
                    .ok = false,
                    .error_message = "User not in whitelist: " + user_id
                };
            }
        }
        
        return AuthResult{
            .ok = true,
            .user_id = user_id,
            .session_id = generate_session_id()
        };
    }

private:
    std::string generate_session_id() {
        return "session-" + std::to_string(Clock::now().time_since_epoch().count());
    }
    
    TrustedProxyConfig config_;
};

// ============================================================================
// 8. 统一认证管理器
// ============================================================================

class AuthManager {
public:
    explicit AuthManager(const AuthConfig& config)
        : config_(config),
          rate_limiter_(std::make_shared<SlidingWindowRateLimiter>(config.rate_limit))
    {
        initialize_authenticator();
    }
    
    AuthResult authenticate(
        const std::string& client_ip,
        const std::unordered_map<std::string, std::string>& headers,
        const std::optional<std::string>& credentials = std::nullopt
    ) {
        // 本地环回地址豁免 (CLI 友好)
        if (config_.rate_limit.exempt_loopback && is_loopback(client_ip)) {
            if (config_.mode == AuthMode::None) {
                return AuthResult{
                    .ok = true,
                    .user_id = "localhost",
                    .session_id = generate_session_id()
                };
            }
        }
        
        if (!authenticator_) {
            return AuthResult{.ok = false, .error_message = "No authenticator configured"};
        }
        
        return authenticator_->authenticate(client_ip, headers, credentials);
    }
    
    AuthMode mode() const { return config_.mode; }

private:
    void initialize_authenticator() {
        switch (config_.mode) {
            case AuthMode::None:
                authenticator_ = nullptr;
                break;
            case AuthMode::Token:
                authenticator_ = std::make_unique<TokenAuthenticator>(
                    config_.token, rate_limiter_);
                break;
            case AuthMode::TrustedProxy:
                authenticator_ = std::make_unique<TrustedProxyAuthenticator>(
                    config_.trusted_proxy);
                break;
            default:
                throw std::runtime_error("Unsupported auth mode");
        }
    }
    
    bool is_loopback(const std::string& ip) const {
        return ip == "127.0.0.1" || ip == "::1" || ip == "localhost";
    }
    
    std::string generate_session_id() {
        return "session-" + std::to_string(Clock::now().time_since_epoch().count());
    }
    
    AuthConfig config_;
    std::shared_ptr<SlidingWindowRateLimiter> rate_limiter_;
    std::unique_ptr<Authenticator> authenticator_;
};

// ============================================================================
// 9. 审计日志系统
// ============================================================================

enum class AuditEventType {
    AuthSuccess,
    AuthFailure,
    RateLimitExceeded,
    ConfigChange,
    ToolExecution,
    FileAccess,
    NetworkRequest,
    PermissionDenied
};

struct AuditLogEntry {
    std::string timestamp;              // ISO 8601 格式
    AuditEventType event;
    std::string session_id;
    std::string agent_id;
    std::string user_id;
    std::string client_ip;
    
    enum class RiskLevel { Low, Medium, High, Critical } risk = RiskLevel::Low;
    
    json details;
};

class AuditLogger {
public:
    static AuditLogger& instance() {
        static AuditLogger logger;
        return logger;
    }
    
    void log(const AuditLogEntry& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        json log_json = {
            {"ts", entry.timestamp},
            {"event", event_type_to_string(entry.event)},
            {"session_id", entry.session_id},
            {"agent_id", entry.agent_id},
            {"user_id", entry.user_id},
            {"client_ip", entry.client_ip},
            {"risk", risk_level_to_string(entry.risk)},
            {"details", entry.details}
        };
        
        // 写入审计日志文件 (实现略,可集成到 StructuredLogger)
        entries_.push_back(log_json);
        
        // 调用回调
        if (callback_) {
            callback_(entry);
        }
    }
    
    void set_callback(std::function<void(const AuditLogEntry&)> callback) {
        callback_ = std::move(callback);
    }
    
    std::vector<json> get_recent_entries(size_t count) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() <= count) {
            return entries_;
        }
        return std::vector<json>(entries_.end() - count, entries_.end());
    }

private:
    AuditLogger() = default;
    
    static std::string event_type_to_string(AuditEventType type) {
        switch (type) {
            case AuditEventType::AuthSuccess: return "auth.success";
            case AuditEventType::AuthFailure: return "auth.failure";
            case AuditEventType::RateLimitExceeded: return "rate_limit.exceeded";
            case AuditEventType::ConfigChange: return "config.change";
            case AuditEventType::ToolExecution: return "tool.exec";
            case AuditEventType::FileAccess: return "fs.access";
            case AuditEventType::NetworkRequest: return "network.request";
            case AuditEventType::PermissionDenied: return "permission.denied";
            default: return "unknown";
        }
    }
    
    static std::string risk_level_to_string(AuditLogEntry::RiskLevel level) {
        switch (level) {
            case AuditLogEntry::RiskLevel::Low: return "low";
            case AuditLogEntry::RiskLevel::Medium: return "medium";
            case AuditLogEntry::RiskLevel::High: return "high";
            case AuditLogEntry::RiskLevel::Critical: return "critical";
            default: return "unknown";
        }
    }
    
    mutable std::mutex mutex_;
    std::vector<json> entries_;
    std::function<void(const AuditLogEntry&)> callback_;
};

} // namespace security
} // namespace hunterclaw
