// ============================================================================
// 工具中断（interrupt）闭环不变量测试
//
// 覆盖的安全不变量 —— 每条都对应一个"错了不报错、只静默走偏"的缺陷:
//   1. 每会话至多一个中断在等      —— 否则答复给错中断（"用PDF"答到"是否发邮件"上）
//   2. 认领幂等                    —— 否则用户连发两条，第二条静默消失
//   3. 认领失败必须回落            —— 绝不吞消息
//   4. abort 立即打断等待          —— 否则用户点停止要等满 900s
//   5. 超时不伪造答复              —— fail-closed
//   6. Waiter 全分支释放槽位       —— 否则该会话再也问不出来
//   7. 替补中断不被旧 Waiter 误删  —— seq 身份比对
//   8. 选项编号 1-based 两处一致   —— 否则用户选 A 系统记 B
//   9. 无 handler 时不问用户       —— 否则白答一次且远端照样卡死
//  10. 多轮确认不递归              —— next_interrupt 交回上层
// ============================================================================

#include "hunterclaw/tools/interrupt_coordinator.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace hunterclaw;
using namespace hunterclaw::tools;

static int g_pass = 0;
static int g_fail = 0;

static void CHECK(bool c, const std::string& what) {
    if (c) { ++g_pass; std::cout << "  PASS: " << what << std::endl; }
    else   { ++g_fail; std::cout << "  FAIL: " << what << std::endl; }
}
static void SECTION(const std::string& n) { std::cout << "\n[" << n << "]" << std::endl; }

static ToolInterrupt make_it(const std::string& kind = "remote_a2a.input_required") {
    ToolInterrupt it;
    it.kind = kind;
    it.resume_token = "h-1";
    it.prompt = "将向 2 位客户发送确认邮件，是否继续？";
    it.requires_user_action = true;
    it.options = {"确认继续", "取消"};
    it.token_ids = {"tok-A", "tok-B"};
    return it;
}

// ── 1 / 6 / 7: 会话槽位 ─────────────────────────────────────────────────────
static void test_slots() {
    SECTION("会话槽位");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    {
        auto w1 = c.begin_wait("s1");
        CHECK(w1.valid(), "首个中断占到槽位");
        CHECK(c.has_pending("s1"), "会话标记为有中断在等");

        // ★★ 每会话至多一个 —— 否则入站答复该给谁无法确定
        auto w2 = c.begin_wait("s1");
        CHECK(!w2.valid(), "同会话第二个中断被拒");

        auto w3 = c.begin_wait("s2");
        CHECK(w3.valid(), "不同会话互不影响");
    }
    // Waiter 析构释放（覆盖"全分支释放"）
    CHECK(!c.has_pending("s1"), "Waiter 析构后槽位释放");
    CHECK(!c.has_pending("s2"), "不同会话同样释放");

    // 空 session_key 不登记：认领按会话定位，空 key 会互相顶掉
    auto w_empty = c.begin_wait("");
    CHECK(!w_empty.valid(), "空 session_key 不占槽位（调用方应 fail-closed）");

    // ★ seq 身份比对：旧 Waiter 释放不能删掉替补中断的槽位
    c.reset_for_testing();
    {
        auto old = c.begin_wait("s3");
        CHECK(old.valid(), "旧中断占位");
        old.release();
        auto fresh = c.begin_wait("s3");
        CHECK(fresh.valid(), "替补中断占位成功");
        old.release();  // 再次释放旧的（已失效）
        CHECK(c.has_pending("s3"), "旧 Waiter 重复释放不会误删替补中断的槽位");
    }
    c.reset_for_testing();
}

// ── 2 / 3 / 4 / 5: 等待与认领 ───────────────────────────────────────────────
static void test_wait_claim() {
    SECTION("等待与认领");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    // 无中断在等 → 认领必须失败（消息回落正常流程，不能吞）
    CHECK(!c.claim_answer("none", "hello"), "无中断在等时认领失败（不吞消息）");

    // 正常认领
    {
        auto w = c.begin_wait("s1");
        std::thread answerer([&c]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            c.claim_answer("s1", "确认继续");
        });
        auto got = w.wait(5000, nullptr);
        answerer.join();
        CHECK(got.has_value() && *got == "确认继续", "认领的答复被等待方拿到");

        // ★ 幂等闸: 已答复过不再认领 —— 否则第二条消息"认领成功"却无人消费
        CHECK(!c.claim_answer("s1", "取消"), "已答复后不再认领（第二条消息回落）");
    }

    // 超时 → 不伪造答复
    {
        auto w = c.begin_wait("s2");
        const auto t0 = std::chrono::steady_clock::now();
        auto got = w.wait(150, nullptr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(!got.has_value(), "超时返回无值（fail-closed，绝不伪造已确认）");
        CHECK(ms >= 100 && ms < 2000, "超时时长符合预期");
    }

    // ★★ abort 立即打断 —— 没有它用户点停止要等满 900s
    {
        auto w = c.begin_wait("s3");
        std::atomic<bool> aborted{false};
        std::thread aborter([&aborted]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            aborted.store(true);
        });
        const auto t0 = std::chrono::steady_clock::now();
        auto got = w.wait(30000, &aborted);
        aborter.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(!got.has_value() && ms < 3000, "abort 立即打断等待（不挂满 30s）");
    }
    c.reset_for_testing();
}

// ── 8: 提示词与选项匹配 ─────────────────────────────────────────────────────
static void test_prompt_and_match() {
    SECTION("提示词与选项匹配");
    auto it = make_it();

    const std::string p = InterruptCoordinator::format_prompt(it);
    CHECK(p.find("将向 2 位客户发送确认邮件") != std::string::npos, "追问正文出现在提示词里");
    // ⚠ 用户看不到自己在确认什么就点同意，这道确认在安全上等于没有
    CHECK(p.find("tok-A") != std::string::npos && p.find("tok-B") != std::string::npos,
          "待确认项列进提示词（用户能看到自己在批什么）");
    CHECK(p.find("1. 确认继续") != std::string::npos, "选项按 1-based 编号列出");
    CHECK(p.find("2. 取消") != std::string::npos, "第二个选项编号为 2");

    // ★ 编号基准必须与 format_prompt 一致
    CHECK(InterruptCoordinator::match_option(it, "1") == "确认继续",
          "数字 1 → 第 1 个选项（与提示词同一基准）");
    CHECK(InterruptCoordinator::match_option(it, "2") == "取消", "数字 2 → 第 2 个选项");
    CHECK(InterruptCoordinator::match_option(it, "取消") == "取消", "label 全等匹配");
    CHECK(InterruptCoordinator::match_option(it, "  确认继续 ") == "确认继续",
          "trim 后匹配");

    // ③ 自由文本透传 —— options 是建议而非白名单
    CHECK(InterruptCoordinator::match_option(it, "只发给张三") == "只发给张三",
          "自由文本原样透传（远端常既给选项也接受自由描述）");

    // 越界数字不当选项，但也不能吃掉可能是 label 的数字
    ToolInterrupt num;
    num.options = {"2025", "2026"};
    CHECK(InterruptCoordinator::match_option(num, "2026") == "2026",
          "label 为数字时按 label 匹配（越界编号不拦路）");

    // 无选项 → 纯自由文本
    ToolInterrupt free;
    free.prompt = "请补充收件人";
    CHECK(InterruptCoordinator::match_option(free, " 张三 ") == "张三",
          "无选项时 trim 后原样返回");
    CHECK(InterruptCoordinator::format_prompt(free).find("直接回复") != std::string::npos,
          "无选项时给出自由回复指引");
}

// ── 9 / 10: 恢复路由 ────────────────────────────────────────────────────────
static void test_resume() {
    SECTION("恢复路由");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    auto it = make_it();

    // ★★ 无 handler → 必须能提前发现，否则用户白答一次且远端照样卡死
    CHECK(!c.has_resume_handler(it.kind), "初始无 handler");
    auto no_h = c.resume(it, "确认继续", nullptr);
    CHECK(!no_h.success && !no_h.error.empty(),
          "无 handler 时 resume 失败且有明确原因（不抛异常、不崩 run）");

    // 注册 handler
    std::string seen_text;
    ToolInterrupt seen_it;
    const bool ok = c.register_resume_handler(
        it.kind, [&](const ToolInterrupt& i, const std::string& text,
                     const std::atomic<bool>*) -> ResumeOutcome {
            seen_it = i;
            seen_text = text;
            ResumeOutcome o;
            o.success = true;
            o.text = "用户答复: " + text + "\n\n远端恢复执行后返回:\n已发送";
            return o;
        });
    CHECK(ok, "handler 注册成功");
    CHECK(c.has_resume_handler(it.kind), "has_resume_handler 反映注册");

    // ⚠ 同 kind 重复注册必须被拒：后注册的会让已在等待的中断恢复到错误子系统
    CHECK(!c.register_resume_handler(it.kind,
              [](const ToolInterrupt&, const std::string&,
                 const std::atomic<bool>*) { return ResumeOutcome{}; }),
          "同 kind 重复注册被拒（保留先注册的）");

    auto out = c.resume(it, "确认继续", nullptr);
    CHECK(out.success, "恢复成功");
    CHECK(seen_it.resume_token == "h-1", "resume_token 原样透传给 handler");
    CHECK(seen_text == "确认继续", "用户答复原样透传");
    // ★ B1: 用户答复作为 tool result 内容，不产生独立 user turn
    CHECK(out.text.find("用户答复: 确认继续") != std::string::npos,
          "结果文本含用户答复（模型才看得懂远端为何继续）");
    CHECK(out.text.find("已发送") != std::string::npos, "结果文本含远端返回");
    CHECK(!out.next_interrupt.has_value(), "单轮确认无后续中断");

    // handler 抛异常 → 转成失败而不是穿出
    c.reset_for_testing();
    c.register_resume_handler("boom", [](const ToolInterrupt&, const std::string&,
                                        const std::atomic<bool>*) -> ResumeOutcome {
        throw std::runtime_error("network down");
    });
    ToolInterrupt bad;
    bad.kind = "boom";
    auto thrown = c.resume(bad, "x", nullptr);
    CHECK(!thrown.success && thrown.error.find("network down") != std::string::npos,
          "handler 异常被转成失败（不崩 run）");

    // ★ 多轮确认: next_interrupt 交回上层，不在 handler 里递归
    c.reset_for_testing();
    c.register_resume_handler("multi", [](const ToolInterrupt&, const std::string&,
                                         const std::atomic<bool>*) -> ResumeOutcome {
        ResumeOutcome o;
        o.success = true;
        o.text = "第一步已确认";
        ToolInterrupt again;
        again.kind = "multi";
        again.resume_token = "h-2";
        again.prompt = "还需要确认第二步";
        again.requires_user_action = true;
        o.next_interrupt = again;
        return o;
    });
    ToolInterrupt m;
    m.kind = "multi";
    auto multi = c.resume(m, "确认", nullptr);
    CHECK(multi.success && multi.next_interrupt.has_value(),
          "多轮确认通过 next_interrupt 交回上层（不递归占死 lane）");
    CHECK(multi.next_interrupt->resume_token == "h-2", "后续中断带新的 resume_token");

    c.reset_for_testing();
}

// ── 端到端：等待 → 认领 → 恢复 ───────────────────────────────────────────────
static void test_end_to_end() {
    SECTION("端到端闭环");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    auto it = make_it();
    c.register_resume_handler(it.kind,
        [](const ToolInterrupt& i, const std::string& text,
           const std::atomic<bool>*) -> ResumeOutcome {
            ResumeOutcome o;
            const std::string picked = InterruptCoordinator::match_option(i, text);
            if (picked == "取消") {
                o.success = false;
                o.error = "用户取消了远端任务";
                return o;
            }
            o.success = true;
            o.text = "用户答复: " + picked + "\n\n远端恢复执行后返回:\n邮件已发送";
            return o;
        });

    // 模拟 ReactEngine: 占位 → 投递 → 等 → 恢复
    std::string delivered;
    auto w = c.begin_wait("sess-e2e");
    CHECK(w.valid(), "占位成功");
    delivered = InterruptCoordinator::format_prompt(it);

    // 模拟 MessageDispatcher: 用户回 "1"（= 确认继续）
    std::thread user([&c]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        c.claim_answer("sess-e2e", "1");
    });
    auto answer = w.wait(5000, nullptr);
    user.join();
    CHECK(answer.has_value(), "拿到用户答复");
    w.release();
    CHECK(!c.has_pending("sess-e2e"), "答复后槽位释放（可接受下一轮确认）");

    auto out = c.resume(it, *answer, nullptr);
    CHECK(out.success, "恢复成功");
    CHECK(out.text.find("确认继续") != std::string::npos,
          "数字答复被归一化成选项原文后发回远端");
    CHECK(out.text.find("邮件已发送") != std::string::npos, "拿到远端恢复后的结果");
    CHECK(!delivered.empty() && delivered.find("tok-A") != std::string::npos,
          "投递给用户的提示词含待确认项");

    // 用户取消路径
    c.reset_for_testing();
    c.register_resume_handler(it.kind,
        [](const ToolInterrupt& i, const std::string& text,
           const std::atomic<bool>*) -> ResumeOutcome {
            ResumeOutcome o;
            if (InterruptCoordinator::match_option(i, text) == "取消") {
                o.success = false;
                o.error = "用户取消了远端任务";
            }
            return o;
        });
    auto cancelled = c.resume(it, "2", nullptr);
    CHECK(!cancelled.success && cancelled.error == "用户取消了远端任务",
          "用户选取消 → 恢复失败并回收远端任务");

    // ⚠ 全等比较而非 find: "不要取消" 里也含"取消"
    ToolInterrupt freeform = make_it();
    freeform.options.clear();
    c.reset_for_testing();
    c.register_resume_handler(freeform.kind,
        [](const ToolInterrupt& i, const std::string& text,
           const std::atomic<bool>*) -> ResumeOutcome {
            ResumeOutcome o;
            const std::string picked = InterruptCoordinator::match_option(i, text);
            o.success = (picked != "取消");
            o.text = picked;
            return o;
        });
    auto notcancel = c.resume(freeform, "不要取消，继续发", nullptr);
    CHECK(notcancel.success, "\"不要取消\" 不被误判为取消（全等比较而非子串）");

    c.reset_for_testing();
}

int main() {
    std::cout << "=== 工具中断（interrupt）闭环测试 ===" << std::endl;
    test_slots();
    test_wait_claim();
    test_prompt_and_match();
    test_resume();
    test_end_to_end();
    std::cout << "\n=== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ===" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
