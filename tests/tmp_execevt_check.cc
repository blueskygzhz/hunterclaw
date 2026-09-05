// 临时验证：exec 事件识别 + prompt 嵌入（对齐 heartbeat-events-filter.ts）
#include "hunterclaw/infra/heartbeat_events_filter.h"
#include <cstdio>
#include <string>

using namespace hunterclaw::infra;

static int g_fail = 0;

static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     detail: %s\n", detail.c_str());
        ++g_fail;
    }
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // ── is_exec_completion_event ────────────────────────────────────────────
    check(is_exec_completion_event("exec finished: build ok"),
          "A1 'exec finished:' -> true");
    check(is_exec_completion_event("exec finished (abc)"),
          "A2 'exec finished (' -> true");
    check(is_exec_completion_event("exec completed (job-1, code 0)"),
          "A3 structured completed -> true");
    check(is_exec_completion_event("exec failed (job-2, code 1) :: boom"),
          "A4 structured failed with output -> true");
    check(is_exec_completion_event("exec failed (job-3, signal SIGKILL)"),
          "A5 structured signal -> true");
    // 旧实现的误判：正文里含该短语但不是完成事件
    check(!is_exec_completion_event("please run exec finished-check later"),
          "A6 mid-sentence 'exec finished-check' -> false (was true before fix)");
    check(!is_exec_completion_event("cron reminder: standup"),
          "A7 cron text -> false");

    // ── is_relayable_exec_completion_event ──────────────────────────────────
    check(is_relayable_exec_completion_event("exec completed (j, code 0) :: hello"),
          "B1 success WITH output -> relayable");
    check(!is_relayable_exec_completion_event("exec completed (j, code 0)"),
          "B2 success WITHOUT output -> not relayable");
    check(is_relayable_exec_completion_event("exec failed (j, code 2)"),
          "B3 failure without output -> relayable");

    // ── build_exec_event_prompt: 文本必须进 prompt ──────────────────────────
    {
        std::string p = build_exec_event_prompt({"exec completed (j1, code 0) :: BUILD_OK_MARKER"}, true);
        check(contains(p, "BUILD_OK_MARKER"),
              "C1 event text embedded in prompt", p.substr(0, 120));
        check(!contains(p, "shown in the system messages above"),
              "C2 no longer claims 'shown in system messages above'");
    }

    // 成功且无输出 -> 事件被丢弃 -> 走 empty 分支
    {
        std::string p = build_exec_event_prompt({"exec completed (j2, code 0)"}, true);
        check(contains(p, "no command output was found"),
              "C3 success-no-output -> empty branch", p.substr(0, 120));
    }

    // 失败且无输出 -> hasMissingOutputFailure 分支
    {
        std::string p = build_exec_event_prompt({"exec failed (j3, code 7)"}, true);
        check(contains(p, "without captured stdout/stderr")
                  && contains(p, "Exec failed (j3, code 7)"),
              "C4 failure-no-output -> missing-output branch", p.substr(0, 160));
    }

    // deliver_to_user=false
    {
        std::string p = build_exec_event_prompt({"exec completed (j4, code 0) :: out"}, false);
        check(contains(p, "user delivery is disabled"),
              "C5 deliver_to_user=false branch", p.substr(0, 120));
    }

    // 非结构化事件原样保留
    {
        std::string p = build_exec_event_prompt({"exec finished: RAW_LINE_MARKER"}, true);
        check(contains(p, "RAW_LINE_MARKER"),
              "C6 unstructured event kept verbatim", p.substr(0, 120));
    }

    // 截断：>8000 字符
    {
        std::string big = "exec failed (j5, code 1) :: " + std::string(9000, 'x');
        std::string p = build_exec_event_prompt({big}, true);
        check(contains(p, "[truncated]"), "C7 >8000 chars -> [truncated] appended");
        check(p.size() < 9200, "C8 truncated prompt bounded",
              std::string("size=") + std::to_string(p.size()));
    }

    // 多事件混合：成功无输出被丢弃，其余保留
    {
        std::string p = build_exec_event_prompt(
            {"exec completed (a, code 0)",              // 丢弃
             "exec completed (b, code 0) :: KEEP_B",    // 保留
             "exec failed (c, code 1)"},                // 保留(missing output)
            true);
        check(contains(p, "KEEP_B") && contains(p, "Exec failed (c, code 1)")
                  && !contains(p, "(a, code 0)"),
              "C9 mixed events filtered correctly", p.substr(0, 200));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
