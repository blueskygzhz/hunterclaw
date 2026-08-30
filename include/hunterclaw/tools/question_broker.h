// ============================================================================
// question_broker.h — 用户提问通道（进程级）
//
// 🎯 严格对齐 OpenClaw 的三个 gateway RPC:
//     question.request      → request_question()
//     question.waitAnswer   → wait_question_answer()
//     question.resolve      → resolve_question() / cancel_question()
//   （src/agents/harness/gateway-question.ts 全程只用这三个方法）
//
// ★★ 为什么是「接口 + 进程级单例」而不是把 gateway 句柄传进 ToolContext:
//   与 ToolApprovalBroker / ToolApprovalRuntime 同一套理由，逐条适用:
//     ① ToolContext 按值拷贝、每会话一个 ToolManager → N 份待答列表，
//        而客户端只有一个 question.resolve 入口 → 其余列表永远等不到答复
//        （表现为随机超时而非报错）。
//     ② tools 不能反向依赖 gateway（gateway → agents → tools 已成链），
//        接口留在 tools、实现由 gateway 侧注入是唯一不成环的做法。
//
// ⚠⚠ 与 ToolApprovalBroker 【并列而非合并】:
//   审批的回值是三选一的决策枚举，提问的回值是【结构化答案】。
//   把提问硬塞进审批通道会得到"用 allow-once 表示用户回了 PDF"这种
//   语义崩塌 —— 这正是方案 A/C 的分界点。两者共用同一 gateway 连接，
//   但记录类型、列表、resolve 入口都必须分开。
// ============================================================================

#pragma once

#include "hunterclaw/tools/user_input_bridge.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

// ── QuestionStatus（对齐 QuestionStatusSchema）─────────────────────────────
enum class QuestionStatus {
    Pending,
    Answered,
    Cancelled,
    Expired,
};

const char* question_status_to_wire(QuestionStatus s);
std::optional<QuestionStatus> question_status_from_wire(const std::string& s);

// ── QuestionWaitResult（对齐 QuestionWaitAnswerResultSchema）───────────────
//
// ⚠ answers 只在 status == Answered 时有意义。
//   OpenClaw 用 discriminated union 表达这个不变量；C++ 侧用
//   "status + optional answers" 表达，由 manager 维护该不变量
//   （与 ExecApprovalRecord 的 decision 同一处理）。
struct QuestionWaitResult {
    QuestionStatus status = QuestionStatus::Pending;
    UserInputAnswers answers;

    bool answered() const { return status == QuestionStatus::Answered; }
};

// ── QuestionBroker ──────────────────────────────────────────────────────────
class QuestionBroker {
public:
    virtual ~QuestionBroker() = default;

    /// 登记一次提问并广播给终端。
    ///
    /// @param explicit_id 指定 id（用于"先占位再登记"的两阶段流程）；
    ///        空则由实现方生成。
    /// @return 提问 id；空字符串表示登记失败（含"没有任何可应答的客户端"）
    ///
    /// ⚠ 必须【同步】完成登记再返回：调用方随后会立刻把这个 id 注册进
    ///   PendingQuestionRegistry 作为认领目标。若登记是异步的，用户
    ///   在登记完成前回的那条消息会被认领成功却无处 resolve
    ///   —— OpenClaw 为此专门做了 registration promise + bufferedAnswers
    ///   （gateway-question.ts:217-246）。同步返回从根上避免这个竞态。
    virtual std::string request_question(const std::vector<UserInputQuestion>& questions,
                                        const std::string& session_key,
                                        const std::string& agent_id,
                                        const std::string& run_id,
                                        int64_t timeout_ms,
                                        const std::string& explicit_id = {}) = 0;

    /// 等待答复。
    ///
    /// @param aborted 运行取消标志。非空且被置位时【立即结束等待】。
    ///        🎯 对齐 gateway-question.ts:352-358 的 onAbort ——
    ///        它在 abort 时同步 claim.dispose() 并 best-effort cancel。
    ///
    /// ⚠⚠ 与审批同理: 没有这个参数, chat.abort 之后等待方仍会挂满
    ///   timeout（ask_user 默认 900s！比审批的 120s 长 7.5 倍），
    ///   而该等待占着 main lane 的唯一槽位。
    ///
    /// @return status=Pending 表示【等待窗口内没等到】(调用方应随后 cancel)
    virtual QuestionWaitResult wait_question_answer(
        const std::string& question_id, int64_t timeout_ms,
        const std::atomic<bool>* aborted = nullptr) = 0;

    /// 用一组答案收敛提问（认领通道用）。
    /// @return false 表示记录已终态或不存在（调用方应视为"认领失败"，
    ///         把消息交回正常 steer 流程）
    virtual bool resolve_question(const std::string& question_id,
                                  const UserInputAnswers& answers,
                                  const std::string& resolved_by) = 0;

    /// 取消提问。
    /// @return false 表示记录已终态或不存在
    virtual bool cancel_question(const std::string& question_id,
                                const std::string& resolved_by) = 0;

    /// 是否存在能应答的客户端。
    /// 🎯 对齐 hasExecApprovalClients() 在 approval 侧的作用:
    ///   没有应答面时【立即失败】而不是白等一个超时。
    ///
    /// ⚠ 这条判断对 ask_user 尤其重要: 默认超时 900s，
    ///   在无人应答的部署里等于把 lane 占死 15 分钟。
    virtual bool has_question_clients() const = 0;
};

// ── QuestionRuntime ─────────────────────────────────────────────────────────
class QuestionRuntime {
public:
    static QuestionRuntime& instance();

    /// 注入提问通道（由 main 在 gateway 就绪后调用一次）。
    ///
    /// ⚠⚠ 只允许设置【一次】—— 与 ToolApprovalRuntime 同理:
    ///   后设的会让先前已登记的提问变成孤儿（等待方在旧 broker 上等，
    ///   答复落到新 broker），那批提问必然等满超时。
    bool set_broker(std::shared_ptr<QuestionBroker> broker);

    /// 取当前提问通道。
    /// ★ 返回 shared_ptr: 调用方会持有它跨越一次可长达 900s 的等待。
    std::shared_ptr<QuestionBroker> broker() const;

    bool has_broker() const;

    /// 仅供测试。
    void reset_for_testing();

private:
    QuestionRuntime() = default;

    mutable std::mutex mu_;
    std::shared_ptr<QuestionBroker> broker_;
};

}  // namespace tools
}  // namespace hunterclaw
