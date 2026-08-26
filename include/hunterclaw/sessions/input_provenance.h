#pragma once
// ============================================================================
// Input Provenance — 严格对齐 OpenClaw sessions/input-provenance.ts
// ============================================================================
// 跨会话消息的来源标注。核心是 annotate_inter_session_prompt_text()：
// 在 prompt 正文前置一段「来源信封」，明确告知模型这段内容来自另一个会话
// 或内部工具，**不是**本会话最终用户的直接指令。
//
// 这是一层安全边界：没有它，A 会话的用户就能通过 sessions_send 让 B 会话
// 把注入内容当作 B 的最终用户指令执行（越权 / prompt 注入）。
// ============================================================================

#include <string>

namespace hunterclaw {
namespace sessions {

/// 対齐 input-provenance.ts:8 INPUT_PROVENANCE_KIND_VALUES
enum class InputProvenanceKind {
    ExternalUser,    ///< "external_user"
    InterSession,    ///< "inter_session"
    InternalSystem,  ///< "internal_system"
};

/// 対齐 input-provenance.ts:12-18 InputProvenance
struct InputProvenance {
    InputProvenanceKind kind = InputProvenanceKind::InterSession;
    std::string origin_session_id;
    std::string source_session_key;
    std::string source_channel;
    std::string source_tool;
};

/// 対齐 input-provenance.ts:22 INTER_SESSION_PROMPT_PREFIX_BASE
extern const std::string INTER_SESSION_PROMPT_PREFIX_BASE;

/// 対齐 input-provenance.ts:29-30 INTER_SESSION_PROMPT_EXPLANATION
extern const std::string INTER_SESSION_PROMPT_EXPLANATION;

std::string to_string(InputProvenanceKind kind);

/// 対齐 input-provenance.ts:149-170 removeFirstInterSessionPromptPrefix
/// 幂等性的基础：重复标注时先摘掉已有信封，避免层层堆叠。
std::string strip_inter_session_prompt_prefix_for_display(const std::string& text);

/// 対齐 input-provenance.ts:178-194 annotateInterSessionPromptText
///
/// 行为要点（顺序不可变）：
///   * kind != inter_session      → 原样返回（対齐:182-184）
///   * text trim 后为空           → 原样返回（対齐:185-187）
///   * 已以该信封开头             → 原样返回，保证**幂等**（対齐:189-191）
///   * 否则先摘旧信封再前置新的   → 信封恒在最顶部（対齐:192-193）
///
/// 最后一条是关键：信封必须在最顶部，否则后续装饰会把安全说明埋在下面，
/// 模型可能先读到被注入的内容。
std::string annotate_inter_session_prompt_text(const std::string& text,
                                               const InputProvenance& provenance);

}  // namespace sessions
}  // namespace hunterclaw
