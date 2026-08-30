// ============================================================================
// interrupt_coordinator.h — 工具中断的等待与恢复中枢
//
// ★★ 它是 ReactEngine 与产出 interrupt 的工具（当前是 remote_a2a）之间
//   【唯一】的接缝。两边都不直接认识对方:
//     · ReactEngine 只知道 "有个 interrupt，我把 prompt 投出去、等答复、
//       然后请你按 kind 恢复"
//     · remote_a2a 只知道 "我注册了 kind='remote_a2a.input_required'
//       的恢复函数"
//   ⚠ 让 ReactEngine 直接调 remote_a2a 会形成 react → remote_a2a 依赖，
//     而 remote_a2a 的工具又要经 ToolManager 被 react 调用 —— 成环。
//
// ── 为什么是「run 内等待」而不是「结束 run + 跨 run 恢复」───────────────
//   跨 run 恢复需要:
//     ① interrupt 跨 run 持久化（重启后作废还是恢复？）
//     ② 恢复时起一个"第一步不是问 LLM 而是先 resume"的特殊 run
//     ③ transcript 里那个 tool_call 先写一个占位 result，恢复后再追加
//   三样都是新机制，且 ③ 会让 prompt cache 在每次中断处失效。
//
//   run 内等待则:
//     · transcript 天然连贯 —— tool_call 拿到的就是【恢复后的真实结果】
//     · 用户的答复被认领消费，不产生独立 user turn（满足"不重发消息"）
//     · 不需要任何持久化
//   代价: 等待期间占着 main lane 的槽位。这与 OpenClaw 的 ask_user
//   完全同构（它也是在 run 内 await），是可接受的已知代价。
//
//   ⚠ 因此 aborted 透传是【必须】的，不是优化: 没有它，用户点停止后
//     仍要等满 kDefaultInterruptWaitMs。
// ============================================================================

#pragma once

#include "hunterclaw/tools/tool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace hunterclaw {
namespace tools {

/// 默认等待用户答复的时长。
/// ★ 与 OpenClaw ask_user 的 900s 一致 —— 这是在问人一句话，不是等一次点按。
///   ⚠ 太短会让用户刚看到消息就已经超时。
constexpr int64_t kDefaultInterruptWaitMs = 900000;

/// abort 轮询间隔。
/// ★ aborted 由外部 atomic 置位，置位方不持本类 mutex 因而无法 notify，
///   只能轮询。50ms 在一次最长 900s 的等待里可忽略，对用户已是"立即"。
constexpr int64_t kInterruptAbortPollMs = 50;

// ── ResumeOutcome ───────────────────────────────────────────────────────────
struct ResumeOutcome {
    bool success = false;
    /// 恢复后的结果文本，会【替换】原 tool result 的内容。
    std::string text;
    /// 失败原因（success=false 时）
    std::string error;
    /// 恢复后【又一次】中断（多轮确认场景）。
    ///
    /// ⚠ ReactEngine 收到它【不递归等待】，而是把它当作本次 tool result
    ///   的新 interrupt 交给下一次循环处理。
    ///   ★ 递归会让单次工具调用无界占用 main lane，且中间的 resume 调用
    ///     无法被 abort 打断。
    std::optional<ToolInterrupt> next_interrupt;
};

/// 恢复函数：把用户答复交给产出 interrupt 的子系统，让它继续执行。
///
/// @param interrupt 原始中断（含 resume_token）
/// @param user_text 用户答复原文
/// @param aborted   运行取消标志
using ResumeHandler = std::function<ResumeOutcome(const ToolInterrupt& interrupt,
                                                 const std::string& user_text,
                                                 const std::atomic<bool>* aborted)>;

class InterruptCoordinator {
public:
    static InterruptCoordinator& instance();

    // ── 恢复函数注册（子系统侧）──────────────────────────────────────────
    /// 注册某个 kind 的恢复函数。
    /// ⚠ 同一 kind 重复注册会被拒绝并保留先注册的那个 ——
    ///   后注册的会让已在等待的中断恢复到错误的子系统上。
    bool register_resume_handler(const std::string& kind, ResumeHandler handler);

    bool has_resume_handler(const std::string& kind) const;

    // ── 等待槽位（ReactEngine 侧）────────────────────────────────────────
    //
    // ★ RAII: 等待路径有多个 return 分支（答到 / 超时 / abort / 恢复失败），
    //   任一分支漏掉释放都会让该会话【永久占着槽位】，之后所有中断
    //   立刻失败，且没有任何报错 —— 只表现为"再也问不出来了"。
    class Waiter {
    public:
        Waiter() = default;
        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        Waiter(Waiter&& o) noexcept { swap(o); }
        Waiter& operator=(Waiter&& o) noexcept {
            if (this != &o) { release(); swap(o); }
            return *this;
        }
        ~Waiter() { release(); }

        bool valid() const { return !session_key_.empty(); }

        /// 阻塞等待用户答复。
        /// @return 无值 = 超时 / abort / 槽位已失效
        std::optional<std::string> wait(int64_t timeout_ms,
                                       const std::atomic<bool>* aborted);

        void release();

    private:
        friend class InterruptCoordinator;
        void swap(Waiter& o) noexcept {
            std::swap(session_key_, o.session_key_);
            std::swap(seq_, o.seq_);
        }
        std::string session_key_;
        uint64_t seq_ = 0;
    };

    /// 占住会话槽位，准备等用户答复。
    ///
    /// @return 无效 Waiter 表示该会话已有中断在等。
    ///
    /// ⚠ 一个会话同时只允许一个中断在等。允许多个会让"下一条消息该给谁"
    ///   无法确定 —— 而给错的后果是把"用 PDF"答到了"是否发送邮件"上。
    Waiter begin_wait(const std::string& session_key);

    // ── 答复投递（MessageDispatcher 侧）─────────────────────────────────
    /// 把一条入站纯文本消息认领为中断答复。
    /// @return true = 已认领（调用方【不要】再走正常 steer / 新 turn）
    ///
    /// ⚠ 认领失败必须让消息回落正常流程，绝不能吞掉。
    bool claim_answer(const std::string& session_key, const std::string& text);

    bool has_pending(const std::string& session_key) const;

    /// 执行恢复（ReactEngine 侧，拿到答复之后调用）。
    /// ⚠ handler 缺失时返回 success=false 而不是抛异常 ——
    ///   ReactEngine 要把失败写进 tool result 让模型知道，而不是崩掉 run。
    ResumeOutcome resume(const ToolInterrupt& interrupt, const std::string& user_text,
                         const std::atomic<bool>* aborted) const;

    /// 渲染追问提示词（含选项编号与回复指引）。
    /// ★ 编号 1-based，且与 match_option() 的解析同一基准 ——
    ///   两处基准不一致会让用户选 A 系统记 B，而且不报错。
    static std::string format_prompt(const ToolInterrupt& interrupt);

    /// 把用户答复匹配到某个选项。
    /// @return 匹配到的选项原文；无选项或匹配不上时返回 trim 后的原文
    static std::string match_option(const ToolInterrupt& interrupt,
                                   const std::string& answer);

    void reset_for_testing();

private:
    InterruptCoordinator() = default;

    struct Slot {
        uint64_t seq = 0;
        bool answered = false;
        std::string answer;
        std::condition_variable cv;
    };

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Slot>> slots_;
    std::map<std::string, ResumeHandler> handlers_;
    uint64_t next_seq_ = 1;

    friend class Waiter;
};

}  // namespace tools
}  // namespace hunterclaw
