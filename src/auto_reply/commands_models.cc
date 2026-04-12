// ============================================================================
// commands_models.cc
// 严格对齐 OpenClaw: auto-reply/reply/commands-models.ts (409行)
//                   + auto-reply/reply/commands-allowlist.ts (574行)
//                   + auto-reply/reply/model-selection.ts (650行)
// ============================================================================

#include "hunterclaw/auto_reply/commands_models.h"
#include "hunterclaw/llm/model_auth.h"
#include <algorithm>
#include <sstream>
#include <cmath>

namespace hunterclaw {
namespace auto_reply {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static constexpr int PAGE_SIZE_DEFAULT = 20;
static constexpr int PAGE_SIZE_MAX = 100;

// Fuzzy token variants (对齐 OC FUZZY_VARIANT_TOKENS)
const std::vector<std::string> FUZZY_VARIANT_TOKENS = {
    "lightning", "preview", "mini", "fast", "turbo",
    "lite", "beta", "small", "nano"
};

int bounded_levenshtein_distance(const std::string& a, const std::string& b, int max_dist) {
    if (a == b) return 0;
    if (a.empty() || b.empty()) return static_cast<int>(std::max(a.size(), b.size()));

    int m = static_cast<int>(a.size());
    int n = static_cast<int>(b.size());

    if (std::abs(m - n) > max_dist) return max_dist + 1;

    std::vector<int> prev(n + 1), curr(n + 1);
    for (int j = 0; j <= n; j++) prev[j] = j;

    for (int i = 1; i <= m; i++) {
        curr[0] = i;
        int min_in_row = i;
        for (int j = 1; j <= n; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j-1] + 1, prev[j-1] + cost});
            min_in_row = std::min(min_in_row, curr[j]);
        }
        if (min_in_row > max_dist) return max_dist + 1;
        std::swap(prev, curr);
    }
    return prev[n];
}

} // namespace

// ============================================================================
// /models Command (对齐 OC: commands-models.ts)
// ============================================================================

ModelsProviderData build_models_provider_data(
    const core::Config& cfg,
    const std::string& agent_id
) {
    ModelsProviderData data;
    data.resolved_default = llm::resolve_configured_model_ref(
        cfg, llm::DEFAULT_PROVIDER, llm::DEFAULT_MODEL);

    auto alias_index = llm::build_model_alias_index(cfg, data.resolved_default.provider);

    // Add default model
    data.by_provider[data.resolved_default.provider].insert(data.resolved_default.model);

    // Add models from agents.defaults.models (allowlist entries)
    if (cfg.data.contains("agents")) {
        const auto& agents = cfg.data["agents"];
        if (agents.contains("defaults") && agents["defaults"].contains("models") &&
            agents["defaults"]["models"].is_object()) {
            for (auto it = agents["defaults"]["models"].begin();
                 it != agents["defaults"]["models"].end(); ++it) {
                auto ref = llm::parse_model_ref(it.key(), data.resolved_default.provider);
                if (ref) {
                    data.by_provider[ref->provider].insert(ref->model);
                }
            }
        }
    }

    // Add models from configured providers
    if (cfg.data.contains("models") && cfg.data["models"].contains("providers") &&
        cfg.data["models"]["providers"].is_object()) {
        for (auto it = cfg.data["models"]["providers"].begin();
             it != cfg.data["models"]["providers"].end(); ++it) {
            auto provider = llm::normalize_provider_id(it.key());
            if (!provider.empty()) {
                data.by_provider[provider]; // ensure entry exists
            }
        }
    }

    // Build sorted provider list
    for (const auto& [p, _] : data.by_provider) {
        data.providers.push_back(p);
    }
    std::sort(data.providers.begin(), data.providers.end());

    // Move default provider to front
    auto default_it = std::find(
        data.providers.begin(), data.providers.end(), data.resolved_default.provider);
    if (default_it != data.providers.end() && default_it != data.providers.begin()) {
        auto val = *default_it;
        data.providers.erase(default_it);
        data.providers.insert(data.providers.begin(), val);
    }

    (void)agent_id; // TODO: agent-specific model filtering
    return data;
}

std::string build_models_text(
    const core::Config& cfg,
    const std::string& agent_id,
    int page,
    int page_size,
    const std::string& filter_provider
) {
    auto data = build_models_provider_data(cfg, agent_id);

    if (page_size <= 0) page_size = PAGE_SIZE_DEFAULT;
    if (page_size > PAGE_SIZE_MAX) page_size = PAGE_SIZE_MAX;

    std::ostringstream ss;
    ss << "📋 **Models**\n\n";
    ss << "Default: **" << data.resolved_default.provider << "/"
       << data.resolved_default.model << "**\n\n";

    // Auth label for default
    auto auth_label = llm::resolve_model_auth_label(
        data.resolved_default.provider, cfg);
    ss << "Auth: " << auth_label << "\n\n";

    int total_models = 0;
    for (const auto& [provider, models] : data.by_provider) {
        if (!filter_provider.empty() &&
            to_lower(provider) != to_lower(filter_provider)) continue;

        ss << "**" << provider << "** (" << models.size() << " models)";

        // Check auth
        bool has_auth = llm::has_available_auth_for_provider(provider, cfg);
        if (!has_auth) ss << " ⚠️ no auth";
        ss << "\n";

        int count = 0;
        for (const auto& model : models) {
            if (count >= page_size) {
                ss << "  ... and " << (static_cast<int>(models.size()) - count) << " more\n";
                break;
            }
            bool is_default = (provider == data.resolved_default.provider &&
                             model == data.resolved_default.model);
            ss << "  • " << model;
            if (is_default) ss << " ⭐";
            ss << "\n";
            count++;
            total_models++;
        }
        ss << "\n";
    }

    ss << "Total: " << total_models << " model(s) across "
       << data.providers.size() << " provider(s)\n";
    ss << "\nUse `/model <provider>/<model>` to switch models.";

    return ss.str();
}

std::string build_models_text_with_allowlist(
    const core::Config& cfg,
    const std::string& agent_id,
    int page
) {
    return build_models_text(cfg, agent_id, page, PAGE_SIZE_DEFAULT, "");
}

// ============================================================================
// /allowlist Command (对齐 OC: commands-allowlist.ts)
// ============================================================================

std::optional<AllowlistCommand> parse_allowlist_command(const std::string& raw) {
    auto trimmed = trim(raw);
    if (trimmed.empty()) {
        return AllowlistCommand{AllowlistAction::LIST, AllowlistScope::ALL};
    }

    std::istringstream iss(trimmed);
    std::string token;
    AllowlistCommand cmd;

    // Parse action
    iss >> token;
    auto lower_action = to_lower(token);
    if (lower_action == "list") cmd.action = AllowlistAction::LIST;
    else if (lower_action == "add") cmd.action = AllowlistAction::ADD;
    else if (lower_action == "remove") cmd.action = AllowlistAction::REMOVE;
    else {
        // Might be scope or entry
        cmd.action = AllowlistAction::LIST;
    }

    // Parse scope
    if (iss >> token) {
        auto lower_scope = to_lower(token);
        if (lower_scope == "dm") cmd.scope = AllowlistScope::DM;
        else if (lower_scope == "group") cmd.scope = AllowlistScope::GROUP;
        else if (lower_scope == "all") cmd.scope = AllowlistScope::ALL;
        else {
            cmd.entry = token; // might be the entry for add/remove
        }
    }

    // Parse entry for add/remove
    if (cmd.entry.empty() && (cmd.action == AllowlistAction::ADD ||
                               cmd.action == AllowlistAction::REMOVE)) {
        if (iss >> token) {
            cmd.entry = token;
        }
    }

    return cmd;
}

std::string build_allowlist_text(
    const core::Config& cfg,
    const AllowlistCommand& cmd
) {
    std::ostringstream ss;

    // Read allowlist from config
    std::vector<std::string> dm_allowlist, group_allowlist;

    if (cfg.data.contains("allowlist")) {
        const auto& al = cfg.data["allowlist"];
        if (al.contains("dm") && al["dm"].is_array()) {
            for (const auto& item : al["dm"]) {
                if (item.is_string()) dm_allowlist.push_back(item.get<std::string>());
            }
        }
        if (al.contains("group") && al["group"].is_array()) {
            for (const auto& item : al["group"]) {
                if (item.is_string()) group_allowlist.push_back(item.get<std::string>());
            }
        }
    }

    if (cmd.action == AllowlistAction::LIST) {
        ss << "📋 **Allowlist**\n\n";

        if (cmd.scope == AllowlistScope::DM || cmd.scope == AllowlistScope::ALL) {
            ss << "**DM** (" << dm_allowlist.size() << " entries):\n";
            if (dm_allowlist.empty()) {
                ss << "  (empty — all DMs allowed)\n";
            } else {
                for (const auto& entry : dm_allowlist) {
                    ss << "  • " << entry << "\n";
                }
            }
            ss << "\n";
        }

        if (cmd.scope == AllowlistScope::GROUP || cmd.scope == AllowlistScope::ALL) {
            ss << "**Group** (" << group_allowlist.size() << " entries):\n";
            if (group_allowlist.empty()) {
                ss << "  (empty — all groups allowed)\n";
            } else {
                for (const auto& entry : group_allowlist) {
                    ss << "  • " << entry << "\n";
                }
            }
        }
    } else {
        ss << "⚠️ /allowlist add/remove requires config write access\n";
        ss << "Use `hunterclaw config` to modify allowlists.";
    }

    return ss.str();
}

// ============================================================================
// Reply-layer Model Selection (对齐 OC: reply/model-selection.ts)
// ============================================================================

ModelSelectionState resolve_model_selection_state(
    const core::Config& cfg,
    const std::string& agent_id,
    const std::optional<std::string>& override_provider,
    const std::optional<std::string>& override_model
) {
    ModelSelectionState state;

    // Resolve configured default
    auto default_ref = llm::resolve_default_model_for_agent(cfg, agent_id);
    state.provider = default_ref.provider;
    state.model = default_ref.model;

    // Apply override if present
    if (override_model && !override_model->empty()) {
        auto override_ref = llm::resolve_persisted_override_model_ref(
            state.provider, override_provider, override_model);
        if (override_ref) {
            state.provider = override_ref->provider;
            state.model = override_ref->model;
        }
    }

    // Build allowed model keys
    auto allowlist_keys = llm::build_configured_allowlist_keys(cfg, state.provider);
    if (allowlist_keys) {
        state.allowed_model_keys = *allowlist_keys;
    }

    return state;
}

std::optional<ModelDirectiveSelection> apply_model_directive(
    const core::Config& cfg,
    const ModelSelectionState& state,
    const std::string& directive_raw
) {
    auto trimmed = trim(directive_raw);
    if (trimmed.empty()) return std::nullopt;

    auto alias_index = llm::build_model_alias_index(cfg, state.provider);
    auto resolved = llm::resolve_model_ref_from_string(
        trimmed, state.provider, &alias_index);
    if (!resolved) return std::nullopt;

    // Check if model is in allowlist (if allowlist exists)
    if (!state.allowed_model_keys.empty()) {
        auto mk = llm::model_key(resolved->ref.provider, resolved->ref.model);
        if (state.allowed_model_keys.find(mk) == state.allowed_model_keys.end()) {
            // Not in allowlist — try fuzzy match
            return std::nullopt;
        }
    }

    bool is_default = (resolved->ref.provider == state.provider &&
                      resolved->ref.model == state.model);

    return ModelDirectiveSelection{
        resolved->ref.provider,
        resolved->ref.model,
        is_default,
        resolved->alias
    };
}

std::optional<ModelDirectiveSelection> fuzzy_match_model(
    const core::Config& cfg,
    const ModelSelectionState& state,
    const std::string& query
) {
    auto lower_query = to_lower(trim(query));
    if (lower_query.empty()) return std::nullopt;

    // First try exact directive
    auto exact = apply_model_directive(cfg, state, query);
    if (exact) return exact;

    // Build candidate list from allowlist + config
    auto data = build_models_provider_data(cfg, "");
    int best_distance = 999;
    std::string best_provider, best_model;

    for (const auto& [provider, models] : data.by_provider) {
        for (const auto& model : models) {
            auto lower_model = to_lower(model);
            auto dist = bounded_levenshtein_distance(lower_query, lower_model, 5);
            if (dist < best_distance) {
                best_distance = dist;
                best_provider = provider;
                best_model = model;
            }

            // Also check provider/model combo
            auto combo = to_lower(provider + "/" + model);
            auto combo_dist = bounded_levenshtein_distance(lower_query, combo, 5);
            if (combo_dist < best_distance) {
                best_distance = combo_dist;
                best_provider = provider;
                best_model = model;
            }
        }
    }

    // Accept if distance <= 3 (reasonable fuzzy threshold)
    if (best_distance <= 3 && !best_model.empty()) {
        return ModelDirectiveSelection{
            best_provider, best_model,
            (best_provider == state.provider && best_model == state.model),
            std::nullopt
        };
    }

    return std::nullopt;
}

} // namespace auto_reply
} // namespace hunterclaw
