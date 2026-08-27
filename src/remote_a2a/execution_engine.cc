// ============================================================================
// remote_a2a/execution_engine.cc
// 三分支执行引擎实现。
// ============================================================================

#include "hunterclaw/remote_a2a/execution_engine.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace hunterclaw {
namespace remote_a2a {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 可中断睡眠：每 100ms 检查一次退出标志。
/// ⚠ 不能直接 sleep_for(60s) —— 退避到 60s 时进程退出会被卡住一分钟。
void interruptible_sleep(int64_t ms, const std::atomic<bool>& stop) {
    const int64_t step = 100;
    int64_t slept = 0;
    while (slept < ms) {
        if (stop.load()) return;
        const int64_t d = std::min(step, ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(d));
        slept += d;
    }
}

}  // namespace

const char* exec_branch_text(ExecBranch b) {
    switch (b) {
        case ExecBranch::None: return "none";
        case ExecBranch::Stream: return "stream";
        case ExecBranch::Probe: return "probe";
        case ExecBranch::Callback: return "callback";
    }
    return "unknown";
}

// ── 配置 ────────────────────────────────────────────────────────────────────

int64_t ExecutionConfig::effective_yield_ms() const {
    if (background) return 0;  // 立即让渡（対标 exec.cc:928 yield_window = 0）
    // ★ clamp 与 exec.cc:930 完全一致：max(10, min(yieldMs, 120000))
    return std::max<int64_t>(10, std::min<int64_t>(yield_ms, 120000));
}

// ── 结果渲染 ────────────────────────────────────────────────────────────────

std::string ExecutionResult::render() const {
    std::ostringstream ss;
    if (yielded) {
        // ★ 措辞対标 exec.cc:983-985 —— 明确告诉模型「还在跑 + 怎么跟进」，
        //   否则模型会认为调用失败并重试，导致远端任务被起两遍。
        ss << "远端任务仍在执行（handle " << handle;
        if (!task_id.empty()) ss << ", task " << task_id;
        ss << "）。使用 remote_a2a_status 跟进结果。";
        if (!text.empty()) ss << "\n当前进度: " << text;
        return ss.str();
    }
    if (!error_text.empty()) {
        ss << "远端任务失败";
        if (reason) ss << "（" << terminal_reason_text(*reason) << "）";
        ss << ": " << error_text;
        if (!text.empty()) ss << "\n已获得的部分结果: " << text;
        return ss.str();
    }
    if (state == TaskState::InputRequired) {
        ss << "远端任务需要补充输入（handle " << handle << "）: " << text;
        return ss.str();
    }
    if (state == TaskState::AuthRequired) {
        ss << "远端任务需要鉴权（handle " << handle << "）: " << text;
        return ss.str();
    }
    return text.empty() ? "远端任务已完成，但未返回内容。" : text;
}

// ── 回调配置 ────────────────────────────────────────────────────────────────

std::string generate_callback_token() {
    static thread_local std::mt19937_64 rng(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ostringstream ss;
    // 32 字节 = 4 × 64bit。★ 短 token 可被暴力猜（回调端点是公网可达的）。
    for (int i = 0; i < 4; ++i) {
        ss << std::hex << std::setw(16) << std::setfill('0') << rng();
    }
    return ss.str();
}

nlohmann::json build_push_config(const std::string& base_url,
                                const std::string& handle,
                                const std::string& token) {
    nlohmann::json c;
    std::string url = base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    c["url"] = url + "/remote-a2a/callback/" + handle;
    c["id"] = handle;
    // ★ 必须带凭据。不带 token 的回调端点等于开放写入口：
    //   任何人猜到 handle 就能伪造「任务已完成，结果是 X」。
    c["authentication"] = nlohmann::json{{"schemes", nlohmann::json::array({"Bearer"})},
                                        {"credentials", token}};
    return c;
}

// ── 构造 ────────────────────────────────────────────────────────────────────

std::shared_ptr<ExecutionEngine> ExecutionEngine::create(
    std::shared_ptr<RemoteAgentClient> client, std::shared_ptr<PendingTaskStore> store,
    const ExecutionConfig& cfg) {
    return std::make_shared<ExecutionEngine>(std::move(client), std::move(store), cfg);
}

ExecutionEngine::ExecutionEngine(std::shared_ptr<RemoteAgentClient> client,
                                 std::shared_ptr<PendingTaskStore> store,
                                 const ExecutionConfig& cfg)
    : client_(std::move(client)), store_(std::move(store)), cfg_(cfg) {}

ExecutionEngine::~ExecutionEngine() { shutdown(); }

void ExecutionEngine::shutdown() {
    stopping_.store(true);
    if (store_) store_->notify_all();
    // ⚠ 后台线程是 detached 的，这里只能等它们自己看到 stopping_ 后退出。
    //   给一个有界的等待，避免析构时 detached 线程还在访问 this。
    const int64_t deadline = now_ms() + 3000;
    while (live_threads_.load() > 0 && now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ── 终态判定（唯一入口）────────────────────────────────────────────────────

std::optional<TerminalReason> ExecutionEngine::evaluate(
    const PendingTask& t, bool transport_dead, const std::string& last_err) const {
    TaskTracking tr;
    tr.task_id = t.task_id;
    tr.context_id = t.context_id;
    tr.remote_state = t.state;
    tr.fp = t.fp;
    tr.consecutive_no_progress = t.consecutive_no_progress;
    tr.max_no_progress = cfg_.probe.max_no_progress;
    tr.elapsed_ms = now_ms() - t.created_at_ms;
    tr.max_task_ms = cfg_.max_task_ms;
    tr.idle_ms = now_ms() - t.last_event_ms;
    tr.idle_timeout_ms = cfg_.idle_timeout_ms;
    tr.transport_dead = transport_dead;
    tr.last_err = last_err;

    // ★ 委托 classify()：顺序即优先级
    //   （HardTimeout > Zombie > RemoteFailed > Canceled > ConnLost）。
    //   在这里重新实现一遍必然与 task_tracking 漂移。
    const TerminalReason r = classify(tr);
    if (r != TerminalReason::Completed) return r;
    // Completed 只在远端确实报了终态时才成立
    if (is_terminal(t.state)) return TerminalReason::Completed;
    return std::nullopt;  // 还在跑
}

// ── Task 快照并入 ───────────────────────────────────────────────────────────

void ExecutionEngine::ingest_task(const std::string& handle, const Task& t,
                                  ProgressFn on_progress) {
    ProgressFingerprint fp = ProgressFingerprint::from_task(t);

    // 中间态文本：只走进度通道，★ 不进 result_text
    std::string progress;
    if (t.status.message) progress = concat_text_parts(t.status.message->parts);

    const bool advanced = store_->note_progress(handle, fp, t.status.state, progress);
    if (advanced && on_progress && !progress.empty()) {
        // ★ is_heartbeat=true：中间态不该占满 system-event 的 20 条上限
        //   （system_events.cc:18 MAX_EVENTS = 20），否则真事件会被挤掉。
        on_progress(progress, true);
    }
    if (!advanced) {
        store_->note_no_progress(handle);
    }

    if (is_terminal(t.status.state)) {
        // 终态：从 artifacts 取正式结果
        const std::string final_text = extract_result_text(t);
        std::string err;
        TerminalReason reason = TerminalReason::Completed;
        switch (t.status.state) {
            case TaskState::Failed:
                reason = TerminalReason::RemoteFailed;
                err = progress.empty() ? "远端任务失败（未提供原因）" : progress;
                break;
            case TaskState::Canceled:
                reason = TerminalReason::Canceled;
                break;
            default:
                reason = TerminalReason::Completed;
                break;
        }
        // ⚠ input-required / auth-required 也是终态，但【不是失败】：
        //   它们是「挂起」，本轮结束等外部回填。用 Completed 归因，
        //   由 state 字段区分（ExecutionResult::render 会分开渲染）。
        store_->update_terminal(handle,
                               reason,
                               final_text.empty() ? progress : final_text, err);
    }
}

// ── ③ 回调注册 ──────────────────────────────────────────────────────────────

void ExecutionEngine::try_register_callback(const std::string& handle,
                                            const std::string& task_id) {
    if (!cfg_.enable_callback || cfg_.callback_base_url.empty()) return;
    if (!client_->capabilities().should_try_push()) return;

    const std::string token = generate_callback_token();
    // ★ 先落 token 再注册。反过来会有窗口期：远端已开始回调，
    //   而本地还没有 token 可比对 → 合法回调被拒。
    store_->set_callback_token(handle, token);

    auto r = client_->set_push_config(
        task_id, build_push_config(cfg_.callback_base_url, handle, token));
    // ★ 失败不阻断 —— 回调只是兜底信号，SSE/探测仍在工作。
    //   （note_capability 已在 client 内部处理：只有 -32003/-32601 才写 No）
    (void)r;
}

// ── ② SSE ───────────────────────────────────────────────────────────────────

bool ExecutionEngine::run_stream(const Message& msg, const std::string& handle,
                                 ExecutionResult* out, ProgressFn on_progress,
                                 int64_t deadline_ms) {
    bool terminal = false;

    auto on_event = [&](const ParsedStreamEvent& ev) -> bool {
        if (stopping_.load()) return false;

        auto cur = store_->get(handle);
        if (!cur) return false;

        ProgressFingerprint fp = cur->fp;
        fp.bump_from_event(ev);
        // ⚠ status_timestamp 要跟着走，否则同一秒内的多次更新会被判无推进
        if (!ev.status_timestamp.empty()) fp.status_timestamp = ev.status_timestamp;

        // ★ artifact 增量只取本次 —— ADK 注释（remote_a2a_agent.py:1327-1331）：
        //   取归一化后的累积 Task 会把之前的分片重复发出，内容成倍重复。
        if (ev.kind == ParsedStreamEvent::Kind::ArtifactUpdate && !ev.text.empty()) {
            store_->append_result(handle, ev.text);
            store_->note_progress(handle, fp, ev.state, "");
        } else {
            // 中间态：只走进度通道
            store_->note_progress(handle, fp, ev.state, ev.text);
            if (on_progress && !ev.text.empty()) on_progress(ev.text, true);
        }

        if (is_terminal(ev.state)) {
            TerminalReason reason = TerminalReason::Completed;
            std::string err;
            if (ev.state == TaskState::Failed) {
                reason = TerminalReason::RemoteFailed;
                err = ev.error_text.empty() ? "远端任务失败（未提供原因）" : ev.error_text;
            } else if (ev.state == TaskState::Canceled) {
                reason = TerminalReason::Canceled;
            }
            store_->update_terminal(handle, reason, "", err);
            terminal = true;
            return false;  // 拿到终态，停止读流
        }

        // 让渡窗口到点 → 停流，交给后台跟进
        if (deadline_ms > 0 && now_ms() >= deadline_ms) return false;
        return true;
    };

    auto r = client_->stream_message(msg, on_event);
    if (r.is_err()) {
        // ⚠ 这里【不】判终态：SSE 失败可能只是不支持流式或中间代理吞了流，
        //   降级到探测分支才是正确处理。判终态会误杀正在正常执行的任务。
        out->error_text = r.error;
        return terminal;
    }
    return terminal;
}

// ── ① 退避探测 ──────────────────────────────────────────────────────────────

bool ExecutionEngine::run_probe(const std::string& handle, ExecutionResult* out,
                                ProgressFn on_progress, int64_t deadline_ms) {
    if (!client_->capabilities().should_try_tasks_get()) {
        // 远端明确不支持 tasks/get（-32601/-32004）→ 无法探测。
        // ⚠ 此时【不能】判失败：任务可能真的在跑，只是我们看不到。
        //   交给回调或让渡，由用户决定何时放弃。
        out->error_text = "远端不支持 tasks/get，无法主动探测进度";
        return false;
    }

    int attempt = 0;
    std::string last_err;

    while (!stopping_.load()) {
        auto cur = store_->get(handle);
        if (!cur) return false;
        if (cur->phase != PendingPhase::Running) return true;  // 别的分支已终结

        // 终态判定（含 HardTimeout / Zombie）
        if (auto reason = evaluate(*cur, !last_err.empty(), last_err)) {
            std::string err;
            if (*reason == TerminalReason::Zombie) {
                err = "远端任务连续 " + std::to_string(cfg_.probe.max_no_progress) +
                      " 次探测无推进，判定为僵死";
            } else if (*reason == TerminalReason::HardTimeout) {
                err = "远端任务超过最长时限 " + std::to_string(cfg_.max_task_ms) + "ms";
            } else if (*reason == TerminalReason::ConnLost) {
                err = "与远端连接不可恢复: " + last_err;
            }
            store_->update_terminal(handle, *reason, "", err);
            return true;
        }

        if (deadline_ms > 0 && now_ms() >= deadline_ms) return false;  // 让渡

        // ★ 指数退避 5/10/20/40/60/60...（probe_policy.h:30-36 已钳制溢出）
        int64_t wait = cfg_.probe.interval_for(attempt);
        if (deadline_ms > 0) {
            const int64_t remain = deadline_ms - now_ms();
            if (remain <= 0) return false;
            wait = std::min(wait, remain);
        }
        interruptible_sleep(wait, stopping_);
        if (stopping_.load()) return false;

        auto t = client_->get_task(cur->task_id);
        if (t.is_err()) {
            last_err = t.error;
            // ★ TaskNotFound 是终态：远端已丢弃该任务，继续探测只会一直拿 -32001
            if (last_err.find("task_not_found") != std::string::npos) {
                store_->update_terminal(handle, TerminalReason::RemoteFailed, "",
                                       "远端已不存在该任务（TaskNotFound）");
                return true;
            }
            // 可恢复错误 → 计一次无推进后继续；不可恢复 → 下一轮 evaluate 判 ConnLost
            store_->note_no_progress(handle);
            ++attempt;
            continue;
        }
        last_err.clear();

        const size_t before = store_->get(handle) ? store_->get(handle)->fp.history_count
                                                 : 0;
        (void)before;
        ingest_task(handle, t.get(), on_progress);

        auto after = store_->get(handle);
        if (!after) return false;
        if (after->phase != PendingPhase::Running) return true;

        // 有推进 → 退避重置（対标 WorkingSubState::Stale 的"重置计时继续等"）
        attempt = (after->consecutive_no_progress == 0) ? 0 : attempt + 1;
    }
    return false;
}

// ── execute ─────────────────────────────────────────────────────────────────

ExecutionResult ExecutionEngine::execute(const Message& msg,
                                         const std::string& session_key,
                                         ProgressFn on_progress) {
    return execute_with_yield(msg, session_key, cfg_.yield_ms, cfg_.background,
                             std::move(on_progress));
}

ExecutionResult ExecutionEngine::execute_with_yield(const Message& msg,
                                                    const std::string& session_key,
                                                    int64_t yield_override_ms,
                                                    bool background,
                                                    ProgressFn on_progress) {
    ExecutionResult out;

    if (!client_ || !store_) {
        out.error_text = "执行引擎未正确初始化";
        return out;
    }

    // 卡必须先就绪（RPC URL 从卡来）
    auto card = client_->ensure_card();
    if (card.is_err()) {
        out.error_text = "远端 AgentCard 不可用: " + card.error;
        return out;
    }

    // ★ 只有让渡窗口可被单次调用覆盖。clamp 与 exec.cc:930 一致。
    //   max_task_ms / probe / 回调配置一律沿用运维配置，模型改不了。
    ExecutionConfig eff = cfg_;
    eff.yield_ms = yield_override_ms;
    eff.background = background;
    const int64_t yield_ms = eff.effective_yield_ms();
    const int64_t deadline = (yield_ms > 0) ? now_ms() + yield_ms : now_ms();

    // ── 首发：优先 SSE（延迟最低），不支持则 send ──────────────────────
    const bool try_stream = client_->capabilities().should_try_streaming();

    std::string handle;
    std::string task_id;
    std::string context_id;

    if (try_stream) {
        // ⚠ SSE 的 taskId 要等第一个事件才知道，所以先用占位登记，
        //   拿到真实 taskId 后……不行 —— 索引是按 task_id 建的。
        //   故：先发一个 send 拿 taskId 太浪费（两次调用）。
        //   实际做法：流式路径先收第一个事件拿 taskId 再登记。
        std::string first_task_id;
        std::string first_ctx_id;
        bool got_first = false;
        std::string pre_progress;
        bool terminal_in_first = false;
        std::string terminal_text;
        std::string terminal_err;
        TaskState terminal_state = TaskState::Working;

        auto probe_first = [&](const ParsedStreamEvent& ev) -> bool {
            if (!ev.task_id.empty() && !got_first) {
                first_task_id = ev.task_id;
                first_ctx_id = ev.context_id;
                got_first = true;
            }
            if (!ev.text.empty()) pre_progress = ev.text;
            if (is_terminal(ev.state)) {
                terminal_in_first = true;
                terminal_state = ev.state;
                terminal_text = ev.text;
                terminal_err = ev.error_text;
                return false;
            }
            // 拿到 taskId 就够了，后续交给正式循环
            return !got_first;
        };

        auto sr = client_->stream_message(msg, probe_first);

        if (sr.is_ok() && got_first) {
            auto reg = store_->register_task(client_->name(), first_task_id,
                                             first_ctx_id, session_key);
            if (reg.is_err()) {
                out.error_text = reg.error;
                return out;
            }
            handle = reg.get();
            task_id = first_task_id;
            context_id = first_ctx_id;
            out.branch = ExecBranch::Stream;

            try_register_callback(handle, task_id);

            if (terminal_in_first) {
                TerminalReason reason = TerminalReason::Completed;
                if (terminal_state == TaskState::Failed) reason = TerminalReason::RemoteFailed;
                else if (terminal_state == TaskState::Canceled) reason = TerminalReason::Canceled;
                store_->update_terminal(handle, reason, terminal_text, terminal_err);
            } else {
                // 继续读流直到终态或让渡窗口结束
                run_stream(msg, handle, &out, on_progress, deadline);
            }
        }
        // ★ SSE 失败/未拿到 taskId → 静默降级到 send + 探测。
        //   不返回错误：SSE 不可用是常见情况（中间代理、旧版服务端）。
    }

    if (handle.empty()) {
        // ── 降级：message/send + 退避探测 ────────────────────────────
        auto sent = client_->send_message(msg);
        if (sent.is_err()) {
            out.error_text = "message/send 失败: " + sent.error;
            return out;
        }
        const SendOutcome& so = sent.get();

        // 无 task 的即时回复 —— 最简单的路径，直接返回
        if (so.has_message && !so.has_task) {
            out.success = true;
            out.completed = true;
            out.branch = ExecBranch::None;
            out.state = TaskState::Completed;
            out.reason = TerminalReason::Completed;
            out.text = so.text();
            return out;
        }
        if (!so.has_task) {
            out.error_text = "远端返回值无法识别";
            return out;
        }

        auto reg = store_->register_task(client_->name(), so.task.id,
                                         so.task.context_id, session_key);
        if (reg.is_err()) {
            out.error_text = reg.error;
            return out;
        }
        handle = reg.get();
        task_id = so.task.id;
        context_id = so.task.context_id;
        out.branch = ExecBranch::Probe;

        try_register_callback(handle, task_id);

        // 首个快照并入（远端可能一次就返回 completed）
        ingest_task(handle, so.task, on_progress);

        auto cur = store_->get(handle);
        if (cur && cur->phase == PendingPhase::Running) {
            run_probe(handle, &out, on_progress, deadline);
        }
    }

    // ── 收敛结果 ────────────────────────────────────────────────────────
    out.handle = handle;
    out.task_id = task_id;
    out.context_id = context_id;

    auto fin = store_->harvest(handle);
    if (fin) {
        out.success = true;
        out.completed = true;
        out.state = fin->state;
        out.reason = fin->reason;
        out.text = fin->result_text.empty() ? fin->progress_text : fin->result_text;
        out.error_text = fin->error_text;
        return out;
    }

    // ── 让渡 ────────────────────────────────────────────────────────────
    // ★ success = true（対标 exec.cc:983 返回 tool_result::text 而非 error）。
    //   返回 error 会让模型认为工具坏了并重试 → 同一远端任务被起两遍。
    out.success = true;
    out.completed = false;
    out.yielded = true;
    auto cur = store_->get(handle);
    if (cur) {
        out.state = cur->state;
        out.text = cur->progress_text;
    }

    if (cfg_.background_follow_up && !stopping_.load()) {
        // detached 线程 + cv 交接。★ 不用 future（项目级约束）。
        live_threads_.fetch_add(1);
        std::thread([this, handle]() {
            background_loop(handle);
            live_threads_.fetch_sub(1);
        }).detach();
    }
    return out;
}

// ── 后台跟进 ────────────────────────────────────────────────────────────────

void ExecutionEngine::background_loop(std::string handle) {
    ExecutionResult dummy;
    // ★ deadline = 0 → 跑到终态或 stopping_。
    //   HardTimeout 由 evaluate() 保证一定会到（max_task_ms），不会真的无限。
    run_probe(handle, &dummy, nullptr, 0);
}

// ── poll_once（回调分支专用）────────────────────────────────────────────────

bool ExecutionEngine::poll_once(const std::string& handle) {
    if (!client_ || !store_) return false;

    auto cur = store_->get(handle);
    if (!cur) return false;
    // 已终结 → 无需再拉（回调可能重复投递，远端在没收到 2xx 时会重试）
    if (cur->phase != PendingPhase::Running) return true;

    if (!client_->capabilities().should_try_tasks_get()) {
        // 远端不支持 tasks/get 却配了回调 —— 无权威来源可用。
        // ⚠ 此时【不能】采信回调 payload 作为替代（那正是我们要防的），
        //   也不能判失败（任务可能真在跑）。只记一次无推进，交给 HardTimeout 兜底。
        store_->note_no_progress(handle);
        return false;
    }

    auto t = client_->get_task(cur->task_id);
    if (t.is_err()) {
        // ★ TaskNotFound 是终态：远端已丢弃该任务
        if (t.error.find("task_not_found") != std::string::npos) {
            store_->update_terminal(handle, TerminalReason::RemoteFailed, "",
                                   "远端已不存在该任务（TaskNotFound）");
            return true;
        }
        store_->note_no_progress(handle);
        return false;
    }

    ingest_task(handle, t.get(), nullptr);

    auto after = store_->get(handle);
    if (!after) return false;
    if (after->phase != PendingPhase::Running) return true;

    // 未终结但要检查 HardTimeout / Zombie（回调分支没有探测循环兜底）
    if (auto reason = evaluate(*after, false, "")) {
        std::string err;
        if (*reason == TerminalReason::HardTimeout) {
            err = "远端任务超过最长时限 " + std::to_string(cfg_.max_task_ms) + "ms";
        } else if (*reason == TerminalReason::Zombie) {
            err = "远端任务连续 " + std::to_string(cfg_.probe.max_no_progress) +
                  " 次无推进，判定为僵死";
        }
        store_->update_terminal(handle, *reason, "", err);
        return true;
    }
    return false;
}

// ── follow_up ───────────────────────────────────────────────────────────────

ExecutionResult ExecutionEngine::follow_up(const std::string& handle, int64_t wait_ms) {
    ExecutionResult out;
    out.handle = handle;

    auto cur = store_->get(handle);
    if (!cur) {
        // ⚠ 一期内存存储：进程重启后句柄全部失效。这是已知取舍。
        out.error_text = "未找到该 handle 对应的远端任务（可能已过期或进程已重启）";
        return out;
    }
    out.task_id = cur->task_id;
    out.context_id = cur->context_id;

    auto done = store_->wait_terminal(handle, wait_ms);
    if (!done) {
        out.success = true;
        out.yielded = true;
        auto now = store_->get(handle);
        if (now) {
            out.state = now->state;
            out.text = now->progress_text;
        }
        return out;
    }

    auto fin = store_->harvest(handle);
    const PendingTask& t = fin ? *fin : *done;
    out.success = true;
    out.completed = true;
    out.state = t.state;
    out.reason = t.reason;
    out.text = t.result_text.empty() ? t.progress_text : t.result_text;
    out.error_text = t.error_text;
    return out;
}

// ── cancel ──────────────────────────────────────────────────────────────────

core::Result<bool> ExecutionEngine::cancel(const std::string& handle) {
    auto cur = store_->get(handle);
    if (!cur) return core::Result<bool>::Err("未找到该 handle 对应的远端任务");

    // ★ 先本地标记 Canceled（粘性），再尽力通知远端。
    //   対标 ADK executor/utils.py:31-48：远端 cancel 往往只塞一个 canceled
    //   事件而【不中断】实际执行。若等远端确认再标记，用户点了取消却看到
    //   任务继续跑 —— 体验上等于取消无效。
    store_->update_terminal(handle, TerminalReason::Canceled, "", "已由本地取消");

    auto r = client_->cancel_task(cur->task_id);
    if (r.is_err()) {
        // 远端拒绝取消不改变本地结论（粘性终态已生效）
        return core::Result<bool>::Ok(false);
    }
    return core::Result<bool>::Ok(true);
}

}  // namespace remote_a2a
}  // namespace hunterclaw
