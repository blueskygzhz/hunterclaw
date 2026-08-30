// ============================================================================
// gateway_question.cc
// 见 gateway_question.h 的编排顺序说明。
// ============================================================================

#include "hunterclaw/tools/gateway_question.h"

#include "hunterclaw/core/logger.h"
#include "hunterclaw/tools/pending_question_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() { return core::get_logger("question"); }

/// 生成 "ask_<32 hex>" 形式的 id。
/// 🎯 对齐 gateway-question.ts:298 `ask_${randomBytes(16).toString("hex")}`
///
/// ⚠ 不能用对象地址或时间戳: 地址会复用、时间戳会撞（同一毫秒内两次提问），
///   而 id 撞了会让第二次提问的答复落到第一条记录上。
std::string generate_question_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex rng_mu;
    uint64_t hi = 0;
    uint64_t lo = 0;
    {
        std::lock_guard<std::mutex> lk(rng_mu);
        hi = rng();
        lo = rng();
    }
    std::ostringstream ss;
    ss << "ask_" << std::hex << std::setfill('0') << std::setw(16) << hi
       << std::setw(16) << lo;
    return ss.str();
}

}  // namespace

// ── clamp_ask_user_timeout_seconds ──────────────────────────────────────────

int64_t clamp_ask_user_timeout_seconds(int64_t raw_seconds) {
    const int64_t base = raw_seconds > 0 ? raw_seconds : kDefaultAskUserTimeoutSeconds;
    return std::min(kMaxAskUserTimeoutSeconds, std::max(kMinAskUserTimeoutSeconds, base));
}

// ── RunGatewayQuestionOutcome ───────────────────────────────────────────────

std::string RunGatewayQuestionOutcome::first_answer(const std::string& question_id) const {
    auto it = answers.answers.find(question_id);
    if (it == answers.answers.end() || it->second.empty()) return {};
    return it->second.front();
}

std::string RunGatewayQuestionOutcome::render(
    const std::vector<UserInputQuestion>& questions) const {
    std::ostringstream ss;
    bool first = true;
    for (const auto& q : questions) {
        if (!first) ss << "\n";
        first = false;
        ss << q.header << ": ";
        auto it = answers.answers.find(q.id);
        if (it == answers.answers.end() || it->second.empty()) {
            // 对齐 answeredResult(): 未作答显式写出来，而不是留空 ——
            // 留空会让模型以为"这一项不需要"。
            ss << "(未作答)";
            continue;
        }
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << it->second[i];
        }
    }
    return ss.str();
}

// ── run_gateway_question ────────────────────────────────────────────────────

RunGatewayQuestionOutcome run_gateway_question(const RunGatewayQuestionParams& params) {
    RunGatewayQuestionOutcome out;

    // ── Stage 0: 校验 ───────────────────────────────────────────────────
    // ★ 在【注册与登记之前】挡住。一旦 record 落库并广播，终端已经渲染，
    //   再报错就晚了（用户会看到一张永远批不掉的卡片）。
    const std::string invalid = validate_user_input_questions(params.questions);
    if (!invalid.empty()) {
        out.status = QuestionStatus::Cancelled;
        out.error = "question 校验失败: " + invalid;
        log()->error("run_gateway_question rejected: {}", invalid);
        return out;
    }

    // ── Stage 1: 取通道 ────────────────────────────────────────────────
    // ★ 取一次并持有 shared_ptr 到返回（等待可达 3600s，期间单例可能被清空）。
    std::shared_ptr<QuestionBroker> broker = params.broker;
    if (!broker) broker = QuestionRuntime::instance().broker();
    if (!broker) {
        // ⚠⚠ fail-closed。没有提问面时【绝不】返回 Answered ——
        //   调用方会据此认为"用户已确认"，而实际上没有任何人被问过。
        out.status = QuestionStatus::Cancelled;
        out.error = "没有可用的提问通道（gateway 未就绪或未注入）";
        log()->error("run_gateway_question: no question broker available");
        return out;
    }

    // ⚠ 没有能应答的客户端时【立即失败】而不是白等一个超时。
    //   ask_user 默认 900s，在无人应答的部署里等于把 main lane 占死 15 分钟。
    //   对齐 approval 侧 hasExecApprovalClients() 的 no-approval-route 处理。
    if (!broker->has_question_clients()) {
        out.status = QuestionStatus::Cancelled;
        out.error = "当前没有能应答提问的客户端";
        log()->warn("run_gateway_question: no clients able to answer");
        return out;
    }

    // ── abort 前置检查 ─────────────────────────────────────────────────
    // 对齐 gateway-question.ts:305 `params.signal?.throwIfAborted()`
    if (params.aborted && params.aborted->load()) {
        out.status = QuestionStatus::Cancelled;
        out.error = "run 已取消";
        return out;
    }

    const std::string question_id =
        params.question_id.empty() ? generate_question_id() : params.question_id;

    // ── Stage 2: 占会话槽位 ────────────────────────────────────────────
    // ★★ 必须在 gateway 登记【之前】: 登记会立刻广播，终端可能在下一毫秒
    //   就把答复送回来；槽位没占上那条纯文本答复就无处认领。
    //
    // ⚠ 槽位占不上（该会话已有 pending question）时【继续往下走】而非失败:
    //   终端点选路径不依赖槽位，仍然可用。只是失去"纯文本认领"这条捷径。
    //   ★ 这里与 OpenClaw 的 throw 不同 —— OpenClaw 的调用点保证了
    //     一个 run 内只有一个 ask_user 在飞；我们这里 remote_a2a 的
    //     input-required 可能与 ask_user 并存，让后者降级比让 run 崩掉好。
    auto claim = PendingQuestionRegistry::instance().register_question(
        params.session_key, question_id, params.questions, broker);
    if (!claim.valid()) {
        log()->warn("question '{}' has no plain-text claim slot; terminal-only",
                    question_id);
    }

    // ── Stage 3: gateway 登记 + 广播 ───────────────────────────────────
    const std::string registered_id = broker->request_question(
        params.questions, params.session_key, params.agent_id, params.run_id,
        params.timeout_ms, question_id);
    if (registered_id.empty() || registered_id != question_id) {
        // ⚠ id 不一致说明实现方私自改了 id —— 那样槽位里记的 id 就是错的，
        //   用户的纯文本答复会 resolve 到一个不存在的记录上。
        //   宁可失败也不能带着不一致往下走。
        out.status = QuestionStatus::Cancelled;
        out.error = registered_id.empty() ? "提问登记失败"
                                          : "提问登记返回了不一致的 id";
        log()->error("run_gateway_question: register failed (want '{}' got '{}')",
                     question_id, registered_id);
        return out;  // claim 析构自动释放槽位
    }

    // ── 登记与取消的竞态 ───────────────────────────────────────────────
    // 对齐 gateway-question.ts:372-380: 取消可能与登记赛跑，
    // 此时要对【已提交的 id】重新取消，且绝不再推送过期提示词。
    if ((params.aborted && params.aborted->load()) || claim.is_cancellation_requested()) {
        const char* why = (params.aborted && params.aborted->load()) ? "run-abort"
                                                                    : "superseded-input";
        // ★ cancel 失败（已终态）意味着答复已经落地 → 补偿读一次
        if (!broker->cancel_question(question_id, why)) {
            auto late = broker->wait_question_answer(question_id, 1000, nullptr);
            if (late.answered()) {
                out.status = QuestionStatus::Answered;
                out.answers = std::move(late.answers);
                return out;
            }
        }
        out.status = QuestionStatus::Cancelled;
        out.error = why;
        return out;
    }

    // ── Stage 4: 推送提示词 ────────────────────────────────────────────
    // ⚠ 推送前查 is_resolving(): 答复已在路上时，此刻推送的提示词
    //   在渲染出来时就已过期 —— 用户会看到"请选择"紧跟自己的答复。
    //   对齐 gateway-question.ts:417-425。
    if (params.deliver_prompt && !claim.is_resolving()) {
        const std::string prompt =
            format_user_input_prompt(params.questions, params.prompt_options) + "\n\n" +
            question_reply_guidance(params.questions);
        try {
            params.deliver_prompt(prompt);
        } catch (const std::exception& ex) {
            // 对齐 first.kind === "delivery-error": 投递失败要取消提问 ——
            // 留着一个用户永远看不到的提问只会等满超时。
            log()->error("question prompt delivery failed: {}", ex.what());
            if (!broker->cancel_question(question_id, "prompt-delivery-failed")) {
                auto late = broker->wait_question_answer(question_id, 1000, nullptr);
                if (late.answered()) {
                    out.status = QuestionStatus::Answered;
                    out.answers = std::move(late.answers);
                    return out;
                }
            }
            out.status = QuestionStatus::Cancelled;
            out.error = std::string("提示词投递失败: ") + ex.what();
            return out;
        }
    }

    // ── Stage 5: 等答复 ────────────────────────────────────────────────
    // ★ 传 aborted —— 与审批链一致。没有它，abort 之后仍会挂满 timeout，
    //   而这里的 timeout 最长 3600s。
    QuestionWaitResult res =
        broker->wait_question_answer(question_id, params.timeout_ms, params.aborted);

    // ── Stage 6: 收尾 ──────────────────────────────────────────────────
    if (res.status == QuestionStatus::Pending) {
        // 对齐 finishAnswer(): 窗口内没等到 → 主动取消
        if (!broker->cancel_question(question_id, "wait-timeout")) {
            // ⚠⚠ cancel 说"已终态" = 答复在【超时判定与取消之间】落地了。
            //   必须补偿读一次，否则用户明明答了却被当成没答。
            auto late = broker->wait_question_answer(question_id, 1000, nullptr);
            if (late.answered()) {
                out.status = QuestionStatus::Answered;
                out.answers = std::move(late.answers);
                log()->info("question '{}' answered in the timeout race window", question_id);
                return out;
            }
        }
        out.status = QuestionStatus::Cancelled;
        out.error = "等待窗口内没有收到答复";
        return out;
    }

    out.status = res.status;
    out.answers = std::move(res.answers);

    // ── abort 路径：必须 best-effort 取消 gateway 记录 ────────────────────
    //
    // 🎯 对齐 gateway-question.ts:352-358 的 onAbort:
    //      claim.dispose();                              // 同步让位
    //      void cancel("run-abort").catch(() => undefined);  // best-effort
    //   以及 catch 块 :747-751 的
    //      cancel(signal?.aborted ? "run-abort" : "harness-error")
    //
    // ⚠⚠ 原先这里【只是 return】—— gateway 记录会一直停在 pending 直到
    //   900s 过期。后果:
    //     ① 终端上那张提问卡片【永远不消失】，用户点了才发现批不掉；
    //     ② 用户真的点了 → resolve 成功 → 但已经没有任何等待方，
    //        答复凭空消失（而 UI 显示"已提交"）；
    //     ③ 会话槽位虽然由 Claim 析构释放了，但同一会话的下一次提问
    //        会与这张僵尸卡片并存，用户分不清该答哪一张。
    //
    // ★ 只在【非答复】终态下取消。已 Answered 的记录不能碰 ——
    //   那会把用户刚给的答案改成 cancelled。
    if (out.status != QuestionStatus::Answered) {
        const char* why = (params.aborted && params.aborted->load()) ? "run-abort"
                                                                    : "harness-error";
        // ★ cancel 返回 false = 已终态。对齐 OpenClaw:
        //   `const terminal = await cancel(...); if (terminal?.status === "answered") return terminal;`
        //   —— 竞态窗口里可能刚好答上了，必须补偿读一次。
        if (!broker->cancel_question(question_id, why)) {
            auto late = broker->wait_question_answer(question_id, 1000, nullptr);
            if (late.answered()) {
                out.status = QuestionStatus::Answered;
                out.answers = std::move(late.answers);
                out.error.clear();
                log()->info("question '{}' answered in the abort race window", question_id);
                return out;
            }
        }
    }

    if (!out.answered() && out.error.empty()) {
        out.error = std::string("提问结束但未获答复: ") +
                    question_status_to_wire(out.status);
    }
    return out;
    // ★ claim 在此析构 → 释放会话槽位（全分支覆盖，见头文件 ⑥）
}

}  // namespace tools
}  // namespace hunterclaw
