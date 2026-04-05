#include "hunterclaw/context/context_engine.h"

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <nlohmann/json.hpp>

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
  // 严格对齐 OpenClaw legacy.ts:compact()
  // 委托给现有的 compaction 逻辑 (CompactionManager)
  //
  // OpenClaw 中 legacy engine 调用 compactEmbeddedPiSessionDirect()，
  // 并从 runtimeContext 中提取 workspaceDir、currentTokenCount 等参数。
  // HunterClaw 中 CompactionManager::compact_session() 是等效实现。
  
  std::cerr << "[LegacyContextEngine] compact() called for session: " << session_id << std::endl;
  
  // 从 runtimeContext 提取参数 (对齐 OpenClaw legacy.ts:84-103)
  int effective_token_count = current_token_count.value_or(0);
  if (effective_token_count == 0 && runtime_context.contains("currentTokenCount")) {
    auto& val = runtime_context["currentTokenCount"];
    if (val.is_number_integer() && val.get<int>() > 0) {
      effective_token_count = val.get<int>();
    }
  }
  
  std::string workspace_dir;
  if (runtime_context.contains("workspaceDir") && runtime_context["workspaceDir"].is_string()) {
    workspace_dir = runtime_context["workspaceDir"].get<std::string>();
  }
  
  // 如果没有足够的信息来执行 compaction，返回 "not compacted"
  // 这对齐了 OpenClaw 中 compactEmbeddedPiSessionDirect 在参数不足时的行为
  if (effective_token_count <= 0 && !force) {
    return CompactResult{
      true,   // ok
      false,  // not compacted
      "No token count available for compaction decision",
      std::nullopt
    };
  }
  
  // 注意: 真正的 compaction 执行路径在 ReactEngine::trigger_compaction() 中，
  // 它已经直接调用了 CompactionManager::compact_session()。
  // LegacyContextEngine::compact() 被 run.ts 的 overflow compaction 循环调用，
  // 此处提供等效的委托。
  //
  // 由于 LegacyContextEngine 不持有 CompactionManager 引用
  // (对齐 OpenClaw: legacy engine 通过动态 import 获取 compactEmbeddedPiSessionDirect)，
  // 我们返回一个需要上层处理的信号。
  //
  // 实际的压缩逻辑仍然在 ReactEngine::trigger_compaction() 中执行，
  // 这里提供了 ContextEngine 接口的正确实现。
  
  return CompactResult{
    true,   // ok
    false,  // not compacted — 由 ReactEngine 层面处理
    "Legacy engine delegates compaction to runtime (ReactEngine::trigger_compaction)",
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

// Default slot id for contextEngine (对齐 OpenClaw slots.ts: DEFAULT_SLOT_BY_KEY)
static const std::string DEFAULT_CONTEXT_ENGINE_SLOT = "legacy";

std::shared_ptr<ContextEngine> resolve_context_engine(const std::string& config_json) {
  // 对齐 OpenClaw registry.ts:resolveContextEngine
  // Resolution order:
  //   1. config.plugins.slots.contextEngine (explicit slot override)
  //   2. Default slot value ("legacy")
  std::string engine_id = DEFAULT_CONTEXT_ENGINE_SLOT;
  
  // Parse config JSON to extract plugins.slots.contextEngine
  try {
    auto config = nlohmann::json::parse(config_json);
    if (config.contains("plugins") && config["plugins"].is_object()) {
      const auto& plugins = config["plugins"];
      if (plugins.contains("slots") && plugins["slots"].is_object()) {
        const auto& slots = plugins["slots"];
        if (slots.contains("contextEngine") && slots["contextEngine"].is_string()) {
          std::string slot_value = slots["contextEngine"].get<std::string>();
          // Trim whitespace (对齐 OpenClaw: slotValue.trim())
          auto start = slot_value.find_first_not_of(" \t\n\r");
          auto end = slot_value.find_last_not_of(" \t\n\r");
          if (start != std::string::npos) {
            slot_value = slot_value.substr(start, end - start + 1);
          }
          if (!slot_value.empty()) {
            engine_id = slot_value;
          }
        }
      }
    }
  } catch (const nlohmann::json::parse_error& e) {
    // Invalid JSON — fall through to default
    std::cerr << "[ContextEngine] Failed to parse config JSON: " << e.what() << std::endl;
  }
  
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
  // 对齐 OpenClaw init.ts: 幂等守卫，只初始化一次
  static bool initialized = false;
  if (initialized) {
    return;
  }
  initialized = true;
  
  // Register built-in engines
  register_legacy_context_engine();
  
  std::cout << "[ContextEngine] Initialized with engines: "
    << [&]() {
      auto ids = list_context_engine_ids();
      std::string result;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) result += ", ";
        result += ids[i];
      }
      return result;
    }() << std::endl;
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
