// ✅ 严格对齐 OpenClaw: src/infra/diagnostic-flags.ts (90行)
// 诊断功能标志系统 — 基于配置和环境变量的诊断功能开关
// 支持通配符匹配 (e.g., "webhook.*", "*")
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace observability {

using json = nlohmann::json;

// ============================================================================
// Constants
// ✅ 对齐 OpenClaw: DIAGNOSTICS_ENV = "OPENCLAW_DIAGNOSTICS"
// ============================================================================

constexpr const char* DIAGNOSTICS_ENV = "HUNTERCLAW_DIAGNOSTICS";

// ============================================================================
// Public API
// ============================================================================

// ✅ 对齐 OpenClaw: resolveDiagnosticFlags(cfg?, env?)
// 合并配置文件 diagnostics.flags + 环境变量 HUNTERCLAW_DIAGNOSTICS
std::vector<std::string> resolve_diagnostic_flags(
    const json& cfg = json::object());

// ✅ 对齐 OpenClaw: matchesDiagnosticFlag(flag, enabledFlags)
// 检查单个 flag 是否匹配已启用的 flags（支持通配符）
bool matches_diagnostic_flag(
    const std::string& flag,
    const std::vector<std::string>& enabled_flags);

// ✅ 对齐 OpenClaw: isDiagnosticFlagEnabled(flag, cfg?, env?)
// 便捷函数：解析 flags 并检查指定 flag 是否启用
bool is_diagnostic_flag_enabled(
    const std::string& flag,
    const json& cfg = json::object());

// ✅ 对齐 OpenClaw: isDiagnosticsEnabled(config?)
// 检查 config.diagnostics.enabled === true
bool is_diagnostics_enabled(const json& cfg = json::object());

}  // namespace observability
}  // namespace hunterclaw
