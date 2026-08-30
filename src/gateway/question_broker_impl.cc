// ============================================================================
// question_broker_impl.cc
// 见 question_broker_impl.h 的对齐说明。
// ============================================================================

#include "hunterclaw/gateway/question_broker_impl.h"

#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace gateway {

namespace {
std::shared_ptr<spdlog::logger> log() { return core::get_logger("question"); }
}  // namespace

GatewayQuestionBroker::GatewayQuestionBroker(std::shared_ptr<QuestionManager> manager,
                                           BroadcastFn broadcast,
                                           HasClientsFn has_clients)
    : manager_(std::move(manager)),
      broadcast_(std::move(broadcast)),
      has_clients_(std::move(has_clients)) {}

// ── request_question ────────────────────────────────────────────────────────

std::string GatewayQuestionBroker::request_question(
    const std::vector<UserInputQuestion>& questions, const std::string& session_key,
    const std::string& agent_id, const std::string& run_id, int64_t timeout_ms,
    const std::string& explicit_id) {
    if (!manager_) {
        // ⚠ 返回空串 → 编排层 fail-closed。不能返回假 id 让它去等 ——
        //   那会等满超时（最长 3600s）才失败。
        log()->error("question requested but manager is null");
        return {};
    }

    std::optional<std::string> opt_id;
    if (!explicit_id.empty()) opt_id = explicit_id;

    QuestionRecord record = manager_->create(questions, timeout_ms, opt_id);
    if (!explicit_id.empty() && record.id != explicit_id) {
        // 不该发生；发生了说明 create() 忽略了 explicit_id。
        // ★ 必须拦住: 编排层会拿 explicit_id 去占会话槽位，
        //   id 不一致时用户的纯文本答复会 resolve 到不存在的记录上。
        log()->error("question create ignored explicit id (want '{}' got '{}')",
                     explicit_id, record.id);
        return {};
    }
    if (!session_key.empty()) record.session_key = session_key;
    if (!agent_id.empty()) record.agent_id = agent_id;
    if (!run_id.empty()) record.run_id = run_id;

    const int64_t effective_timeout = record.expires_at_ms - record.created_at_ms;
    if (!manager_->register_record(record, effective_timeout)) {
        log()->error("question registration failed (id={})", record.id);
        return {};
    }

    // ── 广播，让客户端能看到这条待答 ────────────────────────────────────
    // ⚠⚠ 不广播 = 提问存在但没人知道 → 必然等满超时，而日志里只有一条
    //   "没有收到答复"，完全指不到"事件没发出去"这件事。
    //   与审批侧同一形态的静默失效，故 broadcast_ 为空时也要记 error。
    if (broadcast_) {
        broadcast_("question.requested", record.to_json());
    } else {
        log()->error("question created but no broadcast channel; "
                     "clients cannot see it (id={})", record.id);
    }

    log()->info("question requested: id={} session='{}' timeout={}ms questions={}",
                record.id, session_key, effective_timeout, questions.size());
    return record.id;
}

// ── wait_question_answer ────────────────────────────────────────────────────

tools::QuestionWaitResult GatewayQuestionBroker::wait_question_answer(
    const std::string& question_id, int64_t timeout_ms,
    const std::atomic<bool>* aborted) {
    tools::QuestionWaitResult out;
    if (!manager_) {
        out.status = QuestionStatus::Expired;
        return out;
    }
    // ⚠ timeout_ms<=0 时传 0，由 manager 沿用注册时的超时 —— 不要在这里
    //   换成默认值，否则等待窗口与记录的 expires_at_ms 不一致，
    //   会造成"刚好在边界上"的随机失败。
    //
    // ★ aborted 原样透传：不透传会让 chat.abort 无法打断提问等待，
    //   而这里的等待最长 3600s 且占着 main lane 唯一槽位。
    return manager_->await_answer(question_id, timeout_ms > 0 ? timeout_ms : 0, aborted);
}

// ── resolve / cancel ────────────────────────────────────────────────────────

bool GatewayQuestionBroker::resolve_question(const std::string& question_id,
                                           const UserInputAnswers& answers,
                                           const std::string& resolved_by) {
    if (!manager_) return false;
    const bool ok = manager_->resolve(
        question_id, answers, resolved_by.empty() ? std::nullopt
                                                  : std::make_optional(resolved_by));
    if (ok) broadcast_resolved(question_id, QuestionStatus::Answered, &answers);
    return ok;
}

bool GatewayQuestionBroker::cancel_question(const std::string& question_id,
                                          const std::string& resolved_by) {
    if (!manager_) return false;
    const bool ok = manager_->cancel(
        question_id, resolved_by.empty() ? std::nullopt
                                        : std::make_optional(resolved_by));
    if (ok) broadcast_resolved(question_id, QuestionStatus::Cancelled, nullptr);
    return ok;
}

bool GatewayQuestionBroker::has_question_clients() const {
    // ⚠ 回调缺失时返回 false（fail-closed）：宁可立刻失败也不要白等
    //   一个 900s 的超时。编排层会把它转成一条明确的 error。
    if (!has_clients_) return false;
    return has_clients_();
}

// ── broadcast_resolved ──────────────────────────────────────────────────────

void GatewayQuestionBroker::broadcast_resolved(const std::string& question_id,
                                              QuestionStatus status,
                                              const UserInputAnswers* answers) {
    if (!broadcast_) return;
    nlohmann::json event;
    event["id"] = question_id;
    event["status"] = tools::question_status_to_wire(status);
    // ⚠ answers 只在 answered 时带上 —— 对齐 QuestionResolvedEventSchema
    //   的 union 形状。带一个空 answers 会让终端把"已取消"渲染成
    //   "已作答但内容为空"。
    if (status == QuestionStatus::Answered && answers) {
        nlohmann::json a = nlohmann::json::object();
        nlohmann::json m = nlohmann::json::object();
        for (const auto& [qid, values] : answers->answers) {
            m[qid] = values;
        }
        a["answers"] = std::move(m);
        event["answers"] = std::move(a);
    }
    broadcast_("question.resolved", event);
}

}  // namespace gateway
}  // namespace hunterclaw
