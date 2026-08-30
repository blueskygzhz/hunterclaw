#pragma once

// ============================================================================
// remote_a2a/agent_tool.h
// 把远端 A2A agent 暴露成 HunterClaw Tool。
//
// ── 三个工具，不是一个 ──────────────────────────────────────────────────────
//   remote_a2a__<agent>   每个远端一个（发消息）
//   remote_a2a_status     全局一个（按 handle 跟进让渡任务）
//   remote_a2a_cancel     全局一个（按 handle 取消）
//
// ★ 为什么发送工具按 agent 拆、跟进工具全局共享：
//   · 发送要携带该远端的 description/skills 供模型选择 → 必须一对一
//   · 跟进只需要 handle，handle 里已含归属信息 → 拆开只是让工具清单变胖
//     （工具数量直接吃 context，N 个远端拆成 3N 个工具是纯浪费）
//
// ── 复用范式：McpToolAdapter ────────────────────────────────────────────────
// 対标 mcp/mcp_tool_adapter.h —— 同构关系（对端不属于我、协议化、需消杀）。
// 而非 ACP spawn（那是子进程，有进程所有权，能力发现语义也不同）。
//
// ── ★ session_key 绝不从 params 读 ─────────────────────────────────────────
// params 由模型构造 = 不可信。若允许 params 指定 session_key，
// 模型（或注入了 prompt 的远端卡）就能把任务挂到别人的会话上，
// 进度事件与结果会投递到其他用户。故 session_key 只从 ToolContext 取。
//
// ── ★ requires_approval 绝不从卡读 ─────────────────────────────────────────
// 卡是对端完全可控的（agent_card.h:10 的第三条纪律）。
// 若从卡读，远端只要声明 requires_approval=false 就绕过了本地审批。
// ============================================================================

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hunterclaw/remote_a2a/agent_card.h"
#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"
#include "hunterclaw/remote_a2a/remote_agent_client.h"
#include "hunterclaw/tools/tool.h"

namespace hunterclaw {
namespace remote_a2a {

// 前向声明 —— 恢复函数注册需要它，但不要在此 include agent_registry.h
// ⚠ agent_registry.h 会反向 include 本文件（它要用工具类），成环。
class RemoteA2aRegistry;

// ── 工具名 ──────────────────────────────────────────────────────────────────

constexpr const char* TOOL_PREFIX = "remote_a2a__";
constexpr const char* TOOL_STATUS = "remote_a2a_status";
constexpr const char* TOOL_CANCEL = "remote_a2a_cancel";

/// 工具名总长上限。★ 与 mcp/bundle_mcp_names.cc:16 的 TOOL_NAME_MAX_TOTAL
/// 保持一致（64）—— 部分 LLM provider 对函数名长度有硬限制。
constexpr size_t TOOL_NAME_MAX_TOTAL = 64;

/// 由远端逻辑名生成安全工具名。
///
/// 规则対标 mcp/bundle_mcp_names.cc 的 sanitize_tool_fragment：
///   · 非 [A-Za-z0-9_-] 一律替换为 '-'
///   · 空结果回落到 "agent"
///   · 连同前缀截断到 64 字节
///   · 与 reserved 冲突时追加 "-2" / "-3"...
///
/// ⚠ 名字必须【稳定】：同一份配置每次启动应得到同样的工具名，
///   否则 transcript 里历史的 tool_call 名字会对不上现有工具。
///   因此去重后缀基于传入顺序而非哈希/时间。
std::string safe_remote_a2a_tool_name(const std::string& agent_name,
                                     std::vector<std::string>& reserved);

// ── 进度投递 ────────────────────────────────────────────────────────────────

/// 把远端进度/提示作为系统事件投给指定会话。
///
/// ★ 定义在 agent_tool.cc，但【不】只服务工具内部：
///   agent_registry.cc 的挂起态主动通知（SSE/回调/后台跟进三条异步通道）
///   同样要复用它 —— 那三条通道在别的线程上发现远端挂起，此时已没有
///   tool_call 在等结果，只能靠这条推送让用户知道"远端在等你"。
///
/// @param session_key  目标会话。空 = 直接返回（无处可投）。
/// @param agent_name   归属远端名，用于事件前缀 "[remote-a2a:<name>]"。
/// @param text         投递文本；超过 300 字符会被截断（远端可控文本，
///                     不能整段塞进 prompt）。
/// @param is_heartbeat true = 心跳，会被 system_events 按 context_key 去重，
///                     避免把真正的事件（完成/失败）挤出 20 条上限的队列。
void deliver_progress(const std::string& session_key, const std::string& agent_name,
                      const std::string& text, bool is_heartbeat);

// ── 单个远端的策略（★ 全部来自本地配置，不从卡读）────────────────────────

struct AgentToolPolicy {
    /// 是否需要审批。★ 只从本地配置读。
    bool requires_approval = false;
    /// 审批模式："" / "request" / "report" / "deny" / "defer"
    ///
    /// ★ 空串 = 宿主默认（Request，会阻塞等决策）。
    /// ★ "defer" 用于【收敛方不是本次调用】的场景 ——
    ///   例如用户需要在企微页面上点确认。Request 会独占会话直到超时，
    ///   Defer 则放行并把凭据交给 DeferredApprovalRegistry 稍后收敛。
    std::string approval_mode;
    /// 限定可选决策；空 = 默认全集。
    /// ⚠ 高风险远端应设为 {"allow-once","deny"} —— 不允许 allow-always。
    ///   一次误点永久授权会让后续所有该远端调用免审。
    std::vector<std::string> approval_allowed_decisions;
    /// 是否仅 owner 可用
    bool owner_only = false;
    /// 是否把远端 skills 写进工具描述（省 context 时可关）
    bool include_skills_in_description = true;
    /// 描述总长上限（防远端撑爆 context）
    size_t max_description_chars = 900;
};

// ── 发送工具 ────────────────────────────────────────────────────────────────

class RemoteA2aAgentTool : public tools::Tool {
public:
    RemoteA2aAgentTool(std::string tool_name, std::string agent_name,
                       std::shared_ptr<RemoteAgentClient> client,
                       std::shared_ptr<ExecutionEngine> engine,
                       const AgentToolPolicy& policy,
                       const tools::ToolContext& ctx);

    std::string name() const override { return tool_name_; }
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;

    // 🎯 对齐 OpenClaw `execute: async (toolCallId, args, signal)` 的第三参数。
    // ⚠⚠ 本工具【必须】覆写: 它内部可能因 input-required 发起用户提问并
    //   阻塞最长 900s，且占着 main lane 的唯一槽位。不覆写会让 chat.abort
    //   之后用户仍要等 15 分钟。
    tools::ToolResult execute(const core::JsonValue& params,
                              const std::atomic<bool>* aborted) override;
    bool requires_approval() const override { return policy_.requires_approval; }
    std::string approval_mode() const override { return policy_.approval_mode; }
    std::vector<std::string> approval_allowed_decisions() const override {
        return policy_.approval_allowed_decisions;
    }
    bool owner_only() const override { return policy_.owner_only; }

    /// 预热：拉卡并缓存描述。失败不抛异常（工具仍可注册，调用时再报错）。
    void warm_up();

private:
    /// 组装 description。★ 卡文本已由 card_resolver 消杀过，此处只做长度控制。
    std::string build_description(const AgentCard& card) const;

    std::string tool_name_;
    std::string agent_name_;
    std::shared_ptr<RemoteAgentClient> client_;
    std::shared_ptr<ExecutionEngine> engine_;
    AgentToolPolicy policy_;
    /// ★ 只从 ToolContext 取，不从 params 取（见文件头）
    std::string session_key_;

    mutable std::mutex desc_mu_;
    mutable std::string cached_description_;
};

// ── 跟进工具 ────────────────────────────────────────────────────────────────

/// 引擎查找器：handle → 对应引擎。
/// ★ handle 里不含引擎指针，必须由注册表按归属反查 ——
///   否则 status 工具会需要持有全部引擎的引用，且新增远端时要重建工具。
using EngineLookupFn =
    std::function<std::shared_ptr<ExecutionEngine>(const std::string& handle)>;

class RemoteA2aStatusTool : public tools::Tool {
public:
    RemoteA2aStatusTool(std::shared_ptr<PendingTaskStore> store, EngineLookupFn lookup,
                        const tools::ToolContext& ctx);

    std::string name() const override { return TOOL_STATUS; }
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;

    // 🎯 对齐 OpenClaw `execute: async (toolCallId, args, signal)` 的第三参数。
    // ⚠⚠ 本工具【必须】覆写: 它内部可能因 input-required 发起用户提问并
    //   阻塞最长 900s，且占着 main lane 的唯一槽位。不覆写会让 chat.abort
    //   之后用户仍要等 15 分钟。
    tools::ToolResult execute(const core::JsonValue& params,
                              const std::atomic<bool>* aborted) override;

private:
    std::shared_ptr<PendingTaskStore> store_;
    EngineLookupFn lookup_;
    std::string session_key_;
};

class RemoteA2aCancelTool : public tools::Tool {
public:
    RemoteA2aCancelTool(std::shared_ptr<PendingTaskStore> store, EngineLookupFn lookup,
                        const tools::ToolContext& ctx, bool requires_approval);

    std::string name() const override { return TOOL_CANCEL; }
    std::string description() const override;
    Schema parameters() const override;
    tools::ToolResult execute(const core::JsonValue& params) override;

    // 🎯 对齐 OpenClaw `execute: async (toolCallId, args, signal)` 的第三参数。
    // ⚠⚠ 本工具【必须】覆写: 它内部可能因 input-required 发起用户提问并
    //   阻塞最长 900s，且占着 main lane 的唯一槽位。不覆写会让 chat.abort
    //   之后用户仍要等 15 分钟。
    tools::ToolResult execute(const core::JsonValue& params,
                              const std::atomic<bool>* aborted) override;
    bool requires_approval() const override { return requires_approval_; }

private:
    std::shared_ptr<PendingTaskStore> store_;
    EngineLookupFn lookup_;
    std::string session_key_;
    bool requires_approval_ = false;
};


// ── interrupt 上抛（remote_a2a ←→ ReactEngine 的接缝）──────────────────────
//
// 🎯 设计: remote_a2a 只把远端挂起态翻译成 ToolInterrupt 上抛，
//   由 ReactEngine 负责"投追问给用户 → 等答复 → 调恢复函数"。
//
// ★★ 为什么不在工具内部提问（上一版的做法，已推翻）:
//   ① 工具拿不到 run 的投递面（on_block_reply）与入站消息路由，
//      那两样都在 ReactEngine 手里；
//   ② 每个需要提问的工具都得重写一遍这套逻辑；
//   ③ 被迫另造 question.* 协议面 —— 而它没有任何客户端实现，
//      等满 900s 后会把远端任务 cancel 掉，比不做更糟。

/// 本子系统的中断类型标识。ReactEngine 按它路由到下面注册的恢复函数。
inline constexpr const char* kRemoteA2aInterruptKind = "remote_a2a.input_required";

/// 把远端挂起态翻译成 ToolInterrupt。
///
/// @return 无值 = 不是挂起态，或缺 task_id/handle 无法恢复
///   ⚠ 缺恢复凭据时【绝不】产出 interrupt: 那只会让用户白答一次，
///     而答复无处可去。
std::optional<tools::ToolInterrupt> build_interrupt_from_result(
    const ExecutionResult& r, const std::string& agent_name);

/// 注册 remote_a2a 的恢复函数（进程启动时一次）。
///
/// ★ 这是"用户输入后由 ReactEngine 传给 remote_a2a 恢复远端执行"里
///   remote_a2a 那一半。ReactEngine 不认识 A2A 协议，只调 resume()。
void register_remote_a2a_resume_handler(std::shared_ptr<RemoteA2aRegistry> registry);

}  // namespace remote_a2a
}  // namespace hunterclaw
