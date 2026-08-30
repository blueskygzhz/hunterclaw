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
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

// ── 多轮连续确认 ────────────────────────────────────────────────────────────
//
// ★★ 回归的是一个【已经发生过】的缺陷: handle_tool_interrupt 原先只跑一轮，
//   resume 产出的 next_interrupt 被写回 result.interrupt 就返回，注释声称
//   "交给下一轮循环"，但调用点 (react_engine.cc:2361/2689) 拿到 result 后
//   直接存进 results，【没有任何代码再读 interrupt 字段】—— 只写不读。
//
// ⚠ 后果: 远端第二次返回 input-required 时，第二轮追问的文本混在结果里
//   给了模型，但系统不再等答复 —— 模型当"已完成"继续往下，
//   而远端永久卡在 input-required 直到超时。
//
// 本测试复刻修复后的迭代循环，验证「连续 N 次挂起」能逐轮问到用户。
static void test_multi_round() {
    SECTION("连续多次 input-required（多轮确认）");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    // 远端行为: 前 3 轮都要求确认，第 4 轮才真正完成。
    // ★ 每轮的 next_interrupt 都带 requires_user_action=true，
    //   这是 ReactEngine 继续追问的判据。
    std::atomic<int> remote_rounds{0};
    c.register_resume_handler("remote_a2a.input_required",
        [&remote_rounds](const ToolInterrupt& i, const std::string& text,
                         const std::atomic<bool>*) -> ResumeOutcome {
            const int n = remote_rounds.fetch_add(1) + 1;
            ResumeOutcome o;
            o.success = true;
            o.text = "第" + std::to_string(n) + "轮答复: " +
                     InterruptCoordinator::match_option(i, text);
            if (n < 3) {
                ToolInterrupt nxt;
                nxt.kind = i.kind;
                nxt.resume_token = i.resume_token;  // ★ 同一 handle 续接
                nxt.prompt = "还需要确认第 " + std::to_string(n + 1) + " 项";
                nxt.requires_user_action = true;
                o.next_interrupt = nxt;
            }
            return o;
        });

    ToolInterrupt it;
    it.kind = "remote_a2a.input_required";
    it.resume_token = "ra2a-multi-1";
    it.prompt = "确认第 1 项";
    it.requires_user_action = true;

    // ── 复刻 handle_tool_interrupt 修复后的迭代循环 ──────────────────────
    std::atomic<bool> cancelled{false};
    std::optional<ToolInterrupt> cur = it;
    int user_answers = 0;
    std::string last_text;

    for (int round = 1; cur.has_value(); ++round) {
        CHECK(round <= kMaxInterruptRounds, "轮次不超过上限");
        if (round > kMaxInterruptRounds) break;

        auto w = c.begin_wait("sess-multi");
        // ★★ 每轮都必须能重新占到槽位 —— 上一轮 release() 之后槽位应为空。
        //   ⚠ 若 release 漏做或 seq 比对写错，这里第 2 轮就会失败。
        CHECK(w.valid(), "第 " + std::to_string(round) + " 轮占到槽位");
        if (!w.valid()) break;

        std::thread user([&c, &user_answers]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (c.claim_answer("sess-multi", "确认")) ++user_answers;
        });
        auto ans = w.wait(5000, &cancelled);
        user.join();
        CHECK(ans.has_value(), "第 " + std::to_string(round) + " 轮拿到答复");
        if (!ans.has_value()) break;
        w.release();

        auto out = c.resume(*cur, *ans, &cancelled);
        CHECK(out.success, "第 " + std::to_string(round) + " 轮恢复成功");
        last_text = out.text;
        cur = out.next_interrupt;
        if (!out.success) break;
        if (cur.has_value() && !cur->requires_user_action) break;
    }

    CHECK(remote_rounds.load() == 3, "远端被续接 3 次（连续挂起逐轮处理）[不可回退]");
    CHECK(user_answers == 3, "用户被真正问了 3 次（而非只问 1 次）[不可回退]");
    CHECK(last_text.find("第3轮") != std::string::npos, "最终拿到第 3 轮的结果");
    CHECK(!c.has_pending("sess-multi"), "全部收束后槽位已释放");

    c.reset_for_testing();
}

// ── 死循环防护 ──────────────────────────────────────────────────────────────
//
// ⚠ 远端若因 bug 每轮都回 input-required，无界循环会把用户钉死，
//   且每轮都可能占 main lane 最长 900s。
static void test_round_cap() {
    SECTION("轮次上限（远端一直要求确认）");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    // 恶意/故障远端: 永远返回下一轮确认
    std::atomic<int> calls{0};
    c.register_resume_handler("evil.always_ask",
        [&calls](const ToolInterrupt& i, const std::string&,
                 const std::atomic<bool>*) -> ResumeOutcome {
            calls.fetch_add(1);
            ResumeOutcome o;
            o.success = true;
            o.text = "又要确认";
            ToolInterrupt nxt = i;
            nxt.requires_user_action = true;
            o.next_interrupt = nxt;  // ★ 永不收束
            return o;
        });

    ToolInterrupt it;
    it.kind = "evil.always_ask";
    it.resume_token = "evil-1";
    it.requires_user_action = true;

    std::atomic<bool> cancelled{false};
    std::optional<ToolInterrupt> cur = it;
    bool capped = false;

    for (int round = 1; cur.has_value(); ++round) {
        if (round > kMaxInterruptRounds) { capped = true; break; }
        auto w = c.begin_wait("sess-evil");
        if (!w.valid()) break;
        std::thread user([&c]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            c.claim_answer("sess-evil", "好");
        });
        auto ans = w.wait(3000, &cancelled);
        user.join();
        if (!ans.has_value()) break;
        w.release();
        auto out = c.resume(*cur, *ans, &cancelled);
        cur = out.next_interrupt;
    }

    CHECK(capped, "触顶退出而非无限循环 [不可回退]");
    CHECK(calls.load() == kMaxInterruptRounds,
          "恰好调用上限次数后停止（" + std::to_string(kMaxInterruptRounds) + "）");

    c.reset_for_testing();
}

// ── 多轮中途 abort ──────────────────────────────────────────────────────────
//
// ⚠ 用户在第 2 轮点了停止，【不该】再弹出第 3 轮追问。
static void test_abort_mid_rounds() {
    SECTION("多轮中途 abort");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    std::atomic<bool> cancelled{false};
    c.register_resume_handler("ra2a.multi",
        [&cancelled](const ToolInterrupt& i, const std::string&,
                     const std::atomic<bool>* ab) -> ResumeOutcome {
            ResumeOutcome o;
            // ★ handler 自己也要看 abort（remote_a2a 恢复函数就是这么做的）
            if (ab && ab->load()) {
                o.success = false;
                o.error = "运行已中止，答复未提交给远端";
                return o;
            }
            o.success = true;
            o.text = "ok";
            ToolInterrupt nxt = i;
            nxt.requires_user_action = true;
            o.next_interrupt = nxt;
            // 第一轮结束后用户点停止
            cancelled.store(true);
            return o;
        });

    ToolInterrupt it;
    it.kind = "ra2a.multi";
    it.resume_token = "ra2a-abort-1";
    it.requires_user_action = true;

    std::optional<ToolInterrupt> cur = it;
    int rounds_asked = 0;

    for (int round = 1; cur.has_value(); ++round) {
        if (round > kMaxInterruptRounds) break;
        auto w = c.begin_wait("sess-abort");
        if (!w.valid()) break;
        ++rounds_asked;
        std::thread user([&c]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            c.claim_answer("sess-abort", "好");
        });
        auto ans = w.wait(3000, &cancelled);
        user.join();
        if (!ans.has_value()) break;
        w.release();
        auto out = c.resume(*cur, *ans, &cancelled);
        cur = out.next_interrupt;
        if (!out.success) break;
        // ★★ 每轮开头重新检查 abort —— 修复后的循环正是这么做的
        if (cancelled.load()) break;
    }

    CHECK(rounds_asked == 1, "abort 后不再弹下一轮追问 [不可回退]");

    c.reset_for_testing();
}

// ── 并行工具调用: N 个 a2a 同时中断 ─────────────────────────────────────────
//
// ★★ 这是 ReactEngine 并行路径 (react_engine.cc:2689) 的真实场景:
//   模型一次发起 N 个 remote_a2a 工具，每个远端都返回 input-required。
//   N 个 lambda 跑在【同一个线程池】上并发调用 handle_tool_interrupt。
//
// 设计取舍: 每会话只有一个槽位 —— 第一个抢到的真的问用户，
//   其余 fail-closed 让模型重试。
//   ⚠ 刻意不排队: 排队会让 N 个工具各等最长 900s 并占满线程池，
//     连不相关的工具都无法执行。
static void test_parallel_interrupts() {
    SECTION("并行工具同时中断（每会话单槽位）");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    c.register_resume_handler("ra2a.par",
        [](const ToolInterrupt&, const std::string& t,
           const std::atomic<bool>*) -> ResumeOutcome {
            ResumeOutcome o;
            o.success = true;
            o.text = "远端已继续: " + t;
            return o;
        });

    constexpr int kN = 6;
    std::atomic<int> got_slot{0};      // 抢到槽位的工具数
    std::atomic<int> rejected{0};      // fail-closed 的工具数
    std::atomic<bool> cancelled{false};

    std::vector<std::thread> tools;
    for (int i = 0; i < kN; ++i) {
        tools.emplace_back([&c, &got_slot, &rejected, &cancelled]() {
            // 复刻 handle_tool_interrupt 的 ① 号步骤
            auto w = c.begin_wait("sess-par");
            if (!w.valid()) {
                rejected.fetch_add(1);
                return;  // fail-closed，交给模型重试
            }
            got_slot.fetch_add(1);
            auto ans = w.wait(4000, &cancelled);
            if (ans.has_value()) {
                ToolInterrupt it;
                it.kind = "ra2a.par";
                it.resume_token = "tok";
                (void)c.resume(it, *ans, &cancelled);
            }
            w.release();
        });
    }

    // 等这批线程把槽位争完
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    // ★★ 有且只有一个占到槽位 —— 这是并行安全的核心断言
    CHECK(got_slot.load() == 1, "并行 6 个中断只有 1 个占到槽位 [不可回退]");
    CHECK(c.has_pending("sess-par"), "该会话确有待答确认");

    // 用户答一次，只应喂给那一个
    CHECK(c.claim_answer("sess-par", "确认"), "答复被唯一持槽者认领");
    CHECK(!c.claim_answer("sess-par", "再确认"),
          "同一槽位不重复认领（否则第二条静默消失）[不可回退]");

    for (auto& t : tools) t.join();
    CHECK(rejected.load() == kN - 1, "其余 5 个 fail-closed（未静默丢弃）[不可回退]");
    CHECK(!c.has_pending("sess-par"), "收束后槽位释放");

    // ── 槽位可被后来者复用（前一个已 release）────────────────────────────
    // ⚠ 若 release 的 seq 比对写错，这里会拿不到槽位 ——
    //   表现为"第一次并行确认之后，该会话再也无法发起任何确认"。
    {
        auto w2 = c.begin_wait("sess-par");
        CHECK(w2.valid(), "前一轮释放后，新中断可占槽位 [不可回退]");
        w2.release();
    }

    c.reset_for_testing();
}

// ── 并行 + 不同会话互不干扰 ─────────────────────────────────────────────────
//
// ⚠ 槽位按 session_key 分桶。若实现写成全局单槽，多用户并发时
//   A 用户的确认会把 B 用户的顶掉 —— 这是跨会话串话，很严重。
static void test_parallel_multi_session() {
    SECTION("并行 + 多会话隔离");
    auto& c = InterruptCoordinator::instance();
    c.reset_for_testing();

    constexpr int kSessions = 5;
    std::atomic<int> ok{0};
    std::atomic<bool> cancelled{false};
    std::vector<std::thread> ts;

    for (int s = 0; s < kSessions; ++s) {
        ts.emplace_back([&c, &ok, &cancelled, s]() {
            const std::string key = "sess-" + std::to_string(s);
            auto w = c.begin_wait(key);
            if (!w.valid()) return;
            auto ans = w.wait(4000, &cancelled);
            if (ans.has_value() && *ans == ("ans-" + std::to_string(s))) {
                ok.fetch_add(1);  // ★ 收到的必须是【本会话】的答复
            }
            w.release();
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int s = 0; s < kSessions; ++s) {
        CHECK(c.claim_answer("sess-" + std::to_string(s), "ans-" + std::to_string(s)),
              "会话 " + std::to_string(s) + " 独立认领");
    }
    for (auto& t : ts) t.join();

    CHECK(ok.load() == kSessions,
          "每个会话各自拿到自己的答复（无跨会话串话）[不可回退]");

    c.reset_for_testing();
}

int main() {
    std::cout << "=== 工具中断（interrupt）闭环测试 ===" << std::endl;
    test_slots();
    test_wait_claim();
    test_prompt_and_match();
    test_resume();
    test_end_to_end();
    test_multi_round();
    test_round_cap();
    test_abort_mid_rounds();
    test_parallel_interrupts();
    test_parallel_multi_session();
    std::cout << "\n=== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ===" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
