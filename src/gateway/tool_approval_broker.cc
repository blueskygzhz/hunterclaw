// ============================================================================
// tool_approval_broker.cc — ToolApprovalBroker 的 gateway 侧实现
//
// 🎯 严格对齐 OpenClaw:
//   src/infra/plugin-approvals.ts              (PluginApprovalRequestPayload)
//   src/infra/approval-types.ts                (resolveApprovalRequestKind)
//   src/agents/agent-tools.before-tool-call.approval.ts (请求/等待语义)
//
// ★★ 这是"把 tools 层的审批需求接到 gateway 审批 store"的唯一实现点。
//
//   tools 层【不依赖】gateway (bazel query somepath 为空), 故 tools 侧只
//   声明了 ToolApprovalBroker 接口, 实现放在这里 —— 与 remote_a2a 用
//   std::function 注入路由挂载器是同一手法。
//
// ⚠ 本实现【必须】构造 PluginApprovalRequestPayload (title+description),
//   不能塞进 ExecApprovalRequestPayload 的 command 字段:
//   OpenClaw 按载荷【形状】判别归属 (approval-types.ts:5-15, 注释原文
//   "never from id spelling")。形状错了会导致:
//     · 客户端按 exec 命令卡片渲染工具审批, 显示一个假的"命令"
//     · allow-always 把工具名当命令写进 exec 白名单
//     · canonical approval.resolve 的 kind 路由到错误分支
// ============================================================================

#include "hunterclaw/gateway/tool_approval_broker.h"

#include "hunterclaw/core/json.h"
#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace gateway {

namespace {

std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("tool_approval");
}

}  // namespace

GatewayToolApprovalBroker::GatewayToolApprovalBroker(
    std::shared_ptr<ExecApprovalManager> manager, BroadcastFn broadcast)
    : manager_(std::move(manager)), broadcast_(std::move(broadcast)) {}

std::string GatewayToolApprovalBroker::request_tool_approval(
    const std::string& tool_name,
    const std::string& tool_call_id,
    const std::string& agent_id,
    const std::string& session_key,
    int64_t timeout_ms,
    const std::vector<std::string>& allowed_decisions) {
    if (!manager_) {
        // ⚠ 返回空串 → Stage 1 判为不可用并【拒绝执行】。
        //   不能返回一个假 id 让它去等 —— 那会等满超时才失败。
        log()->error("tool approval requested but manager is null (tool={})",
                     tool_name);
        return {};
    }

    PluginApprovalRequestPayload payload;
    // ★ title / description 是 plugin 归属的判据, 必须非空。
    payload.title = "工具需要审批: " + tool_name;
    payload.description =
        "Agent 请求执行工具 '" + tool_name + "'，需要你确认后才会继续。";
    payload.tool_name = tool_name;
    if (!tool_call_id.empty()) payload.tool_call_id = tool_call_id;
    if (!agent_id.empty()) payload.agent_id = agent_id;
    if (!session_key.empty()) payload.session_key = session_key;
    // ⚠ severity 用 warning 而非 critical: critical 在部分客户端会触发
    //   强提醒/置顶。工具审批是常规交互, 滥用 critical 会让真正紧急的
    //   审批失去区分度。
    payload.severity = std::string("warning");
    if (!allowed_decisions.empty()) {
        payload.allowed_decisions = allowed_decisions;
    }

    auto record = manager_->create_plugin(payload, timeout_ms);
    const int64_t effective_timeout = record.expires_at_ms - record.created_at_ms;
    if (!manager_->register_record(record, effective_timeout)) {
        log()->error("tool approval registration failed (tool={} id={})",
                     tool_name, record.id);
        return {};
    }

    // ── 广播, 让客户端能看到这条待审 ────────────────────────────────────
    // ⚠⚠ 不广播 = 审批请求存在但没人知道 → 必然等满超时,
    //   而日志里只有一条"审批超时", 完全指不到"事件没发出去"这件事。
    //   这正是"静默失效"最典型的形态, 故这里即使 broadcast_ 为空也要记 error。
    if (broadcast_) {
        nlohmann::json event;
        event["id"] = record.id;
        event["request"] = record.request_to_json();
        event["createdAtMs"] = record.created_at_ms;
        event["expiresAtMs"] = record.expires_at_ms;
        broadcast_("plugin.approval.requested", event);
    } else {
        log()->error("tool approval created but no broadcast channel; "
                     "clients cannot see it (tool={} id={})", tool_name, record.id);
    }

    log()->info("tool approval requested: tool={} id={} timeout={}ms",
                tool_name, record.id, effective_timeout);
    return record.id;
}

std::optional<std::string> GatewayToolApprovalBroker::wait_tool_approval(
    const std::string& approval_id, int64_t timeout_ms,
    const std::atomic<bool>* aborted) {
    if (!manager_) return std::nullopt;
    // ⚠ timeout_ms<=0 时传 0 给 await_decision, 由它沿用注册时的超时 ——
    //   不要在这里换成某个默认值, 否则会与 create_plugin 钳制出的
    //   实际过期时间不一致(等待比记录活得更久或更短, 两者都会造成
    //   "刚好在边界上"的随机失败)。
    //
    // ★ aborted 原样透传 —— 对齐 OpenClaw approval.ts:340-341 的
    //   abortPromise race。不透传会让 chat.abort 无法打断审批等待,
    //   而该等待占着 main lane 唯一槽位。
    return manager_->await_decision(approval_id, timeout_ms > 0 ? timeout_ms : 0,
                                    aborted);
}

}  // namespace gateway
}  // namespace hunterclaw
