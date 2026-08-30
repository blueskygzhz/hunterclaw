// ============================================================================
// question_manager.h — 用户提问记录的生命周期管理
//
// 🎯 严格对齐 OpenClaw 的 question 记录语义
//    (packages/gateway-protocol/src/schema/questions.ts +
//     question.request / waitAnswer / resolve 三个 server method)
//
// ★★ 结构上【刻意照抄 ExecApprovalManager】: 同一套
//   pending_ + cv + grace period + abort 轮询。
//
//   ⚠ 但【不复用 ExecApprovalManager 本身】—— 这一点与"审批只能有一个
//     store"的结论并不矛盾，理由是两者的【记录类型与决策形状不同】:
//       · approval: 回值是 allow-once / allow-always / deny 三选一
//       · question: 回值是 Record<questionId, string[]> 结构化答案
//     硬塞进同一个 store 会得到"用 allow-once 表示用户回了 PDF"这种
//     语义崩塌。OpenClaw 也是分开的两套 schema、两套 server method。
//
//   ★ 真正需要"一个 store"的是【同一类记录的多个入口】: approval 有
//     approval.resolve 与 plugin.approval.resolve 两个入口，所以必须
//     共用一个列表。question 只有 question.resolve 一个入口，天然唯一。
// ============================================================================

#pragma once

#include "hunterclaw/tools/user_input_bridge.h"
#include "hunterclaw/tools/question_broker.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;
using tools::QuestionStatus;
using tools::QuestionWaitResult;
using tools::UserInputAnswers;
using tools::UserInputQuestion;

// ── 常量 ────────────────────────────────────────────────────────────────────
static constexpr int64_t DEFAULT_QUESTION_TIMEOUT_MS = 900000;   // 900s (ask_user 默认)
static constexpr int64_t MAX_QUESTION_TIMEOUT_MS = 3600000;      // 3600s

// 已收敛记录的保留期，供迟到的 await_answer 取用。
// ★ 与 approval 侧 RESOLVED_ENTRY_GRACE_MS 同一理由: 收敛与等待方返回
//   之间有窗口，立刻删记录会让等待方读到"不存在"而误判为超时。
static constexpr int64_t QUESTION_RESOLVED_ENTRY_GRACE_MS = 15000;

// abort 轮询间隔。
// ★ aborted 由外部 atomic 置位，置位方不持本类 mutex 因而无法 notify，
//   只能轮询。50ms 在一次最长 3600s 的等待里可忽略，而对用户已是"立即"。
static constexpr int64_t kQuestionAbortPollIntervalMs = 50;

// ── QuestionRecord（对齐 QuestionRecordSchema）─────────────────────────────
struct QuestionRecord {
    std::string id;
    std::vector<UserInputQuestion> questions;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> run_id;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    QuestionStatus status = QuestionStatus::Pending;
    // ⚠ 不变量: 仅当 status == Answered 时有值。由本类维护。
    std::optional<UserInputAnswers> answers;
    std::optional<std::string> resolved_by;

    /// 调用方 metadata（best-effort，用于审计）
    std::optional<std::string> requested_by_conn_id;
    std::optional<std::string> requested_by_device_id;
    std::optional<std::string> requested_by_client_id;

    /// 线上形状（对齐 QuestionRecordSchema，含 questionId 字段名转换）
    json to_json() const;
};

// ── QuestionIdLookupResult ──────────────────────────────────────────────────
struct QuestionIdLookupResult {
    enum class Kind { Exact, Prefix, Ambiguous, None };
    Kind kind = Kind::None;
    std::string id;
    std::vector<std::string> ids;
};

// ── QuestionManager ─────────────────────────────────────────────────────────
class QuestionManager {
public:
    QuestionManager() = default;
    ~QuestionManager() = default;
    QuestionManager(const QuestionManager&) = delete;
    QuestionManager& operator=(const QuestionManager&) = delete;

    /// 创建记录（不入表）。timeout_ms 会被钳到 [1, MAX_QUESTION_TIMEOUT_MS]。
    QuestionRecord create(const std::vector<UserInputQuestion>& questions,
                          int64_t timeout_ms,
                          const std::optional<std::string>& explicit_id = std::nullopt);

    /// 入表。id 已存在且未收敛 → 幂等返回 true；已收敛 → false。
    bool register_record(const QuestionRecord& record, int64_t timeout_ms);

    /// 用答案收敛。
    /// @return false = 记录不存在或已终态
    bool resolve(const std::string& record_id, const UserInputAnswers& answers,
                 const std::optional<std::string>& resolved_by = std::nullopt);

    /// 取消。
    bool cancel(const std::string& record_id,
                const std::optional<std::string>& resolved_by = std::nullopt);

    /// 标记过期（无应答面时立即使用）。
    bool expire(const std::string& record_id,
                const std::optional<std::string>& resolved_by = std::nullopt);

    /// 阻塞等待答复。
    ///
    /// @param timeout_ms 0 = 用注册时的超时
    /// @param aborted 非空且被置位时【立即结束等待】
    /// @return status=Pending 表示窗口内未收敛（调用方应随后 cancel）；
    ///         记录不存在也返回 Pending 以外的终态（Expired）
    QuestionWaitResult await_answer(const std::string& record_id, int64_t timeout_ms = 0,
                                   const std::atomic<bool>* aborted = nullptr);

    std::optional<QuestionRecord> get_snapshot(const std::string& record_id) const;

    /// 列出未收敛且未过期的记录。
    /// ⚠ 已收敛的记录在 grace 期内仍留在表里（供迟到的 await），
    ///   但【不能】出现在待答列表里 —— 否则 UI 会一直显示已答过的条目。
    std::vector<QuestionRecord> list_pending() const;

    /// 精确/前缀查找（对齐 approval 侧 lookup_pending_id）。
    QuestionIdLookupResult lookup_pending_id(const std::string& input) const;

    bool has_pending() const;

private:
    struct PendingEntry {
        QuestionRecord record;
        std::condition_variable cv;
        bool settled = false;
        int64_t timeout_ms = 0;
    };

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<PendingEntry>> pending_;

    /// 统一的终态写入。
    /// ★ resolve / cancel / expire 三条路径共用 —— 三处各写一遍必然漂移，
    ///   而漂移点是"answers 只在 Answered 时有值"这条不变量。
    bool settle(const std::string& record_id, QuestionStatus status,
                const std::optional<UserInputAnswers>& answers,
                const std::optional<std::string>& resolved_by);

    static int64_t now_ms();
    static std::string generate_uuid();
};

// ── question_manager_instance ───────────────────────────────────────────────
// 进程级唯一的提问 store。
//
// ★★ 为什么必须唯一（与 approval 的"一个 store"同理）:
//   客户端只有【一个】question.resolve 入口。存在两个 store 时，
//   答复落到哪个取决于实现细节，另一个 store 里的提问永远等不到答复
//   —— 表现为随机超时而非报错，是最难定位的一类故障。
//
// ⚠ 与 ExecApprovalManager 的持有方式【有意不同】: 后者挂在 GatewayServer
//   上（server->exec_approval_manager()）。这里用函数内 static 而不塞进
//   GatewayServer，原因是 main.cc 需要在构造 broker 时拿到它，而 broker
//   的注入点在 gateway->start() 之后、AgentInstance 创建之前 ——
//   走 server 访问器要额外改 gateway.h/gateway.cc 两处成员初始化，
//   而"唯一性"这个真正的约束用函数内 static 已经完全满足。
std::shared_ptr<QuestionManager> question_manager_instance();

}  // namespace gateway
}  // namespace hunterclaw
