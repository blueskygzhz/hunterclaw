//============================================================================
// dangerous_tools.h - Shared tool-risk constants
//
// 🎯 严格对齐 OpenClaw: src/security/dangerous-tools.ts
// 集中管理工具风险常量, 避免 gateway HTTP 限制与安全审计逻辑漂移。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace hunterclaw {
namespace security {

// 🎯 对齐 OpenClaw DEFAULT_GATEWAY_HTTP_TOOL_DENY
// 默认通过 Gateway HTTP `POST /tools/invoke` 拒绝的高风险工具:
// 它们提供会话编排、控制平面操作或交互式流程, 不适合无交互 HTTP 面。
const std::vector<std::string>& default_gateway_http_tool_deny();

// 🎯 对齐 OpenClaw GATEWAY_CONTROL_PLANE_TOOLS
// 敏感控制平面工具 (automations 可持久化定时任务; gateway 暴露配置/schema)。
const std::vector<std::string>& gateway_control_plane_tools();

// 🎯 对齐 OpenClaw GATEWAY_OWNER_ONLY_CORE_TOOLS
// 在 Gateway 作用域面上需要 sender 为 owner 身份的核心工具。
// 非 owner 的携带身份调用者不得获得 server 凭据包装。
const std::vector<std::string>& gateway_owner_only_core_tools();

// 🎯 判定某工具名是否属于 owner-only 核心工具(大小写不敏感)。
bool is_gateway_owner_only_core_tool(const std::string& tool_name);

// 🎯 判定某工具名是否属于 Gateway HTTP 默认拒绝工具 (大小写不敏感)。
bool is_default_gateway_http_tool_denied(const std::string& tool_name);

}  // namespace security
}  // namespace hunterclaw
