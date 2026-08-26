// ============================================================================
// acp_tool_status.cc
// 🎯 严格对齐 OpenClaw: src/acp/tool-status.ts
// ============================================================================

#include "hunterclaw/acp/acp_tool_status.h"

#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace acp {

namespace {

/// 対齐 @openclaw/normalization-core/string-coerce
/// normalizeOptionalLowercaseString —— trim + lowercase，空则视为无值。
std::string normalize_lowercase(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(ws);
    std::string out = s.substr(b, e - b + 1);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

std::optional<AcpToolTerminalOutcome> resolve_acp_tool_terminal_outcome(
    const std::string& status) {
    // 対齐 tool-status.ts:15-16 —— 归一化后为空即返回 undefined
    const std::string normalized = normalize_lowercase(status);
    if (normalized.empty()) return std::nullopt;

    // 対齐 tool-status.ts:3-9 ACP_TOOL_TERMINAL_OUTCOMES
    // 严格相等查表，5 个键 → 3 个终态。
    // ⚠ 不含 "canceled"（单 l），不做子串匹配。
    if (normalized == "completed") return AcpToolTerminalOutcome::Completed;
    if (normalized == "done")      return AcpToolTerminalOutcome::Completed;
    if (normalized == "failed")    return AcpToolTerminalOutcome::Failed;
    if (normalized == "error")     return AcpToolTerminalOutcome::Failed;
    if (normalized == "cancelled") return AcpToolTerminalOutcome::Cancelled;

    // 対齐:16-18 —— 表里没有即非终态（如 "pending" / "in_progress"）
    return std::nullopt;
}

const char* to_string(AcpToolTerminalOutcome outcome) {
    switch (outcome) {
        case AcpToolTerminalOutcome::Completed: return "completed";
        case AcpToolTerminalOutcome::Failed:    return "failed";
        case AcpToolTerminalOutcome::Cancelled: return "cancelled";
    }
    return "completed";
}

} // namespace acp
} // namespace hunterclaw
