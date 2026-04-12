#pragma once
// ============================================================================
// model_selection_display.h
// 严格对齐 OpenClaw: model-selection-display.ts (91行)
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace llm {

// ============================================================================
// ModelDisplaySelectionParams (对齐 OC: ModelDisplaySelectionParams)
// ============================================================================

struct ModelDisplaySelectionParams {
    std::optional<std::string> runtime_provider;
    std::optional<std::string> runtime_model;
    std::optional<std::string> override_provider;
    std::optional<std::string> override_model;
    std::optional<std::string> fallback_model;
};

/// 按优先级 runtime > override > fallback 解析模型引用字符串
/// 对齐 OC: resolveModelDisplayRef
std::optional<std::string> resolve_model_display_ref(const ModelDisplaySelectionParams& params);

/// 从引用中提取斜杠后的短名称
/// 对齐 OC: resolveModelDisplayName
std::string resolve_model_display_name(const ModelDisplaySelectionParams& params);

// ============================================================================
// SessionInfoModelSelectionParams (对齐 OC: SessionInfoModelSelectionParams)
// ============================================================================

struct SessionInfoModelSelectionParams {
    std::optional<std::string> current_provider;
    std::optional<std::string> current_model;
    std::optional<std::string> entry_provider;
    std::optional<std::string> entry_model;
    std::optional<std::string> override_provider;
    std::optional<std::string> override_model;
};

struct SessionInfoModelSelection {
    std::optional<std::string> model_provider;
    std::optional<std::string> model;
};

/// 解析会话级模型选择，优先 entry > override > current
/// 对齐 OC: resolveSessionInfoModelSelection
SessionInfoModelSelection resolve_session_info_model_selection(
    const SessionInfoModelSelectionParams& params);

} // namespace llm
} // namespace hunterclaw
