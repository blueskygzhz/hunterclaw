// ============================================================================
// question_manager.cc
// 见 question_manager.h 的对齐说明。
// ============================================================================

#include "hunterclaw/gateway/question_manager.h"

#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <chrono>

namespace hunterclaw {
namespace gateway {

namespace {
std::shared_ptr<spdlog::logger> log() { return core::get_logger("question"); }
}  // namespace

// ── helpers ─────────────────────────────────────────────────────────────────

int64_t QuestionManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string QuestionManager::generate_uuid() {
    uuid_t u;
    uuid_generate(u);
    char buf[37];
    uuid_unparse_lower(u, buf);
    return std::string("ask_") + buf;
}

// ── QuestionRecord::to_json ─────────────────────────────────────────────────

json QuestionRecord::to_json() const {
    json j;
    j["id"] = id;

    json qs = json::array();
    for (const auto& q : questions) {
        json o;
        // ★ 内部 `id` → 线上 `questionId`（对齐 QuestionInputFields）。
        //   ⚠ 不能直接输出 `id`: 终端按 schema 校验，字段名不对会整条丢弃，
        //     表现为"卡片弹出来但没有题目"。
        o["questionId"] = q.id;
        o["header"] = q.header;
        o["question"] = q.question;
        json opts = json::array();
        for (const auto& op : q.options) {
            json oo;
            oo["label"] = op.label;
            if (op.description.has_value() && !op.description->empty()) {
                oo["description"] = *op.description;
            }
            opts.push_back(std::move(oo));
        }
        o["options"] = std::move(opts);
        if (q.multi_select) o["multiSelect"] = true;
        if (q.is_other) o["isOther"] = true;
        if (q.is_secret) o["isSecret"] = true;
        qs.push_back(std::move(o));
    }
    j["questions"] = std::move(qs);

    if (agent_id.has_value() && !agent_id->empty()) j["agentId"] = *agent_id;
    if (session_key.has_value() && !session_key->empty()) j["sessionKey"] = *session_key;
    if (run_id.has_value() && !run_id->empty()) j["runId"] = *run_id;
    j["createdAtMs"] = created_at_ms;
    j["expiresAtMs"] = expires_at_ms;
    j["status"] = tools::question_status_to_wire(status);

    // ⚠ answers 只在 Answered 时输出 —— 维持 schema 的不变量。
    //   输出一个空 answers 会让终端把"已取消"渲染成"已作答但内容为空"。
    if (status == QuestionStatus::Answered && answers.has_value()) {
        json a = json::object();
        json m = json::object();
        for (const auto& [qid, values] : answers->answers) {
            m[qid] = values;
        }
        a["answers"] = std::move(m);
        j["answers"] = std::move(a);
    }
    if (resolved_by.has_value() && !resolved_by->empty()) j["resolvedBy"] = *resolved_by;
    return j;
}

// ── create ──────────────────────────────────────────────────────────────────

QuestionRecord QuestionManager::create(const std::vector<UserInputQuestion>& questions,
                                      int64_t timeout_ms,
                                      const std::optional<std::string>& explicit_id) {
    QuestionRecord r;
    r.id = (explicit_id.has_value() && !explicit_id->empty()) ? *explicit_id
                                                              : generate_uuid();
    r.questions = questions;
    r.created_at_ms = now_ms();
    // ⚠ 钳制上限: 调用方可能传 24h，那会让一条记录长期占着会话槽位
    //   （每会话只允许一个 pending question），后续提问全部注册失败。
    const int64_t clamped = std::max<int64_t>(
        1, std::min<int64_t>(timeout_ms > 0 ? timeout_ms : DEFAULT_QUESTION_TIMEOUT_MS,
                             MAX_QUESTION_TIMEOUT_MS));
    r.expires_at_ms = r.created_at_ms + clamped;
    r.status = QuestionStatus::Pending;
    return r;
}

// ── register_record ─────────────────────────────────────────────────────────

bool QuestionManager::register_record(const QuestionRecord& record, int64_t timeout_ms) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pending_.find(record.id);
    if (it != pending_.end()) {
        // 幂等: 同 id 重复注册且未收敛 → 视作成功（对齐 approval 侧）
        if (!it->second->settled) return true;
        // ⚠ 已收敛的 id 不能复用: 新的等待方会立刻读到旧答案。
        log()->error("question id '{}' already settled; refusing re-registration",
                     record.id);
        return false;
    }
    auto entry = std::make_shared<PendingEntry>();
    entry->record = record;
    entry->timeout_ms = std::max<int64_t>(
        1, std::min<int64_t>(timeout_ms > 0 ? timeout_ms : DEFAULT_QUESTION_TIMEOUT_MS,
                             MAX_QUESTION_TIMEOUT_MS));
    entry->settled = false;
    pending_[record.id] = std::move(entry);
    return true;
}

// ── settle（resolve / cancel / expire 共用）─────────────────────────────────

bool QuestionManager::settle(const std::string& record_id, QuestionStatus status,
                            const std::optional<UserInputAnswers>& answers,
                            const std::optional<std::string>& resolved_by) {
    std::shared_ptr<PendingEntry> entry;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.find(record_id);
        if (it == pending_.end()) return false;
        entry = it->second;
        // ⚠ 已终态 → false。这是【认领通道判断"要不要回落 steer"的依据】,
        //   不能宽容处理成 true。
        if (entry->settled) return false;

        entry->record.status = status;
        // ★ 维持不变量: answers 只在 Answered 时写入。
        if (status == QuestionStatus::Answered && answers.has_value()) {
            entry->record.answers = *answers;
        } else {
            entry->record.answers.reset();
        }
        entry->record.resolved_by = resolved_by;
        entry->settled = true;
    }
    // ★ 在锁外 notify: 被唤醒者会立刻抢锁，持锁 notify 会让它们
    //   醒来即阻塞（惊群 + 无谓的上下文切换）。
    entry->cv.notify_all();
    return true;
}

bool QuestionManager::resolve(const std::string& record_id, const UserInputAnswers& answers,
                             const std::optional<std::string>& resolved_by) {
    const bool ok = settle(record_id, QuestionStatus::Answered, answers, resolved_by);
    if (ok) {
        log()->info("question '{}' answered by '{}'", record_id,
                    resolved_by.value_or("unknown"));
    }
    return ok;
}

bool QuestionManager::cancel(const std::string& record_id,
                            const std::optional<std::string>& resolved_by) {
    const bool ok = settle(record_id, QuestionStatus::Cancelled, std::nullopt, resolved_by);
    if (ok) {
        log()->info("question '{}' cancelled ({})", record_id,
                    resolved_by.value_or("unknown"));
    }
    return ok;
}

bool QuestionManager::expire(const std::string& record_id,
                            const std::optional<std::string>& resolved_by) {
    return settle(record_id, QuestionStatus::Expired, std::nullopt, resolved_by);
}

// ── await_answer ────────────────────────────────────────────────────────────

QuestionWaitResult QuestionManager::await_answer(const std::string& record_id,
                                               int64_t timeout_ms,
                                               const std::atomic<bool>* aborted) {
    QuestionWaitResult out;

    std::shared_ptr<PendingEntry> entry;
    int64_t effective_timeout = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.find(record_id);
        if (it == pending_.end()) {
            // ⚠ 不存在 → Expired 而非 Pending。
            //   返回 Pending 会让调用方去 cancel 一条不存在的记录，
            //   然后按"竞态窗口"再补偿读一次 —— 白跑两次 RPC。
            out.status = QuestionStatus::Expired;
            return out;
        }
        entry = it->second;
        effective_timeout = timeout_ms > 0 ? timeout_ms : entry->timeout_ms;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(effective_timeout);

    std::unique_lock<std::mutex> lk(mutex_);
    while (!entry->settled) {
        // ★ abort 优先于超时判定 —— 与审批链一致。
        //   顺序颠倒会在"abort 与 deadline 同时到"时报超时，
        //   而用户看到的是自己点了取消。
        if (aborted && aborted->load()) {
            out.status = QuestionStatus::Cancelled;
            return out;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        // ★ 分段等待: aborted 是外部 atomic，置位方不持本类 mutex 无法
        //   notify，只能轮询。
        auto slice = std::chrono::milliseconds(kQuestionAbortPollIntervalMs);
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < slice) slice = remaining;
        entry->cv.wait_for(lk, slice);
    }

    // 再查一次 aborted: 循环可能因 settled 或 deadline 退出，
    // 但 abort 也许恰在最后一片里置位。
    if (!entry->settled && aborted && aborted->load()) {
        out.status = QuestionStatus::Cancelled;
        return out;
    }

    if (!entry->settled) {
        out.status = QuestionStatus::Pending;
        return out;
    }

    out.status = entry->record.status;
    if (out.status == QuestionStatus::Answered && entry->record.answers.has_value()) {
        out.answers = *entry->record.answers;
    }

    // ── grace 期清理 ───────────────────────────────────────────────────
    // ⚠ 不在这里立刻 erase: 可能还有第二个等待方（例如认领通道的补偿读）
    //   在路上。按 resolved 时间 + grace 让后来者也能读到。
    const int64_t cutoff = now_ms() - QUESTION_RESOLVED_ENTRY_GRACE_MS;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second->settled && it->second->record.created_at_ms < cutoff) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

// ── 查询 ────────────────────────────────────────────────────────────────────

std::optional<QuestionRecord> QuestionManager::get_snapshot(
    const std::string& record_id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = pending_.find(record_id);
    if (it == pending_.end()) return std::nullopt;
    return it->second->record;
}

std::vector<QuestionRecord> QuestionManager::list_pending() const {
    std::vector<QuestionRecord> out;
    const int64_t now = now_ms();
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [id, entry] : pending_) {
        if (!entry || entry->settled) continue;
        if (entry->record.expires_at_ms <= now) continue;
        out.push_back(entry->record);
    }
    return out;
}

QuestionIdLookupResult QuestionManager::lookup_pending_id(const std::string& input) const {
    QuestionIdLookupResult res;
    if (input.empty()) return res;

    std::lock_guard<std::mutex> lk(mutex_);
    // 精确优先
    auto exact = pending_.find(input);
    if (exact != pending_.end() && exact->second && !exact->second->settled) {
        res.kind = QuestionIdLookupResult::Kind::Exact;
        res.id = input;
        return res;
    }

    std::vector<std::string> matches;
    for (const auto& [id, entry] : pending_) {
        if (!entry || entry->settled) continue;
        if (id.size() >= input.size() && id.compare(0, input.size(), input) == 0) {
            matches.push_back(id);
        }
    }
    if (matches.empty()) return res;
    if (matches.size() > 1) {
        // ⚠ 歧义时【报错而非猜第一个】: 猜错会把答案写到别人的提问上。
        res.kind = QuestionIdLookupResult::Kind::Ambiguous;
        res.ids = std::move(matches);
        return res;
    }
    res.kind = QuestionIdLookupResult::Kind::Prefix;
    res.id = matches.front();
    return res;
}

bool QuestionManager::has_pending() const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [id, entry] : pending_) {
        (void)id;
        if (entry && !entry->settled) return true;
    }
    return false;
}

// ── question_manager_instance ───────────────────────────────────────────────

std::shared_ptr<QuestionManager> question_manager_instance() {
    // ★ 函数内 static: C++11 起初始化线程安全，且避开跨 TU 静态初始化
    //   顺序问题（main 在 gateway 就绪后取它，而某些 TU 的静态对象更早构造）。
    static std::shared_ptr<QuestionManager> inst = std::make_shared<QuestionManager>();
    return inst;
}

}  // namespace gateway
}  // namespace hunterclaw
