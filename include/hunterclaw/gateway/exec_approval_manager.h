// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
//
// ExecApprovalManager - 严格对齐 OpenClaw gateway/exec-approval-manager.ts
// 管理 exec.approval.request / waitDecision / resolve 的生命周期

#pragma once

#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <atomic>
#include <map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace gateway {

using json = nlohmann::json;

// ============================================================================
// ExecApprovalDecision (对齐 OpenClaw ExecApprovalDecision)
// ============================================================================
// "allow-once" | "allow-always" | "deny"
using ExecApprovalDecision = std::string;

// ============================================================================
// ExecApprovalRequestPayload (对齐 OpenClaw ExecApprovalRequestPayload)
// ============================================================================
struct ExecApprovalRequestPayload {
    std::string command;
    std::optional<std::string> command_preview;
    std::optional<std::vector<std::string>> command_argv;
    std::optional<std::vector<std::string>> env_keys;
    json system_run_binding = nullptr;   // SystemRunApprovalBinding | null
    json system_run_plan = nullptr;      // SystemRunApprovalPlan | null
    std::optional<std::string> cwd;
    std::optional<std::string> node_id;
    std::optional<std::string> host;
    std::optional<std::string> security;
    std::optional<std::string> ask;
    std::optional<std::string> agent_id;
    std::optional<std::string> resolved_path;
    std::optional<std::string> session_key;
    std::optional<std::string> turn_source_channel;
    std::optional<std::string> turn_source_to;
    std::optional<std::string> turn_source_account_id;
    json turn_source_thread_id = nullptr;  // string | number | null

    json to_json() const;
    static ExecApprovalRequestPayload from_json(const json& j);
};

// ============================================================================
// PluginApprovalRequestPayload (对齐 OpenClaw PluginApprovalRequestPayload)
//   src/infra/plugin-approvals.ts:17-33
//
// ★★ 这是【工具审批】的载荷类型, 与 ExecApprovalRequestPayload 并列。
//
// ⚠ 为什么不能把工具审批塞进 ExecApprovalRequestPayload:
//   OpenClaw 用 payload 的【形状】判别归属 (resolveApprovalRequestKind,
//   approval-types.ts:5-15): 有 command → exec, 有 title+description → plugin。
//   把工具名写进 command 会让它被识别成 exec approval, 于是:
//     · 客户端 UI 按 exec 卡片渲染, 显示一个假的"命令"
//     · allow-always 会把工具名当命令写进 exec 白名单
//     · canonical approval.resolve 的 kind 路由到错误的处理分支
//   这不是字段好看与否的问题, 是归属判别会出错。
// ============================================================================
struct PluginApprovalRequestPayload {
    std::optional<std::string> plugin_id;
    std::string title;          // ★ 必填 —— 与 description 共同标识 plugin 归属
    std::string description;    // ★ 必填
    std::optional<std::string> detail;
    std::optional<std::string> severity;   // "info" | "warning" | "critical"
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_call_id;
    // 可限定本次审批允许的决策集合 (对齐 allowedDecisions)。
    // 为空表示用默认全集 allow-once / allow-always / deny。
    // ★ 存在的意义: 某些操作【不该】允许 allow-always
    //   (例如"确认转账 5000 元" —— 一次性授权可以, 永久授权绝不行)。
    std::optional<std::vector<std::string>> allowed_decisions;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> turn_source_channel;
    std::optional<std::string> turn_source_to;
    std::optional<std::string> turn_source_account_id;
    json turn_source_thread_id = nullptr;  // string | number | null

    json to_json() const;
    static PluginApprovalRequestPayload from_json(const json& j);
};

// 对齐 OpenClaw 常量 (plugin-approvals.ts:51-56)
static constexpr int64_t DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS = 120000;
static constexpr int64_t MAX_PLUGIN_APPROVAL_TIMEOUT_MS = 600000;
static constexpr size_t PLUGIN_APPROVAL_TITLE_MAX_LENGTH = 80;
static constexpr size_t PLUGIN_APPROVAL_DESCRIPTION_MAX_LENGTH = 512;
static constexpr size_t PLUGIN_APPROVAL_DETAIL_MAX_LENGTH = 16384;

// 默认允许的决策集合 (对齐 DEFAULT_PLUGIN_APPROVAL_DECISIONS)
const std::vector<std::string>& default_plugin_approval_decisions();

// ============================================================================
// ApprovalKind (对齐 OpenClaw ChannelApprovalKind, approval-types.ts:2)
// ============================================================================
enum class ApprovalKind { Exec, Plugin };

const char* approval_kind_to_wire(ApprovalKind k);          // "exec" / "plugin"
std::optional<ApprovalKind> approval_kind_from_wire(const std::string& s);

// ── resolve_approval_request_kind ───────────────────────────────────────────
// 逐字对齐 OpenClaw resolveApprovalRequestKind() (approval-types.ts:5-15):
//
//   const isExec   = "command" in request;
//   const isPlugin = "title" in request && "description" in request;
//   if (isExec === isPlugin) throw ...;
//
// ★★ 按 payload 的【形状】判别, 【绝不】按 id 拼写 ——
//   OpenClaw 的注释原文: "never from id spelling"。
//   理由: id 是不透明标识, 用 "plugin:" 前缀判别会在两处失效:
//     ① 客户端只传了 id 的前缀 (lookup_pending_id 支持前缀匹配) 时,
//        前缀可能刚好被截断
//     ② 任何生成 id 的地方忘了加前缀, 归属就静默错了, 且无从校验
//
// ⚠ 歧义时【报错而非猜测】: 既像又不像、两者都不像, 都说明载荷有问题。
//   兜底猜一个会让错误的载荷一路流到 UI 渲染才暴露。
//
// @return nullopt 表示无法确定唯一归属 (调用方应视为错误)
std::optional<ApprovalKind> resolve_approval_request_kind(const json& request);

// ============================================================================
// ExecApprovalRecord (对齐 OpenClaw ExecApprovalRecord)
// ============================================================================
struct ExecApprovalRecord {
    std::string id;
    ExecApprovalRequestPayload request;

    // ★★ Plugin (工具) 审批的载荷。有值时本记录归属 plugin, 否则归属 exec。
    //
    //   对齐 OpenClaw 的【一个 store + 两类 payload】结构:
    //   plugin-approvals.ts 直接 import ExecApprovalDecision, 两类审批
    //   共用同一决策枚举与同一等待/解决机制, 只有请求载荷不同。
    //
    //   ⚠ 不另建一个 PluginApprovalManager: 那会产生两个待审列表,
    //     而客户端只有一个 approval.resolve 入口 —— 决策打到哪个列表
    //     取决于实现细节, 另一个列表的请求永远等不到决策 → 必然超时。
    //     这正是"两套 manager"最危险的地方: 它表现为随机超时而非报错。
    std::optional<PluginApprovalRequestPayload> plugin_request;

    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    // Caller metadata (best-effort)
    std::optional<std::string> requested_by_conn_id;
    std::optional<std::string> requested_by_device_id;
    std::optional<std::string> requested_by_client_id;
    // Resolution state
    std::optional<int64_t> resolved_at_ms;
    std::optional<ExecApprovalDecision> decision;
    std::optional<std::string> resolved_by;

    /// 本记录的归属。
    /// ★ 由载荷【形状】决定 (plugin_request 是否有值), 不看 id 拼写 ——
    ///   与 resolve_approval_request_kind() 同一原则。
    ApprovalKind kind() const {
        return plugin_request.has_value() ? ApprovalKind::Plugin : ApprovalKind::Exec;
    }

    /// 用于广播/快照的 request 字段 (按归属输出对应形状)。
    json request_to_json() const;
};

// ============================================================================
// ExecApprovalIdLookupResult (对齐 OpenClaw ExecApprovalIdLookupResult)
// ============================================================================
struct ExecApprovalIdLookupResult {
    enum class Kind { Exact, Prefix, Ambiguous, None };
    Kind kind = Kind::None;
    std::string id;                    // for Exact/Prefix
    std::vector<std::string> ids;      // for Ambiguous
};

// ============================================================================
// ExecApprovalManager (对齐 OpenClaw ExecApprovalManager class)
// ============================================================================
// Grace period to keep resolved entries for late awaitDecision calls
static constexpr int64_t RESOLVED_ENTRY_GRACE_MS = 15000;

// abort 轮询间隔。
// ★ 取 50ms 是权衡: aborted 由外部 atomic 置位, 置位方不持本类 mutex
//   因而无法 notify, 只能轮询。
//   ⚠ 不能取太大(abort 响应迟钝, 失去意义), 也不必太小 ——
//     50ms 的唤醒开销在一次最长 120s 的等待里可忽略,
//     而对用户而言 50ms 已是"立即"。
static constexpr int64_t kAbortPollIntervalMs = 50;

class ExecApprovalManager {
public:
    ExecApprovalManager() = default;
    ~ExecApprovalManager() = default;

    // Non-copyable
    ExecApprovalManager(const ExecApprovalManager&) = delete;
    ExecApprovalManager& operator=(const ExecApprovalManager&) = delete;

    // ── create() ──────────────────────────────────────────────────────────
    // 对齐 OpenClaw: create(request, timeoutMs, id?)
    ExecApprovalRecord create(
        const ExecApprovalRequestPayload& request,
        int64_t timeout_ms,
        const std::optional<std::string>& explicit_id = std::nullopt
    );

    // ── create_plugin() ───────────────────────────────────────────────────
    // 对齐 OpenClaw plugin approval 创建路径 (共用同一 store)。
    //
    // ★ 与 create() 并列而非重载: 两者的载荷类型不同, 重载会让
    //   "传错了载荷类型"变成编译期沉默(隐式转换风险), 显式命名更安全。
    // ⚠ timeout_ms 会被钳到 [1, MAX_PLUGIN_APPROVAL_TIMEOUT_MS]。
    ExecApprovalRecord create_plugin(
        const PluginApprovalRequestPayload& request,
        int64_t timeout_ms,
        const std::optional<std::string>& explicit_id = std::nullopt
    );

    // ── register_record() ─────────────────────────────────────────────────
    // 对齐 OpenClaw: register(record, timeoutMs)
    // 同步注册，返回 future-like 等待句柄 (通过 await_decision 等待)
    // 如果 ID 已存在且未 resolved，幂等返回；如果已 resolved，抛出异常
    bool register_record(const ExecApprovalRecord& record, int64_t timeout_ms);

    // ── resolve() ─────────────────────────────────────────────────────────
    // 对齐 OpenClaw: resolve(recordId, decision, resolvedBy?)
    bool resolve(
        const std::string& record_id,
        const ExecApprovalDecision& decision,
        const std::optional<std::string>& resolved_by = std::nullopt
    );

    // ── expire() ──────────────────────────────────────────────────────────
    // 对齐 OpenClaw: expire(recordId, resolvedBy?)
    bool expire(
        const std::string& record_id,
        const std::optional<std::string>& resolved_by = std::nullopt
    );

    // ── await_decision() ──────────────────────────────────────────────────
    // 对齐 OpenClaw: awaitDecision(recordId)
    // 阻塞等待决策，返回 decision 或 nullopt（超时/过期/被取消）
    // 如果 ID 不存在，返回 nullopt
    //
    // @param aborted 运行取消标志。非空且被置位时【立即结束等待】。
    //        🎯 对齐 OpenClaw approval.ts:340-341
    //           `Promise.race([waitPromise, abortPromise])`。
    //
    // ⚠⚠ 没有这个参数时, chat.abort 之后等待方仍会挂满 timeout(默认 120s)。
    //   而该等待运行在 main lane 的唯一 worker 上
    //   (command_queue.cc:54 max_concurrent=1) —— 后果是全 gateway 的
    //   推理排队两分钟, 且用户没有任何手段脱身。
    //
    // ★ 实现上用 cv.wait_for + 谓词轮询(kAbortPollIntervalMs) 而非
    //   条件变量唤醒: aborted 是外部 atomic, 置位方(ReactEngine::cancel)
    //   不持有本类的 mutex, 无法 notify。轮询是唯一不改调用方的做法。
    std::optional<ExecApprovalDecision> await_decision(
        const std::string& record_id,
        int64_t timeout_ms = 0,  // 0 = 使用注册时的超时
        const std::atomic<bool>* aborted = nullptr
    );

    // ── get_snapshot() ────────────────────────────────────────────────────
    // 对齐 OpenClaw: getSnapshot(recordId)
    std::optional<ExecApprovalRecord> get_snapshot(const std::string& record_id) const;

    // ── consume_allow_once() ──────────────────────────────────────────────
    // 对齐 OpenClaw: consumeAllowOnce(recordId)
    bool consume_allow_once(const std::string& record_id);

    // ── lookup_pending_id() ───────────────────────────────────────────────
    // 对齐 OpenClaw: lookupPendingId(input)
    // 支持精确匹配和前缀匹配
    ExecApprovalIdLookupResult lookup_pending_id(const std::string& input) const;

    // ── has_pending() ─────────────────────────────────────────────────────
    // 检查是否有待处理的 approval（用于 hasExecApprovalClients 判断）
    bool has_pending() const;

    // ── list_pending() ────────────────────────────────────────────────────
    // 列出待决策的审批记录, 可按归属过滤。
    //
    // ★ 新增此方法是为了修一个真实缺陷: plugin_approval.cc 的
    //   plugin.approval.list 原实现【永远返回空数组】(它只调 has_pending()
    //   然后什么都不做)。客户端因此看不到任何待审的工具审批 ——
    //   表现为"审批请求发了但列表里没有", 而日志里一个字都没有。
    //
    // ⚠ 只返回【未决策且未过期】的记录: 已决策的记录会在 grace 期内
    //   保留于 pending_ 中以便迟到的 await_decision 取用, 但它们
    //   不该出现在"待审列表"里(否则 UI 会一直显示已批过的条目)。
    // @param kind nullopt 表示不过滤
    std::vector<ExecApprovalRecord> list_pending(
        std::optional<ApprovalKind> kind = std::nullopt) const;

private:
    struct PendingEntry {
        ExecApprovalRecord record;
        std::condition_variable cv;
        bool decided = false;
        int64_t timeout_ms = 0;
    };

    mutable std::mutex mutex_;
    // record_id -> PendingEntry
    std::map<std::string, std::shared_ptr<PendingEntry>> pending_;

    static int64_t now_ms();
    static std::string generate_uuid();
};

} // namespace gateway
} // namespace hunterclaw
