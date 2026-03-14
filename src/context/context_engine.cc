#include "hunterclaw/context/context_engine.h"

#include <iostream>
#include <mutex>
#include <stdexcept>

namespace hunterclaw {
namespace context {

// ============================================================================
// LegacyContextEngine Implementation
// ============================================================================

LegacyContextEngine::LegacyContextEngine() = default;
LegacyContextEngine::~LegacyContextEngine() = default;

ContextEngineInfo LegacyContextEngine::get_info() const {
  return ContextEngineInfo{
    "legacy",
    "Legacy Context Engine",
    "1.0.0",
    false  // does not own compaction
  };
}

IngestResult LegacyContextEngine::ingest(const std::string& session_id,
                                         const std::string& message_json,
                                         bool is_heartbeat) {
  // No-op: SessionManager handles message persistence in the legacy flow
  return IngestResult{false};
}

AssembleResult LegacyContextEngine::assemble(const std::string& session_id,
                                             const std::vector<std::string>& messages_json,
                                             const std::optional<int>& token_budget) {
  // Pass-through: the existing sanitize -> validate -> limit -> repair pipeline
  // in the agent runtime handles context assembly for the legacy engine.
  // We just return the messages as-is with a rough token estimate.
  return AssembleResult{
    messages_json,
    0,  // Caller handles estimation
    std::nullopt
  };
}

void LegacyContextEngine::after_turn(const std::string& session_id,
                                     const std::string& session_file,
                                     const std::vector<std::string>& messages_json,
                                     int pre_prompt_message_count,
                                     const std::optional<std::string>& auto_compaction_summary,
                                     bool is_heartbeat,
                                     const std::optional<int>& token_budget,
                                     const ContextEngineRuntimeContext& runtime_context) {
  // No-op: legacy flow persists context directly in SessionManager.
}

CompactResult LegacyContextEngine::compact(const std::string& session_id,
                                           const std::string& session_file,
                                           const std::optional<int>& token_budget,
                                           bool force,
                                           const std::optional<int>& current_token_count,
                                           const std::string& compaction_target,
                                           const std::optional<std::string>& custom_instructions,
                                           const ContextEngineRuntimeContext& runtime_context) {
  // In a real implementation, this would delegate to the existing compaction logic
  // (e.g., compactEmbeddedPiSessionDirect in OpenClaw).
  // For now, we return a simple result indicating compaction is not implemented.
  
  std::cerr << "[LegacyContextEngine] compact() called for session: " << session_id << std::endl;
  std::cerr << "[LegacyContextEngine] This is a placeholder - real compaction logic needed" << std::endl;
  
  // Placeholder: return "not compacted"
  return CompactResult{
    true,   // ok
    false,  // not compacted
    "Legacy compaction not implemented in C++ port",
    std::nullopt
  };
}

void LegacyContextEngine::dispose() {
  // Nothing to clean up for legacy engine
}

// ============================================================================
// Context Engine Registry
// ============================================================================

namespace {
struct ContextEngineRegistryState {
  std::unordered_map<std::string, ContextEngineFactory> engines;
  std::mutex mutex;
};

ContextEngineRegistryState& get_global_registry_state() {
  static ContextEngineRegistryState state;
  return state;
}
}  // namespace

void register_context_engine(const std::string& id, ContextEngineFactory factory) {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.engines[id] = factory;
}

ContextEngineFactory get_context_engine_factory(const std::string& id) {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  
  auto it = state.engines.find(id);
  if (it != state.engines.end()) {
    return it->second;
  }
  return nullptr;
}

std::vector<std::string> list_context_engine_ids() {
  auto& state = get_global_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  
  std::vector<std::string> ids;
  ids.reserve(state.engines.size());
  for (const auto& [id, _] : state.engines) {
    ids.push_back(id);
  }
  return ids;
}

std::shared_ptr<ContextEngine> resolve_context_engine(const std::string& config_json) {
  // Parse config to get the engine id
  // For now, we just default to "legacy"
  std::string engine_id = "legacy";
  
  // TODO: Parse config_json to get config.plugins.slots.contextEngine
  // For now, hardcode to "legacy"
  
  auto factory = get_context_engine_factory(engine_id);
  if (!factory) {
    auto available_ids = list_context_engine_ids();
    std::string available = "(none)";
    if (!available_ids.empty()) {
      available = "";
      for (size_t i = 0; i < available_ids.size(); ++i) {
        if (i > 0) available += ", ";
        available += available_ids[i];
      }
    }
    
    throw std::runtime_error(
      "Context engine \"" + engine_id + "\" is not registered. " +
      "Available engines: " + available
    );
  }
  
  return factory();
}

void register_legacy_context_engine() {
  register_context_engine("legacy", []() -> std::shared_ptr<ContextEngine> {
    return std::make_shared<LegacyContextEngine>();
  });
}

void ensure_context_engines_initialized() {
  // Register built-in engines
  register_legacy_context_engine();
  
  // Could register other built-in engines here in the future
  std::cout << "[ContextEngine] Initialized with engines: ";
  auto ids = list_context_engine_ids();
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << ids[i];
  }
  std::cout << std::endl;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string subagent_end_reason_to_string(SubagentEndReason reason) {
  switch (reason) {
    case SubagentEndReason::DELETED: return "deleted";
    case SubagentEndReason::COMPLETED: return "completed";
    case SubagentEndReason::SWEPT: return "swept";
    case SubagentEndReason::RELEASED: return "released";
  }
  return "unknown";
}

std::optional<SubagentEndReason> string_to_subagent_end_reason(const std::string& str) {
  if (str == "deleted") return SubagentEndReason::DELETED;
  if (str == "completed") return SubagentEndReason::COMPLETED;
  if (str == "swept") return SubagentEndReason::SWEPT;
  if (str == "released") return SubagentEndReason::RELEASED;
  return std::nullopt;
}

}  // namespace context
}  // namespace hunterclaw
