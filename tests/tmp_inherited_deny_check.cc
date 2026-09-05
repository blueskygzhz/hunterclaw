// 临时验证：继承工具策略两道闸（严格对齐 inherited-tool-deny.ts）
#include "hunterclaw/agents/inherited_tool_deny.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace hunterclaw::agents;

static int g_fail = 0;
static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     %s\n", detail.c_str());
        ++g_fail;
    }
}
static std::string show(const std::optional<std::string>& v) {
    return v.has_value() ? *v : "(nullopt)";
}

int main() {
    // ── normalize_policy_tool_name：别名表 ────────────────────────────────
    check(normalize_policy_tool_name("bash") == "exec",
          "T01 alias bash -> exec", normalize_policy_tool_name("bash"));
    check(normalize_policy_tool_name("apply-patch") == "apply_patch",
          "T02 alias apply-patch -> apply_patch");
    check(normalize_policy_tool_name("cron") == "automations",
          "T03 alias cron -> automations");
    check(normalize_policy_tool_name("  ExEc  ") == "exec",
          "T04 trim + lowercase", normalize_policy_tool_name("  ExEc  "));
    check(normalize_policy_tool_name("   ").empty(), "T05 whitespace -> empty");
    check(normalize_policy_tool_name("read") == "read", "T06 non-alias passthrough");

    // ── 空列表不拦截 ──────────────────────────────────────────────────────
    check(!find_acp_unsupported_inherited_tool_deny({}).has_value(),
          "T07 empty denylist -> no block");
    check(!find_acp_unsupported_inherited_tool_allow({}).has_value(),
          "T08 empty allowlist -> no block (means unrestricted)");

    // ── deny 闸：命中 ACP 必需工具 ────────────────────────────────────────
    {
        auto r = find_acp_unsupported_inherited_tool_deny({"exec"});
        check(r.has_value() && *r == "exec", "T09 deny exec -> blocked", show(r));
    }
    {
        auto r = find_acp_unsupported_inherited_tool_deny({"read"});
        check(r.has_value() && *r == "read", "T10 deny read -> blocked", show(r));
    }
    {
        // 别名必须生效：deny "bash" 等价于 deny "exec"
        auto r = find_acp_unsupported_inherited_tool_deny({"bash"});
        check(r.has_value() && *r == "exec",
              "T11 deny bash blocks exec (alias applied)", show(r));
    }
    {
        // 不相关的 deny 不应拦截
        auto r = find_acp_unsupported_inherited_tool_deny({"web_search", "memory"});
        check(!r.has_value(), "T12 unrelated deny -> no block", show(r));
    }
    {
        // glob：deny "fs_*" 命中 fs_delete / fs_move / fs_write
        auto r = find_acp_unsupported_inherited_tool_deny({"fs_*"});
        check(r.has_value() && r->rfind("fs_", 0) == 0,
              "T13 glob deny fs_* blocks an fs_ tool", show(r));
    }
    {
        // 通配符全 deny
        auto r = find_acp_unsupported_inherited_tool_deny({"*"});
        check(r.has_value() && *r == "apply_patch",
              "T14 deny * blocks first required tool", show(r));
    }
    {
        // 返回顺序须与 ACP_UNSUPPORTED_INHERITED_TOOL_DENY 一致（apply_patch 在最前）
        auto r = find_acp_unsupported_inherited_tool_deny({"read", "apply_patch"});
        check(r.has_value() && *r == "apply_patch",
              "T15 returns first match in canonical order", show(r));
    }

    // ── allow 闸：缺少 ACP 必需工具 ───────────────────────────────────────
    {
        // 只允许 read ⇒ 缺 apply_patch（列表首项）
        auto r = find_acp_unsupported_inherited_tool_allow({"read"});
        check(r.has_value() && *r == "apply_patch",
          "T16 allowlist missing required tool -> blocked", show(r));
    }
    {
        // 6 个必需工具全给 ⇒ 放行
        auto r = find_acp_unsupported_inherited_tool_allow(
            {"apply_patch", "edit", "exec", "process", "read", "write"});
        check(!r.has_value(), "T17 all required tools allowed -> pass", show(r));
    }
    {
        // 通配符 allow ⇒ 放行
        auto r = find_acp_unsupported_inherited_tool_allow({"*"});
        check(!r.has_value(), "T18 allow * -> pass", show(r));
    }
    {
        // 真源特例：allow 含 write 应覆盖 apply_patch
        auto r = find_acp_unsupported_inherited_tool_allow(
            {"write", "edit", "exec", "process", "read"});
        check(!r.has_value(),
              "T19 allow write covers apply_patch (openclaw special case)", show(r));
    }
    {
        // 别名：allow "bash" 应满足 exec
        auto r = find_acp_unsupported_inherited_tool_allow(
            {"apply_patch", "edit", "bash", "process", "read", "write"});
        check(!r.has_value(), "T20 allow bash satisfies exec (alias)", show(r));
    }

    // ── is_tool_allowed_by_policy_name 语义 ───────────────────────────────
    {
        ToolPolicy p;  // 无 allow 无 deny
        check(is_tool_allowed_by_policy_name("exec", p),
              "T21 empty policy -> allowed");
    }
    {
        ToolPolicy p;
        p.deny = std::vector<std::string>{"exec"};
        p.allow = std::vector<std::string>{"exec"};
        check(!is_tool_allowed_by_policy_name("exec", p),
              "T22 deny wins over allow");
    }
    {
        ToolPolicy p;
        p.allow = std::vector<std::string>{"read"};
        check(!is_tool_allowed_by_policy_name("exec", p),
              "T23 non-empty allow excludes others");
        check(is_tool_allowed_by_policy_name("read", p),
              "T24 allow-listed tool passes");
    }
    {
        ToolPolicy p;
        p.allow = std::vector<std::string>{"write"};
        check(is_tool_allowed_by_policy_name("apply_patch", p),
              "T25 apply_patch covered by write allow");
        check(!is_tool_allowed_by_policy_name("read", p),
              "T26 write allow does not cover read");
    }
    {
        // glob 中的正则元字符必须被转义："a.c" 不应匹配 "abc"
        ToolPolicy p;
        p.deny = std::vector<std::string>{"a.c"};
        check(is_tool_allowed_by_policy_name("abc", p),
              "T27 regex metachar '.' escaped (a.c !~ abc)");
    }
    {
        ToolPolicy p;
        p.deny = std::vector<std::string>{"ex*"};
        check(!is_tool_allowed_by_policy_name("exec", p),
              "T28 prefix glob ex* matches exec");
    }

    // ── expand_tool_groups ────────────────────────────────────────────────
    {
        auto out = expand_tool_groups({"bash", "read", "read", "  "});
        // bash→exec、read 去重、空串丢弃
        bool ok = out.size() == 2 && out[0] == "exec" && out[1] == "read";
        std::string dump;
        for (const auto& s : out) dump += s + ",";
        check(ok, "T29 expand normalizes + dedups + drops empty", dump);
    }

    // ── 错误文案逐字对齐 ──────────────────────────────────────────────────
    check(format_acp_inherited_tool_deny_error("exec") ==
              "runtime=\"acp\" is unavailable because the requester denies exec. "
              "Use runtime=\"subagent\".",
          "T30 deny error text matches source exactly",
          format_acp_inherited_tool_deny_error("exec"));
    check(format_acp_inherited_tool_allow_error("read") ==
              "runtime=\"acp\" is unavailable because the requester does not allow read. "
              "Use runtime=\"subagent\".",
          "T31 allow error text matches source exactly",
          format_acp_inherited_tool_allow_error("read"));

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
