#pragma once
// ============================================================================
// commands_models.h
// 严格对齐 OpenClaw: auto-reply/reply/commands-models.ts (409行)
//                   + auto-reply/reply/commands-allowlist.ts (574行)
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/llm/model_selection.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// /models command (对齐 OC: commands-models.ts)
// ============================================================================

struct ModelsProviderData {
    /// provider → set of model IDs
    std::map<std::string, std::set<std::string>> by_provider;
    /// ordered provider list
    std::vector<std::string> providers;
    /// resolved default model
    llm::ModelRef resolved_default;
    /// model display names (provider/model → display name)
    std::map<std::string, std::string> model_names;
};

/// Build provider/model data from config
/// 对齐 OC: buildModelsProviderData
ModelsProviderData build_models_provider_data(
    const core::Config& cfg,
    const std::string& agent_id = "");

/// Build the /models text output
/// 对齐 OC: handleModelsCommand
std::string build_models_text(
    const core::Config& cfg,
    const std::string& agent_id = "",
    int page = 0,
    int page_size = 20,
    const std::string& filter_provider = "");

/// Build the /models text with allowed model set
std::string build_models_text_with_allowlist(
    const core::Config& cfg,
    const std::string& agent_id = "",
    int page = 0);

// ============================================================================
// /allowlist command (对齐 OC: commands-allowlist.ts)
// ============================================================================

enum class AllowlistScope { DM, GROUP, ALL };
enum class AllowlistAction { LIST, ADD, REMOVE };

struct AllowlistCommand {
    AllowlistAction action = AllowlistAction::LIST;
    AllowlistScope scope = AllowlistScope::ALL;
    std::string entry;      // for add/remove
    std::optional<std::string> channel;
    std::optional<std::string> account;
    bool resolve_names = false;
};

/// Parse /allowlist command arguments
/// 对齐 OC: parseAllowlistCommand
std::optional<AllowlistCommand> parse_allowlist_command(const std::string& raw);

/// Build the /allowlist output text
/// 对齐 OC: handleAllowlistCommand
std::string build_allowlist_text(
    const core::Config& cfg,
    const AllowlistCommand& cmd);

// ============================================================================
// Reply-layer model selection (对齐 OC: reply/model-selection.ts 650行)
// ============================================================================

struct ModelDirectiveSelection {
    std::string provider;
    std::string model;
    bool is_default = false;
    std::optional<std::string> alias;
};

struct ModelSelectionState {
    std::string provider;
    std::string model;
    std::set<std::string> allowed_model_keys;
    bool reset_model_override = false;
    bool needs_model_catalog = false;
};

/// Resolve model selection state for a reply context
/// 对齐 OC: resolveModelSelectionState
ModelSelectionState resolve_model_selection_state(
    const core::Config& cfg,
    const std::string& agent_id,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model);

/// Apply a model directive (e.g., /model anthropic/claude-opus-4-0725)
/// 对齐 OC: applyModelDirective
std::optional<ModelDirectiveSelection> apply_model_directive(
    const core::Config& cfg,
    const ModelSelectionState& state,
    const std::string& directive_raw);

/// Fuzzy-match a model name against the allowlist
/// 对齐 OC: boundedLevenshteinDistance + fuzzyMatchModel
std::optional<ModelDirectiveSelection> fuzzy_match_model(
    const core::Config& cfg,
    const ModelSelectionState& state,
    const std::string& query);

} // namespace auto_reply
} // namespace hunterclaw
