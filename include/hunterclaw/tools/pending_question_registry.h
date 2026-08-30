// ============================================================================
// pending_question_registry.h — 「下一条用户消息认领为答复」的会话级槽位
//
// 🎯 严格对齐 OpenClaw src/agents/harness/gateway-question.ts:
//     pendingAgentQuestions                  → PendingQuestionRegistry
//     registerPendingAgentQuestion()         → register()
//     claimPendingAgentQuestionAnswer()      → claim_answer()
//     cancelPendingAgentQuestionForSession() → cancel_for_session()
//
// ★★ 这是方案 C 的核心：它让「用户在聊天框里随手回一句话」不走
//   steer（新 user turn），而是被【认领】成某个 pending question 的答复。
//   没有这一层，用户的回复只会变成一条新的 user 消息注入对话，
//   而正在阻塞等答复的那个工具调用永远等不到东西。
//
// ── 与 OpenClaw 的一处【有意简化】────────────────────────────────────────
//   OpenClaw 有 `registration: Promise` + `bufferedAnswers` 两个字段，
//   用来处理"提问尚未在 gateway 登记完成，用户的回复就已经到了"
//   （gateway-question.ts:217-246）。那是因为它的 question.request 是
//   异步 RPC，注册与登记天然并发。
//
//   本实现里 QuestionBroker::request_question() 是【同步】的（见其注释），
//   调用方拿到 id 之后才 register() —— 于是"登记未完成就被认领"这个
//   状态在时间上不存在，buffered 路径也就没有对应物。
//
//   ⚠ 这个简化【依赖 request_question 保持同步】。若哪天把它改成异步，
//     必须把 buffered 路径补回来，否则用户的第一条回复会被认领成功
//     却无处 resolve —— 表现为"用户明明答了，agent 还在等"。
// ============================================================================

#pragma once

#include "hunterclaw/tools/question_broker.h"
#include "hunterclaw/tools/user_input_bridge.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

class PendingQuestionRegistry {
public:
    static PendingQuestionRegistry& instance();

    // ── RAII 句柄 ────────────────────────────────────────────────────────
    //
    // ★ 用 RAII 而非手动 dispose(): 提问的等待路径有多个 return 分支
    //   （答到 / 超时 / 取消 / abort / 异常），任一分支漏掉释放都会让
    //   该会话【永久占着槽位】—— 之后所有提问都注册失败，且没有任何
    //   报错，只表现为"ask_user 再也弹不出来了"。
    class Claim {
    public:
        Claim() = default;
        Claim(const Claim&) = delete;
        Claim& operator=(const Claim&) = delete;
        Claim(Claim&& other) noexcept { swap(other); }
        Claim& operator=(Claim&& other) noexcept {
            if (this != &other) {
                release();
                swap(other);
            }
            return *this;
        }
        ~Claim() { release(); }

        bool valid() const { return !session_key_.empty(); }

        /// 是否已有人在收敛（认领中 / 取消中）。
        /// 🎯 对齐 claim.isResolving() —— 用于"答复已在路上，别再推送提示词"。
        bool is_resolving() const;

        /// 是否被请求取消（同一条入站消息改走了别的路径）。
        /// 🎯 对齐 claim.isCancellationRequested()
        bool is_cancellation_requested() const;

        /// 提前释放槽位（abort 路径需要同步让位，好让替补提问能注册）。
        /// 🎯 对齐 gateway-question.ts:352-357 的 onAbort 里的 claim.dispose()
        void release();

    private:
        friend class PendingQuestionRegistry;
        void swap(Claim& other) noexcept {
            std::swap(session_key_, other.session_key_);
            std::swap(question_id_, other.question_id_);
        }
        std::string session_key_;
        std::string question_id_;
    };

    /// 把一个提问登记为本会话下一条纯文本消息的认领目标。
    ///
    /// @return 无效 Claim 表示【该会话已有 pending question】。
    ///
    /// ⚠ 一个会话同时只允许一个 pending question（对齐 OpenClaw 的
    ///   `throw new Error("session already has a pending gateway question")`）。
    ///   允许多个会让"下一条消息该认领哪一个"无法确定 —— 而认领错了
    ///   的后果是把"用 PDF"答到了"是否发送邮件"上。
    ///   ★ 这里返回无效 Claim 而不抛异常: 调用方（工具）应据此回退到
    ///     "不提问，直接返回让模型自行判断"，而不是让整个 run 崩掉。
    Claim register_question(const std::string& session_key,
                            const std::string& question_id,
                            const std::vector<UserInputQuestion>& questions,
                            std::shared_ptr<QuestionBroker> broker);

    /// 尝试把一条入站纯文本消息认领为答复。
    ///
    /// 🎯 对齐 claimPendingAgentQuestionAnswer()
    ///
    /// @param persist 认领【确定要消费这条消息之后】才调用的落库回调。
    ///        ⚠ 不能更早调用: 认领失败时消息会回落到正常 steer 流程，
    ///          提前落库会让这一轮被记两遍
    ///          （OpenClaw 原注释: "persisting earlier double-records the
    ///           turn when the claim falls through"）。
    /// @return true = 已认领（调用方【不要】再 steer）
    bool claim_answer(const std::string& session_key, const std::string& text,
                      const std::function<void()>& persist = nullptr);

    /// 在同一条入站消息改走别的路径之前取消提问。
    ///
    /// 🎯 对齐 cancelPendingAgentQuestionForSession()
    ///   典型场景: 用户回的是【图片】而非文本 —— 图片无法映射成答案，
    ///   但它显然是对提问的回应，继续挂着那个提问只会让用户困惑。
    ///
    /// @return true = 已取消
    bool cancel_for_session(const std::string& session_key,
                            const std::string& resolved_by);

    /// 该会话是否有 pending question（诊断用）。
    bool has_pending(const std::string& session_key) const;

    /// 仅供测试。
    void reset_for_testing();

private:
    PendingQuestionRegistry() = default;

    struct Entry {
        std::string question_id;
        std::string session_key;
        std::vector<UserInputQuestion> questions;
        std::shared_ptr<QuestionBroker> broker;
        bool cancel_requested = false;
        // ★ resolving 是【幂等闸】: 认领与取消都会置位，置位后其他路径
        //   一律拒绝。没有它，用户连发两条消息会触发两次 resolve，
        //   第二次拿到"已终态"错误后可能把消息误判为"未认领"而再走
        //   steer —— 同一条内容既做了答复又做了新指令。
        bool resolving = false;
    };

    mutable std::mutex mu_;
    // session_key -> Entry（每会话至多一个）
    std::map<std::string, std::shared_ptr<Entry>> pending_;

    std::shared_ptr<Entry> get_locked(const std::string& session_key) const;
    void release_slot(const std::string& session_key, const std::string& question_id);

    friend class Claim;
};

}  // namespace tools
}  // namespace hunterclaw
