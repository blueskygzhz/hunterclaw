// ============================================================================
// 用户提问通道（方案 C）不变量测试
//
// 🎯 对齐 OpenClaw:
//   src/agents/harness/user-input-bridge.ts
//   src/agents/harness/gateway-question.ts
//   packages/gateway-protocol/src/schema/questions.ts
//
// 覆盖的安全不变量（每一条都对应一个"错了不会报错、只会静默走偏"的缺陷）:
//   1. 选项固定时【拒绝】自由文本         —— 否则"随便"会被当枚举值发给远端
//   2. 编号基准 1-based 两处一致          —— 否则用户选 A 系统记 B
//   3. 位置回落保留空行                   —— 否则答案整体错位一格
//   4. 每会话至多一个 pending question    —— 否则认领到哪一个不确定
//   5. 认领幂等（resolving 闸）           —— 否则同一条消息既答复又当新指令
//   6. 认领失败必须回落（不吞消息）
//   7. answers 只在 Answered 时有值       —— 否则"已取消"被渲染成"已作答"
//   8. 超时窗口内的竞态答复要被补偿读到
//   9. abort 立即打断等待
//  10. 无通道 / 无客户端 → fail-closed，绝不伪造 Answered
// ============================================================================

#include "hunterclaw/gateway/question_manager.h"
#include "hunterclaw/tools/gateway_question.h"
#include "hunterclaw/tools/pending_question_registry.h"
#include "hunterclaw/tools/question_broker.h"
#include "hunterclaw/tools/user_input_bridge.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace hunterclaw;
using namespace hunterclaw::tools;

static int g_pass = 0;
static int g_fail = 0;

static void CHECK(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::cout << "  PASS: " << what << std::endl;
    } else {
        ++g_fail;
        std::cout << "  FAIL: " << what << std::endl;
    }
}

static void SECTION(const std::string& name) {
    std::cout << "\n[" << name << "]" << std::endl;
}

static UserInputQuestion make_q(const std::string& id, bool is_other,
                                std::vector<std::string> labels) {
    UserInputQuestion q;
    q.id = id;
    q.header = "标题";
    q.question = "请选择一项";
    q.is_other = is_other;
    for (auto& l : labels) q.options.push_back({l, std::nullopt});
    return q;
}

// ============================================================================
// 1 + 2: normalize —— 编号基准与自由文本边界
// ============================================================================
static void test_normalize() {
    SECTION("答案归一化");

    auto q = make_q("fmt", false, {"PDF", "Excel", "CSV"});

    // ★ 1-based: "1" → 第一个选项。
    //   ⚠ 若哪天写成 0-based，用户选 1 会拿到 Excel —— 而且不报错。
    CHECK(normalize_user_input_answer("1", q).value_or("") == "PDF",
          "数字 1 → 第 1 个选项（1-based）");
    CHECK(normalize_user_input_answer("3", q).value_or("") == "CSV",
          "数字 3 → 第 3 个选项");

    // 越界数字不当选项，但也不能直接拒 —— 要继续往下匹配 label
    CHECK(!normalize_user_input_answer("9", q).has_value(),
          "越界数字在无 other 时被拒");

    // label 大小写不敏感，但返回【原始大小写】
    CHECK(normalize_user_input_answer("pdf", q).value_or("") == "PDF",
          "label 大小写不敏感，返回原始大小写");

    // ★★ 安全边界: 选项固定且 !is_other → 自由文本必须被拒。
    //   ⚠ 放行会把"随便""都行"当成有效枚举值发给远端。
    CHECK(!normalize_user_input_answer("随便", q).has_value(),
          "选项固定且 !is_other → 拒绝自由文本");

    auto q_other = make_q("fmt2", true, {"PDF", "Excel"});
    CHECK(normalize_user_input_answer("只要前两页", q_other).value_or("") == "只要前两页",
          "is_other=true → 接受自由文本");

    // 无选项 → 纯自由文本
    UserInputQuestion q_free;
    q_free.id = "free";
    q_free.header = "备注";
    q_free.question = "补充说明";
    CHECK(normalize_user_input_answer("  hello  ", q_free).value_or("") == "hello",
          "无选项时 trim 后原样返回");
    CHECK(!normalize_user_input_answer("   ", q_free).has_value(), "纯空白 → 无答案");

    // 选项 label 恰好是数字时不能被编号解析吃掉
    auto q_num = make_q("year", false, {"2025", "2026"});
    CHECK(normalize_user_input_answer("2026", q_num).value_or("") == "2026",
          "label 为数字时按 label 匹配（越界编号不拦路）");
}

// ============================================================================
// 3: build_answers —— keyed 与位置回落
// ============================================================================
static void test_build_answers() {
    SECTION("多题答案拆解");

    std::vector<UserInputQuestion> qs = {
        make_q("fmt", false, {"PDF", "Excel"}),
        make_q("scope", false, {"全部", "仅本月"}),
    };

    // 单题：整条文本
    std::vector<UserInputQuestion> one = {qs[0]};
    auto a1 = build_user_input_answers(one, "Excel");
    CHECK(a1.answers["fmt"].size() == 1 && a1.answers["fmt"][0] == "Excel",
          "单题：整条文本归一化");

    // keyed by id
    auto a2 = build_user_input_answers(qs, "fmt: PDF\nscope: 仅本月");
    CHECK(a2.answers["fmt"].size() == 1 && a2.answers["fmt"][0] == "PDF" &&
              a2.answers["scope"].size() == 1 && a2.answers["scope"][0] == "仅本月",
          "keyed by question id");

    // keyed by 1-based 序号
    auto a3 = build_user_input_answers(qs, "1: Excel\n2: 全部");
    CHECK(a3.answers["fmt"][0] == "Excel" && a3.answers["scope"][0] == "全部",
          "keyed by 1-based 序号");

    // 位置回落
    auto a4 = build_user_input_answers(qs, "PDF\n全部");
    CHECK(a4.answers["fmt"][0] == "PDF" && a4.answers["scope"][0] == "全部",
          "位置回落：按行序对应");

    // ★★ 保留空行: 用户空一行表示"第一题不答"
    //   ⚠ 若切行时吃掉空 token，"全部"会被记到 fmt 上 —— 静默错位。
    auto a5 = build_user_input_answers(qs, "\n全部");
    CHECK(a5.answers["fmt"].empty() && a5.answers["scope"].size() == 1 &&
              a5.answers["scope"][0] == "全部",
          "位置回落保留空行（不整体前移）");
}

// ============================================================================
// 校验
// ============================================================================
static void test_validate() {
    SECTION("question 校验");

    CHECK(!validate_user_input_questions({}).empty(), "空列表被拒");

    auto bad_id = make_q("Bad-Id", false, {"A"});
    CHECK(!validate_user_input_questions({bad_id}).empty(), "非法 id 被拒");

    auto long_header = make_q("ok", false, {"A"});
    long_header.header = "这个标题特别特别特别长超过十二个字";
    CHECK(!validate_user_input_questions({long_header}).empty(),
          "header 超长被拒（UI 契约）");

    auto too_many = make_q("ok", false, {"A", "B", "C", "D", "E"});
    CHECK(!validate_user_input_questions({too_many}).empty(), "选项超过 4 个被拒");

    // ⚠ 重名 label: 答复按 label 文本匹配，重名会让"选第 2 个"解析成第 1 个
    auto dup = make_q("ok", false, {"PDF", "pdf"});
    CHECK(!validate_user_input_questions({dup}).empty(),
          "选项 label 重名被拒（大小写不敏感）");

    auto dup_id_a = make_q("same", false, {"A"});
    auto dup_id_b = make_q("same", false, {"B"});
    CHECK(!validate_user_input_questions({dup_id_a, dup_id_b}).empty(),
          "question id 重复被拒");

    CHECK(validate_user_input_questions({make_q("ok", false, {"A", "B"})}).empty(),
          "合法 question 通过");
}

// ============================================================================
// 7 + 8 + 9: QuestionManager 不变量
// ============================================================================
static void test_manager() {
    SECTION("QuestionManager 不变量");

    gateway::QuestionManager mgr;
    auto qs = std::vector<UserInputQuestion>{make_q("fmt", false, {"PDF", "Excel"})};

    // 超时钳制
    auto rec = mgr.create(qs, 99999999, std::nullopt);
    CHECK(rec.expires_at_ms - rec.created_at_ms <= gateway::MAX_QUESTION_TIMEOUT_MS,
          "超时被钳到上限（避免长期占死会话槽位）");

    // 收敛后 answers 只在 Answered 时有值
    auto r1 = mgr.create(qs, 5000, std::string("q_cancel"));
    CHECK(mgr.register_record(r1, 5000), "注册成功");
    CHECK(mgr.cancel("q_cancel", std::string("test")), "取消成功");
    auto snap = mgr.get_snapshot("q_cancel");
    CHECK(snap.has_value() && snap->status == QuestionStatus::Cancelled &&
              !snap->answers.has_value(),
          "已取消 → answers 无值（不会被渲染成'已作答但为空'）");
    auto j = snap->to_json();
    CHECK(!j.contains("answers"), "线上形状里 cancelled 不带 answers 字段");
    CHECK(j["questions"][0].contains("questionId"),
          "线上字段名是 questionId 而非 id");

    // 重复收敛 → false（认领通道据此判断是否回落）
    CHECK(!mgr.cancel("q_cancel", std::string("test")), "重复收敛返回 false");

    // 已收敛后不出现在待答列表
    bool listed = false;
    for (const auto& r : mgr.list_pending()) {
        if (r.id == "q_cancel") listed = true;
    }
    CHECK(!listed, "已收敛记录不出现在待答列表");

    // 答案唤醒等待方
    auto r2 = mgr.create(qs, 5000, std::string("q_answer"));
    mgr.register_record(r2, 5000);
    std::thread resolver([&mgr]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        UserInputAnswers a;
        a.answers["fmt"] = {"PDF"};
        mgr.resolve("q_answer", a, std::string("operator"));
    });
    auto waited = mgr.await_answer("q_answer", 3000, nullptr);
    resolver.join();
    CHECK(waited.answered() && waited.answers.answers["fmt"][0] == "PDF",
          "resolve 唤醒等待方并带回答案");

    // 未收敛 → 窗口内返回 Pending（调用方随后 cancel）
    auto r3 = mgr.create(qs, 60000, std::string("q_pending"));
    mgr.register_record(r3, 60000);
    auto pend = mgr.await_answer("q_pending", 120, nullptr);
    CHECK(pend.status == QuestionStatus::Pending, "窗口内未收敛 → Pending");

    // ★ abort 立即打断（不等满 60s）
    std::atomic<bool> aborted{false};
    auto t0 = std::chrono::steady_clock::now();
    std::thread aborter([&aborted]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        aborted.store(true);
    });
    auto ab = mgr.await_answer("q_pending", 60000, &aborted);
    aborter.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    CHECK(ab.status == QuestionStatus::Cancelled && elapsed < 2000,
          "abort 立即打断等待（不占满 timeout）");

    // 不存在 → Expired 而非 Pending
    auto missing = mgr.await_answer("no_such_id", 100, nullptr);
    CHECK(missing.status == QuestionStatus::Expired,
          "记录不存在 → Expired（避免调用方白跑一次 cancel+补偿读）");

    // 前缀查找与歧义
    auto r4 = mgr.create(qs, 5000, std::string("abc_one"));
    auto r5 = mgr.create(qs, 5000, std::string("abc_two"));
    mgr.register_record(r4, 5000);
    mgr.register_record(r5, 5000);
    CHECK(mgr.lookup_pending_id("abc_one").kind ==
              gateway::QuestionIdLookupResult::Kind::Exact,
          "精确匹配优先");
    CHECK(mgr.lookup_pending_id("abc").kind ==
              gateway::QuestionIdLookupResult::Kind::Ambiguous,
          "歧义前缀报错而非猜第一个");
}

// ============================================================================
// FakeBroker —— 用于注册表与编排层测试
// ============================================================================
class FakeBroker : public QuestionBroker {
public:
    std::shared_ptr<gateway::QuestionManager> mgr =
        std::make_shared<gateway::QuestionManager>();
    bool clients = true;
    int resolve_calls = 0;
    int cancel_calls = 0;
    UserInputAnswers last_answers;

    std::string request_question(const std::vector<UserInputQuestion>& questions,
                                const std::string&, const std::string&,
                                const std::string&, int64_t timeout_ms,
                                const std::string& explicit_id) override {
        std::optional<std::string> id;
        if (!explicit_id.empty()) id = explicit_id;
        auto rec = mgr->create(questions, timeout_ms, id);
        if (!mgr->register_record(rec, timeout_ms)) return {};
        return rec.id;
    }
    QuestionWaitResult wait_question_answer(const std::string& qid, int64_t t,
                                           const std::atomic<bool>* ab) override {
        return mgr->await_answer(qid, t, ab);
    }
    bool resolve_question(const std::string& qid, const UserInputAnswers& a,
                          const std::string& by) override {
        ++resolve_calls;
        last_answers = a;
        return mgr->resolve(qid, a, by);
    }
    bool cancel_question(const std::string& qid, const std::string& by) override {
        ++cancel_calls;
        return mgr->cancel(qid, by);
    }
    bool has_question_clients() const override { return clients; }
};

// ============================================================================
// 4 + 5 + 6: PendingQuestionRegistry
// ============================================================================
static void test_registry() {
    SECTION("会话槽位与认领");

    auto& reg = PendingQuestionRegistry::instance();
    reg.reset_for_testing();

    auto broker = std::make_shared<FakeBroker>();
    auto qs = std::vector<UserInputQuestion>{make_q("fmt", true, {"PDF", "Excel"})};

    const std::string sess = "sess-1";
    const std::string qid = broker->request_question(qs, sess, "a", "r", 5000, "q1");
    CHECK(qid == "q1", "explicit id 被沿用（编排层要拿它占槽位）");

    {
        auto claim = reg.register_question(sess, qid, qs, broker);
        CHECK(claim.valid(), "槽位注册成功");
        CHECK(reg.has_pending(sess), "会话有 pending question");

        // ★★ 每会话至多一个
        const std::string qid2 = broker->request_question(qs, sess, "a", "r", 5000, "q2");
        auto claim2 = reg.register_question(sess, qid2, qs, broker);
        CHECK(!claim2.valid(), "同会话第二个 pending question 被拒");

        // 认领
        CHECK(reg.claim_answer(sess, "Excel"), "纯文本被认领为答复");
        CHECK(broker->resolve_calls == 1, "认领走到 broker->resolve");
        CHECK(broker->last_answers.answers["fmt"][0] == "Excel", "答案被正确归一化");

        // ★ 幂等: 已 resolving，第二条消息不再认领 → 必须回落 steer
        CHECK(!reg.claim_answer(sess, "PDF"),
              "已收敛后不再认领（消息回落 steer，不被吞掉）");
        CHECK(broker->resolve_calls == 1, "不产生第二次 resolve");
    }
    // Claim 析构释放槽位
    CHECK(!reg.has_pending(sess), "Claim 析构后槽位释放");

    // 无 pending 时认领必须失败（不能吞消息）
    CHECK(!reg.claim_answer("sess-none", "hello"), "无 pending 时认领失败");

    // cancel_for_session
    reg.reset_for_testing();
    auto broker2 = std::make_shared<FakeBroker>();
    const std::string qid3 = broker2->request_question(qs, "sess-2", "a", "r", 5000, "q3");
    {
        auto claim = reg.register_question("sess-2", qid3, qs, broker2);
        CHECK(claim.valid(), "注册成功");
        CHECK(reg.cancel_for_session("sess-2", "image-reply"),
              "图片回复触发取消（不留下用户看不懂的悬挂提问）");
        CHECK(broker2->cancel_calls == 1, "走到 broker->cancel");
        CHECK(claim.is_cancellation_requested(), "claim 反映取消已请求");
        CHECK(!reg.claim_answer("sess-2", "PDF"), "取消后不再认领");
    }

    reg.reset_for_testing();
}

// ============================================================================
// 10: 编排层 fail-closed
// ============================================================================
static void test_orchestration() {
    SECTION("编排层 fail-closed");

    PendingQuestionRegistry::instance().reset_for_testing();
    auto qs = std::vector<UserInputQuestion>{make_q("fmt", true, {"PDF", "Excel"})};

    // ── 无通道 → 绝不 Answered ──────────────────────────────────────────
    {
        RunGatewayQuestionParams p;
        p.questions = qs;
        p.session_key = "s";
        p.broker = nullptr;  // 且 QuestionRuntime 未注入
        auto out = run_gateway_question(p);
        CHECK(!out.answered() && !out.error.empty(),
              "无提问通道 → fail-closed（不伪造用户确认）");
    }

    // ── 无客户端 → 立即失败，不白等 900s ────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        broker->clients = false;
        RunGatewayQuestionParams p;
        p.questions = qs;
        p.session_key = "s";
        p.broker = broker;
        p.timeout_ms = 60000;
        const auto t0 = std::chrono::steady_clock::now();
        auto out = run_gateway_question(p);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(!out.answered() && ms < 1000,
              "无可应答客户端 → 立即失败（不占死 lane）");
    }

    // ── 校验不通过 → 不登记不广播 ───────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        RunGatewayQuestionParams p;
        p.questions = {make_q("Bad-Id", false, {"A"})};
        p.session_key = "s";
        p.broker = broker;
        auto out = run_gateway_question(p);
        CHECK(!out.answered() && out.error.find("校验失败") != std::string::npos,
              "校验不通过 → 在登记前就拒绝");
    }

    // ── 正常答复 ────────────────────────────────────────────────────────
    {
        PendingQuestionRegistry::instance().reset_for_testing();
        auto broker = std::make_shared<FakeBroker>();
        std::string delivered;

        RunGatewayQuestionParams p;
        p.questions = qs;
        p.session_key = "sess-ok";
        p.broker = broker;
        p.timeout_ms = 4000;
        p.question_id = "q_ok";
        p.deliver_prompt = [&delivered](const std::string& t) { delivered = t; };

        std::thread answerer([&broker]() {
            // 等提问登记完成后再答（登记是同步的，但 wait 要先进去）
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            UserInputAnswers a;
            a.answers["fmt"] = {"PDF"};
            broker->mgr->resolve("q_ok", a, "operator");
        });
        auto out = run_gateway_question(p);
        answerer.join();

        CHECK(out.answered(), "拿到用户答复");
        CHECK(out.first_answer("fmt") == "PDF", "答案内容正确（这是方案 C 的核心）");
        CHECK(!delivered.empty(), "提示词已投递");
        CHECK(delivered.find("PDF") != std::string::npos, "提示词里列出了选项");
        CHECK(out.render(qs).find("PDF") != std::string::npos, "render 输出含答案");
        CHECK(!PendingQuestionRegistry::instance().has_pending("sess-ok"),
              "结束后槽位已释放（全分支覆盖）");
    }

    // ── 超时 → cancel 且不伪造答案 ──────────────────────────────────────
    {
        PendingQuestionRegistry::instance().reset_for_testing();
        auto broker = std::make_shared<FakeBroker>();
        RunGatewayQuestionParams p;
        p.questions = qs;
        p.session_key = "sess-to";
        p.broker = broker;
        p.timeout_ms = 150;
        auto out = run_gateway_question(p);
        CHECK(!out.answered(), "超时 → 非 Answered");
        CHECK(broker->cancel_calls >= 1, "超时后主动取消提问（不留悬挂记录）");
    }

    // ── abort 立即返回 ──────────────────────────────────────────────────
    {
        PendingQuestionRegistry::instance().reset_for_testing();
        auto broker = std::make_shared<FakeBroker>();
        std::atomic<bool> aborted{false};
        RunGatewayQuestionParams p;
        p.questions = qs;
        p.session_key = "sess-ab";
        p.broker = broker;
        p.timeout_ms = 30000;
        p.aborted = &aborted;
        std::thread aborter([&aborted]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            aborted.store(true);
        });
        const auto t0 = std::chrono::steady_clock::now();
        auto out = run_gateway_question(p);
        aborter.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(!out.answered() && ms < 3000, "abort 立即结束（不挂满 30s）");
        // ★★ 对齐 gateway-question.ts:352-358 onAbort 里的
        //   `void cancel("run-abort").catch(() => undefined)`
        //   ⚠ 缺了这一步，gateway 记录会一直 pending 到 900s 过期:
        //     终端那张卡片永不消失，用户点了才发现批不掉；更糟的是用户
        //     真点了会 resolve 成功，但已无等待方 —— 答复凭空消失。
        CHECK(broker->cancel_calls >= 1, "abort 后 best-effort 取消 gateway 记录");
    }

    PendingQuestionRegistry::instance().reset_for_testing();
}

// ============================================================================
// 超时钳制（ask_user）
// ============================================================================
static void test_timeout_clamp() {
    SECTION("ask_user 超时钳制");
    CHECK(clamp_ask_user_timeout_seconds(0) == kDefaultAskUserTimeoutSeconds,
          "0 → 默认 900s");
    CHECK(clamp_ask_user_timeout_seconds(-5) == kDefaultAskUserTimeoutSeconds,
          "负数 → 默认");
    CHECK(clamp_ask_user_timeout_seconds(1) == kMinAskUserTimeoutSeconds,
          "过小 → 下限 30s（避免用户刚看到就超时）");
    CHECK(clamp_ask_user_timeout_seconds(99999) == kMaxAskUserTimeoutSeconds,
          "过大 → 上限 3600s（避免长期占死会话槽位）");
    CHECK(clamp_ask_user_timeout_seconds(600) == 600, "区间内原样保留");
}

int main() {
    std::cout << "=== 用户提问通道（方案 C）测试 ===" << std::endl;

    test_normalize();
    test_build_answers();
    test_validate();
    test_manager();
    test_registry();
    test_orchestration();
    test_timeout_clamp();

    std::cout << "\n=== 结果: " << g_pass << " 通过, " << g_fail << " 失败 ==="
              << std::endl;
    return g_fail == 0 ? 0 : 1;
}
