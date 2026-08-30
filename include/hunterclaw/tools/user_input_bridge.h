// ============================================================================
// user_input_bridge.h — 结构化提问 与 自由文本答复 之间的双向桥
//
// 🎯 严格对齐 OpenClaw src/agents/harness/user-input-bridge.ts
//
// ★★ 这一层【买到的是什么】：让"用户在聊天里随手回一句话"能被精确地
//   映射回某个 question 的某个选项。没有它，方案 C 就退化成方案 A ——
//   只能拿到一个布尔决策，拿不到"用户选了哪个/说了什么"。
//
// ★ 放在 tools/ 而不是 agents/ 的原因（与 OpenClaw 的目录不同，是有意的）：
//   本层的两端消费者分别是
//     · remote_a2a / ask_user 工具        （注册提问）—— hunterclaw_remote_a2a → tools
//     · auto_reply/message_dispatcher     （认领答复）—— hunterclaw_agents    → tools
//     · gateway 侧 QuestionManager        （落库与广播）—— hunterclaw_gateway → agents → tools
//   放在 tools/ 是三者唯一的公共下游。放在 agents/ 会让 remote_a2a 反向
//   依赖 agents，而 agents 已经依赖 remote_a2a —— 直接成环。
// ============================================================================

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

// ── 协议约束（逐条对齐 gateway-protocol/schema/questions.ts）──────────────
//
// ⚠ 这些上限【不是】随手取的数字，是 UI 契约：
//   header 超过 12 字会把终端的 chip/tag 撑破行；options 超过 4 个
//   在按钮式渲染下放不下（buildAgentHarnessQuestionPresentation 只在
//   1 个 question 且 2~4 个 option 时才发按钮，否则退化为纯文本）。
//   在这里挡住比让终端截断好 —— 截断会让"用户看到的选项"与
//   "服务端认的选项"不一致，而答复是按文本匹配的。
constexpr size_t kQuestionHeaderMaxLength = 12;   // QuestionHeaderSchema maxLength
constexpr size_t kQuestionOptionsMaxItems = 4;    // options maxItems
constexpr size_t kQuestionsMinItems = 1;          // questions minItems
constexpr size_t kQuestionsMaxItems = 3;          // questions maxItems

// ── UserInputOption（对齐 AgentHarnessUserInputOption）─────────────────────
struct UserInputOption {
    std::string label;                    // NonEmptyString
    std::optional<std::string> description;
};

// ── UserInputQuestion（对齐 AgentHarnessUserInputQuestion）────────────────
//
// ★ 字段名 `id` 对应线上协议的 `questionId`。
//   OpenClaw 在 harness 内部叫 id、在 gateway 线上叫 questionId，
//   两处靠 `{ questionId: id, ...rest }` 互转（gateway-question.ts:299）。
//   这里保持同一分工：本结构是【内部形状】，线上形状由 to_wire() 产出。
struct UserInputQuestion {
    std::string id;          // ^[a-z][a-z0-9_]*$
    std::string header;      // ≤ 12 chars
    std::string question;    // NonEmptyString
    bool multi_select = false;
    bool is_other = false;   // 允许用户答"选项之外的内容"
    bool is_secret = false;  // 答复可能含敏感信息 → 提示词里加警告
    std::vector<UserInputOption> options;
};

// ── UserInputAnswers（对齐 AgentHarnessUserInputAnswers）──────────────────
//
// ⚠ value 是 vector 而非单值：multiSelect 时一个 question 有多个答案。
//   即使单选也用 vector（0 个 = 未作答），这样"未作答"与"答了空串"
//   在结构上可区分 —— 前者是 `[]`，后者不会产生（空串被 normalize 丢弃）。
struct UserInputAnswers {
    std::map<std::string, std::vector<std::string>> answers;
};

// ── 提示词渲染 ──────────────────────────────────────────────────────────────
struct UserInputPromptOptions {
    std::optional<std::string> intro;           // 默认 "Agent needs input:"
    std::optional<std::string> secret_warning;
    std::optional<std::string> other_label;
};

/// 渲染给用户看的提问文本。
/// 逐行对齐 formatAgentHarnessUserInputPrompt()：
///   多问题时带 "1. " 序号，单问题时不带；选项按 1-based 编号列出。
///
/// ★ 编号必须与 normalize_user_input_answer() 的解析【同一套】：
///   用户回 "2" 时必须落到这里印出的第 2 个选项上。两处编号基准
///   不一致（一边 0-based 一边 1-based）会让用户选 A 系统记 B，
///   而且不报错。
std::string format_user_input_prompt(const std::vector<UserInputQuestion>& questions,
                                     const UserInputPromptOptions& options = {});

/// 附在提示词末尾的"怎么回"指引（对齐 questionReplyGuidance）。
std::string question_reply_guidance(const std::vector<UserInputQuestion>& questions);

// ── 答复解析 ────────────────────────────────────────────────────────────────

/// 把一条自由文本归一化成某个 question 的合法答案。
/// 逐条对齐 normalizeAgentHarnessUserInputAnswer()：
///   ① 纯数字 → 按 1-based 取选项（转 0-based 只在取数组那一刻做）
///   ② 全等 label（大小写不敏感）→ 取该 label 的【原始大小写】
///   ③ 有固定选项且 !is_other → nullopt（拒绝选项外的自由文本）
///   ④ 其余 → trim 后的原文（空则 nullopt）
///
/// ★ ③ 是这套设计的关键：选项固定时【不接受】自由文本。
///   否则用户回一句"随便"会被当成有效答案传给远端，
///   而远端只认那几个枚举值。
std::optional<std::string> normalize_user_input_answer(const std::string& answer,
                                                       const UserInputQuestion& question);

/// 把一条用户消息拆成全部 question 的答案。
/// 逐条对齐 buildAgentHarnessUserInputAnswers()：
///   · 单 question → 整条文本归一化后作为唯一答案
///   · 多 question → 先试 "key: value" 形式（key 可为 id / header /
///     question 原文 / 1-based 序号），再回落到【按行位置】对应
///
/// ⚠ 位置回落时必须保留空行：跳过一个答案会让后面所有答复整体
///   错位一格（用户空一行表示"这个不答"，吃掉空行就变成
///   "第 2 个答案被记到第 1 个问题上"）。
UserInputAnswers build_user_input_answers(const std::vector<UserInputQuestion>& questions,
                                          const std::string& input_text);

/// 空答复（对齐 emptyAgentHarnessUserInputAnswers）。
UserInputAnswers empty_user_input_answers();

// ── 校验 ────────────────────────────────────────────────────────────────────

/// 校验一组 question 是否满足协议约束。
/// @return 空字符串表示通过；否则返回人可读的拒绝原因
///
/// ⚠ 在【注册前】挡住，而不是等 gateway 或终端去挡：
///   一旦 record 落库并广播出去，终端已经渲染了，再报错就晚了。
std::string validate_user_input_questions(const std::vector<UserInputQuestion>& questions);

/// question id 是否合法（^[a-z][a-z0-9_]*$）。
bool is_valid_question_id(const std::string& id);

}  // namespace tools
}  // namespace hunterclaw
