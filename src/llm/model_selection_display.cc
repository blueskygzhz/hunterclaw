// ============================================================================
// model_selection_display.cc
// 严格对齐 OpenClaw: model-selection-display.ts (91行)
// ============================================================================

#include "hunterclaw/llm/model_selection_display.h"
#include <algorithm>

namespace hunterclaw {
namespace llm {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::optional<std::string> trim_opt(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    auto trimmed = trim(*s);
    return trimmed.empty() ? std::nullopt : std::optional(trimmed);
}

} // namespace

// ============================================================================
// resolveModelDisplayRef (对齐 OC L9-42)
// ============================================================================

std::optional<std::string> resolve_model_display_ref(const ModelDisplaySelectionParams& params) {
    auto runtime_model = trim_opt(params.runtime_model);
    auto runtime_provider = trim_opt(params.runtime_provider);

    if (runtime_model) {
        if (runtime_model->find('/') != std::string::npos) {
            return runtime_model;
        }
        if (runtime_provider) {
            return *runtime_provider + "/" + *runtime_model;
        }
        return runtime_model;
    }
    if (runtime_provider) {
        return runtime_provider;
    }

    auto override_model = trim_opt(params.override_model);
    auto override_provider = trim_opt(params.override_provider);

    if (override_model) {
        if (override_model->find('/') != std::string::npos) {
            return override_model;
        }
        if (override_provider) {
            return *override_provider + "/" + *override_model;
        }
        return override_model;
    }
    if (override_provider) {
        return override_provider;
    }

    auto fallback_model = trim_opt(params.fallback_model);
    return fallback_model;
}

// ============================================================================
// resolveModelDisplayName (对齐 OC L44-54)
// ============================================================================

std::string resolve_model_display_name(const ModelDisplaySelectionParams& params) {
    auto model_ref = resolve_model_display_ref(params);
    if (!model_ref) {
        return "model n/a";
    }
    auto slash = model_ref->rfind('/');
    if (slash != std::string::npos && slash < model_ref->size() - 1) {
        return model_ref->substr(slash + 1);
    }
    return *model_ref;
}

// ============================================================================
// resolveSessionInfoModelSelection (对齐 OC L65-90)
// ============================================================================

SessionInfoModelSelection resolve_session_info_model_selection(
    const SessionInfoModelSelectionParams& params
) {
    // entry takes priority
    if (params.entry_provider.has_value() || params.entry_model.has_value()) {
        return SessionInfoModelSelection{
            params.entry_provider ? params.entry_provider
                : (params.current_provider ? params.current_provider : std::nullopt),
            params.entry_model ? params.entry_model
                : (params.current_model ? params.current_model : std::nullopt)
        };
    }

    auto override_model = trim_opt(params.override_model);
    if (override_model) {
        auto override_provider = trim_opt(params.override_provider);
        auto current_provider = params.current_provider;
        return SessionInfoModelSelection{
            override_provider ? override_provider : current_provider,
            override_model
        };
    }

    return SessionInfoModelSelection{
        params.current_provider,
        params.current_model
    };
}

} // namespace llm
} // namespace hunterclaw
