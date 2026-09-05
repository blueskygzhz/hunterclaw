#pragma once
// inherited_tool_deny.h
// Aligned with openclaw: src/agents/inherited-tool-deny.ts
//                        src/agents/tool-policy-match.ts   (isToolAllowedByPolicyName)
//                        src/agents/glob-pattern.ts        (compileGlobPatterns / matchesAny)
//                        src/agents/tool-policy-shared.ts  (normalizeToolName / expandToolGroups)
//
// 作用：ACP runtime 依赖一批「必须可用」的宿主工具（read / write / exec / edit /
// apply_patch / process ...）。如果调用方自身的工具策略把这些工具 deny 掉，或者
// allowlist 里没有它们，那么 ACP 子会话运行起来也必然失败。真源在 spawn 前用两道
// 闸提前拦掉，并提示改用 runtime="subagent"。
//
// ⚠ 数据源现状：HunterClaw 目前没有「继承工具策略」机制（真源里 inherited
//   allow/deny 由 skills/runtime/tool-dispatch.ts 的 explicitDenylist 生产）。
//   因此本模块提供的两道闸在当前调用链下输入恒为空、恒不拦截。逻辑与接口已按真源
//   实现，待工具策略管道补齐后把列表填入 SpawnAcpContext 即自动生效。

#include <string>
#include <vector>
#include <optional>
#include <map>

namespace hunterclaw {
namespace agents {

// ── normalize_policy_tool_name ───────────────────────────────────────────────
// Aligned with openclaw: normalizeToolName (tool-policy-shared.ts:58-61)
//   normalizeLowercaseStringOrEmpty(name) 之后套用 TOOL_NAME_ALIASES。
//
// 注意：与 acp/acp_client.h 里的 `normalize_tool_name` 不同名同义 —— 后者是 ACP
// approval-classifier 专用版本（无别名表、限制字符集），不能混用。
std::string normalize_policy_tool_name(const std::string& name);

// ── expand_tool_groups ───────────────────────────────────────────────────────
// Aligned with openclaw: expandToolGroups (tool-policy-shared.ts:117-129)
//
// 把策略列表里的工具组（真源 key 形如 "group:openclaw" / "group:<sectionId>"）
// 展开成具体工具名，其余条目原样保留，最后去重。
//
// ⚠ HunterClaw 尚无工具目录（真源 CORE_TOOL_DEFINITIONS），组表为空，
//   因此 "group:*" 条目不会被展开。对不含组名的输入行为与真源完全一致。
std::vector<std::string> expand_tool_groups(const std::vector<std::string>& list);

// ── ToolPolicy ───────────────────────────────────────────────────────────────
// Aligned with openclaw: SandboxToolPolicy { allow?: string[]; deny?: string[] }
struct ToolPolicy {
    std::optional<std::vector<std::string>> allow;
    std::optional<std::vector<std::string>> deny;
};

// ── is_tool_allowed_by_policy_name ───────────────────────────────────────────
// Aligned with openclaw: isToolAllowedByPolicyName (tool-policy-match.ts:40-45)
//
// 判定顺序（deny 优先，空 allow 表示「未被 deny 即放行」）：
//   1. 命中 deny glob            → false
//   2. allow 为空                → true
//   3. 命中 allow glob           → true
//   4. apply_patch 且 allow 含 write → true（真源特例：write 是宽泛写权限）
//   5. 否则                      → false
bool is_tool_allowed_by_policy_name(const std::string& name, const ToolPolicy& policy);

// ── normalize_inherited_tool_denylist / allowlist ────────────────────────────
// Aligned with openclaw: normalizeInheritedToolDenylist (:34-44)
//                        normalizeInheritedToolAllowlist (:52-54, 同一实现)
// 逐项 normalize_policy_tool_name，丢弃空串，保序去重。
std::vector<std::string> normalize_inherited_tool_denylist(
    const std::vector<std::string>& value);
std::vector<std::string> normalize_inherited_tool_allowlist(
    const std::vector<std::string>& value);

// ── find_acp_unsupported_inherited_tool_deny ─────────────────────────────────
// Aligned with openclaw: findAcpUnsupportedInheritedToolDeny (:61-70)
// 返回第一个被调用方 deny 掉、但 ACP 必需的工具名；无冲突返回 nullopt。
// 空列表直接返回 nullopt（不拦截）。
std::optional<std::string> find_acp_unsupported_inherited_tool_deny(
    const std::vector<std::string>& inherited_deny);

// ── find_acp_unsupported_inherited_tool_allow ────────────────────────────────
// Aligned with openclaw: findAcpUnsupportedInheritedToolAllow (:72-81)
// 返回第一个不在调用方 allowlist 内、但 ACP 必需的工具名；无冲突返回 nullopt。
// 空列表直接返回 nullopt（空 allowlist 语义为「不限制」）。
std::optional<std::string> find_acp_unsupported_inherited_tool_allow(
    const std::vector<std::string>& inherited_allow);

// ── 错误文案 ─────────────────────────────────────────────────────────────────
// Aligned with openclaw: formatAcpInheritedToolDenyError (:83-85)
//                        formatAcpInheritedToolAllowError (:87-89)
std::string format_acp_inherited_tool_deny_error(const std::string& tool_name);
std::string format_acp_inherited_tool_allow_error(const std::string& tool_name);

} // namespace agents
} // namespace hunterclaw
