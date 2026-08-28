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
                    "(key={} old_id={})", key, existing->second.approval.approval_id);
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
                    "(key={} id={})", entries_.size(), oldest->first,
                    oldest->second.approval.approval_id);
        entries_.erase(oldest);
    }
    return true;
}

DeferredOutcome DeferredApprovalRegistry::resolve(
    const std::string& key, const std::string& current_fingerprint,
    int64_t wait_ms) {
    DeferredOutcome out;

    // ── 取出并【立即移除】 ──────────────────────────────────────────────
    // ★ 一次性语义 (对齐 OpenClaw 的 .finally(() => pending.delete(key)))。
    //   ⚠ 先移除再等待, 而不是等完再移除: 等待可能长达 120s,
    //     期间同键的第二次 resolve 会拿到同一张凭据并同时等待,
    //     两者都会认为自己拿到了那次批准 —— 一次授权被用两次。
    Entry entry;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            out.resolution = DeferredResolution::NotFound;
            out.reason = "no pending deferred approval for this call";
            return out;
        }
        entry = it->second;
        entries_.erase(it);
    }

    // ── 参数指纹比对 ────────────────────────────────────────────────────
    // ★★ 必须在【等待之前】做。
    //   用户批准的是【登记时那份参数】。若参数已被 hook 改写,
    //   这次批准就不该用于新参数 —— 那等于"拿着批准 A 的授权执行 B",
    //   是权限提升。OpenClaw 的处置同样是【拒绝】(codec.ts:168-176 +
    //   permissions.ts:158-167)。
    //
    // ⚠ 放在等待之后也能挡住, 但会白等一次用户操作 ——
    //   用户点了同意却被告知拒绝, 体验上无法解释。
    if (!current_fingerprint.empty() &&
        current_fingerprint != entry.params_fingerprint) {
        log()->warn("deferred approval params were rewritten; refusing "
                    "(key={} id={})", key, entry.approval.approval_id);
        out.resolution = DeferredResolution::ParamsRewritten;
        out.reason = "tool params were rewritten after approval was requested; "
                     "refusing the original request";
        return out;
    }

    // ── 等待决策 ────────────────────────────────────────────────────────
    auto broker = ToolApprovalRuntime::instance().broker();
    if (!broker) {
        // ⚠ 审批通道在 Defer 发起之后消失(如 shutdown)。
        //   【不能】当成批准 —— 未决 == 未获批。
        log()->error("deferred approval cannot be resolved: no approval surface "
                     "(key={} id={})", key, entry.approval.approval_id);
        out.resolution = DeferredResolution::TimedOut;
        out.reason = "approval surface is no longer available";
        return out;
    }

    auto decision = broker->wait_tool_approval(entry.approval.approval_id, wait_ms);
    if (!decision.has_value()) {
        out.resolution = DeferredResolution::TimedOut;
        out.reason = "approval timed out";
        return out;
    }

    const std::string& d = *decision;
    if (d == "allow-once" || d == "allow-always") {
        out.resolution = DeferredResolution::Approved;
        out.decision = d;
        return out;
    }
    if (d == "deny") {
        out.resolution = DeferredResolution::Denied;
        out.reason = "denied by user";
        return out;
    }

    // ⚠ 未知决策字符串 —— 不当批准（与 Stage 1 的处理一致）。
    log()->error("deferred approval returned unknown decision '{}' (key={})", d, key);
    out.resolution = DeferredResolution::Denied;
    out.reason = "approval returned an unknown decision";
    return out;
}

bool DeferredApprovalRegistry::discard(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    log()->info("discarding deferred approval (key={} id={})", key,
                it->second.approval.approval_id);
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
