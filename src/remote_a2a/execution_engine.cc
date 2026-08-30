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

// ── 二次确认信号解析（★ 全部集中在此，报文位置定了只改这里）─────────────────

namespace {

/// 从一个 JSON 对象里抽 requiresUserAction。
/// ★ 抽成函数是因为要对 part.metadata 与 part.data 各扫一遍 ——
///   两处都可能承载，见 scan_confirm_object 的调用点说明。
void scan_user_action_flag(const nlohmann::json& obj, ConfirmSignal& sig) {
    if (!obj.is_object()) return;
    // 取「或」——任一处声明即为真
    for (const char* k : {"requiresUserAction", "requires_user_action",
                          "requireUserAction", "require_user_action"}) {
        auto it = obj.find(k);
        if (it != obj.end() && it->is_boolean() && it->get<bool>()) {
            sig.requires_user_action = true;
        }
    }
}

/// 从一个 JSON 对象里抽 tokenIds。
///
/// ★ 键名兼容四种写法。实测企微用的是 `token_id_list`
///   (status.message.parts[].data)，而最初只认 tokenIds/token_ids ——
///   ⚠ 键名不匹配的后果不是"少一个字段"，而是【整条二次确认链路静默失效】:
///     token 空 → requires_user_action 保持配置默认(false) →
///     模型把"请确认"当普通追问代答"好的" → 邮件真的发出去。
///     宁可多认几个别名，也不能漏。
void scan_token_ids(const nlohmann::json& obj, ConfirmSignal& sig) {
    if (!obj.is_object()) return;
    for (const char* k : {"tokenIds", "token_ids", "tokenIdList", "token_id_list"}) {
        auto it = obj.find(k);
        if (it == obj.end() || !it->is_array()) continue;
        for (const auto& v : *it) {
            if (!v.is_string()) continue;
            const std::string s = v.get<std::string>();
            // ⚠ 空串与超长值一律丢弃：它们会原样进 details 供终端消费，
            //   终端拿一个空 token 去拉页面只会失败；超长值可能是
            //   远端把整段 JSON 塞进来（防御性截断上限 512）。
            if (s.empty() || s.size() > 512) continue;
            sig.token_ids.push_back(s);
        }
    }
}

}  // namespace

ConfirmSignal extract_confirm_signal_from_parts(const std::vector<Part>& parts,
                                               bool force_user_action) {
    ConfirmSignal sig;
    // ★ 本地配置优先加严：远端【能加严不能放松】。
    //   与 requires_approval 绝不从卡读同一原则 —— 若只听远端，
    //   远端声明 requiresUserAction=false 就能绕过本地的用户确认要求。
    sig.requires_user_action = force_user_action;

    for (const auto& p : parts) {
        // ── 同时扫 metadata 与 data ──────────────────────────────────────
        // ★★ 两处都要扫，不能只扫一处。
        //
        //   A2A 规范(a2a.proto:224-242)里 Part 的 `data`(oneof content) 与
        //   `metadata` 是【两个独立字段】，都合法。实测企微把 token 放在
        //   `data` 里、只用 `metadata.wecom_part_type` 做分类:
        //
        //     { "data":     { "token_id_list": [...] },
        //       "metadata": { "wecom_part_type": "approval" } }
        //
        //   ⚠⚠ 曾经这里只扫 metadata，且遇到 `!p.metadata.is_object()`
        //     就 continue —— 那会把上面这种 part 整个跳过。后果是
        //     【二次确认静默不触发】: 模型把"将向 2 位客户发送确认邮件，
        //     请确认。"当成普通追问并代用户答"好的"，邮件真的发出去。
        //     这正是整条链路要防的事。
        //
        //   ★ 不依赖 wecom_part_type=="approval" 来定位 part:
        //     那是厂商私有约定(规范的正式扩展机制是 Message.extensions,
        //     a2a.proto:274，企微并未声明)。按【字段名】扫全部 part
        //     才能同时兼容其他实现。
        scan_user_action_flag(p.metadata, sig);
        scan_user_action_flag(p.data, sig);
        scan_token_ids(p.metadata, sig);
        scan_token_ids(p.data, sig);
    }

    // ⚠ 去重但【保序】：终端可能按顺序逐个确认，
    //   用 set 去重会打乱顺序，让"先确认哪一个"变得不可预期。
    std::vector<std::string> uniq;
    for (const auto& s : sig.token_ids) {
        bool dup = false;
        for (const auto& u : uniq) {
            if (u == s) { dup = true; break; }
        }
        if (!dup) uniq.push_back(s);
    }
    sig.token_ids = std::move(uniq);

    // ★ 有 tokenIds 就意味着"有东西待用户确认" —— 隐含 requires_user_action。
    //   ⚠ 不做这个推断会出现"给了 token 却没标 requiresUserAction"的报文
    //     被当成模型可代答，而那些 token 永远没人去确认。
    if (!sig.token_ids.empty()) sig.requires_user_action = true;

    return sig;
}

// ── 两个薄包装：都委派给 extract_confirm_signal_from_parts ──────────────────
//
// ⚠ 不要在任一包装里补充/修改判定逻辑。判据只能有一处 ——
//   当初"只有 Task 版"就是因为 SSE 版被忘了，而缺失是静默的。

ConfirmSignal extract_confirm_signal(const Task& t, bool force_user_action) {
    if (!t.status.message.has_value()) {
        ConfirmSignal sig;
        sig.requires_user_action = force_user_action;
        return sig;
    }
    return extract_confirm_signal_from_parts(t.status.message->parts, force_user_action);
}

ConfirmSignal extract_confirm_signal(const ParsedStreamEvent& ev, bool force_user_action) {
    if (!ev.status_message.has_value()) {
        ConfirmSignal sig;
        sig.requires_user_action = force_user_action;
        return sig;
    }
    return extract_confirm_signal_from_parts(ev.status_message->parts, force_user_action);
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
        // 让渡时"当前进度"来自中间态，不是正式结果
        const std::string& progress = prompt_text.empty() ? text : prompt_text;
        if (!progress.empty()) ss << "\n当前进度: " << progress;
        return ss.str();
    }

    // ★ 挂起态必须在 error_text 分支【之前】判定。
    //   input-required / auth-required 不是失败，而是一次正常的往返中断；
    //   若落进失败分支，模型会认为工具坏了并重试，而重试会用同样的输入
    //   再起一个新任务 —— 远端上于是堆积一串永远等不到输入的僵尸任务。
    if (is_suspended()) {
        const bool need_auth = (state == TaskState::AuthRequired);
        // ★★ 措辞按「谁来答」分流。
        //   requires_user_action 时必须明说是【用户】确认 ——
        //   否则模型会把"补充输入"理解成自己的任务。
        if (requires_user_action) {
            ss << "远端任务需要【用户】确认后才能继续";
        } else {
            ss << (need_auth ? "远端任务需要鉴权后才能继续"
                             : "远端任务需要补充输入才能继续");
        }
        ss << "（handle " << handle;
        // ★★ task_id 必须出现在【文本】里。
        //   ToolResult.details 不进 prompt（react_engine.cc:1076-1088 只读
        //   content），模型只能看到这段文本。缺 task_id 就无法续接，
        //   整个 input-required 流程直接断掉。
        if (!task_id.empty()) ss << ", task_id " << task_id;
        if (!context_id.empty()) ss << ", context_id " << context_id;
        ss << "）。";

        // ★★ 追问与部分结果【都要】输出，不能二选一。
        //   远端可以同时给出「已完成的部分」与「还缺什么」；
        //   只输出其一会让模型看到"需要补充输入: <部分结果>"，
        //   完全不知道要补什么。
        if (!prompt_text.empty()) {
            ss << "\n远端要求: " << prompt_text;
        } else if (requires_user_action) {
            ss << "\n远端未说明具体确认内容，请向用户转述并请其确认。";
        } else {
            // 远端没说要什么 —— 明确告诉模型，避免它凭空猜测后乱填
            ss << "\n远端未说明具体需要什么，请根据上下文判断或向用户询问。";
        }
        if (!text.empty()) {
            ss << "\n已获得的部分结果: " << text;
        }

        if (requires_user_action) {
            // ★★★ 必须【显式禁止代答】。
            //
            //   没有这句，模型完全可能自己填一个"确认"——
            //   远端要的是用户的决定，拿到的是模型的猜测。
            //   而这类错误极难发现：流程走通了、结果返回了，
            //   只是决定权被悄悄拿走了。
            ss << "\n⚠ 此项必须由用户明确答复，你【不得】代为判断、猜测或"
                  "自行确认。请向用户转述上述内容并等待其明确答复。";
            if (!pending_token_ids.empty()) {
                ss << "\n（有 " << pending_token_ids.size()
                   << " 项待用户在终端确认）";
            }
            ss << "\n拿到用户答复后，再次调用本工具并带上 task_id";
            if (!context_id.empty()) ss << " 与 context_id";
            ss << "，message 填写用户的原话。";
        } else {
            // 明确的续接指令：模型知道"要做什么"才不会卡住或改走别的工具
            ss << "\n补齐后请再次调用本工具，并带上 task_id";
            if (!context_id.empty()) ss << " 与 context_id";
            ss << "，message 填写补充内容。";
        }
        return ss.str();
    }

    if (!error_text.empty()) {
        ss << "远端任务失败";
        if (reason) ss << "（" << terminal_reason_text(*reason) << "）";
        ss << ": " << error_text;
        if (!text.empty()) ss << "\n已获得的部分结果: " << text;
        return ss.str();
    }

    // 正常完成：artifacts 优先，无 artifacts 时回落到 status.message
    if (!text.empty()) return text;
    if (!prompt_text.empty()) return prompt_text;
    return "远端任务已完成，但未返回内容。";
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
    // ★★ HardTimeout 按【本轮交互】计时，不是按记录创建时间。
    //   ⚠ 用 created_at_ms 会把"用户思考确认的那几分钟"算进远端的执行
    //     时限里 —— 多轮确认的任务在第二轮就被判超时，而远端其实刚开始跑。
    //   ★ 向后兼容: turn_started_at_ms 为 0（老记录）时回落 created_at_ms。
    const int64_t turn_start =
        t.turn_started_at_ms > 0 ? t.turn_started_at_ms : t.created_at_ms;
    tr.elapsed_ms = now_ms() - turn_start;
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

    if (!is_terminal(t.status.state)) return;

    // ── 终态收敛 ────────────────────────────────────────────────────────
    const bool suspended = is_suspended_state(t.status.state);

    // ⚠ input-required / auth-required 也是终态，但【不是失败】：
    //   它们是「挂起」，本轮结束等外部回填。用 Completed 归因，
    //   由 state 字段区分（ExecutionResult::render 会分开渲染）。
    TerminalReason reason = TerminalReason::Completed;
    std::string err;
    switch (t.status.state) {
        case TaskState::Failed:
            reason = TerminalReason::RemoteFailed;
            err = progress.empty() ? "远端任务失败（未提供原因）" : progress;
            break;
        case TaskState::Canceled:
            reason = TerminalReason::Canceled;
            break;
        default:
            break;
    }

    // 终态：从 artifacts 取正式结果
    const std::string final_text = extract_result_text(t);
    // ★ 挂起态【不】把 status.message 回落进 result_text。
    //   status.message 此时是「还需要什么」的追问，不是结果。若把它当结果：
    //     ① 追问与部分结果挤在同一字段，render 无法同时呈现
    //     ② 追问被当成「已获得的结果」持久化，harvest 取到一句提问
    //   追问始终留在 progress_text 里，由 render 从 prompt_text 取。
    const std::string terminal_text =
        suspended ? final_text : (final_text.empty() ? progress : final_text);

    // ★★ 全部收敛动作交给 settle_terminal —— 它是【唯一】的落终态入口。
    //   ⚠ 不要在这里内联那四步: 这个模式已经漏过三次（本处曾是唯一有
    //     confirm 提取的地方，SSE 两条路径都漏了）。
    settle_terminal(handle, reason, t.status.state, terminal_text, err,
                    t.status.message.has_value() ? &t.status.message->parts : nullptr,
                    // ★ 挂起时把 status.message 的文本当【追问】传入 ——
                    //   settle_terminal 会写进 progress_text 而非 result_text。
                    suspended ? progress : std::string());
}

// ── 挂起态主动通知 ──────────────────────────────────────────────────────────

bool ExecutionEngine::set_suspend_resolver(SuspendResolver resolver) {
    std::lock_guard<std::mutex> lk(resolver_mu_);
    if (suspend_resolver_) {
        // ⚠ 见头文件: 重复注入会让先前在飞的挂起任务的通知落到旧回调上。
        return false;
    }
    if (!resolver) return false;
    suspend_resolver_ = std::move(resolver);
    return true;
}

void ExecutionEngine::maybe_notify_suspended(const std::string& handle) {
    // ★ shutdown 中不再发起提问: 提问会阻塞等待（最长 900s），
    //   而 detached 线程正在被等着退出。
    if (stopping_.load()) return;

    auto cur = store_->get(handle);
    if (!cur) return;
    // ★ 判据与 try_resolve_input_required_via_question 完全一致。
    //   ⚠ 两处不一致会出现"engine 认为该问、工具层认为不该问"，
    //     结果是 claim 被白白消耗掉、谁都没问。
    if (!is_suspended_state(cur->state)) return;
    if (!cur->requires_user_action) return;

    SuspendResolver resolver;
    {
        std::lock_guard<std::mutex> lk(resolver_mu_);
        resolver = suspend_resolver_;
    }
    // ⚠ 必须出锁再调: resolver 内部会 engine->execute() 做续接，
    //   持 resolver_mu_ 调用会死锁（execute 路径也会读 resolver）。
    if (!resolver) return;
    // ★★ 必须吞掉所有异常。
    //   本函数会在 SSE 回调线程 / 回调 HTTP 线程 / detached 后台线程里执行，
    //   ⚠ detached 线程里逃逸的异常会直接 std::terminate 整个进程。
    //
    //   ★ 这里【不打日志】—— src/remote_a2a/ 全模块刻意零日志依赖
    //     （便于用 stub 做单测）。记录责任在 resolver 实现方，它本来就在
    //     有 logger 的工具层。
    try {
        resolver(handle);
    } catch (...) {
    }
}

// ── settle_terminal —— 终态收敛的唯一入口 ────────────────────────────────────
// 见 execution_engine.h 里对四步顺序的说明。
void ExecutionEngine::settle_terminal(const std::string& handle, TerminalReason reason,
                                     TaskState remote_state,
                                     const std::string& result_text,
                                     const std::string& error_text,
                                     const std::vector<Part>* parts,
                                     const std::string& prompt_text) {
    const bool suspended = is_suspended_state(remote_state);

    // ① confirm 信号（必须最先 —— update_terminal 会 notify 等待方）
    if (suspended && parts != nullptr) {
        ConfirmSignal sig =
            extract_confirm_signal_from_parts(*parts, cfg_.require_user_confirm_on_input);
        if (sig.requires_user_action || !sig.token_ids.empty()) {
            store_->note_confirm_signal(handle, sig.requires_user_action, sig.token_ids);
        }
    }

    // ② 挂起时把追问写进 progress_text（必须在 update_terminal 之前 ——
    //    note_progress 会拒绝已终结的记录）
    if (suspended && !prompt_text.empty()) {
        // ⚠ ProgressFingerprint 刻意不含 state（只比对"有没有新内容"，
        //   见 task_tracking.h）。这里用追问长度让指纹产生变化，
        //   确保 note_progress 认定"有推进"从而真的写入 progress_text。
        ProgressFingerprint fp;
        fp.last_status_message_len = prompt_text.size();
        store_->note_progress(handle, fp, remote_state, prompt_text);
    }

    // ③ 落终态（带远端真实 state）
    store_->update_terminal(handle, reason, result_text, error_text, remote_state);

    // ④ 挂起通知（必须在 ③ 之后 —— resolver 会读快照）
    maybe_notify_suspended(handle);
}


// ── ③ 回调注册 ──────────────────────────────────────────────────────────────

void ExecutionEngine::try_register_callback(const std::string& handle,
                                            const std::string& task_id) {
    if (!cfg_.enable_callback || cfg_.callback_base_url.empty()) return;
    if (!client_->capabilities().should_try_push()) return;

    // ── 续接场景：handle 复用时【不要】重复注册 ──────────────────────────
    //
    // ★★ 复用 handle 意味着回调 URL 完全没变，远端那份 push config 仍然有效。
    //
    // ⚠⚠ 重复注册会制造一个"本地已换锁、远端还拿旧钥匙"的窗口:
    //   下面第 501 行是【先覆盖本地 token，再通知远端】。若 set_push_config
    //   失败（网络抖动 / 远端限流），远端仍用旧 token 推送，而本地只认新
    //   token → 该任务的【所有回调被判非法而丢弃】，续接这一轮彻底退化成
    //   只靠退避探测。而日志里只会看到"回调注册失败（不阻断）"这一条 warn，
    //   完全指不到"回调从此全被拒"这个后果。
    //
    // ★ 已有 token 就直接复用，连 rotate 都不做 —— rotate 的收益（凭据轮换）
    //   远小于上述风险，而 token 本身已是随机且随记录 TTL 失效。
    if (auto cur = store_->get(handle)) {
        if (!cur->callback_token.empty()) return;
    }

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

ExecutionEngine::StreamRunResult ExecutionEngine::run_stream(
    const Message& msg, const std::string& session_key, ExecutionResult* out,
    ProgressFn on_progress, int64_t deadline_ms) {
    StreamRunResult res;

    // ★★ 惰性登记：第一个带 task_id 的事件到达时就地 register_task。
    //   ⚠ 绝不能"先开一条流探 task_id、关掉、再开第二条流" ——
    //     那会让同一条 msg 被 message/stream 发两次:
    //       · 首发 → 远端建两个 task，第二个成孤儿（副作用执行两遍）
    //       · 续接 → "确认，请继续"投递两次（邮件可能发两遍）
    //     见头文件 run_stream 的完整说明。
    uint64_t my_epoch = 0;

    auto on_event = [&](const ParsedStreamEvent& ev) -> bool {
        if (stopping_.load()) return false;

        // ── 惰性登记 ────────────────────────────────────────────────────
        if (res.handle.empty()) {
            // ⚠ 还没拿到 task_id 的事件先放过 —— 远端可能先推一个纯 message
            //   事件再给 task。此时无处落库，但也不能断流。
            if (ev.task_id.empty()) {
                // ★ 仍然把文本透给用户：这是真实进度，丢掉会让用户以为卡住。
                if (on_progress && !ev.text.empty()) on_progress(ev.text, true);
                return true;
            }
            auto reg = store_->register_task(client_->name(), ev.task_id,
                                             ev.context_id, session_key);
            if (reg.is_err()) {
                res.error = reg.error;
                return false;
            }
            res.handle = reg.get();
            res.task_id = ev.task_id;
            res.context_id = ev.context_id;
            // ★ 回调注册只做一次，且必须在拿到 handle 之后。
            try_register_callback(res.handle, res.task_id);
            // ★★ epoch 必须在登记之后取 —— 续接会 ++epoch，取早了会拿到上一代。
            my_epoch = store_->current_epoch(res.handle);
        }

        const std::string& handle = res.handle;

        // ★ 每个事件都校验代号: 本流若已属于上一代，立刻停止读取。
        //   ⚠ 返回 false 会让 client 关闭这条 SSE 连接 —— 这正是
        //     "挂起后所有线程都要退出"要的效果。
        if (my_epoch != 0 && store_->current_epoch(handle) != my_epoch) return false;

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
            const bool suspended = is_suspended_state(ev.state);

            // ★★ 唯一落终态入口 —— 四步顺序、confirm 提取、挂起通知全在里面。
            //   ⚠ 这条路径【曾经整块缺失 confirm 提取】: run_stream 不经
            //     ingest_task，而当初四步是内联在 ingest_task 里的。
            //     结果 SSE 上 requires_user_action 恒为 false，二次确认全废。
            settle_terminal(handle, reason, ev.state,
                            suspended ? std::string() : ev.text, err,
                            ev.status_message.has_value() ? &ev.status_message->parts
                                                          : nullptr,
                            suspended ? ev.text : std::string());
            res.terminal = true;
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
        //
        // ⚠⚠ 但若已经登记成功（远端确实建了 task，只是流中断了），
        //   【绝不能】让调用方降级去重发 message/send —— 那又是一次重复投递。
        //   stream_ok=true 表示"任务已在远端建立"，调用方应改走探测。
        res.error = r.error;
        res.stream_ok = !res.handle.empty();
        if (out) out->error_text = r.error;
        return res;
    }
    res.stream_ok = true;
    return res;
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

    // ★★ 记下本线程所属的交互代号。
    //   ⚠ 每轮循环都要校验: 本线程可能正睡在退避里（最长 60s），期间远端
    //     挂起、用户答复、续接已把 phase 拉回 Running。没有 epoch 校验的话
    //     本线程醒来会判定"任务还在跑"而继续探测，与新一轮抢同一条记录 ——
    //     两个线程双倍推进 consecutive_no_progress → 误判 Zombie 把正常
    //     任务杀掉，且 fp 比较基准互相覆盖。
    const uint64_t my_epoch = store_->current_epoch(handle);
    if (my_epoch == 0) return false;  // 记录已不存在

    while (!stopping_.load()) {
        // ★ epoch 校验必须在【读记录之前】，且每轮都做。
        //   放在循环末尾会让本轮的 get_task / note_progress 已经污染过记录。
        if (store_->current_epoch(handle) != my_epoch) {
            // 已进入下一代交互 —— 本线程的任务结束了。
            // ★ 返回 false 而非 true: true 表示"已取得终态"，会让调用方
            //   去 harvest 一条【属于新一代】的记录。
            return false;
        }
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

        // ★ 睡醒后【立刻】再校验一次 epoch —— 这是最关键的一处。
        //   ⚠ 上面的循环头校验发生在睡之前；这一次退避可能长达 60s，
        //     期间续接已经发生。不在这里拦住，下一行的 get_task 就会
        //     以【上一代】的身份去探测远端，并把结果写进新一代的记录。
        if (store_->current_epoch(handle) != my_epoch) return false;

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
    // ★ 流已建立但未取得终态时为 true —— 此时远端【已经有 task 在跑】，
    //   绝不能降级去重发 message/send。
    bool stream_established = false;

    if (try_stream) {
        // ★★ 一次 message/stream 走完全程，登记在首个带 task_id 的事件里惰性完成。
        //   ⚠ 曾经这里是"先开流探 task_id → 关流 → 再开流读全程"，
        //     导致同一条 msg 被发两次（首发建两个 task；续接投递两次确认）。
        //     见 run_stream 头文件注释。
        auto sr = run_stream(msg, session_key, &out, on_progress, deadline);
        if (!sr.handle.empty()) {
            handle = sr.handle;
            task_id = sr.task_id;
            context_id = sr.context_id;
            out.branch = ExecBranch::Stream;
            stream_established = true;
            // ★ 流内已经 settle_terminal 过（若拿到终态），这里无需再处理。
            //   未拿到终态 = 让渡窗口到点或流被对端关闭 → 下方 harvest 会
            //   返回空，走让渡路径 + 后台跟进。
        }
        // ★ SSE 失败且未登记 → 静默降级到 send + 探测。
        //   不返回错误：SSE 不可用是常见情况（中间代理、旧版服务端）。
    }

    if (handle.empty() && !stream_established) {
        // ── 降级：message/send + 退避探测 ────────────────────────────
        // ⚠ 只有在【远端确实没有 task 在跑】时才走这里。stream_established
        //   为真说明流已建立过（哪怕后来断了），此时重发 message/send 会
        //   造成第二次投递 —— 首发建两个 task，续接把确认发两遍。
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
    } else if (stream_established && !handle.empty()) {
        // ── 流中断但任务已在远端 → 用探测接续，【不】重发消息 ──────────
        // ★ 典型场景: 中间代理在 60s 无数据时掐断 SSE，而远端任务还在跑。
        //   ⚠ 这里若重发 message/send，远端会多出一个重复 task。
        auto cur = store_->get(handle);
        if (cur && cur->phase == PendingPhase::Running &&
            client_->capabilities().should_try_tasks_get()) {
            run_probe(handle, &out, on_progress, deadline);
        }
    }

    // ── 收敛结果 ────────────────────────────────────────────────────────
    // ⚠ handle 可能为空: SSE 建了流但远端【从未给出 task_id】（协议不合规
    //   的实现），而 stream_established 又挡住了降级。此时既没有可跟踪的
    //   任务也没有结果 —— 必须显式报错，不能拿空 handle 去 harvest
    //   （那会静默返回"让渡"，让模型以为任务在后台跑，实际什么都没有）。
    if (handle.empty()) {
        out.success = false;
        out.error_text = out.error_text.empty()
                             ? "远端未返回可跟踪的 task（流中无 taskId）"
                             : out.error_text;
        return out;
    }

    out.handle = handle;
    out.task_id = task_id;
    out.context_id = context_id;

    auto fin = store_->harvest(handle);
    if (fin) {
        out.success = true;
        out.completed = true;
        out.state = fin->state;
        out.reason = fin->reason;
        // ★ 两个字段【原样分开】带出，不在这里做二选一 ——
        //   合并逻辑归 render() 一处所有（它才知道当前状态该怎么呈现）。
        //   在这里就合并会让挂起态丢掉追问（见 render 里的说明）。
        out.text = fin->result_text;
        out.prompt_text = fin->progress_text;
        out.requires_user_action = fin->requires_user_action;
        out.pending_token_ids = fin->pending_token_ids;
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
        out.text = cur->result_text;
        out.prompt_text = cur->progress_text;
        out.requires_user_action = cur->requires_user_action;
        out.pending_token_ids = cur->pending_token_ids;
    }

    if (cfg_.background_follow_up && !stopping_.load()) {
        // detached 线程 + cv 交接。★ 不用 future（项目级约束）。
        // ★★ on_progress 必须传下去。原先是 nullptr —— 后台跟进期间的进度
        //   与挂起时那句"远端要求…"一个字都投递不出去，用户完全不知道
        //   远端在等他。⚠ 按值捕获: 让渡后本栈帧立刻销毁，捕引用必悬垂。
        live_threads_.fetch_add(1);
        std::thread([this, handle, on_progress]() {
            background_loop(handle, on_progress);
            live_threads_.fetch_sub(1);
        }).detach();
    }
    return out;
}

// ── 后台跟进 ────────────────────────────────────────────────────────────────

void ExecutionEngine::background_loop(std::string handle, ProgressFn on_progress) {
    ExecutionResult dummy;
    // ★ deadline = 0 → 跑到终态或 stopping_。
    //   HardTimeout 由 evaluate() 保证一定会到（max_task_ms），不会真的无限。
    //
    // ★★ 挂起（input-required / auth-required）时本线程如何退出:
    //   run_probe 的循环头 `cur->phase != PendingPhase::Running → return true`
    //   会在挂起把 phase 推到 Terminal 后立刻命中。
    //
    //   ⚠ 但退出判定发生在 interruptible_sleep 之后（退避最长 60s）。这段
    //     时间里用户可能已经答复、续接已把 phase 拉回 Running ——
    //     run_probe 内部的 epoch 校验负责拦住这种"僵尸线程复活"，
    //     见其 my_epoch 相关注释。
    run_probe(handle, &dummy, std::move(on_progress), 0);
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
            out.text = now->result_text;
            out.prompt_text = now->progress_text;
            out.requires_user_action = now->requires_user_action;
            out.pending_token_ids = now->pending_token_ids;
        }
        return out;
    }

    auto fin = store_->harvest(handle);
    const PendingTask& t = fin ? *fin : *done;
    out.success = true;
    out.completed = true;
    out.state = t.state;
    out.reason = t.reason;
    // ★ 与 execute() 一致：分开带出，合并逻辑只在 render() 里做一份。
    //   ⚠ 这里若沿用 `result_text.empty() ? progress_text : result_text`，
    //     follow_up 拿到的挂起态就会丢掉追问 —— 而 follow_up 恰恰是
    //     input-required 最常见的入口（让渡后模型来取结果）。
    out.text = t.result_text;
    out.prompt_text = t.progress_text;
    out.requires_user_action = t.requires_user_action;
    out.pending_token_ids = t.pending_token_ids;
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
