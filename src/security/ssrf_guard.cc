#include "hunterclaw/security/ssrf_guard.h"
#include "hunterclaw/shared/utils.h"  // for is_private_or_loopback_ip_address
#include <regex>
#include <chrono>
#include <netdb.h>
#include <arpa/inet.h>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace security {

namespace {
auto log() {
    static auto logger = spdlog::get("ssrf");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("ssrf");
    }
    return logger;
}

// 当前时间戳 (毫秒)
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace

SsrfGuard::SsrfGuard(const SsrfGuardConfig& config)
    : config_(config) {}

std::string SsrfGuard::extract_hostname(const std::string& url) {
    // 提取 hostname: http://example.com:8080/path -> example.com
    std::regex url_regex(R"(^(?:https?:\/\/)?([^:\/\s]+))");
    std::smatch match;
    if (std::regex_search(url, match, url_regex) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

std::optional<std::string> SsrfGuard::resolve_hostname(const std::string& hostname) {
    // ✅ 对齐 OpenClaw: DNS 解析
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;  // IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    if (ret != 0 || !res) {
        log()->warn("DNS resolve failed for {}: {}", hostname, gai_strerror(ret));
        return std::nullopt;
    }
    
    // 获取第一个 IP 地址
    std::string ip;
    char ipstr[INET6_ADDRSTRLEN];
    
    if (res->ai_family == AF_INET) {
        // IPv4
        auto* ipv4 = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
        inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, sizeof(ipstr));
        ip = ipstr;
    } else if (res->ai_family == AF_INET6) {
        // IPv6
        auto* ipv6 = reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
        inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipstr, sizeof(ipstr));
        ip = ipstr;
    }
    
    freeaddrinfo(res);
    
    if (ip.empty()) {
        return std::nullopt;
    }
    
    return ip;
}

bool SsrfGuard::is_private_or_special_ip(const std::string& ip) {
    // ✅ 对齐 OpenClaw: 使用 is_private_or_loopback_ip_address (来自 utils.cc)
    return is_private_or_loopback_ip_address(ip);
}

bool SsrfGuard::is_ipv6_private_range(const std::string& ip) {
    // ✅ 对齐 OpenClaw: 完整的 IPv6 私有地址检测
    if (ip.find(':') == std::string::npos) {
        return false;  // 不是 IPv6
    }
    
    // IPv6 私有地址范围:
    // - Loopback: ::1
    // - Link-local: fe80::/10
    // - Unique local: fc00::/7
    // - Multicast: ff00::/8
    
    if (ip == "::1" || ip.find("::ffff:127.") == 0) {
        return true;  // Loopback
    }
    
    if (ip.substr(0, 4) == "fe80" || ip.substr(0, 4) == "fe90" ||
        ip.substr(0, 4) == "fea0" || ip.substr(0, 4) == "feb0") {
        return config_.block_ipv6_linklocal;  // Link-local
    }
    
    if (ip.substr(0, 2) == "fc" || ip.substr(0, 2) == "fd") {
        return config_.block_ipv6_uniquelocal;  // Unique local
    }
    
    if (ip.substr(0, 2) == "ff") {
        return config_.block_ipv6_multicast;  // Multicast
    }
    
    return false;
}

bool SsrfGuard::is_allowed_domain(const std::string& domain) {
    if (config_.allowed_domains.empty()) {
        return true;  // 没有白名单,默认允许
    }
    
    for (const auto& pattern : config_.allowed_domains) {
        std::string suffix = "." + pattern;
        if (domain == pattern || 
            (domain.size() > suffix.size() && 
             domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0)) {
            return true;
        }
    }
    
    return false;
}

bool SsrfGuard::is_blocked_domain(const std::string& domain) {
    for (const auto& pattern : config_.blocked_domains) {
        std::string suffix = "." + pattern;
        if (domain == pattern || 
            (domain.size() > suffix.size() && 
             domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0)) {
            return true;
        }
    }
    
    return false;
}

SsrfCheckResult SsrfGuard::check_url(const std::string& url) {
    SsrfCheckResult result;
    
    // 1. URL 格式检查
    if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
        result.reason = "Only HTTP/HTTPS URLs are allowed";
        return result;
    }
    
    // 2. 提取 hostname
    std::string hostname = extract_hostname(url);
    if (hostname.empty()) {
        result.reason = "Invalid URL format";
        return result;
    }
    
    // 3. 域名黑名单检查
    if (is_blocked_domain(hostname)) {
        result.reason = "Domain is blocked: " + hostname;
        log()->warn("SSRF blocked: domain blacklist - {}", hostname);
        return result;
    }
    
    // 4. 域名白名单检查
    if (!is_allowed_domain(hostname)) {
        result.reason = "Domain is not in allowlist: " + hostname;
        log()->warn("SSRF blocked: domain not whitelisted - {}", hostname);
        return result;
    }
    
    // 5. DNS 解析
    auto ip_opt = resolve_hostname(hostname);
    if (!ip_opt) {
        result.reason = "Failed to resolve hostname: " + hostname;
        return result;
    }
    
    std::string ip = *ip_opt;
    result.resolved_ip = ip;
    
    // 6. ✅ 对齐 OpenClaw: IPv4 私有地址检查
    if (is_private_or_special_ip(ip)) {
        result.reason = "Resolved to private/special IP: " + ip;
        log()->warn("SSRF blocked: private IP - {} -> {}", hostname, ip);
        return result;
    }
    
    // 7. ✅ 对齐 OpenClaw: IPv6 私有地址检查 (新增!)
    if (config_.enable_ipv6_check && is_ipv6_private_range(ip)) {
        result.reason = "Resolved to private IPv6 address: " + ip;
        log()->warn("SSRF blocked: private IPv6 - {} -> {}", hostname, ip);
        return result;
    }
    
    // 8. ✅ 对齐 OpenClaw: DNS rebinding 防护 - 缓存 DNS 结果
    if (config_.enable_dns_rebinding_check) {
        DnsCacheEntry cache_entry;
        cache_entry.ip = ip;
        cache_entry.resolve_time_ms = now_ms();
        cache_entry.ttl_seconds = config_.max_dns_ttl_seconds;
        dns_cache_[hostname] = cache_entry;
    }
    
    result.allowed = true;
    log()->info("SSRF check passed: {} -> {}", hostname, ip);
    return result;
}

SsrfCheckResult SsrfGuard::verify_during_request(
    const std::string& url,
    const std::string& resolved_ip
) {
    SsrfCheckResult result;
    
    // ✅ 对齐 OpenClaw: DNS rebinding 防护
    // 检查 IP 是否发生变化 (TOCTOU attack)
    
    std::string hostname = extract_hostname(url);
    if (hostname.empty()) {
        result.reason = "Invalid URL";
        return result;
    }
    
    // 检查缓存
    auto it = dns_cache_.find(hostname);
    if (it != dns_cache_.end()) {
        const auto& cached = it->second;
        
        // TTL 检查
        int64_t elapsed_ms = now_ms() - cached.resolve_time_ms;
        if (elapsed_ms > cached.ttl_seconds * 1000) {
            // 缓存过期,重新解析
            auto new_ip_opt = resolve_hostname(hostname);
            if (!new_ip_opt) {
                result.reason = "DNS rebinding check: failed to re-resolve";
                return result;
            }
            
            std::string new_ip = *new_ip_opt;
            if (new_ip != cached.ip) {
                result.reason = "DNS rebinding detected: IP changed from " +
                               cached.ip + " to " + new_ip;
                log()->error("SSRF attack: DNS rebinding - {} changed from {} to {}",
                            hostname, cached.ip, new_ip);
                return result;
            }
        }
        
        // 验证当前 IP 与缓存一致
        if (resolved_ip != cached.ip) {
            result.reason = "IP mismatch: expected " + cached.ip +
                           ", got " + resolved_ip;
            log()->error("SSRF attack: IP mismatch - {} expected {} but got {}",
                        hostname, cached.ip, resolved_ip);
            return result;
        }
    }
    
    // 再次检查 IP 是否为私有地址
    if (is_private_or_special_ip(resolved_ip)) {
        result.reason = "IP is private during request: " + resolved_ip;
        log()->error("SSRF attack: private IP during request - {}", resolved_ip);
        return result;
    }
    
    if (config_.enable_ipv6_check && is_ipv6_private_range(resolved_ip)) {
        result.reason = "IPv6 is private during request: " + resolved_ip;
        log()->error("SSRF attack: private IPv6 during request - {}", resolved_ip);
        return result;
    }
    
    result.allowed = true;
    result.resolved_ip = resolved_ip;
    return result;
}

SsrfCheckResult SsrfGuard::check_redirect(
    const std::string& original_url,
    const std::string& redirect_url,
    int current_redirect_count
) {
    SsrfCheckResult result;
    result.redirect_count = current_redirect_count + 1;
    
    // ✅ 对齐 OpenClaw: 重定向链检查
    
    // 1. 检查重定向次数限制
    if (config_.enable_redirect_check &&
        result.redirect_count > config_.max_redirects) {
        result.reason = "Too many redirects: " +
                       std::to_string(result.redirect_count) +
                       " (max: " + std::to_string(config_.max_redirects) + ")";
        log()->warn("SSRF blocked: redirect chain too long - {} redirects from {}",
                   result.redirect_count, original_url);
        return result;
    }
    
    // 2. 对重定向目标执行完整的 SSRF 检查
    auto redirect_check = check_url(redirect_url);
    if (!redirect_check.allowed) {
        result.reason = "Redirect target blocked: " + redirect_check.reason;
        log()->warn("SSRF blocked: redirect to blocked URL - {} -> {}",
                   original_url, redirect_url);
        return result;
    }
    
    result.allowed = true;
    result.resolved_ip = redirect_check.resolved_ip;
    log()->info("Redirect check passed: {} -> {} (redirect #{})",
               original_url, redirect_url, result.redirect_count);
    return result;
}

// 工厂函数和全局单例

std::shared_ptr<SsrfGuard> create_default_ssrf_guard() {
    SsrfGuardConfig config;
    // 使用默认配置 (严格模式)
    config.enable_dns_rebinding_check = true;
    config.enable_redirect_check = true;
    config.enable_ipv6_check = true;
    config.max_redirects = 5;
    
    return std::make_shared<SsrfGuard>(config);
}

SsrfGuard& get_global_ssrf_guard() {
    static SsrfGuard guard;
    return guard;
}

} // namespace security
} // namespace hunterclaw
