#pragma once
// ============================================================================
// response_prefix_template.h — 严格对齐 OpenClaw response-prefix-template.ts (103行)
// ============================================================================

#include <string>
#include <optional>

namespace hunterclaw {
namespace react {

// 对齐 response-prefix-template.ts:10-21: ResponsePrefixContext
struct ResponsePrefixContext {
    std::optional<std::string> model;
    std::optional<std::string> model_full;
    std::optional<std::string> provider;
    std::optional<std::string> thinking_level;
    std::optional<std::string> identity_name;
};

// 对齐 response-prefix-template.ts:40-69: resolveResponsePrefixTemplate
std::optional<std::string> resolve_response_prefix_template(
    const std::optional<std::string>& tmpl,
    const ResponsePrefixContext& context
);

// 对齐 response-prefix-template.ts:84-91: extractShortModelName
std::string extract_short_model_name(const std::string& full_model);

// 对齐 response-prefix-template.ts:96-103: hasTemplateVariables
bool has_template_variables(const std::optional<std::string>& tmpl);

} // namespace react
} // namespace hunterclaw
