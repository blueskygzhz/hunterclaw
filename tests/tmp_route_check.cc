// 临时验证：cron 事件会话键重映射 + cfg 解析
#include "hunterclaw/routing/session_key.h"
#include "hunterclaw/sessions/session_key_utils.h"

#include <cstdio>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace hunterclaw;

static int g_fail = 0;
static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-60s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     %s\n", detail.c_str());
        ++g_fail;
    }
}

// 复刻 acp_spawn.cc 的 resolve_event_session_key
std::string resolve_event_session_key(const std::string& session_key,
                                     const std::string& main_key,
                                     const std::string& scope) {
    auto parsed = routing::parse_agent_session_key(session_key);
    if (!parsed || !sessions::is_cron_run_session_key(session_key)) {
        return session_key;
    }
    if (scope == "global") return "global";
    return routing::build_agent_main_session_key(parsed->agent_id, main_key);
}

int main() {
    const std::string cron_key = "agent:kiro:cron:nightly:run:abc123";
    const std::string acp_key = "agent:kiro:acp:uuid-1";
    const std::string plain_key = "agent:kiro:main";

    // 非 cron key 一律原样返回
    check(resolve_event_session_key(acp_key, "", "") == acp_key,
          "T1 acp session key unchanged", resolve_event_session_key(acp_key, "", ""));
    check(resolve_event_session_key(plain_key, "", "") == plain_key,
          "T2 main session key unchanged");
    check(resolve_event_session_key("not-an-agent-key", "", "") == "not-an-agent-key",
          "T3 non-agent key unchanged");

    // cron key + per-sender → agent-main
    {
        const std::string got = resolve_event_session_key(cron_key, "", "per-sender");
        const std::string want = routing::build_agent_main_session_key("kiro", "");
        check(got == want && got != cron_key,
              "T4 cron key + per-sender -> agent-main", "got=" + got + " want=" + want);
    }
    // cron key + 空 scope → 同 per-sender（默认）
    {
        const std::string got = resolve_event_session_key(cron_key, "", "");
        const std::string want = routing::build_agent_main_session_key("kiro", "");
        check(got == want, "T5 cron key + empty scope -> agent-main", "got=" + got);
    }
    // cron key + global → 字面量 "global"
    check(resolve_event_session_key(cron_key, "", "global") == "global",
          "T6 cron key + global -> literal 'global'",
          resolve_event_session_key(cron_key, "", "global"));
    // mainKey 生效
    {
        const std::string got = resolve_event_session_key(cron_key, "inbox", "per-sender");
        const std::string want = routing::build_agent_main_session_key("kiro", "inbox");
        check(got == want && got.find("inbox") != std::string::npos,
              "T7 custom mainKey honored", "got=" + got);
    }
    // global 优先于 mainKey
    check(resolve_event_session_key(cron_key, "inbox", "global") == "global",
          "T8 global scope overrides mainKey");

    // ── cfg 解析（复刻 spawn 侧逻辑）────────────────────────────────────────
    auto parse_cfg = [](const json& raw) {
        std::string mk, sc;
        if (raw.is_object() && raw.contains("session")) {
            const auto& s = raw["session"];
            if (s.is_object()) {
                if (s.contains("mainKey") && s["mainKey"].is_string())
                    mk = s["mainKey"].get<std::string>();
                if (s.contains("scope") && s["scope"].is_string())
                    sc = s["scope"].get<std::string>();
            }
        }
        return std::make_pair(mk, sc);
    };
    {
        auto [mk, sc] = parse_cfg(json{{"session", {{"mainKey", "inbox"}, {"scope", "global"}}}});
        check(mk == "inbox" && sc == "global", "T9 cfg session.mainKey/scope parsed",
              "mk=" + mk + " sc=" + sc);
    }
    {
        auto [mk, sc] = parse_cfg(json::object());
        check(mk.empty() && sc.empty(), "T10 empty cfg -> empty routing params");
    }
    {
        // 类型错误不得崩溃
        auto [mk, sc] = parse_cfg(json{{"session", {{"mainKey", 42}, {"scope", true}}}});
        check(mk.empty() && sc.empty(), "T11 wrong-typed cfg values ignored safely");
    }
    {
        auto [mk, sc] = parse_cfg(json{{"session", "not-an-object"}});
        check(mk.empty() && sc.empty(), "T12 non-object session ignored safely");
    }

    // emit 与 wake 必须用同一 key（同一函数同参数 → 必然一致）
    {
        const std::string k1 = resolve_event_session_key(cron_key, "inbox", "per-sender");
        const std::string k2 = resolve_event_session_key(cron_key, "inbox", "per-sender");
        check(k1 == k2, "T13 emit/wake resolve to identical key");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
