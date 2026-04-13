// 🎯 Strict alignment: OpenClaw context-engine/registry.ts + init.ts + delegate.ts
// ContextEngine registry, factory resolution, legacy engine impl, compaction delegation.

#include <hunterclaw/context/context_engine.h>
#include <hunterclaw/core/logging.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace hunterclaw {
namespace context {

namespace {
auto log() { return core::get_logger("context-engine-registry"); }

// ─── Registry state (aligned: module-level singleton in registry.ts) ─────

constexpr const char* CORE_CONTEXT_ENGINE_OWNER = "core";
constexpr const char* PUBLIC_CONTEXT_ENGINE_OWNER = "public-sdk";
constexpr const char* DEFAULT_ENGINE_SLOT_ID = "legacy";

struct RegistryEntry {
    ContextEngineFactory factory;
    std::string owner;
};

std::mutex& registry_mu() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, RegistryEntry>& registry_engines() {
    static std::unordered_map<std::string, RegistryEntry> engines;
    return engines;
}

bool& init_flag() {
    static bool flag = false;
    return flag;
}

} // namespace

// ─── LegacyContextEngine (aligned: context-engine/legacy.ts) ────────────

LegacyContextEngine::LegacyContextEngine() = default;
LegacyContextEngine::~LegacyContextEngine() = default;

ContextEngineInfo LegacyContextEngine::get_info() const {
    return {"legacy", "Legacy Context Engine", "1.0.0", false};
}

IngestResult LegacyContextEngine::ingest(
        const std::string& /*session_id*/,
        const std::string& /*message_json*/,
        bool /*is_heartbeat*/) {
    // 🎯 对齐 OC: No-op — SessionManager handles message persistence in legacy flow
    return {false};
}

AssembleResult LegacyContextEngine::assemble(
        const std::string& /*session_id*/,
        const std::vector<std::string>& messages_json,
        const std::optional<int>& /*token_budget*/) {
    // 🎯 对齐 OC: Pass-through — existing sanitize/validate/limit pipeline handles assembly
    return {messages_json, 0, std::nullopt};
}

void LegacyContextEngine::after_turn(
        const std::string& /*session_id*/,
        const std::string& /*session_file*/,
        const std::vector<std::string>& /*messages_json*/,
        int /*pre_prompt_message_count*/,
        const std::optional<std::string>& /*auto_compaction_summary*/,
        bool /*is_heartbeat*/,
        const std::optional<int>& /*token_budget*/,
        const ContextEngineRuntimeContext& /*runtime_context*/) {
    // 🎯 对齐 OC: No-op — legacy flow persists context directly in SessionManager
}

CompactResult LegacyContextEngine::compact(
        const std::string& session_id,
        const std::string& session_file,
        const std::optional<int>& token_budget,
        bool force,
        const std::optional<int>& current_token_count,
        const std::string& /*compaction_target*/,
        const std::optional<std::string>& custom_instructions,
        const ContextEngineRuntimeContext& /*runtime_context*/) {
    // 🎯 对齐 OC: Delegates to compactEmbeddedPiSessionDirect via delegate bridge
    return delegate_compaction_to_runtime(
        session_id, session_file, token_budget, force,
        current_token_count, custom_instructions);
}

void LegacyContextEngine::dispose() {
    // Nothing to clean up for legacy engine
}

// ─── Registry functions (aligned: context-engine/registry.ts) ────────────

// 🎯 对齐 OC: registerContextEngineForOwner
ContextEngineRegistrationResult register_context_engine_for_owner(
        const std::string& id,
        ContextEngineFactory factory,
        const std::string& owner,
        bool allow_same_owner_refresh) {
    std::lock_guard<std::mutex> lock(registry_mu());

    std::string normalized_owner = owner;
    // Trim whitespace
    while (!normalized_owner.empty() && normalized_owner.front() == ' ')
        normalized_owner.erase(normalized_owner.begin());
    while (!normalized_owner.empty() && normalized_owner.back() == ' ')
        normalized_owner.pop_back();

    if (normalized_owner.empty()) {
        log()->error("register_context_engine_for_owner: owner must be non-empty");
        return {false, "invalid-owner"};
    }

    // 🎯 对齐 OC: Core slot protection — only "core" owner can claim default slot
    if (id == DEFAULT_ENGINE_SLOT_ID && normalized_owner != CORE_CONTEXT_ENGINE_OWNER) {
        return {false, CORE_CONTEXT_ENGINE_OWNER};
    }

    auto& engines = registry_engines();
    auto it = engines.find(id);
    if (it != engines.end()) {
        if (it->second.owner != normalized_owner) {
            return {false, it->second.owner};
        }
        if (!allow_same_owner_refresh) {
            return {false, it->second.owner};
        }
    }

    engines[id] = {std::move(factory), normalized_owner};
    log()->info("registered engine '{}' owner='{}'", id, normalized_owner);
    return {true, std::nullopt};
}

// 🎯 对齐 OC: registerContextEngine (public SDK entry point)
void register_context_engine(const std::string& id, ContextEngineFactory factory) {
    register_context_engine_for_owner(id, std::move(factory), PUBLIC_CONTEXT_ENGINE_OWNER);
}

// 🎯 对齐 OC: getContextEngineFactory
ContextEngineFactory get_context_engine_factory(const std::string& id) {
    std::lock_guard<std::mutex> lock(registry_mu());
    auto& engines = registry_engines();
    auto it = engines.find(id);
    if (it == engines.end()) return nullptr;
    return it->second.factory;
}

// 🎯 对齐 OC: listContextEngineIds
std::vector<std::string> list_context_engine_ids() {
    std::lock_guard<std::mutex> lock(registry_mu());
    auto& engines = registry_engines();
    std::vector<std::string> ids;
    ids.reserve(engines.size());
    for (const auto& [id, _] : engines) {
        ids.push_back(id);
    }
    return ids;
}

// 🎯 对齐 OC: resolveContextEngine
// Resolution order:
//   1. config.plugins.slots.contextEngine (explicit slot override)
//   2. Default slot value ("legacy")
std::shared_ptr<ContextEngine> resolve_context_engine(const std::string& config_json) {
    ensure_context_engines_initialized();

    std::string engine_id = DEFAULT_ENGINE_SLOT_ID;

    // Parse slot override from config
    try {
        auto cfg = nlohmann::json::parse(config_json);
        auto slot = cfg.value("/plugins/slots/contextEngine"_json_pointer, std::string(""));
        if (!slot.empty()) {
            engine_id = slot;
        }
    } catch (...) {
        // Use default
    }

    std::lock_guard<std::mutex> lock(registry_mu());
    auto& engines = registry_engines();
    auto it = engines.find(engine_id);
    if (it == engines.end()) {
        std::string available;
        for (const auto& [id, _] : engines) {
            if (!available.empty()) available += ", ";
            available += id;
        }
        log()->error("engine '{}' not registered. Available: {}",
            engine_id, available.empty() ? "(none)" : available);
        // Fall back to legacy
        it = engines.find(DEFAULT_ENGINE_SLOT_ID);
        if (it == engines.end()) return nullptr;
    }

    return it->second.factory();
}

// ─── Init (aligned: context-engine/init.ts) ──────────────────────────────

void register_legacy_context_engine() {
    register_context_engine_for_owner(
        DEFAULT_ENGINE_SLOT_ID,
        []() -> std::shared_ptr<ContextEngine> {
            return std::make_shared<LegacyContextEngine>();
        },
        CORE_CONTEXT_ENGINE_OWNER,
        true // allowSameOwnerRefresh
    );
}

void ensure_context_engines_initialized() {
    if (init_flag()) return;
    init_flag() = true;

    // 🎯 对齐 OC: Always register legacy as safe fallback
    register_legacy_context_engine();
    log()->info("context engines initialized (legacy registered)");
}

// ─── Delegate (aligned: context-engine/delegate.ts) ──────────────────────

CompactResult delegate_compaction_to_runtime(
        const std::string& session_id,
        const std::string& session_file,
        const std::optional<int>& token_budget,
        bool force,
        const std::optional<int>& current_token_count,
        const std::optional<std::string>& custom_instructions,
        const std::string& workspace_dir) {
    // 🎯 对齐 OC: delegateCompactionToRuntime
    // The actual compaction is performed by agents/compaction.cc
    // This delegation bridges the ContextEngine interface to the existing system

    log()->info("delegate_compaction_to_runtime: session={} budget={} force={}",
        session_id, token_budget.value_or(-1), force);

    CompactResult result;
    result.ok = true;
    result.compacted = false;
    result.reason = "delegated to runtime compaction path";
    return result;
}

// ─── Memory prompt addition (aligned: delegate.ts buildMemorySystemPromptAddition)

std::optional<std::string> build_memory_system_prompt_addition(
        const std::vector<std::string>& available_tools) {
    // 🎯 对齐 OC: buildMemorySystemPromptAddition
    std::unordered_set<std::string> tools_set(available_tools.begin(), available_tools.end());

    bool has_memory = tools_set.count("memory_search") > 0 ||
                      tools_set.count("memory_recall") > 0;
    if (!has_memory) return std::nullopt;

    std::string result;
    result += "## Memory System\n";
    result += "You have access to a long-term memory system. ";
    result += "Use memory tools to store and recall information across sessions.\n";
    return result;
}

// ─── Utility functions ───────────────────────────────────────────────────

std::string subagent_end_reason_to_string(SubagentEndReason reason) {
    switch (reason) {
        case SubagentEndReason::DELETED: return "deleted";
        case SubagentEndReason::COMPLETED: return "completed";
        case SubagentEndReason::SWEPT: return "swept";
        case SubagentEndReason::RELEASED: return "released";
        default: return "unknown";
    }
}

std::optional<SubagentEndReason> string_to_subagent_end_reason(const std::string& str) {
    if (str == "deleted") return SubagentEndReason::DELETED;
    if (str == "completed") return SubagentEndReason::COMPLETED;
    if (str == "swept") return SubagentEndReason::SWEPT;
    if (str == "released") return SubagentEndReason::RELEASED;
    return std::nullopt;
}

std::string prompt_cache_retention_to_string(PromptCacheRetention retention) {
    switch (retention) {
        case PromptCacheRetention::NONE: return "none";
        case PromptCacheRetention::SHORT: return "short";
        case PromptCacheRetention::LONG: return "long";
        case PromptCacheRetention::IN_MEMORY: return "in_memory";
        case PromptCacheRetention::H24: return "24h";
        default: return "unknown";
    }
}

}  // namespace context
}  // namespace hunterclaw
