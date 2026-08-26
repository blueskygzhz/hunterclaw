// ============================================================================
// remote_a2a/card_resolver.cc
// AgentCard 拉取 / 校验 / 消杀 / 缓存。
// ============================================================================

#include "hunterclaw/remote_a2a/card_resolver.h"

#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "hunterclaw/remote_a2a/card_validator.h"
#include "hunterclaw/remote_a2a/url_safety.h"

namespace hunterclaw {
namespace remote_a2a {

namespace {

bool is_http_source(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/// 卡体积上限。★ 远端可控 —— 不设上限就是「一次 GET 吃掉几个 G 内存」。
constexpr size_t kMaxCardBytes = 512 * 1024;

}  // namespace

std::vector<std::string> candidate_card_urls(const std::string& source) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& u) {
        if (u.empty()) return;
        if (seen.insert(u).second) out.push_back(u);
    };

    if (!is_http_source(source)) return out;  // 本地文件走另一条路

    // 调用方已明确指定 well-known 路径 → 不再猜
    if (ends_with(source, WELL_KNOWN_CARD_PATH) ||
        ends_with(source, WELL_KNOWN_CARD_PATH_LEGACY)) {
        add(source);
        return out;
    }

    auto p = parse_url(source);
    if (!p) {
        add(source);  // 解析不了也试一次，让 HTTP 层去报错（错误信息更具体）
        return out;
    }

    const bool bare = p->path.empty() || p->path == "/";
    if (!bare) {
        // 带自定义 path：可能就是卡地址，先原样试
        add(source);
    }
    // ★ 用 origin 拼接而非字符串追加 —— 避免 "https://h/a2a/" + "/.well-known/x"
    //   产生 "https://h/a2a//.well-known/x"
    const std::string origin = p->origin();
    add(origin + WELL_KNOWN_CARD_PATH);
    add(origin + WELL_KNOWN_CARD_PATH_LEGACY);  // 0.2 / a2a-sdk 旧版
    return out;
}

std::shared_ptr<CardResolver> CardResolver::create(const CardResolverConfig& cfg,
                                                   infra::HttpClient::Ptr http) {
    return std::make_shared<CardResolver>(cfg, std::move(http));
}

CardResolver::CardResolver(const CardResolverConfig& cfg, infra::HttpClient::Ptr http)
    : cfg_(cfg),
      http_(http ? std::move(http) : infra::HttpClient::create()),
      cred_(CredentialResolver::create()) {}

bool CardResolver::is_stale() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_cache_) return true;
    if (cfg_.cache_ttl_ms <= 0) return false;  // 0 = 永不过期
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - cached_at_)
                         .count();
    return age >= cfg_.cache_ttl_ms;
}

std::optional<AgentCard> CardResolver::cached_card() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cached_;
}

core::Result<CardResolution> CardResolver::resolve() {
    // 快路径：缓存有效
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (has_cache_ && cfg_.cache_ttl_ms > 0) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - cached_at_)
                                 .count();
            if (age < cfg_.cache_ttl_ms) {
                CardResolution r;
                r.card = *cached_;
                r.source_url = cached_source_url_;
                r.rpc_url = cached_rpc_url_;
                r.auth_warnings = cached_warnings_;
                r.from_cache = true;
                return core::Result<CardResolution>::Ok(std::move(r));
            }
        } else if (has_cache_) {
            CardResolution r;
            r.card = *cached_;
            r.source_url = cached_source_url_;
            r.rpc_url = cached_rpc_url_;
            r.auth_warnings = cached_warnings_;
            r.from_cache = true;
            return core::Result<CardResolution>::Ok(std::move(r));
        }
    }
    return refresh();
}

core::Result<CardResolution> CardResolver::refresh() {
    auto r = fetch_and_process(cfg_.auth.authenticate_card_request);
    if (r.is_ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        cached_ = r.get().card;
        cached_source_url_ = r.get().source_url;
        cached_rpc_url_ = r.get().rpc_url;
        cached_warnings_ = r.get().auth_warnings;
        cached_at_ = std::chrono::steady_clock::now();
        has_cache_ = true;
        return r;
    }

    // ★ 刷新失败 → 保留旧卡。
    //   远端重启期间拉卡必然失败；此时置空缓存会让工具清单突然少一项，
    //   正在进行的会话会看到工具消失。旧卡的 URL 仍然有效，
    //   最坏只是 skill 描述过期。
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (has_cache_) {
            CardResolution stale;
            stale.card = *cached_;
            stale.source_url = cached_source_url_;
            stale.rpc_url = cached_rpc_url_;
            stale.auth_warnings = cached_warnings_;
            stale.auth_warnings.push_back("卡刷新失败，继续使用缓存版本: " + r.error);
            stale.from_cache = true;
            return core::Result<CardResolution>::Ok(std::move(stale));
        }
    }
    return r;  // 从未成功过 → 真失败
}

core::Result<CardResolution> CardResolver::resolve_per_invocation() {
    // ★ 绝不写共享缓存：authenticated extended card 的作用域是单个已认证
    //   session（A2A 规范 §3.1.1.1）。缓存它等于把 A 用户的卡给 B 用户用。
    return fetch_and_process(true);
}

core::Result<CardResolution> CardResolver::fetch_and_process(bool with_credential) {
    // ── ① fetch ──────────────────────────────────────────────────────────
    nlohmann::json raw;
    std::string source_url;

    if (is_http_source(cfg_.source)) {
        auto f = fetch_json(with_credential);
        if (f.is_err()) return core::Result<CardResolution>::Err(f.error);
        raw = std::move(f.get().first);
        source_url = std::move(f.get().second);
    } else {
        auto f = read_local_json();
        if (f.is_err()) return core::Result<CardResolution>::Err(f.error);
        raw = std::move(f.get());
        source_url = "";  // 非网络来源 → validator 第①条会放行
    }

    // ── ② parse ──────────────────────────────────────────────────────────
    auto parsed = parse_agent_card(raw);
    if (parsed.is_err()) {
        return core::Result<CardResolution>::Err("AgentCard 解析失败: " + parsed.error);
    }
    const AgentCard& card_raw = parsed.get();

    // ── ③ validate（★ 必须在入缓存前）────────────────────────────────────
    auto v = validate_card_rpc_targets(card_raw, source_url, cfg_.resolve_dns);
    if (v.is_err()) {
        // ⚠ 校验失败的卡一个字段都不进缓存 —— 它可能是攻击者构造的
        return core::Result<CardResolution>::Err("AgentCard SSRF 校验失败: " + v.error);
    }

    const std::string rpc_url = select_rpc_url(card_raw);
    if (rpc_url.empty()) {
        return core::Result<CardResolution>::Err("AgentCard 未提供可用 RPC URL");
    }

    // ── ④ sanitize（★ 只有消杀后的卡才出这个函数）───────────────────────
    CardResolution out;
    out.card = sanitize_agent_card(card_raw, cfg_.sanitize);
    // ⚠ 消杀不动 URL（URL 由 validator 负责），但要确认 sanitize 没有意外改动
    out.card.url = card_raw.url;
    out.card.interfaces = card_raw.interfaces;
    out.source_url = source_url;
    out.rpc_url = rpc_url;
    out.from_cache = false;
    out.auth_warnings = check_auth_consistency(card_raw.security_schemes, cfg_.auth);
    return core::Result<CardResolution>::Ok(std::move(out));
}

core::Result<std::pair<nlohmann::json, std::string>> CardResolver::fetch_json(
    bool with_credential) {
    const std::vector<std::string> urls = candidate_card_urls(cfg_.source);
    if (urls.empty()) {
        return core::Result<std::pair<nlohmann::json, std::string>>::Err(
            "无法从 source 推导拉卡 URL: " + cfg_.source);
    }

    std::string last_err;
    for (const std::string& u : urls) {
        auto r = http_get_json(u, with_credential);
        if (r.is_ok()) {
            return core::Result<std::pair<nlohmann::json, std::string>>::Ok(
                {std::move(r.get()), u});
        }
        last_err = r.error;
        // ⚠ 不因单个路径 404 就放弃 —— 新旧 well-known 路径都要试。
        //   但 401/403 例外：路径对了只是没凭据，继续试别的路径没有意义
        //   而且会把同一个 401 打三遍（触发对端限流/告警）。
        if (last_err.find("HTTP 401") != std::string::npos ||
            last_err.find("HTTP 403") != std::string::npos) {
            break;
        }
    }
    return core::Result<std::pair<nlohmann::json, std::string>>::Err(
        "拉取 AgentCard 失败: " + last_err);
}

core::Result<nlohmann::json> CardResolver::http_get_json(const std::string& url,
                                                         bool with_credential) {
    // ★ 拉卡地址本身也要过 SSRF —— 这是用户配置的，但配置可能来自不可信渠道
    //   （比如通过 gateway 接口动态注册的远端）。
    {
        auto p = parse_url(url);
        if (!p) {
            return core::Result<nlohmann::json>::Err("非法拉卡 URL: " + url);
        }
        const bool loopback = is_loopback_host(p->host);
        if (p->scheme != "https" && !(loopback && cfg_.allow_insecure_loopback)) {
            return core::Result<nlohmann::json>::Err(
                "拉卡必须使用 https（仅 loopback 允许 http）: " + url);
        }
        if (!loopback && cfg_.resolve_dns) {
            if (is_metadata_hostname(p->host)) {
                return core::Result<nlohmann::json>::Err("拉卡 URL 指向云 metadata 主机");
            }
            auto ip = resolve_first_ip(p->host);
            if (!ip) {
                return core::Result<nlohmann::json>::Err("无法解析拉卡主机: " + p->host);
            }
            if (is_private_or_reserved(*ip)) {
                return core::Result<nlohmann::json>::Err(
                    "拉卡 URL 解析到私有/保留网段 " + *ip);
            }
        }
    }

    infra::HttpRequestOptions opt;
    opt.timeout_ms = cfg_.timeout_ms;
    opt.headers["Accept"] = "application/json";

    if (with_credential) {
        auto c = cred_->resolve(cfg_.auth);
        if (c.is_err()) {
            return core::Result<nlohmann::json>::Err("拉卡凭据解析失败: " + c.error);
        }
        // ★ to_headers() 对空 token 【不产出】 header —— 不会发出 "Bearer "
        for (const auto& [k, v] : c.get().to_headers()) opt.headers[k] = v;
    }

    auto resp = http_->get(url, opt);
    if (resp.is_err()) {
        return core::Result<nlohmann::json>::Err(resp.error);
    }
    const auto& r = resp.get();
    if (!r.ok) {
        return core::Result<nlohmann::json>::Err(
            "HTTP " + std::to_string(r.status_code) + " @ " + url);
    }
    if (r.body.size() > kMaxCardBytes) {
        return core::Result<nlohmann::json>::Err(
            "AgentCard 体积超限（" + std::to_string(r.body.size()) + " > " +
            std::to_string(kMaxCardBytes) + " 字节）");
    }
    try {
        return core::Result<nlohmann::json>::Ok(nlohmann::json::parse(r.body));
    } catch (const std::exception& e) {
        return core::Result<nlohmann::json>::Err(std::string("AgentCard 非法 JSON: ") +
                                                e.what());
    }
}

core::Result<nlohmann::json> CardResolver::read_local_json() const {
    std::ifstream f(cfg_.source, std::ios::binary);
    if (!f) {
        return core::Result<nlohmann::json>::Err("无法打开卡文件: " + cfg_.source);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string body = ss.str();
    if (body.size() > kMaxCardBytes) {
        return core::Result<nlohmann::json>::Err("卡文件体积超限");
    }
    try {
        return core::Result<nlohmann::json>::Ok(nlohmann::json::parse(body));
    } catch (const std::exception& e) {
        return core::Result<nlohmann::json>::Err(std::string("卡文件非法 JSON: ") +
                                                e.what());
    }
}

}  // namespace remote_a2a
}  // namespace hunterclaw
