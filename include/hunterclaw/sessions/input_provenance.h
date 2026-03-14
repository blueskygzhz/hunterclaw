#ifndef HUNTERCLAW_SESSIONS_INPUT_PROVENANCE_H
#define HUNTERCLAW_SESSIONS_INPUT_PROVENANCE_H

#include <string>
#include <optional>
#include "hunterclaw/core/json.h"

namespace hunterclaw {
namespace sessions {

/**
 * Input Provenance System
 * 100% 对齐 OpenClaw sessions/input-provenance.ts
 * 
 * 功能:
 * 1. 输入来源追踪 (external_user, inter_session, internal_system)
 * 2. 来源验证与规范化
 * 3. User Message 来源标注
 * 4. Inter-Session 来源检测
 */

// ============================================================================
// Types (100% 对齐 OpenClaw)
// ============================================================================

/// 输入来源类型 (对齐 InputProvenanceKind)
enum class InputProvenanceKind {
    ExternalUser,    ///< external_user - 外部用户输入
    InterSession,    ///< inter_session - 会话间传递
    InternalSystem   ///< internal_system - 内部系统生成
};

/// 输入来源信息 (对齐 InputProvenance)
struct InputProvenance {
    InputProvenanceKind kind;                    ///< 来源类型
    std::optional<std::string> origin_session_id;  ///< 原始会话ID
    std::optional<std::string> source_session_key; ///< 来源会话key
    std::optional<std::string> source_channel;     ///< 来源频道
    std::optional<std::string> source_tool;        ///< 来源工具
    
    InputProvenance() : kind(InputProvenanceKind::ExternalUser) {}
    explicit InputProvenance(InputProvenanceKind k) : kind(k) {}
    
    /// 转换为 JSON
    nlohmann::json to_json() const;
    
    /// 从 JSON 解析
    static std::optional<InputProvenance> from_json(const nlohmann::json& j);
};

// ============================================================================
// Input Provenance Utilities (100% 对齐 OpenClaw)
// ============================================================================

/**
 * 规范化输入来源
 * 
 * 对齐 OpenClaw: normalizeInputProvenance
 * 
 * 行为:
 * 1. ✅ 验证 kind 字段
 * 2. ✅ Trim + normalize optional fields
 * 3. ✅ Return std::nullopt if invalid
 * 
 * @param value 输入值 (JSON object)
 * @return 规范化的来源信息,无效则返回 nullopt
 */
std::optional<InputProvenance> normalize_input_provenance(const nlohmann::json& value);

/**
 * 为 User Message 添加输入来源
 * 
 * 对齐 OpenClaw: applyInputProvenanceToUserMessage
 * 
 * 行为:
 * 1. ✅ 只处理 role=user 的消息
 * 2. ✅ 如果已有 provenance,保留不覆盖
 * 3. ✅ 添加 provenance 字段
 * 
 * @param message Agent 消息 (JSON)
 * @param provenance 输入来源 (可选)
 * @return 添加了 provenance 的消息
 */
nlohmann::json apply_input_provenance_to_user_message(
    const nlohmann::json& message,
    const std::optional<InputProvenance>& provenance
);

/**
 * 检测是否为 Inter-Session 来源
 * 
 * 对齐 OpenClaw: isInterSessionInputProvenance
 * 
 * @param value 输入值 (JSON)
 * @return 是否为 inter_session 来源
 */
bool is_inter_session_input_provenance(const nlohmann::json& value);

/**
 * 检测 User Message 是否来自 Inter-Session
 * 
 * 对齐 OpenClaw: hasInterSessionUserProvenance
 * 
 * 行为:
 * 1. ✅ 检查 role=user
 * 2. ✅ 检查 provenance.kind=inter_session
 * 
 * @param message Agent 消息 (JSON)
 * @return 是否为 inter_session 来源的 user message
 */
bool has_inter_session_user_provenance(const nlohmann::json& message);

// ============================================================================
// Helper Functions
// ============================================================================

/// InputProvenanceKind to string
std::string to_string(InputProvenanceKind kind);

/// String to InputProvenanceKind
std::optional<InputProvenanceKind> provenance_kind_from_string(const std::string& str);

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_INPUT_PROVENANCE_H
