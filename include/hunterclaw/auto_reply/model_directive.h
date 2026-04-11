#pragma once

// 严格对齐 OpenClaw auto-reply/model.ts (52行)
// + auto-reply/model-runtime.ts (97行)
// /model 指令解析 + provider/model 格式化

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Model Directive (对齐 model.ts:4-51)
// ============================================================================

struct ModelDirectiveResult {
  std::string cleaned;             // Body with directive removed
  std::optional<std::string> raw_model;
  std::optional<std::string> raw_profile;
  bool has_directive = false;
};

/// Extract /model directive from message body.
/// 对齐 extractModelDirective
ModelDirectiveResult extract_model_directive(
    const std::string& body,
    const std::vector<std::string>& aliases = {}
);

// ============================================================================
// Model Runtime (对齐 model-runtime.ts:7-97)
// ============================================================================

/// Format provider/model reference.
/// 对齐 formatProviderModelRef
std::string format_provider_model_ref(
    const std::string& provider,
    const std::string& model
);

/// Resolve selected and active model from session entry.
/// 对齐 resolveSelectedAndActiveModel
struct SelectedAndActiveModel {
  std::string selected_provider;
  std::string selected_model;
  std::string active_provider;
  std::string active_model;
  std::string selected_label;
  std::string active_label;
  bool is_fallback;
};

SelectedAndActiveModel resolve_selected_and_active_model(
    const std::string& session_entry_json
);

}  // namespace auto_reply
}  // namespace hunterclaw
