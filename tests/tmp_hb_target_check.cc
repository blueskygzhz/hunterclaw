// 临时验证：定向唤醒的 session_key → agent_id 回退
#include "hunterclaw/routing/resolver.h"
#include <cstdio>
#include <string>

using namespace hunterclaw::routing;

int main() {
    int fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& got,
                     const char* want) {
        std::printf("%-56s %s", name, ok ? "PASS\n" : "FAIL");
        if (!ok) {
            std::printf("  got=\"%s\" want=\"%s\"\n", got.c_str(), want);
            ++fail;
        }
    };

    // ACP 子会话 spawn 时父会话的典型 key
    {
        auto got = resolve_agent_id_from_session_key("agent:main:main");
        check(got == "main", "agent:main:main -> main", got, "main");
    }
    // 非 main agent
    {
        auto got = resolve_agent_id_from_session_key("agent:kiro:main");
        check(got == "kiro", "agent:kiro:main -> kiro", got, "kiro");
    }
    // ACP 子会话自身的 key
    {
        auto got = resolve_agent_id_from_session_key("agent:kiro:acp:abc-123");
        check(got == "kiro", "agent:kiro:acp:{uuid} -> kiro", got, "kiro");
    }
    // 频道会话
    {
        auto got = resolve_agent_id_from_session_key("agent:main:wx:direct:u123");
        check(got == "main", "agent:main:wx:direct:u123 -> main", got, "main");
    }
    // 非 agent 形状 → 回退默认（HunterClaw 既有行为，真源此处会 throw）
    {
        auto got = resolve_agent_id_from_session_key("wx:u123");
        check(!got.empty(), "non-agent key -> non-empty default", got, "<non-empty>");
        std::printf("    note: non-agent key resolved to \"%s\"\n", got.c_str());
    }

    std::printf("\n%s (%d failure%s)\n", fail == 0 ? "ALL PASS" : "FAILED", fail,
                fail == 1 ? "" : "s");
    return fail == 0 ? 0 : 1;
}
