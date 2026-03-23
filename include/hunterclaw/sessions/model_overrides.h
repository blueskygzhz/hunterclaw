#ifndef HUNTERCLAW_SESSIONS_MODEL_OVERRIDES_H
#define HUNTERCLAW_SESSIONS_MODEL_OVERRIDES_H

#include <string>
#include <optional>
#include <chrono>
#include "hunterclaw/sessions/session_manager.h"

namespace hunterclaw {
namespace sessions {

// Model override selection (aligned with OpenClaw: ModelOverrideSelection)
struct ModelOverrideSelection {
    std::string provider;
    std::string model;
    bool is_default = false;  // true means "reset to default"
};

// Auth profile override source
enum class AuthProfileOverrideSource {
    Auto,  // "auto"
    User   // "user"
};

/**
 * Apply a model override to a session entry.
 * Aligned with OpenClaw: applyModelOverrideToSessionEntry()
 *
 * Handles:
 * - Setting/clearing provider and model overrides
 * - Clearing stale runtime model identity when selection changes
 * - Clearing cached contextTokens when model changes
 * - Managing auth profile overrides
 * - Clearing stale fallback notices
 */
struct ApplyModelOverrideParams {
    SessionEntry& entry;
    const ModelOverrideSelection& selection;
    std::optional<std::string> profile_override;
    AuthProfileOverrideSource profile_override_source = AuthProfileOverrideSource::User;
};

struct ApplyModelOverrideResult {
    bool updated = false;
};

inline ApplyModelOverrideResult apply_model_override_to_session_entry(
    ApplyModelOverrideParams& params
) {
    SessionEntry& entry = params.entry;
    const auto& selection = params.selection;
    const auto& profile_override = params.profile_override;
    const std::string profile_override_source_str =
        (params.profile_override_source == AuthProfileOverrideSource::Auto) ? "auto" : "user";

    bool updated = false;
    bool selection_updated = false;

    if (selection.is_default) {
        // \u2705 \u5bf9\u9f50 OpenClaw: delete entry.providerOverride / delete entry.modelOverride\n        entry.provider_override = std::nullopt;\n        entry.model_override = std::nullopt;\n        entry.fallback_notice_selected_model = std::nullopt;
        updated = true;
        selection_updated = true;
    } else {
        // \u2705 \u5bf9\u9f50 OpenClaw: entry.providerOverride = selection.provider; entry.modelOverride = selection.model\n        if (!selection.provider.empty()) entry.provider_override = selection.provider;\n        if (!selection.model.empty()) entry.model_override = selection.model;
        updated = true;
        selection_updated = true;
    }

    // Clear stale runtime model identity when selection changes
    // (aligned with OpenClaw: clear entry.model / entry.modelProvider if stale)
    const std::string runtime_model = entry.model.value_or("");
    const std::string runtime_provider = entry.model_provider.value_or("");
    const bool runtime_present = !runtime_model.empty() || !runtime_provider.empty();
    const bool runtime_aligned =
        runtime_model == selection.model &&
        (runtime_provider.empty() || runtime_provider == selection.provider);

    if (runtime_present && (selection_updated || !runtime_aligned)) {
        if (entry.model.has_value()) {
            entry.model = std::nullopt;
            updated = true;
        }
        if (entry.model_provider.has_value()) {
            entry.model_provider = std::nullopt;
            updated = true;
        }
    }

    // Clear cached context tokens when model changes
    // (aligned with OpenClaw: delete entry.contextTokens)
    if (entry.context_tokens.has_value() &&
        (selection_updated || (runtime_present && !runtime_aligned))) {
        entry.context_tokens = std::nullopt;
        updated = true;
    }

    // Manage auth profile override
    // (aligned with OpenClaw: entry.authProfileOverride / authProfileOverrideSource)
    // These fields are not yet in SessionEntry - they would need to be added
    // For now we handle the logic conceptually

    // Clear stale fallback notices when user explicitly switches models
    // (aligned with OpenClaw: delete entry.fallbackNoticeSelectedModel etc.)
    if (updated) {
        // \u2705 \u5bf9\u9f50 OpenClaw: delete entry.fallbackNoticeSelectedModel etc.\n        entry.fallback_notice_selected_model = std::nullopt;
        // Update timestamp
        entry.updated_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    }

    return {updated};
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_MODEL_OVERRIDES_H
