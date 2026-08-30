// ============================================================================
// pending_question_registry.cc
// 见 pending_question_registry.h 的对齐说明。
// ============================================================================

#include "hunterclaw/tools/pending_question_registry.h"

#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() { return core::get_logger("question"); }

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

}  // namespace

// ── singleton ───────────────────────────────────────────────────────────────

PendingQuestionRegistry& PendingQuestionRegistry::instance() {
    static PendingQuestionRegistry inst;
    return inst;
}

std::shared_ptr<PendingQuestionRegistry::Entry> PendingQuestionRegistry::get_locked(
    const std::string& session_key) const {
    auto it = pending_.find(session_key);
    return it == pending_.end() ? nullptr : it->second;
}

void PendingQuestionRegistry::release_slot(const std::string& session_key,
                                          const std::string& question_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pending_.find(session_key);
    // ⚠ 必须比对 question_id 才删: 若本槽位已被【替补提问】占用
    //   （前一个提问 abort 后立刻起了新的），旧 Claim 的析构不能把
    //   新提问的槽位删掉。对齐 OpenClaw dispose() 里的
    //   `if (pendingAgentQuestions.get(sessionKey) === state)` 身份比对。
    if (it != pending_.end() && it->second && it->second->question_id == question_id) {
        pending_.erase(it);
    }
}

// ── Claim ───────────────────────────────────────────────────────────────────

bool PendingQuestionRegistry::Claim::is_resolving() const {
    if (!valid()) return false;
    auto& reg = PendingQuestionRegistry::instance();
    std::lock_guard<std::mutex> lk(reg.mu_);
    auto e = reg.get_locked(session_key_);
    if (!e || e->question_id != question_id_) return false;
    return e->cancel_requested || e->resolving;
}

bool PendingQuestionRegistry::Claim::is_cancellation_requested() const {
    if (!valid()) return false;
    auto& reg = PendingQuestionRegistry::instance();
    std::lock_guard<std::mutex> lk(reg.mu_);
    auto e = reg.get_locked(session_key_);
    if (!e || e->question_id != question_id_) return false;
    return e->cancel_requested;
}

void PendingQuestionRegistry::Claim::release() {
    if (!valid()) return;
    PendingQuestionRegistry::instance().release_slot(session_key_, question_id_);
    session_key_.clear();
    question_id_.clear();
}

// ── register_question ───────────────────────────────────────────────────────

PendingQuestionRegistry::Claim PendingQuestionRegistry::register_question(
    const std::string& session_key, const std::string& question_id,
    const std::vector<UserInputQuestion>& questions,
    std::shared_ptr<QuestionBroker> broker) {
    Claim claim;
    const std::string key = trim_copy(session_key);

    // ⚠ session_key 为空时【不登记】: 认领是按会话定位的，
    //   空 key 会让所有无会话上下文的提问挤在同一个槽位里互相顶掉。
    //   返回无效 Claim → 调用方仍可正常等待 gateway 答复（终端点选路径），
    //   只是失去"纯文本认领"这条捷径。
    if (key.empty() || question_id.empty()) {
        log()->warn("pending question not registered: empty session_key or question_id");
        return claim;
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto existing = get_locked(key);
    if (existing) {
        // 对齐 OpenClaw 的 "session already has a pending gateway question"
        log()->warn("session '{}' already has pending question '{}'; refusing '{}'",
                    key, existing->question_id, question_id);
        return claim;
    }

    auto e = std::make_shared<Entry>();
    e->question_id = question_id;
    e->session_key = key;
    e->questions = questions;
    e->broker = std::move(broker);
    pending_[key] = e;

    claim.session_key_ = key;
    claim.question_id_ = question_id;
    log()->info("pending question '{}' registered for session '{}'", question_id, key);
    return claim;
}

// ── claim_answer ────────────────────────────────────────────────────────────

bool PendingQuestionRegistry::claim_answer(const std::string& session_key,
                                          const std::string& text,
                                          const std::function<void()>& persist) {
    const std::string key = trim_copy(session_key);
    if (key.empty()) return false;

    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        e = get_locked(key);
        // 对齐 OpenClaw: `if (!state || state.cancelRequested || state.resolving) return false;`
        if (!e || e->cancel_requested || e->resolving) return false;
        // ★ 在锁内置位 resolving —— 这是幂等闸，必须先占再干活，
        //   否则两条并发消息会同时通过检查。
        e->resolving = true;
    }

    // ⚠ broker 缺失: 提问登记时通道还在、认领时没了。
    //   必须把 resolving 放回去并返回 false，让消息走正常 steer ——
    //   否则这条用户消息会被【静默吞掉】(既没答复也没进对话)。
    if (!e->broker) {
        std::lock_guard<std::mutex> lk(mu_);
        e->resolving = false;
        log()->error("claim failed: question broker unavailable (question '{}')",
                     e->question_id);
        return false;
    }

    const UserInputAnswers answers = build_user_input_answers(e->questions, text);

    // ★ 落库【只在确定要消费这条消息之后】做。
    //   见头文件说明: 提前落库会在认领回落时把这一轮记两遍。
    if (persist) {
        try {
            persist();
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(mu_);
            e->resolving = false;
            log()->error("claim failed while persisting turn: {}", ex.what());
            return false;
        }
    }

    const bool ok = e->broker->resolve_question(e->question_id, answers, "plain-text");
    if (!ok) {
        // ⚠ 记录已终态（用户在终端点了按钮，或已超时）。
        //   ★ 此时【返回 false】让消息回落 steer，且【不】复位 resolving:
        //     记录已终态说明这个提问的生命周期结束了，等待方要么已经
        //     拿到答复要么已经超时；把它当"还能认领"只会让下一条消息
        //     再撞一次同样的失败。
        //     对齐 OpenClaw: isTerminalAgentQuestionError → return false。
        log()->info("claim fell through: question '{}' already terminal", e->question_id);
        return false;
    }

    log()->info("claimed plain-text answer for question '{}' (session '{}')",
                e->question_id, key);
    return true;
}

// ── cancel_for_session ──────────────────────────────────────────────────────

bool PendingQuestionRegistry::cancel_for_session(const std::string& session_key,
                                               const std::string& resolved_by) {
    const std::string key = trim_copy(session_key);
    if (key.empty()) return false;

    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        e = get_locked(key);
        // 对齐 OpenClaw: `if (!state || state.resolving) return false;`
        //   ★ 注意这里【不】检查 cancel_requested —— 重复取消是幂等的，
        //     而 resolving 已经覆盖了"取消进行中"的情况。
        if (!e || e->resolving) return false;
        e->cancel_requested = true;
        e->resolving = true;
    }

    if (!e->broker) {
        std::lock_guard<std::mutex> lk(mu_);
        e->cancel_requested = false;
        e->resolving = false;
        return false;
    }

    const bool ok = e->broker->cancel_question(e->question_id, resolved_by);
    if (!ok) {
        // 已终态视作取消成功（对齐 isTerminalAgentQuestionError → return true）:
        // 目标状态已经达成，报失败会让调用方做多余的补偿动作。
        log()->info("cancel: question '{}' already terminal ({})", e->question_id, resolved_by);
        return true;
    }
    log()->info("cancelled pending question '{}' ({})", e->question_id, resolved_by);
    return true;
}

// ── has_pending / reset ─────────────────────────────────────────────────────

bool PendingQuestionRegistry::has_pending(const std::string& session_key) const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<bool>(get_locked(trim_copy(session_key)));
}

void PendingQuestionRegistry::reset_for_testing() {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();
}

}  // namespace tools
}  // namespace hunterclaw
