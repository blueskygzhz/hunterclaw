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

#include <memory>
#include <string>
#include <vector>

#include "hunterclaw/remote_a2a/agent_card.h"
#include "hunterclaw/remote_a2a/execution_engine.h"
#include "hunterclaw/remote_a2a/pending_tasks.h"
#include "hunterclaw/remote_a2a/remote_agent_client.h"
#include "hunterclaw/tools/tool.h"

namespace hunterclaw {
namespace remote_a2a {

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

// ── 单个远端的策略（★ 全部来自本地配置，不从卡读）────────────────────────

struct AgentToolPolicy {
    /// 是否需要审批。★ 只从本地配置读。
    bool requires_approval = false;
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
    bool requires_approval() const override { return policy_.requires_approval; }
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
    bool requires_approval() const override { return requires_approval_; }

private:
    std::shared_ptr<PendingTaskStore> store_;
    EngineLookupFn lookup_;
    std::string session_key_;
    bool requires_approval_ = false;
};

// ── 辅助 ────────────────────────────────────────────────────────────────────

/// 由用户文本构造 A2A Message。
/// @param task_id    续接已有任务时传入（input-required 回填场景）
/// @param context_id 续接同一上下文
Message build_user_message(const std::string& text, const std::string& task_id,
                          const std::string& context_id);

/// ★ 会话归属校验：handle 必须属于当前会话。
///
/// 这是【必需】的而非防御性的：handle 是半公开的（出现在工具返回文本里，
/// 模型可能把它写进别的会话）。不校验则 A 会话能读走 B 会话的任务结果。
/// @return 空串表示通过；否则为拒绝原因
std::string check_handle_ownership(const PendingTaskStore& store,
                                  const std::string& handle,
                                  const std::string& session_key);

/// 把中间态进度投递到 system-event 通道。
///
/// ⚠ `chat.progress` 【不存在】—— 全仓 grep 只有 chat.send/abort/history/inject。
///   实际用 infra::enqueue_system_event（exec 同通道）。
/// ⚠ 中间态必须带 context_key 走"心跳"语义，否则会把 MAX_EVENTS=20
///   （system_events.cc:18）的队列刷满，把真正的事件挤掉。
void deliver_progress(const std::string& session_key, const std::string& agent_name,
                     const std::string& text, bool is_heartbeat);

}  // namespace remote_a2a
}  // namespace hunterclaw
