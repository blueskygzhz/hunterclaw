// ============================================================================
// remote_a2a/config.cc
// remoteA2a 配置解析。
// ============================================================================

#include "hunterclaw/remote_a2a/config.h"

#include <algorithm>
#include <set>

#include "hunterclaw/remote_a2a/url_safety.h"

namespace hunterclaw {
namespace remote_a2a {

namespace {

std::string jstr(const nlohmann::json& j, const char* camel, const char* snake = nullptr) {
    if (!j.is_object()) return {};
    auto it = j.find(camel);
    if (it != j.end() && it->is_string()) return it->get<std::string>();
    if (snake) {
        it = j.find(snake);
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    }
    return {};
}

bool jbool(const nlohmann::json& j, const char* camel, bool def,
           const char* snake = nullptr) {
    if (!j.is_object()) return def;
    auto it = j.find(camel);
    if (it != j.end() && it->is_boolean()) return it->get<bool>();
    if (snake) {
        it = j.find(snake);
        if (it != j.end() && it->is_boolean()) return it->get<bool>();
    }
    return def;
}

int64_t jint(const nlohmann::json& j, const char* camel, int64_t def,
             const char* snake = nullptr) {
    if (!j.is_object()) return def;
    auto it = j.find(camel);
    if (it != j.end() && it->is_number()) return it->get<int64_t>();
    if (snake) {
        it = j.find(snake);
        if (it != j.end() && it->is_number()) return it->get<int64_t>();
    }
    return def;
}

/// camelCase → snake_case（配置文件用 camel，parse_auth_spec 只认 snake）。
///
/// ⚠ 这个转换必须存在：配置文件里写 tokenEnv 是 HunterClaw 全局风格，
///   而 auth.h 的 parse_auth_spec 按 A2A/OpenAPI 习惯用 token_env。
///   不转换的话用户写 tokenEnv 会得到「必须指定 token_env」这种自相矛盾的报错。
std::string camel_to_snake(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') {
            if (i > 0) out.push_back('_');
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

/// 把 auth 节点的键名同时提供 camel 与 snake 两种写法，交给 parse_auth_spec。
/// ★ 保留原键（不删）—— 明文凭据检测（auth.cc:83-94）依赖原键名命中，
///   若只保留 snake 版本，用户写 apiKey: "xxx" 会绕过检测。
nlohmann::json normalize_auth_keys(const nlohmann::json& in) {
    if (!in.is_object()) return in;
    nlohmann::json out = in;
    for (auto it = in.begin(); it != in.end(); ++it) {
        const std::string snake = camel_to_snake(it.key());
        if (snake != it.key() && !out.contains(snake)) {
            out[snake] = it.value();
        }
    }
    // "kind" 与 "type" 互为别名（配置文档里用 kind 更自然）
    if (!out.contains("type") && out.contains("kind")) out["type"] = out["kind"];
    return out;
}

/// clamp 到正数区间；非法值回落默认
int64_t clamp_positive(int64_t v, int64_t def, int64_t lo, int64_t hi) {
    if (v <= 0) return def;
    return std::max(lo, std::min(v, hi));
}

}  // namespace

RemoteAgentConfig default_remote_agent_config() {
    RemoteAgentConfig c;
    c.enabled = true;
    return c;
}

core::Result<RemoteA2aConfig> parse_remote_a2a_config(const nlohmann::json& node) {
    RemoteA2aConfig cfg;

    // 缺失或非 object → 未启用（★ 不是错误：绝大多数部署不用 remote A2A）
    if (!node.is_object()) {
        cfg.enabled = false;
        return core::Result<RemoteA2aConfig>::Ok(std::move(cfg));
    }

    cfg.enabled = jbool(node, "enabled", false);
    cfg.callback_base_url = jstr(node, "callbackBaseUrl", "callback_base_url");

    if (!cfg.enabled) {
        return core::Result<RemoteA2aConfig>::Ok(std::move(cfg));
    }

    // ── 全局 store 配置 ─────────────────────────────────────────────────
    {
        auto it = node.find("store");
        if (it != node.end() && it->is_object()) {
            const int64_t me = jint(*it, "maxEntries", 256, "max_entries");
            cfg.store.max_entries = static_cast<size_t>(clamp_positive(me, 256, 8, 4096));
            cfg.store.terminal_ttl_ms =
                clamp_positive(jint(*it, "terminalTtlMs", 1800000, "terminal_ttl_ms"),
                              1800000, 60000, 24LL * 3600 * 1000);
            cfg.store.running_max_ms =
                clamp_positive(jint(*it, "runningMaxMs", 6LL * 3600 * 1000,
                                   "running_max_ms"),
                              6LL * 3600 * 1000, 60000, 72LL * 3600 * 1000);
        }
    }

    // ── 回调端点配置 ────────────────────────────────────────────────────
    {
        auto it = node.find("callback");
        if (it != node.end() && it->is_object()) {
            cfg.callback.max_failures_per_window = static_cast<int>(clamp_positive(
                jint(*it, "maxFailuresPerWindow", 10, "max_failures_per_window"), 10, 1,
                1000));
            cfg.callback.failure_window_ms = clamp_positive(
                jint(*it, "failureWindowMs", 60000, "failure_window_ms"), 60000, 1000,
                3600000);
            const int64_t mb = jint(*it, "maxBodyBytes", 65536, "max_body_bytes");
            cfg.callback.max_body_bytes =
                static_cast<size_t>(clamp_positive(mb, 65536, 512, 4 * 1024 * 1024));
            cfg.callback.echo_handle = jbool(*it, "echoHandle", false, "echo_handle");
        }
    }

    // ── defaults ────────────────────────────────────────────────────────
    ExecutionConfig def_exec;
    int64_t def_card_ttl = 15 * 60 * 1000;
    int def_timeout_ms = 30000;
    {
        auto it = node.find("defaults");
        if (it != node.end() && it->is_object()) {
            const nlohmann::json& d = *it;
            def_exec.yield_ms = jint(d, "yieldMs", def_exec.yield_ms, "yield_ms");
            def_exec.max_task_ms = jint(d, "maxTaskMs", def_exec.max_task_ms, "max_task_ms");
            def_exec.idle_timeout_ms =
                jint(d, "idleTimeoutMs", def_exec.idle_timeout_ms, "idle_timeout_ms");
            def_card_ttl = jint(d, "cardTtlMs", def_card_ttl, "card_ttl_ms");
            def_timeout_ms = static_cast<int>(
                jint(d, "timeoutMs", def_timeout_ms, "timeout_ms"));

            auto pit = d.find("probe");
            if (pit != d.end() && pit->is_object()) {
                def_exec.probe.base_interval_ms = clamp_positive(
                    jint(*pit, "baseIntervalMs", 5000, "base_interval_ms"), 5000, 500,
                    600000);
                def_exec.probe.max_interval_ms = clamp_positive(
                    jint(*pit, "maxIntervalMs", 60000, "max_interval_ms"), 60000, 1000,
                    600000);
                const int64_t mnp = jint(*pit, "maxNoProgress", 6, "max_no_progress");
                // ★ 下限 2：probe_policy.h:26 已注明 —— 设 1 会因单次抖动误判 Zombie
                def_exec.probe.max_no_progress =
                    static_cast<int>(clamp_positive(mnp, 6, 2, 100));
                // base 不得大于 max（否则 interval_for 恒返回 max，退避失效）
                if (def_exec.probe.base_interval_ms > def_exec.probe.max_interval_ms) {
                    def_exec.probe.max_interval_ms = def_exec.probe.base_interval_ms;
                    cfg.warnings.push_back(
                        "probe.baseIntervalMs 大于 maxIntervalMs，已把 max 提到与 base 相同");
                }
            }
        }
    }

    // ── agents ──────────────────────────────────────────────────────────
    auto ait = node.find("agents");
    if (ait == node.end() || !ait->is_array() || ait->empty()) {
        return core::Result<RemoteA2aConfig>::Err(
            "remoteA2a.enabled=true 但未配置任何 agents");
    }

    std::set<std::string> seen_names;
    bool any_callback = false;

    for (const auto& a : *ait) {
        if (!a.is_object()) {
            cfg.warnings.push_back("agents 中存在非对象条目，已跳过");
            continue;
        }

        RemoteAgentConfig rc;
        rc.name = jstr(a, "name");
        if (rc.name.empty()) {
            cfg.warnings.push_back("agent 缺少 name，已跳过");
            continue;
        }
        // ★ 名字必须唯一：工具名由它派生，重名会让后者覆盖前者（静默丢工具）
        if (!seen_names.insert(rc.name).second) {
            cfg.warnings.push_back("agent 名重复，已跳过: " + rc.name);
            continue;
        }

        rc.enabled = jbool(a, "enabled", true);
        if (!rc.enabled) continue;

        rc.card_source = jstr(a, "card", "card_source");
        if (rc.card_source.empty()) {
            cfg.warnings.push_back("agent '" + rc.name + "' 缺少 card，已跳过");
            continue;
        }

        // 鉴权（★ 单个 agent 的凭据配置错误只跳过该项，不拖垮整体）
        AuthSpec auth;
        {
            auto authit = a.find("auth");
            nlohmann::json auth_node =
                (authit != a.end()) ? normalize_auth_keys(*authit) : nlohmann::json();
            auto pa = parse_auth_spec(auth_node);
            if (pa.is_err()) {
                cfg.warnings.push_back("agent '" + rc.name + "' 鉴权配置无效，已跳过: " +
                                      pa.error);
                continue;
            }
            auth = pa.get();
        }

        // card resolver
        rc.card.source = rc.card_source;
        rc.card.cache_ttl_ms =
            std::max<int64_t>(0, jint(a, "cardTtlMs", def_card_ttl, "card_ttl_ms"));
        rc.card.timeout_ms =
            static_cast<int>(clamp_positive(jint(a, "timeoutMs", def_timeout_ms,
                                                "timeout_ms"),
                                           def_timeout_ms, 1000, 120000));
        rc.card.auth = auth;
        rc.card.resolve_dns = jbool(a, "resolveDns", true, "resolve_dns");
        rc.card.allow_insecure_loopback =
            jbool(a, "allowInsecureLoopback", true, "allow_insecure_loopback");
        {
            auto sit = a.find("sanitize");
            if (sit != a.end() && sit->is_object()) {
                const int64_t ms = jint(*sit, "maxSkills", 8, "max_skills");
                rc.card.sanitize.max_skills =
                    static_cast<size_t>(clamp_positive(ms, 8, 1, 64));
                const int64_t md = jint(*sit, "maxDescription", 500, "max_description");
                rc.card.sanitize.max_description =
                    static_cast<size_t>(clamp_positive(md, 500, 40, 4000));
            }
        }

        // client
        rc.client.name = rc.name;
        rc.client.auth = auth;
        rc.client.rpc_timeout_ms = rc.card.timeout_ms;
        rc.client.stream_idle_timeout_ms = static_cast<int>(
            clamp_positive(jint(a, "streamIdleTimeoutMs", def_exec.idle_timeout_ms,
                               "stream_idle_timeout_ms"),
                          def_exec.idle_timeout_ms, 1000, 3600000));
        rc.client.stream_total_timeout_ms = static_cast<int>(
            std::max<int64_t>(0, jint(a, "streamTotalTimeoutMs", 0,
                                     "stream_total_timeout_ms")));
        rc.client.force_streaming = jbool(a, "forceStreaming", false, "force_streaming");
        rc.client.force_no_streaming =
            jbool(a, "forceNoStreaming", false, "force_no_streaming");
        if (rc.client.force_streaming && rc.client.force_no_streaming) {
            // ⚠ 两个都开时以禁用为准（安全默认：宁可降级也不要行为不确定）
            rc.client.force_streaming = false;
            cfg.warnings.push_back("agent '" + rc.name +
                                  "' 同时设置 forceStreaming 与 forceNoStreaming，"
                                  "以 forceNoStreaming 为准");
        }

        // exec
        rc.exec = def_exec;
        rc.exec.yield_ms = jint(a, "yieldMs", def_exec.yield_ms, "yield_ms");
        rc.exec.max_task_ms = clamp_positive(
            jint(a, "maxTaskMs", def_exec.max_task_ms, "max_task_ms"),
            def_exec.max_task_ms, 1000, 24LL * 3600 * 1000);
        rc.exec.idle_timeout_ms = clamp_positive(
            jint(a, "idleTimeoutMs", def_exec.idle_timeout_ms, "idle_timeout_ms"),
            def_exec.idle_timeout_ms, 1000, 3600000);
        rc.exec.background = jbool(a, "background", false);
        rc.exec.background_follow_up =
            jbool(a, "backgroundFollowUp", true, "background_follow_up");
        rc.exec.enable_callback = jbool(a, "enableCallback", false, "enable_callback");
        rc.exec.callback_base_url = cfg.callback_base_url;
        if (rc.exec.enable_callback) any_callback = true;

        // ★ 策略字段只从这里读，绝不从卡读（agent_card.h:10 第三条纪律）
        rc.policy.requires_approval =
            jbool(a, "requiresApproval", false, "requires_approval");
        rc.policy.owner_only = jbool(a, "ownerOnly", false, "owner_only");
        rc.policy.include_skills_in_description =
            jbool(a, "includeSkills", true, "include_skills");
        rc.policy.max_description_chars = static_cast<size_t>(clamp_positive(
            jint(a, "maxDescriptionChars", 900, "max_description_chars"), 900, 80, 4000));

        cfg.agents.push_back(std::move(rc));
    }

    if (cfg.agents.empty()) {
        return core::Result<RemoteA2aConfig>::Err(
            "remoteA2a.enabled=true 但没有任何可用的 agent 配置");
    }

    // ── 回调基址校验 ────────────────────────────────────────────────────
    if (any_callback) {
        if (cfg.callback_base_url.empty()) {
            return core::Result<RemoteA2aConfig>::Err(
                "启用了 enableCallback 但未配置 remoteA2a.callbackBaseUrl");
        }
        auto p = parse_url(cfg.callback_base_url);
        if (!p) {
            return core::Result<RemoteA2aConfig>::Err(
                "callbackBaseUrl 非法（仅支持 http/https）: " + cfg.callback_base_url);
        }
        // ★ 回调地址必须 https（loopback 除外）——
        //   回调 body 里带任务结果，明文传输等于把结果公开；
        //   且 Bearer token 会随请求头明文发出。
        if (p->scheme != "https" && !is_loopback_host(p->host)) {
            return core::Result<RemoteA2aConfig>::Err(
                "callbackBaseUrl 必须使用 https（仅 loopback 允许 http）: " +
                cfg.callback_base_url);
        }
    }

    return core::Result<RemoteA2aConfig>::Ok(std::move(cfg));
}

}  // namespace remote_a2a
}  // namespace hunterclaw
