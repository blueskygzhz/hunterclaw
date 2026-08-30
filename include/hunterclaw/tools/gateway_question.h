// ============================================================================
// gateway_question.h — 一次完整的「向用户提问并等答复」编排
//
// 🎯 严格对齐 OpenClaw runAgentHarnessGatewayQuestion()
//    (src/agents/harness/gateway-question.ts:295-476)
//
// ★★ 这是方案 C 的唯一对外入口。ask_user 工具与 remote_a2a 的
//   input-required 处理【都走这里】，不各自拼装 —— 与
//   与"同一判据必须只有一处实现"同理:
//   同一判据散落多处必然漂移，而这里漂移的是"用户是否真的答过"。
//
// ── 编排顺序（每一步的先后都有理由，不可调换）────────────────────────────
//   ① 占会话槽位（register_question）
//        必须在登记之前: 登记会立刻广播出去，终端可能在下一毫秒就
//        把答复送回来。槽位没占上的话那条答复无处认领。
//   ② gateway 登记 + 广播（request_question）
//   ③ 推送提示词给用户（deliver_prompt）
//        ⚠ 推送前要查 is_resolving(): 若答复已在路上（终端点选 /
//          纯文本已被认领），此刻推送的提示词在渲染出来时就已经过期，
//          用户会看到"请选择"紧跟着自己的答复，非常困惑。
//          对齐 gateway-question.ts:417-425。
//   ④ 阻塞等答复（wait_question_answer，带 aborted）
//   ⑤ 窗口内没等到 → 主动 cancel，并对 cancel 失败做一次补偿读
//        ⚠ cancel 返回"已终态"意味着答复在【超时判定与取消之间】
//          落了地。此时必须再读一次，否则用户明明答了却被当成没答。
//          对齐 finishAnswer() + cancel() 里的 waitAnswer 兜底。
//   ⑥ 释放槽位（Claim 析构，全分支覆盖）
// ============================================================================

#pragma once

#include "hunterclaw/tools/question_broker.h"
#include "hunterclaw/tools/user_input_bridge.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

// ── 超时常量（对齐 ask-user-tool.ts:15-19）──────────────────────────────────
constexpr int64_t kDefaultAskUserTimeoutSeconds = 900;
constexpr int64_t kMinAskUserTimeoutSeconds = 30;
constexpr int64_t kMaxAskUserTimeoutSeconds = 3600;

// gateway 侧等待的额外宽限（ASK_USER_RPC_GRACE_MS / QUESTION_RPC_GRACE_MS）。
//
// ⚠ 等待方与记录【不能同时到期】。同时到期会形成竞态边界:
//   用户最后一刻作答 → 等待方已按超时返回, 答案却写进了记录
//   → 用户看到"已提交", agent 看到"没人答"。
// ★ 只加在等待侧, 不改 expires_at_ms —— 终端倒计时仍以记录为准。
constexpr int64_t kQuestionGatewayGraceMs = 10000;

/// 把用户给的秒数钳进合法区间（对齐 ask-user-tool.ts:201-204）。
int64_t clamp_ask_user_timeout_seconds(int64_t raw_seconds);

// ── RunGatewayQuestionParams ────────────────────────────────────────────────
struct RunGatewayQuestionParams {
    std::vector<UserInputQuestion> questions;
    std::string session_key;
    std::string agent_id;
    std::string run_id;
    int64_t timeout_ms = kDefaultAskUserTimeoutSeconds * 1000;

    /// 指定 question id（留空自动生成）。
    std::string question_id;

    /// 把提示词投递到用户可见面。
    /// 🎯 对齐 `delivery: Pick<EmbeddedRunAttemptParams, "onBlockReply"|"onPartialReply">`
    ///
    /// ⚠ 留空表示【只有终端卡片、没有聊天内提示词】。
    ///   在 IM 类频道上这等于用户根本不知道被问了什么 —— 除非提问内容
    ///   已经通过别的途径（例如工具返回文本）告知过用户。
    std::function<void(const std::string& text)> deliver_prompt;

    /// 运行取消标志。
    const std::atomic<bool>* aborted = nullptr;

    /// 提示词渲染选项。
    UserInputPromptOptions prompt_options;

    /// 审批/提问通道。留空则回落 QuestionRuntime::instance()。
    /// ★ 显式赋值仅用于单测注入 fake。
    std::shared_ptr<QuestionBroker> broker;
};

// ── RunGatewayQuestionOutcome ───────────────────────────────────────────────
struct RunGatewayQuestionOutcome {
    QuestionStatus status = QuestionStatus::Cancelled;
    UserInputAnswers answers;

    /// 失败原因（通道缺失 / 校验不通过 / 登记失败）。
    /// ⚠ status 为非 Answered 时调用方【不应】自行编造答案继续 ——
    ///   那正是方案 C 要消除的行为。
    std::string error;

    bool answered() const { return status == QuestionStatus::Answered; }

    /// 取某个 question 的第一个答案（便捷读法）。
    std::string first_answer(const std::string& question_id) const;

    /// 把答案渲染成 "header: value" 多行文本。
    /// 🎯 对齐 answeredResult() 里的 `${question.header}: ${values.join(", ")}`
    std::string render(const std::vector<UserInputQuestion>& questions) const;
};

/// 执行一次完整的提问-等答复流程。
/// 全部失败路径都是 fail-closed: 拿不到用户答复时【绝不】伪造 Answered。
RunGatewayQuestionOutcome run_gateway_question(const RunGatewayQuestionParams& params);

}  // namespace tools
}  // namespace hunterclaw
