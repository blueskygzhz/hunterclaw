// ============================================================================
// remote_a2a/pending_tasks.cc
// 让渡任务登记表（内存版）。
// ============================================================================

#include "hunterclaw/remote_a2a/pending_tasks.h"

#include <chrono>
#include <random>
#include <sstream>

namespace hunterclaw {
namespace remote_a2a {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char* pending_phase_text(PendingPhase p) {
    switch (p) {
        case PendingPhase::Running: return "running";
        case PendingPhase::Terminal: return "terminal";
        case PendingPhase::Harvested: return "harvested";
    }
    return "unknown";
}

std::string make_pending_handle(uint64_t seq) {
    // 句柄 = 序号 + 随机后缀。
    // ⚠ 纯序号会被外部猜到（回调接口的 handle 是半公开的），
    //   加随机后缀让「猜句柄 + 猜 token」的难度叠乘。
    static thread_local std::mt19937_64 rng(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&seq));
    std::ostringstream ss;
    ss << "ra2a-" << seq << "-" << std::hex << (rng() & 0xFFFFFFFFull);
    return ss.str();
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    // ⚠ 长度不同也要跑完全程 —— 提前返回会泄漏长度信息。
    //   两串长度不同时结果必为 false，但耗时与 a 的长度相关（不与内容相关）。
    unsigned char diff = (a.size() == b.size()) ? 0 : 1;
    const size_t n = a.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char bc =
            (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(a[i]) ^ bc;
    }
    return diff == 0;
}

// ── 构造 ────────────────────────────────────────────────────────────────────

std::shared_ptr<PendingTaskStore> PendingTaskStore::create(
    const PendingStoreConfig& cfg) {
    return std::make_shared<PendingTaskStore>(cfg);
}

PendingTaskStore::PendingTaskStore(const PendingStoreConfig& cfg) : cfg_(cfg) {}

PendingTask* PendingTaskStore::find_locked(const std::string& handle) {
    auto it = tasks_.find(handle);
    return it == tasks_.end() ? nullptr : &it->second;
}

const PendingTask* PendingTaskStore::find_locked(const std::string& handle) const {
    auto it = tasks_.find(handle);
    return it == tasks_.end() ? nullptr : &it->second;
}

// ── 登记 ────────────────────────────────────────────────────────────────────

bool PendingTaskStore::evict_one_locked() {
    const int64_t now = now_ms();
    // 优先淘汰 Harvested（结果已被取走，没人还需要它）
    std::string victim;
    int64_t oldest = 0;
    for (const auto& [h, t] : tasks_) {
        if (t.phase != PendingPhase::Harvested) continue;
        if (victim.empty() || t.updated_at_ms < oldest) {
            victim = h;
            oldest = t.updated_at_ms;
        }
    }
    // 其次淘汰超 TTL 的 Terminal
    if (victim.empty()) {
        for (const auto& [h, t] : tasks_) {
            if (t.phase != PendingPhase::Terminal) continue;
            if (now - t.updated_at_ms < cfg_.terminal_ttl_ms) continue;
            if (victim.empty() || t.updated_at_ms < oldest) {
                victim = h;
                oldest = t.updated_at_ms;
            }
        }
    }
    // ★ 绝不淘汰 Running：被淘汰的在途任务会永远等不到结果，
    //   而调用方还在 wait_terminal 上等它 —— 直接变成一次挂死。
    if (victim.empty()) return false;

    auto it = tasks_.find(victim);
    if (it != tasks_.end()) {
        auto idx = by_task_id_.find(it->second.task_id);
        if (idx != by_task_id_.end() && idx->second == victim) {
            by_task_id_.erase(idx);
        }
        tasks_.erase(it);
    }
    return true;
}

core::Result<std::string> PendingTaskStore::register_task(
    const std::string& agent_name, const std::string& task_id,
    const std::string& context_id, const std::string& session_key) {
    if (task_id.empty()) {
        return core::Result<std::string>::Err("task_id 为空，无法登记");
    }

    std::lock_guard<std::mutex> lk(mu_);

    // ── 续接：同一 task_id 的挂起记录【复用同一个 handle】────────────────────
    //
    // ★★ A2A 的续接语义是「在同一个 task_id 上再发一次 message/send 或
    //   message/stream」。因此本地必须复用同一条记录，而不是新建。
    //
    // ⚠⚠ 新建 handle 会造成四个真实故障，且全部是静默的:
    //   ① 回调失联 —— push config 注册的 URL 里带的是【旧 handle】。新建
    //      handle 后远端仍往旧 URL 推，而旧记录已 Harvested，回调被丢弃，
    //      于是续接后的这一轮只能靠退避探测，回调形同虚设。
    //   ② 去重闸失效方向反了 —— question_raised 挂在 handle 上。新 handle
    //      恰好让多轮确认能继续问（看似正常），但这是【巧合】: 一旦哪天
    //      按 task_id 复用就会把第二轮确认吃掉。语义应当由"复用 + 显式重置"
    //      表达，而不是靠新建绕过。
    //   ③ 容量泄漏 —— 多轮确认的长任务在 store 里留下 N 条 Harvested 记录，
    //      各自占 max_entries 额度直到 TTL 到。
    //   ④ 结果碎片化 —— 前几轮的 result_text 留在旧记录里，新记录从零开始，
    //      调用方按新 handle 取结果会丢掉之前的产出。
    //
    // ★ 只在【挂起态】复用。已 Completed/Failed/Canceled 的 task 不能再
    //   续接（A2A 里终结的 task 不接受新 message），此时新建是正确的 ——
    //   那是"复用了同一个 task_id 起新交互"，语义上确实是另一条记录。
    auto idx = by_task_id_.find(task_id);
    if (idx != by_task_id_.end()) {
        auto existing = tasks_.find(idx->second);
        if (existing != tasks_.end() && existing->second.agent_name == agent_name &&
            is_suspended_state(existing->second.state)) {
            PendingTask& t = existing->second;
            const int64_t rnow = now_ms();

            // ── 拉回运行态 ──────────────────────────────────────────────
            // ⚠ phase 可能是 Terminal 也可能是 Harvested（execute 末尾会
            //   harvest）。两者都要拉回 Running，否则 run_probe 第一次
            //   evaluate 就看到 phase != Running 而立刻返回"已终态"，
            //   本轮一次远端调用都不会发生。
            t.phase = PendingPhase::Running;
            t.reason.reset();
            // ★ state 必须离开挂起态: 留着 InputRequired 会让 is_terminal()
            //   立刻为真，同上，本轮直接空转结束。
            t.state = TaskState::Submitted;

            // ── 清掉上一轮的挂起痕迹 ────────────────────────────────────
            // ⚠ progress_text 存的是上一轮那句追问（"还需要订单号"）。
            //   不清会让本轮 render() 把【已经答过的追问】再输出一遍，
            //   模型会以为远端又在问同一件事。
            t.progress_text.clear();
            t.error_text.clear();
            // ⚠ token 已经确认过了。留着会让下一次 build_interrupt 把旧
            //   token 再列一遍，用户被要求确认自己刚确认过的东西。
            t.pending_token_ids.clear();
            // ★ requires_user_action 在 note_confirm_signal 里是【单调置位】
            //   （远端能加严不能放松）。这里可以重置，因为这是一次【新的
            //   A2A 交互】—— 旧交互的加严不该延续，否则一旦某轮要求过人工
            //   确认，之后每一轮都会被判定需要确认，永远问不完。
            t.requires_user_action = false;
            // ★ 去重闸随之重置: 新一轮若再挂起，必须能再问一次。
            t.question_raised = false;
            t.consecutive_no_progress = 0;

            // ── 显式保留的东西 ──────────────────────────────────────────
            // · result_text  ：前几轮的产出，续接后要能一起交付
            // · fp           ：指纹按远端 history/artifact 计数推进，续接后
            //                  history 会增长 —— 保留旧值才能正确检测推进；
            //                  清零会让第一次 ingest 误判为"大幅推进"
            // · callback_token / created_at_ms ：见下方回调说明
            // ★★ 交互代号 +1 —— 让上一代的 background/probe 线程在下次
            //   循环校验时立刻退出，不会与本轮抢同一条记录。
            //   ⚠ 没有这一步，老线程醒来看到 phase==Running 会继续探测，
            //     两个线程双倍推进 consecutive_no_progress → 误判 Zombie
            //     把正常任务杀掉。
            ++t.epoch;
            // ★ HardTimeout 必须按【本轮】计时。沿用 created_at_ms 会让
            //   用户思考的那几分钟被算进远端执行时限，第二轮直接判超时。
            t.turn_started_at_ms = rnow;
            t.updated_at_ms = rnow;
            t.last_event_ms = rnow;
            // context_id 可能在续接时才由远端给出，非空就更新
            if (!context_id.empty()) t.context_id = context_id;
            if (!session_key.empty()) t.session_key = session_key;

            return core::Result<std::string>::Ok(t.handle);
        }
    }

    if (tasks_.size() >= cfg_.max_entries) {
        if (!evict_one_locked()) {
            return core::Result<std::string>::Err(
                "在途 A2A 任务已达上限 " + std::to_string(cfg_.max_entries) +
                "，且全部在途，无法登记新任务");
        }
    }

    const int64_t now = now_ms();
    PendingTask t;
    t.handle = make_pending_handle(++seq_);
    t.epoch = 1;  // ★ 从 1 起：0 保留给 current_epoch() 的"记录不存在"
    t.agent_name = agent_name;
    t.task_id = task_id;
    t.context_id = context_id;
    t.session_key = session_key;
    t.phase = PendingPhase::Running;
    t.state = TaskState::Submitted;
    t.created_at_ms = now;
    // ⚠ 必须在 created_at_ms 赋值【之后】，否则拿到的是 0，
    //   HardTimeout 会用 1970 做基准 → 首次 evaluate 立刻判超时。
    t.turn_started_at_ms = now;
    t.updated_at_ms = now;
    t.last_event_ms = now;

    const std::string handle = t.handle;
    // ⚠ 同一 task_id 重复登记时索引指向最新的 handle（旧记录仍可按 handle 取）
    by_task_id_[task_id] = handle;
    tasks_.emplace(handle, std::move(t));
    return core::Result<std::string>::Ok(handle);
}

std::optional<PendingTask> PendingTaskStore::get(const std::string& handle) const {
    std::lock_guard<std::mutex> lk(mu_);
    const PendingTask* t = find_locked(handle);
    if (!t) return std::nullopt;
    return *t;
}

uint64_t PendingTaskStore::current_epoch(const std::string& handle) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tasks_.find(handle);
    // ★ 记录不存在 → 0。调用方（退避循环）应视为"我该退出了"。
    if (it == tasks_.end()) return 0;
    return it->second.epoch;
}

std::optional<std::string> PendingTaskStore::handle_of_task(
    const std::string& task_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_task_id_.find(task_id);
    if (it == by_task_id_.end()) return std::nullopt;
    return it->second;
}

// ── 进度 ────────────────────────────────────────────────────────────────────

bool PendingTaskStore::note_progress(const std::string& handle,
                                     const ProgressFingerprint& fp, TaskState state,
                                     const std::string& progress_text) {
    std::lock_guard<std::mutex> lk(mu_);
    bool advanced = false;
    note_progress_locked(handle, 0, fp, state, progress_text, &advanced);
    return advanced;
}

bool PendingTaskStore::note_progress_if_current(const std::string& handle,
                                                uint64_t epoch,
                                                const ProgressFingerprint& fp,
                                                TaskState state,
                                                const std::string& progress_text,
                                                bool* out_advanced) {
    std::lock_guard<std::mutex> lk(mu_);
    return note_progress_locked(handle, epoch, fp, state, progress_text, out_advanced);
}

// ★ 校验与写入共处同一临界区 —— 这是这组接口存在的全部理由。
//   调用方自己比对 current_epoch() 再写是 TOCTOU: 两次加锁之间续接可能发生。
// @return 代号是否有效（true = 本次写入被接受，或记录处于非 Running 但代号正确）
bool PendingTaskStore::note_progress_locked(const std::string& handle, uint64_t epoch,
                                            const ProgressFingerprint& fp,
                                            TaskState state,
                                            const std::string& progress_text,
                                            bool* out_advanced) {
    if (out_advanced) *out_advanced = false;
    PendingTask* t = find_locked(handle);
    if (!t) return false;
    if (epoch != 0 && t->epoch != epoch) return false;  // 上一代的写入，丢弃
    // ★ 已终结的记录不再接受进度更新 —— 否则「取消后远端继续推进度」
    //   会把 Canceled 的记录改回 running 的样子。
    // ⚠ 但这【不是】代号问题: 返回 true 让调用方知道"你没过期，只是记录终结了"。
    if (t->phase != PendingPhase::Running) return true;

    const bool changed = (t->fp != fp);
    t->fp = fp;
    t->state = state;
    if (!progress_text.empty()) {
        // ★ 中间态只覆盖 progress_text，【不】进 result_text
        t->progress_text = progress_text;
    }
    t->updated_at_ms = now_ms();
    if (changed) {
        t->last_event_ms = t->updated_at_ms;
        t->consecutive_no_progress = 0;
    }
    if (out_advanced) *out_advanced = changed;
    return true;
}

void PendingTaskStore::append_result(const std::string& handle,
                                     const std::string& delta) {
    if (delta.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    (void)append_result_locked(handle, 0, delta);
}

bool PendingTaskStore::append_result_if_current(const std::string& handle,
                                               uint64_t epoch,
                                               const std::string& delta) {
    if (delta.empty()) return true;  // 空增量不算失败
    std::lock_guard<std::mutex> lk(mu_);
    return append_result_locked(handle, epoch, delta);
}

bool PendingTaskStore::append_result_locked(const std::string& handle, uint64_t epoch,
                                           const std::string& delta) {
    PendingTask* t = find_locked(handle);
    if (!t) return false;
    if (epoch != 0 && t->epoch != epoch) return false;
    // ⚠ 已终结不是代号问题 —— 返回 true，让调用方区分"过期"与"记录终结"。
    //   （与 note_progress_locked 保持一致的语义）
    if (t->phase != PendingPhase::Running) return true;
    if (!t->result_text.empty()) t->result_text += "\n";
    t->result_text += delta;
    t->updated_at_ms = now_ms();
    t->last_event_ms = t->updated_at_ms;
    return true;
}

int PendingTaskStore::note_no_progress(const std::string& handle) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (!t) return -1;
    if (t->phase != PendingPhase::Running) return t->consecutive_no_progress;
    t->updated_at_ms = now_ms();
    return ++t->consecutive_no_progress;
}

bool PendingTaskStore::note_no_progress_if_current(const std::string& handle,
                                                   uint64_t epoch) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (!t) return false;
    // ★★ 这个校验是防 Zombie 误判的关键: 两代线程各自计数会【双倍推进】
    //   consecutive_no_progress，把正常运行的任务判成僵死并杀掉。
    if (epoch != 0 && t->epoch != epoch) return false;
    if (t->phase != PendingPhase::Running) return true;  // 已终结，无需计数
    t->updated_at_ms = now_ms();
    ++t->consecutive_no_progress;
    return true;
}

// ── 终结 ────────────────────────────────────────────────────────────────────

bool PendingTaskStore::note_confirm_signal(const std::string& handle,
                                          bool requires_user_action,
                                          const std::vector<std::string>& token_ids) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (!t) return false;

    bool changed = false;
    // ★ 单调置位 —— 只从 false→true，绝不反向。
    //   ⚠ 远端通常只在【触发那一刻】的快照里带 metadata，后续 tasks/get
    //     往往不再重复。若按最新快照覆盖，标记会被清掉，
    //     措辞退回"请补充输入"，模型于是自行代答 —— 决定权被悄悄拿走。
    if (requires_user_action && !t->requires_user_action) {
        t->requires_user_action = true;
        changed = true;
    }
    // 同理：空列表不覆盖已有 token（那通常意味着"这次快照没带"而非"清空了"）
    if (!token_ids.empty() && t->pending_token_ids != token_ids) {
        t->pending_token_ids = token_ids;
        changed = true;
    }
    if (changed) t->updated_at_ms = now_ms();
    return changed;
}

bool PendingTaskStore::try_claim_question(const std::string& handle) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (!t) return false;

    // ★★ 读旧值 + 置新值必须在【同一临界区】内完成。
    //   ⚠ 拆成 get() 判断 + 单独 set 会留下竞态窗口: SSE 线程与探测线程
    //     可能同时读到 false 然后都去提问。而 PendingQuestionRegistry
    //     每会话只收一个 pending question，第二个会静默降级成"仅终端"，
    //     于是"回一句话就能答"这条路对谁都不生效 —— 用户以为答了，
    //     agent 还在等。
    if (t->question_raised) return false;
    t->question_raised = true;
    t->updated_at_ms = now_ms();
    return true;
}

bool PendingTaskStore::update_terminal(const std::string& handle,
                                       TerminalReason reason, const std::string& text,
                                       const std::string& error_text,
                                       std::optional<TaskState> remote_state) {
    return update_terminal_if_current(handle, 0, reason, text, error_text, remote_state);
}

bool PendingTaskStore::update_terminal_if_current(const std::string& handle,
                                                  uint64_t epoch,
                                                  TerminalReason reason,
                                                  const std::string& text,
                                                  const std::string& error_text,
                                                  std::optional<TaskState> remote_state) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        PendingTask* t = find_locked(handle);
        if (!t) return false;

        // ★★ 代号校验必须在这里（锁内、粘性判定之前）。
        //   ⚠ 这是最危险的一个写入: 上一代线程若用过期状态把新一代记录
        //     写成终态，正在进行的续接会直接"完成"，用户拿到上一轮的结果，
        //     而真正的远端任务无人再管。
        if (epoch != 0 && t->epoch != epoch) return false;

        // ★ 粘性终态保护（対标 run_wait.h:105-108）：
        //   已经是 HardTimeout / Canceled 时，后到的普通终态不覆盖。
        //   场景：用户取消 → 远端才把 completed 推回来。
        //   若允许覆盖，用户会看到「我取消的任务成功了」。
        if (t->reason.has_value() && is_sticky(*t->reason) && !is_sticky(reason)) {
            return false;
        }
        // 同为粘性时，先到的胜（HardTimeout 与 Canceled 不互相覆盖）
        if (t->reason.has_value() && is_sticky(*t->reason) && is_sticky(reason) &&
            *t->reason != reason) {
            return false;
        }

        t->reason = reason;
        // ★ 远端给了真实状态就【原样保留】，只有在没有远端状态时才由 reason 推导。
        //
        //   曾经这里无条件写 terminal_reason_to_task_state(reason)，
        //   于是 input-required（归因为 Completed，因为它不是失败）
        //   被写成 TaskState::Completed —— 远端"我还需要订单号"的事实
        //   被抹成"任务成功了"，render() 把追问当最终答案输出，续接流程断裂。
        //
        // ⚠ 非终态一律忽略：一条 working 会把记录写成
        //   "phase=Terminal 但 state=working" 的矛盾态，
        //   后续 is_terminal(state) 判断会与 phase 不一致。
        if (remote_state.has_value() && is_terminal(*remote_state)) {
            t->state = *remote_state;
        } else if (reason == TerminalReason::Completed && is_suspended_state(t->state)) {
            // ★★ 自保：挂起态不被"成功"抹掉。
            //
            //   上面那条 remote_state 分支要求【调用方记得传第五参】才生效。
            //   漏传时 reason=Completed 会推导出 TaskState::Completed,
            //   把 note_progress 刚写入的 InputRequired / AuthRequired 覆盖掉 ——
            //   于是 is_suspended() 变 false、render() 不输出 task_id,
            //   模型无法续接, 挂起的任务永久卡死。
            //
            //   ⚠ 为什么在这里兜而不是只靠调用方自觉:
            //     "挂起"与"成功"是互斥事实, 由本地 reason 推导出的
            //     Completed 【没有资格】否定远端刚刚声明的挂起状态。
            //     这是数据一致性约束, 不该依赖每个调用点都不出错。
            //
            //   ★ 只对 Completed 设此例外。失败类 reason(HardTimeout /
            //     Zombie / RemoteFailed / ConnLost / Canceled) 【必须】
            //     覆盖挂起态 —— 那些是本地权威判定: 任务确实结束了,
            //     不管远端此前说它在等什么。若也保留挂起态,
            //     一个超时的任务会一直显示"等用户输入", 永远等不到。
        } else {
            t->state = terminal_reason_to_task_state(reason);
        }
        // Harvested 不回退到 Terminal（结果已被取走）
        if (t->phase == PendingPhase::Running) t->phase = PendingPhase::Terminal;
        if (!text.empty()) {
            if (!t->result_text.empty()) t->result_text += "\n";
            t->result_text += text;
        }
        if (!error_text.empty()) t->error_text = error_text;
        t->updated_at_ms = now_ms();
        t->last_event_ms = t->updated_at_ms;
        changed = true;
    }
    // ⚠ 必须在锁外通知，避免等待者被唤醒后立刻又阻塞在同一把锁上
    if (changed) cv_.notify_all();
    return changed;
}

std::optional<PendingTask> PendingTaskStore::harvest(const std::string& handle) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (!t) return std::nullopt;
    if (t->phase == PendingPhase::Running) return std::nullopt;  // 未终结，不阻塞
    PendingTask copy = *t;
    t->phase = PendingPhase::Harvested;
    t->updated_at_ms = now_ms();
    return copy;
}

std::optional<PendingTask> PendingTaskStore::wait_terminal(const std::string& handle,
                                                           int64_t timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);

    auto ready = [&]() -> bool {
        const PendingTask* t = find_locked(handle);
        // ⚠ 记录被删除也算"就绪"，否则会一直等一个不存在的东西
        return t == nullptr || t->phase != PendingPhase::Running;
    };

    if (timeout_ms <= 0) {
        if (!ready()) return std::nullopt;
    } else {
        // ★ wait_for + 谓词，天然处理虚假唤醒
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready)) {
            return std::nullopt;  // 超时：★ 记录仍在表里，后台线程继续跟进
        }
    }

    const PendingTask* t = find_locked(handle);
    if (!t) return std::nullopt;
    return *t;
}

void PendingTaskStore::notify_all() { cv_.notify_all(); }

// ── 回调 token ──────────────────────────────────────────────────────────────

void PendingTaskStore::set_callback_token(const std::string& handle,
                                          const std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    PendingTask* t = find_locked(handle);
    if (t) t->callback_token = token;
}

bool PendingTaskStore::verify_callback_token(const std::string& handle,
                                             const std::string& token) const {
    std::lock_guard<std::mutex> lk(mu_);
    const PendingTask* t = find_locked(handle);
    if (!t) return false;
    // ★ 未绑定 token 的任务【拒绝】任何回调 ——
    //   若返回 true，等于给未启用回调的任务开了一个无鉴权写入口。
    if (t->callback_token.empty()) return false;
    return constant_time_equals(t->callback_token, token);
}

// ── 清理 ────────────────────────────────────────────────────────────────────

size_t PendingTaskStore::sweep() {
    std::vector<std::string> to_erase;
    size_t affected = 0;
    bool any_terminal = false;

    {
        std::lock_guard<std::mutex> lk(mu_);
        const int64_t now = now_ms();

        for (auto& [h, t] : tasks_) {
            if (t.phase == PendingPhase::Running) {
                // ★ 在途任务的绝对上限：防止远端永不回应导致记录永久驻留。
                //   判 HardTimeout 而非直接删除 —— 调用方可能还在 wait_terminal 上等，
                //   直接删除会让它等到超时才发现，且拿不到任何原因。
                if (now - t.created_at_ms >= cfg_.running_max_ms) {
                    t.reason = TerminalReason::HardTimeout;
                    t.state = terminal_reason_to_task_state(TerminalReason::HardTimeout);
                    t.phase = PendingPhase::Terminal;
                    t.error_text = "任务在途超过上限，判定为硬超时";
                    t.updated_at_ms = now;
                    ++affected;
                    any_terminal = true;
                }
                continue;
            }
            if (now - t.updated_at_ms >= cfg_.terminal_ttl_ms) {
                to_erase.push_back(h);
            }
        }

        for (const std::string& h : to_erase) {
            auto it = tasks_.find(h);
            if (it == tasks_.end()) continue;
            auto idx = by_task_id_.find(it->second.task_id);
            if (idx != by_task_id_.end() && idx->second == h) by_task_id_.erase(idx);
            tasks_.erase(it);
            ++affected;
        }
    }

    if (any_terminal) cv_.notify_all();
    return affected;
}

std::vector<PendingTask> PendingTaskStore::list_by_session(
    const std::string& session_key) const {
    std::vector<PendingTask> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [h, t] : tasks_) {
        if (t.session_key == session_key) out.push_back(t);
    }
    return out;
}

size_t PendingTaskStore::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return tasks_.size();
}

size_t PendingTaskStore::running_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& [h, t] : tasks_) {
        if (t.phase == PendingPhase::Running) ++n;
    }
    return n;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
