#pragma once

// ============================================================================
// remote_a2a/card_resolver.h
// AgentCard 获取 —— 拉取 / 消杀 / SSRF 校验 / 缓存 / TTL 刷新。
//
// ── 完整的四步流水线（顺序不可换）─────────────────────────────────────────
//   ① fetch      HTTP GET .well-known/agent-card.json（含旧路径回退）
//   ② parse      畸形字段一律安全默认
//   ③ validate   ★ SSRF —— 必须在 sanitize 之前，因为消杀不动 URL，
//                 而校验失败的卡【一个字段都不该进内存缓存】
//   ④ sanitize   ★ prompt 注入防护 —— 必须在进缓存之前，
//                 保证缓存里存的就是可以直接进 prompt 的安全版本
//
// ⚠ 若把 ④ 放到「取用时」而不是「入缓存时」，会有两个后果：
//   · 每次组装 prompt 都要重跑消杀（热路径上的无谓开销）
//   · 更糟：任何一个忘记调 sanitize 的取用点就是一个注入漏洞
//   所以本类【只对外暴露消杀后的卡】，原始卡不出这个类。
//
// ── 与 ADK 的差异 ──────────────────────────────────────────────────────────
// │                │ ADK                          │ 本实现                  │
// │ 缓存           │ self._agent_card 无锁无 TTL   │ mutex + TTL             │
// │ description    │ 直接赋值 (:688-689)           │ 消杀后赋值              │
// │ 刷新           │ 无（进程内永不过期）           │ TTL 到期惰性刷新        │
// │ 刷新失败       │ —                             │ ★ 保留旧卡（不置空）     │
// │ 路径回退       │ 单一常量 (:46-49)             │ 新→旧两个路径依次尝试    │
//
// ★「刷新失败保留旧卡」是刻意的：远端重启期间拉卡会失败，
//   若此时把缓存置空，正在进行的会话会突然失去这个 agent（工具清单抖动）。
//   旧卡至少 URL 还对，最坏情况是 skill 描述过期 —— 远小于工具消失的代价。
// ============================================================================

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "hunterclaw/core/types.h"
#include "hunterclaw/infra/http_client.h"
#include "hunterclaw/remote_a2a/agent_card.h"
#include "hunterclaw/remote_a2a/card_sanitizer.h"
#include "hunterclaw/remote_a2a/credential_resolver.h"

namespace hunterclaw {
namespace remote_a2a {

// ── well-known 路径 ─────────────────────────────────────────────────────────
// A2A 0.3+ 用 agent-card.json；0.2 及 a2a-sdk 旧版用 agent.json。
// ADK 只硬编码一个（remote_a2a_agent.py:46-49 的 try/except ImportError
// 回退到 "/.well-known/agent.json"），我们两个都试。
constexpr const char* WELL_KNOWN_CARD_PATH = "/.well-known/agent-card.json";
constexpr const char* WELL_KNOWN_CARD_PATH_LEGACY = "/.well-known/agent.json";

struct CardResolverConfig {
    /// 卡来源：http(s):// URL、本地文件路径，或 base URL（自动补 well-known）
    std::string source;

    /// 缓存 TTL。0 表示永不过期（対标 ADK 行为，不推荐）
    int64_t cache_ttl_ms = 15 * 60 * 1000;  // 15 min

    int timeout_ms = 10000;

    /// SSRF 第⑤条（DNS 解析后 IP 网段）。单测置 false 以免真实 DNS。
    bool resolve_dns = true;

    /// 允许 http（仅 loopback 场景；非 loopback 的 http 仍由 validator 拒）
    bool allow_insecure_loopback = true;

    SanitizeLimits sanitize;

    /// 拉卡是否带凭据。★ 带凭据拉到的是 authenticated extended card，
    /// 其作用域是单个已认证 session（A2A 规范 §3.1.1.1），
    /// 因此【不能进共享缓存】—— 见 resolve_per_invocation()。
    AuthSpec auth;
};

/// 一次解析的结果
struct CardResolution {
    /// ★ 已消杀，可直接进 prompt
    AgentCard card;
    /// 实际拉卡成功的 URL（validate 用的就是这个 origin）
    std::string source_url;
    /// 实际选中的 RPC URL
    std::string rpc_url;
    /// 是否命中缓存
    bool from_cache = false;
    /// 卡与本地鉴权配置的兼容性警告（不阻断，仅提示）
    std::vector<std::string> auth_warnings;
};

class CardResolver {
public:
    static std::shared_ptr<CardResolver> create(const CardResolverConfig& cfg,
                                                infra::HttpClient::Ptr http = nullptr);

    explicit CardResolver(const CardResolverConfig& cfg, infra::HttpClient::Ptr http);

    /// 获取卡。命中未过期缓存则直接返回；否则拉取 → 校验 → 消杀 → 入缓存。
    ///
    /// ★ 刷新失败且已有旧卡 → 返回旧卡 + from_cache=true，【不返回 Err】。
    ///   理由见文件头。仅在「从来没成功过」时返回 Err。
    core::Result<CardResolution> resolve();

    /// 强制刷新（忽略 TTL）。失败同样保留旧卡。
    core::Result<CardResolution> refresh();

    /// per-invocation 卡（带凭据拉取）。★ 结果【绝不】写入共享缓存。
    /// 対标 ADK 的 per_invocation_card 分支（remote_a2a_agent.py:974-987
    /// 注释 "Build a per-invocation client; never cached on shared state."）
    core::Result<CardResolution> resolve_per_invocation();

    /// 缓存是否已过期（含"从未拉取"）
    bool is_stale() const;

    /// 只读快照。★ 无卡时返回空 optional，不触发网络。
    std::optional<AgentCard> cached_card() const;

    const CardResolverConfig& config() const { return cfg_; }

private:
    /// 完整流水线：fetch → parse → validate → sanitize
    core::Result<CardResolution> fetch_and_process(bool with_credential);

    /// 从 URL 拉取原始 JSON。返回 {json, 实际成功的 url}。
    /// 路径回退：source 直接命中 → 新 well-known → 旧 well-known。
    core::Result<std::pair<nlohmann::json, std::string>> fetch_json(
        bool with_credential);

    /// 单次 GET
    core::Result<nlohmann::json> http_get_json(const std::string& url,
                                               bool with_credential);

    /// 本地文件读取（source 非 http/https 时）
    core::Result<nlohmann::json> read_local_json() const;

    CardResolverConfig cfg_;
    infra::HttpClient::Ptr http_;
    std::shared_ptr<CredentialResolver> cred_;

    // ★ ADK 的 self._agent_card 是无锁裸字段；多个会话并发首次解析会重复拉卡
    //   甚至撕裂读。这里加锁。
    mutable std::mutex mu_;
    std::optional<AgentCard> cached_;
    std::string cached_source_url_;
    std::string cached_rpc_url_;
    std::vector<std::string> cached_warnings_;
    std::chrono::steady_clock::time_point cached_at_{};
    bool has_cache_ = false;
};

/// 由 source 推导候选拉卡 URL 列表（保序、去重）。
///
/// 规则：
///   · source 以 "/.well-known/..." 结尾 → 只用它（调用方已明确指定）
///   · source 是 base URL（无 path 或 path == "/"）→ 新路径、旧路径
///   · source 带其他 path → 先原样试（可能是自定义卡路径），
///     再拼 origin + 两个 well-known
///
/// ⚠ 拼接时必须用 origin 而不是字符串追加，否则
///   "https://h/a2a/" + "/.well-known/x" 会得到双斜杠路径。
std::vector<std::string> candidate_card_urls(const std::string& source);

}  // namespace remote_a2a
}  // namespace hunterclaw
