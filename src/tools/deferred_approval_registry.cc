// ============================================================================
// deferred_approval_registry.cc
// 见 deferred_approval_registry.h 的对齐说明。
// ============================================================================

#include "hunterclaw/tools/deferred_approval_registry.h"

#include "hunterclaw/core/logger.h"
#include "hunterclaw/tools/tool_approval_runtime.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("tool_approval");
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

const char* deferred_resolution_text(DeferredResolution r) {
    switch (r) {
        case DeferredResolution::Approved:        return "approved";
        case DeferredResolution::Denied:          return "denied";
        case DeferredResolution::TimedOut:        return "timed_out";
        case DeferredResolution::Cancelled:       return "cancelled";
        case DeferredResolution::Unavailable:     return "unavailable";
        case DeferredResolution::ParamsRewritten: return "params_rewritten";
        case DeferredResolution::NotFound:        return "not_found";
    }
    return "unknown";
}

std::string params_fingerprint(const core::JsonValue& params) {
    // ★ JsonValue 内部用 std::map 存对象成员 (types.h:278), 故 to_string()
    //   的键顺序【天然稳定】—— 等价于 OpenClaw 的 stableStringify。
    //   ⚠ 若哪天 obj_val_ 换成 unordered_map, 这里就必须自己排序,
    //     否则同一份参数会产生不同指纹, 每次都被判为"被改写"而拒绝执行。
    return params.to_string();
}

DeferredApprovalRegistry& DeferredApprovalRegistry::instance() {
    static DeferredApprovalRegistry inst;
    return inst;
}

bool DeferredApprovalRegistry::put(const std::string& key,
                                   const DeferredApproval& approval,
                                   const std::string& fingerprint) {
    if (key.empty()) {
        // ⚠ 空键会让所有凭据挤在同一条记录上, 互相覆盖。
        //   宁可登记失败(调用方会走 fail-closed)也不能静默混淆。
        log()->error("deferred approval registry: empty key, refusing to register");
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);

    // 同键已存在 → 取消旧的再覆盖 (对齐 OpenClaw
    // setNativeHookRelayPreToolUseApproval:86-88)。
    // ⚠ 不清理会让旧审批一直挂在待审列表, 用户看到两张同样的卡片,
    //   而其中一张永远不会被任何人收敛。
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        log()->warn("deferred approval registry: replacing pending approval "
                    "(key={} tool={})", key, existing->second.approval.tool_name);
        entries_.erase(existing);
    }

    Entry e;
    e.approval = approval;
    e.params_fingerprint = fingerprint;
    e.created_at_ms = now_ms();
    e.seq = ++seq_;
    entries_[key] = std::move(e);

    // 容量淘汰 —— 淘汰【最旧】的一条 (对齐 OpenClaw 的 oldestKey 逻辑)。
    //
    // ★ 淘汰而非拒绝新登记: 表满通常意味着旧凭据没人收敛(泄漏)。
    //   拒绝新登记会让后续所有 Defer 调用失败, 把一个泄漏问题
    //   扩大成"功能整体不可用"; 淘汰最旧的能自愈。
    if (entries_.size() > kMaxEntries) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.seq < oldest->second.seq) oldest = it;
        }
        log()->warn("deferred approval registry full ({}); evicting oldest "
                    "(key={} tool={})", entries_.size(), oldest->first,
                    oldest->second.approval.tool_name);
        entries_.erase(oldest);
    }
    return true;
}

DeferredOutcome DeferredApprovalRegistry::do_wait(const DeferredApproval& approval,
                                                  int64_t wait_ms,
                                                  const std::atomic<bool>* aborted) {
    DeferredOutcome out;

    // ── 薄包装 ──────────────────────────────────────────────────────────
    // 🎯 逐字对齐 OpenClaw requestDeferredPluginToolApproval
    //    (agent-tools.before-tool-call.approval.ts:457-471):
    //      return requestPluginToolApproval({ ...deferred, signal });
    //    —— 它【只是转发】, 不自己实现 create/wait/判定。
    //
    // ⚠⚠ 此前这里复制了一整套 create+wait+判定逻辑, 与 Stage 1 的
    //   Request 分支并行维护。重复已经咬过两次(消费端 allowed_decisions
    //   校验、aborted 透传都得改两个文件), 且行为已经漂移:
    //     · broker 缺失   Request→Failure{unavailable}  Defer→TimedOut
    //     · 越权决策       Request→Failure{timed_out}    Defer→TimedOut
    //     · 被 abort      Request→Failure{cancelled}    Defer→TimedOut
    //   三种不同原因被压成同一个 TimedOut, 使"审批面挂了"与"用户没响应"
    //   在审计上无法区分 —— 而这正是 Stage 1 特意分开的东西。
    //   收敛后不可能再漂移。
    //
    // ★ broker 在此解析后【按值传给】共享实现: 它内部会阻塞等待,
    //   持 shared_ptr 保证等待期间对象不被 shutdown 销毁。
    //   broker 为空时共享实现自己 fail-closed(不放行), 无需在这里预判。
    auto broker = ToolApprovalRuntime::instance().broker();

    // ★ 超时用凭据里记下的值(Stage 1 声明时的), 不用收敛点的 ——
    //   用户看到的倒计时应与声明时一致。
    const int64_t timeout_ms = wait_ms > 0 ? wait_ms : approval.timeout_ms;

    HookOutcome o = request_plugin_tool_approval(
        broker, approval.tool_name, approval.tool_call_id, approval.agent_id,
        approval.session_key, timeout_ms, approval.allowed_decisions, aborted,
        core::JsonValue::object());   // params 在此路径不参与判定

    // ── HookOutcome → DeferredOutcome ───────────────────────────────────
    // ★ 按 disposition 映射, 保住 Stage 1 已经分好的审计类别 ——
    //   不再把三种原因压成 TimedOut。
    if (o.kind == HookOutcomeKind::Pass) {
        out.resolution = DeferredResolution::Approved;
        out.decision = o.approval_resolution.value_or("allow-once");
        return out;
    }

    const std::string disp = o.disposition.value_or("");
    if (disp == disposition::kBlocked) {
        // 审批跑完、结论是拒绝
        out.resolution = DeferredResolution::Denied;
    } else if (disp == disposition::kCancelled) {
        out.resolution = DeferredResolution::Cancelled;
    } else if (disp == disposition::kFailed) {
        // broker 缺失 / 发起失败 —— 审批【面】不可用, 不是用户没响应
        out.resolution = DeferredResolution::Unavailable;
    } else {
        // kTimedOut: 等满超时 / 越权决策(OpenClaw 归为 TIMEOUT)
        out.resolution = DeferredResolution::TimedOut;
    }
    out.reason = o.reason.value_or("approval was not granted");
    return out;
}

DeferredOutcome DeferredApprovalRegistry::resolve(
    const std::string& key, const std::string& current_fingerprint,
    int64_t wait_ms, const std::atomic<bool>* aborted) {
    DeferredOutcome out;

    std::shared_future<DeferredOutcome> shared;
    std::promise<DeferredOutcome> promise;
    DeferredApproval approval;
    bool i_am_executor = false;
    uint64_t my_seq = 0;   // 收尾时确认"删的是自己那条"

    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            // 已收敛过 / 已被淘汰 / 从未登记。
            // ⚠ OpenClaw 对"决议后的第三次调用"返回 undefined(未处理),
            //   语义上等同这里的 NotFound —— 都表示"没有这张凭据"。
            out.resolution = DeferredResolution::NotFound;
            out.reason = "no pending deferred approval for this call";
            return out;
        }
        Entry& e = it->second;

        // ── 参数指纹比对 ────────────────────────────────────────────────
        // ★★ 必须在等待之前做, 且【每个调用者各自比对】。
        //   用户批准的是【登记时那份参数】。若参数已被 hook 改写,
        //   这次批准就不该用于新参数 —— 那等于"拿着批准 A 的授权执行 B",
        //   是权限提升。OpenClaw 同样是拒绝 (codec.ts:168-176 +
        //   permissions.ts:158-167)。
        //
        // ★ 不共享此判定: 两条入站通道看到的参数可能不同,
        //   各自都必须与批准时一致才行。
        //
        // ⚠ 放在等待之后也能挡住, 但会白等一次用户操作 ——
        //   用户点了同意却被告知拒绝, 体验上无法解释。
        if (!current_fingerprint.empty() &&
            current_fingerprint != e.params_fingerprint) {
            log()->warn("deferred approval params were rewritten; refusing "
                        "(key={} tool={})", key, e.approval.tool_name);
            out.resolution = DeferredResolution::ParamsRewritten;
            out.reason = "tool params were rewritten after approval was requested; "
                         "refusing the original request";
            return out;
        }

        if (!e.wait_started) {
            // ── 第一个收敛者: 发起真正的等待 ─────────────────────────────
            // 🎯 对齐 OpenClaw permissions.ts:129
            //      pendingApproval.resolutionPromise ??= resolve...(...)
            e.wait_started = true;
            shared = promise.get_future().share();
            e.pending_wait = shared;
            approval = e.approval;
            // ★ 记下这一条的 seq 用于收尾时确认身份。
            //   ⚠ 不能用 approval_id: Defer 在 Stage 1 已不再 create 审批,
            //     凭据里没有 id(id 是收敛点才生成的)。seq 由 put 单调分配,
            //     覆盖时会换新值, 正好能区分"同键但已被换过的新凭据"。
            my_seq = e.seq;
            i_am_executor = true;
        } else {
            // ── 并发的后续收敛者: 复用同一次等待 ─────────────────────────
            // ★★ 拿到与第一个调用者【相同的结论】, 而不是"因为已被占用
            //   所以失败"。同键 = 同一次逻辑工具调用, 它可能被两条入站
            //   通道分别收敛; 若第二个拿到拒绝, 则"能否执行"取决于哪条
            //   通道先到 —— 用户明确批准过的操作会随机失败。
            //   OpenClaw 测试对此有硬断言
            //   (native-hook-relay.test.ts:2415-2434)。
            shared = e.pending_wait;
        }
    }

    if (!i_am_executor) {
        // ⚠ 不持锁等待。shared_future 允许多次 get(), 每次返回同一份值。
        return shared.get();
    }

    // ⚠ 等待【不持锁】: 它可能长达 120s, 持锁会让 put/discard/size
    //   在此期间全部卡死, 整张表不可用。
    DeferredOutcome result = do_wait(approval, wait_ms, aborted);
    promise.set_value(result);

    // ── 完成后移除 ──────────────────────────────────────────────────────
    // ★ 对齐 OpenClaw 的 `.finally(() => pending.delete(key))`。
    //   ⚠ 是"完成之后"而非"进入时立刻" —— 立刻移除会让并发的第二个
    //     收敛者找不到凭据, 退化成上面那个随机失败的情形。
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        // 只删自己那条: 等待期间可能已被 put 覆盖成新凭据(seq 会换新值),
        // 误删会把一张尚未收敛的新凭据丢掉。
        if (it != entries_.end() && it->second.seq == my_seq) {
            entries_.erase(it);
        }
    }
    return result;
}

bool DeferredApprovalRegistry::discard(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    // ★ 丢弃未收敛的凭据【无需通知任何人】: Stage 1 不再 create 审批,
    //   所以此刻 broker 里根本没有对应请求, 用户也没见过卡片。
    //   ⚠ 这正是"收敛点才发起"带来的额外好处 —— 旧实现下 discard
    //     必须额外去 broker 撤掉那条已广播的请求, 漏掉就留下一张
    //     永远无人收敛的僵尸卡片。
    log()->info("discarding deferred approval (key={} tool={})", key,
                it->second.approval.tool_name);
    entries_.erase(it);
    return true;
}

size_t DeferredApprovalRegistry::discard_prefix(const std::string& prefix) {
    if (prefix.empty()) {
        // ⚠ 空前缀会匹配全部 —— 那是 reset 而不是"清理某个 run"。
        //   静默清空整张表会让其他 run 的在途凭据全部失效。
        log()->error("discard_prefix called with empty prefix; refusing");
        return 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = entries_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    if (n > 0) {
        log()->info("discarded {} deferred approval(s) with prefix '{}'", n, prefix);
    }
    return n;
}

size_t DeferredApprovalRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

void DeferredApprovalRegistry::reset_for_testing() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
    seq_ = 0;
}

}  // namespace tools
}  // namespace hunterclaw
