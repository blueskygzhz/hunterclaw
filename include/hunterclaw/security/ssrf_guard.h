#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace security {

// ✅ 严格对齐 OpenClaw: src/common/ssrf.ts + fetchWithSsrfGuard

// SSRF 防护策略配置
struct SsrfGuardConfig {
    // DNS rebinding 防护
    bool enable_dns_rebinding_check = true;
    int max_dns_ttl_seconds = 300;  // 5分钟内重新验证
    
    // 重定向链防护
    bool enable_redirect_check = true;
    int max_redirects = 5;  // 最多跟踪 5 次重定向
    
    // IP 白名单/黑名单
    std::vector<std::string> allowed_ip_ranges;
    std::vector<std::string> blocked_ip_ranges;
    
    // 域名白名单/黑名单
    std::vector<std::string> allowed_domains;
    std::vector<std::string> blocked_domains;
    
    // IPv6 防护
    bool enable_ipv6_check = true;
    bool block_ipv6_linklocal = true;      // 阻止 fe80::/10
    bool block_ipv6_uniquelocal = true;    // 阻止 fc00::/7
    bool block_ipv6_multicast = true;      // 阻止 ff00::/8
    
    // 超时设置
    int dns_resolve_timeout_ms = 5000;
    int http_request_timeout_ms = 30000;
};

// SSRF 检查结果
struct SsrfCheckResult {
    bool allowed = false;
    std::string reason;
    std::string resolved_ip;  // 最终解析的 IP 地址
    int redirect_count = 0;   // 实际重定向次数
};

// SSRF Guard - 对齐 OpenClaw 的 SSRF 防护机制
class SsrfGuard {
public:
    explicit SsrfGuard(const SsrfGuardConfig& config = SsrfGuardConfig{});
    
    // 🎯 对齐 OpenClaw: fetchWithSsrfGuard
    // 检查 URL 是否安全 (在 HTTP 请求前调用)
    SsrfCheckResult check_url(const std::string& url);
    
    // 🎯 对齐 OpenClaw: DNS rebinding 防护
    // 在请求过程中重新验证 IP (防止 TOCTOU)
    SsrfCheckResult verify_during_request(
        const std::string& url,
        const std::string& resolved_ip
    );
    
    // 🎯 对齐 OpenClaw: 重定向链检查
    // 验证重定向目标 URL
    SsrfCheckResult check_redirect(
        const std::string& original_url,
        const std::string& redirect_url,
        int current_redirect_count
    );
    
private:
    SsrfGuardConfig config_;
    
    // DNS 缓存 (用于 rebinding 检测)
    struct DnsCacheEntry {
        std::string ip;
        int64_t resolve_time_ms;
        int64_t ttl_seconds;
    };
    std::map<std::string, DnsCacheEntry> dns_cache_;
    
    // 辅助函数
    std::optional<std::string> resolve_hostname(const std::string& hostname);
    bool is_private_or_special_ip(const std::string& ip);
    bool is_ipv6_private_range(const std::string& ip);
    bool is_allowed_domain(const std::string& domain);
    bool is_blocked_domain(const std::string& domain);
    std::string extract_hostname(const std::string& url);
};

// ✅ 工厂函数: 创建默认的 SSRF Guard
std::shared_ptr<SsrfGuard> create_default_ssrf_guard();

// ✅ 全局单例访问 (与 OpenClaw 对齐)
SsrfGuard& get_global_ssrf_guard();

} // namespace security
} // namespace hunterclaw
