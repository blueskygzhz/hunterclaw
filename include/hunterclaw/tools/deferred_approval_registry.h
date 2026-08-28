// ============================================================================
// deferred_approval_registry.h — Defer 模式审批凭据的登记与收敛
//
// 🎯 严格对齐 OpenClaw:
//   src/agents/harness/native-hook-relay-permissions.ts
//     setNativeHookRelayPreToolUseApproval()       登记 + 取消旧的 + 容量淘汰
//     resolveNativeHookRelayDeferredToolApproval() 一次性解析
//     removeNativeHookRelayPreToolUseApprovals()   按 run 批量清理
//   src/agents/harness/native-hook-relay-codec.ts:168-176
//     nativeHookRelayParamsWereRewritten()         参数指纹比对
//
// ★★ 为什么 Defer 必须有登记表:
//   Defer 的语义是"审批已发起但不在这里等, 工具照常执行"。若发出去就不管:
//     · 凭据无人收敛 ⇒ 审批一直挂到 timeout 才过期, 期间占着待审列表
//     · 用户点了"拒绝"也【没人看】 ⇒ 审批形同虚设
//   登记表让"稍后收敛"成为可能, 也是二期「二次确认」的落点。
// ============================================================================

#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/before_tool_call_policy.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace hunterclaw {
namespace tools {

/// 收敛结果
enum class DeferredResolution {
    Approved,        // allow-once / allow-always
    Denied,          // 用户拒绝
    TimedOut,        // 无人决策
    ParamsRewritten, // ★ 批准后参数被改写 → 视为拒绝（见下）
    NotFound,        // 无此凭据（已收敛过 / 已被淘汰 / 从未登记）
};

const char* deferred_resolution_text(DeferredResolution r);

struct DeferredOutcome {
    DeferredResolution resolution = DeferredResolution::NotFound;
    /// Approved 时为 "allow-once" / "allow-always"
    std::string decision;
    std::string reason;

    bool approved() const { return resolution == DeferredResolution::Approved; }
};

// ── DeferredApprovalRegistry ────────────────────────────────────────────────
// 进程级登记表。
//
// ★ 为什么进程级: 与 ToolApprovalRuntime 同理 —— broker 是进程级的,
//   凭据也必须在同一个表里, 否则"哪个表持有这张凭据"取决于是哪个会话
//   发起的, 而收敛方（可能是另一条路径）找不到它。
class DeferredApprovalRegistry {
public:
    static DeferredApprovalRegistry& instance();

    /// 登记一张凭据。
    ///
    /// @param key 幂等键。建议 run_id + ":" + tool_call_id
    ///        ⚠ 不能只用 tool_call_id: 不同 run 的 id 可能重复,
    ///          撞键会让 A 的凭据被 B 顶掉（下面会取消旧的）。
    /// @param params_fingerprint 发起审批时的参数指纹。
    ///        ★★ 必须记下来。用户批准的是【那一份参数】;
    ///          若之后 hook 改写了参数还沿用这次批准, 等于
    ///          "拿着批准 A 的授权去执行 B" —— 这是权限提升。
    ///          OpenClaw 对此的处置是【拒绝】(codec.ts:168-176)。
    ///
    /// ⚠ 同键已存在时会【取消旧凭据】再覆盖 —— 对齐 OpenClaw
    ///   setNativeHookRelayPreToolUseApproval:87。不取消会让旧审批
    ///   一直挂在待审列表里, 用户看到两张同样的卡片。
    ///
    /// @return 是否登记成功（key 为空则失败）
    bool put(const std::string& key, const DeferredApproval& approval,
             const std::string& params_fingerprint);

    /// 收敛一张凭据（阻塞等待决策）。
    ///
    /// ★ 一次性: 无论结果如何, 返回前都会把凭据从表中移除 ——
    ///   对齐 OpenClaw 的 `.finally(() => pending.delete(key))`。
    ///   否则同一张凭据被收敛两次, 第二次会拿到已过期的结论。
    ///
    /// @param current_params_fingerprint 当前(可能被 hook 改写过的)参数指纹。
    ///        与登记时不一致 ⇒ 返回 ParamsRewritten（视为拒绝）。
    ///        传空表示跳过比对（调用方确认参数未变）。
    /// @param wait_ms <=0 时沿用 broker 的默认超时
    DeferredOutcome resolve(const std::string& key,
                           const std::string& current_params_fingerprint = "",
                           int64_t wait_ms = 0);

    /// 丢弃一张凭据（不等决策）。
    /// ⚠ 仅在"已经决定不执行这个工具"时调用。丢弃【不会】撤销远端审批,
    ///   它会自然过期 —— 主动撤销需要 broker 支持 cancel, 目前没有。
    bool discard(const std::string& key);

    /// 按前缀批量丢弃（run 结束时清理）。
    /// 对齐 OpenClaw removeNativeHookRelayPreToolUseApprovals()。
    /// @return 丢弃的条数
    size_t discard_prefix(const std::string& key_prefix);

    size_t size() const;

    /// 仅供测试
    void reset_for_testing();

    /// 容量上限。超出时淘汰最旧的一条（对齐 OpenClaw 的
    /// MAX_NATIVE_HOOK_RELAY_INVOCATIONS 淘汰逻辑）。
    ///
    /// ⚠ 淘汰而非拒绝新登记: 拒绝会让新工具调用无法走 Defer,
    ///   而表满通常是因为旧凭据没人收敛（泄漏）—— 淘汰最旧的
    ///   能自愈, 拒绝只会把故障扩大。
    static constexpr size_t kMaxEntries = 256;

private:
    DeferredApprovalRegistry() = default;

    struct Entry {
        DeferredApproval approval;
        std::string params_fingerprint;
        int64_t created_at_ms = 0;
        uint64_t seq = 0;   // 插入序，用于"最旧"判定
    };

    mutable std::mutex mu_;
    std::map<std::string, Entry> entries_;
    uint64_t seq_ = 0;
};

// ── params_fingerprint ──────────────────────────────────────────────────────
// 稳定序列化参数用于比对。
//
// ★ 对齐 OpenClaw stableStringify: 键必须【有序】,
//   否则同一份参数因序列化顺序不同产生不同指纹, 每次都被判为"被改写"。
std::string params_fingerprint(const core::JsonValue& params);

}  // namespace tools
}  // namespace hunterclaw
