#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace context {

// Forward declarations
class ContextEngine;

// ============================================================================
// Context Engine Result Types
// ============================================================================

/**
 * Result of assemble() - ordered messages ready for the model.
 */
struct AssembleResult {
  std::vector<std::string> messages_json;  // Ordered AgentMessages as JSON strings
  int estimated_tokens;
  std::optional<std::string> system_prompt_addition;  // Optional prepended instructions
};

/**
 * Result of compact() - context compaction outcome.
 */
struct CompactResult {
  bool ok;
  bool compacted;
  std::optional<std::string> reason;
  
  struct Details {
    std::optional<std::string> summary;
    std::optional<std::string> first_kept_entry_id;
    int tokens_before;
    std::optional<int> tokens_after;
    std::optional<std::string> details_json;  // Additional details as JSON
  };
  std::optional<Details> result;
};

/**
 * Result of ingest() - whether a message was ingested.
 */
struct IngestResult {
  bool ingested;  // false if duplicate or no-op
};

/**
 * Result of ingestBatch() - batch ingestion outcome.
 */
struct IngestBatchResult {
  int ingested_count;  // Number of messages ingested from the batch
};

/**
 * Result of bootstrap() - engine initialization outcome.
 */
struct BootstrapResult {
  bool bootstrapped;  // Whether bootstrap ran and initialized the engine's store
  std::optional<int> imported_messages;  // Number of historical messages imported
  std::optional<std::string> reason;  // Optional reason when bootstrap was skipped
};

/**
 * Context engine metadata.
 */
struct ContextEngineInfo {
  std::string id;
  std::string name;
  std::optional<std::string> version;
  bool owns_compaction = false;  // True when engine manages its own compaction lifecycle
};

/**
 * Subagent spawn preparation result.
 */
struct SubagentSpawnPreparation {
  std::function<void()> rollback;  // Roll back pre-spawn setup when subagent launch fails
};

/**
 * Subagent end reason.
 */
enum class SubagentEndReason {
  DELETED,
  COMPLETED,
  SWEPT,
  RELEASED
};

/**
 * Runtime context for context engine operations.
 * Opaque key-value map for engine-specific state.
 * Aligned with OpenClaw: ContextEngineRuntimeContext = Record<string, unknown>
 */
using ContextEngineRuntimeContext = nlohmann::json;

// ============================================================================
// ContextEngine Interface
// ============================================================================

/**
 * ContextEngine defines the pluggable contract for context management.
 *
 * Required methods define a generic lifecycle; optional methods allow engines
 * to provide additional capabilities (retrieval, lineage, etc.).
 */
class ContextEngine {
 public:
  virtual ~ContextEngine() = default;
  
  /**
   * Engine identifier and metadata.
   */
  virtual ContextEngineInfo get_info() const = 0;
  
  /**
   * Initialize engine state for a session, optionally importing historical context.
   */
  virtual BootstrapResult bootstrap(const std::string& session_id, 
                                     const std::string& session_file) {
    // Default: no-op
    return BootstrapResult{false, std::nullopt, "Not implemented"};
  }
  
  /**
   * Ingest a single message into the engine's store.
   */
  virtual IngestResult ingest(const std::string& session_id,
                              const std::string& message_json,
                              bool is_heartbeat = false) = 0;
  
  /**
   * Ingest a completed turn batch as a single unit.
   */
  virtual IngestBatchResult ingest_batch(const std::string& session_id,
                                         const std::vector<std::string>& messages_json,
                                         bool is_heartbeat = false) {
    // Default: ingest messages one by one
    int count = 0;
    for (const auto& msg : messages_json) {
      if (ingest(session_id, msg, is_heartbeat).ingested) {
        count++;
      }
    }
    return IngestBatchResult{count};
  }
  
  /**
   * Execute optional post-turn lifecycle work after a run attempt completes.
   * Engines can use this to persist canonical context and trigger background
   * compaction decisions.
   */
  virtual void after_turn(const std::string& session_id,
                          const std::string& session_file,
                          const std::vector<std::string>& messages_json,
                          int pre_prompt_message_count,
                          const std::optional<std::string>& auto_compaction_summary = std::nullopt,
                          bool is_heartbeat = false,
                          const std::optional<int>& token_budget = std::nullopt,
                          const ContextEngineRuntimeContext& runtime_context = {}) {
    // Default: no-op
  }
  
  /**
   * Assemble model context under a token budget.
   * Returns an ordered set of messages ready for the model.
   */
  virtual AssembleResult assemble(const std::string& session_id,
                                  const std::vector<std::string>& messages_json,
                                  const std::optional<int>& token_budget = std::nullopt) = 0;
  
  /**
   * Compact context to reduce token usage.
   * May create summaries, prune old turns, etc.
   */
  virtual CompactResult compact(const std::string& session_id,
                                const std::string& session_file,
                                const std::optional<int>& token_budget = std::nullopt,
                                bool force = false,
                                const std::optional<int>& current_token_count = std::nullopt,
                                const std::string& compaction_target = "budget",  // "budget" or "threshold"
                                const std::optional<std::string>& custom_instructions = std::nullopt,
                                const ContextEngineRuntimeContext& runtime_context = {}) = 0;
  
  /**
   * Prepare context-engine-managed subagent state before the child run starts.
   *
   * Implementations can return a rollback handle that is invoked when spawn
   * fails after preparation succeeds.
   */
  virtual std::optional<SubagentSpawnPreparation> prepare_subagent_spawn(
      const std::string& parent_session_key,
      const std::string& child_session_key,
      const std::optional<int>& ttl_ms = std::nullopt) {
    // Default: no preparation
    return std::nullopt;
  }
  
  /**
   * Notify the context engine that a subagent lifecycle ended.
   */
  virtual void on_subagent_ended(const std::string& child_session_key,
                                 SubagentEndReason reason) {
    // Default: no-op
  }
  
  /**
   * Dispose of any resources held by the engine.
   */
  virtual void dispose() {
    // Default: no-op
  }
};

// ============================================================================
// LegacyContextEngine Implementation
// ============================================================================

/**
 * LegacyContextEngine wraps the existing compaction behavior behind the
 * ContextEngine interface, preserving 100% backward compatibility.
 *
 * - ingest: no-op (SessionManager handles message persistence)
 * - assemble: pass-through (existing sanitize/validate/limit pipeline handles this)
 * - compact: delegates to existing compaction logic
 */
class LegacyContextEngine : public ContextEngine {
 public:
  LegacyContextEngine();
  ~LegacyContextEngine() override;
  
  ContextEngineInfo get_info() const override;
  
  IngestResult ingest(const std::string& session_id,
                      const std::string& message_json,
                      bool is_heartbeat = false) override;
  
  AssembleResult assemble(const std::string& session_id,
                          const std::vector<std::string>& messages_json,
                          const std::optional<int>& token_budget = std::nullopt) override;
  
  void after_turn(const std::string& session_id,
                  const std::string& session_file,
                  const std::vector<std::string>& messages_json,
                  int pre_prompt_message_count,
                  const std::optional<std::string>& auto_compaction_summary = std::nullopt,
                  bool is_heartbeat = false,
                  const std::optional<int>& token_budget = std::nullopt,
                  const ContextEngineRuntimeContext& runtime_context = {}) override;
  
  CompactResult compact(const std::string& session_id,
                        const std::string& session_file,
                        const std::optional<int>& token_budget = std::nullopt,
                        bool force = false,
                        const std::optional<int>& current_token_count = std::nullopt,
                        const std::string& compaction_target = "budget",
                        const std::optional<std::string>& custom_instructions = std::nullopt,
                        const ContextEngineRuntimeContext& runtime_context = {}) override;
  
  void dispose() override;
};

// ============================================================================
// Context Engine Factory
// ============================================================================

/**
 * A factory that creates a ContextEngine instance.
 * Supports async creation for engines that need DB connections etc.
 */
using ContextEngineFactory = std::function<std::shared_ptr<ContextEngine>()>;

// ============================================================================
// Transcript Rewrite Types (对齐 OpenClaw context-engine/types.ts:62-85)
// ============================================================================

struct TranscriptRewriteReplacement {
  std::string entry_id;    // Existing transcript entry id to replace
  std::string message_json; // Replacement message content as JSON
};

struct TranscriptRewriteRequest {
  std::vector<TranscriptRewriteReplacement> replacements;
};

struct TranscriptRewriteResult {
  bool changed;
  int bytes_freed;
  int rewritten_entries;
  std::optional<std::string> reason;
};

using ContextEngineMaintenanceResult = TranscriptRewriteResult;

// ============================================================================
// Prompt Cache Types (对齐 OpenClaw context-engine/types.ts:87-128)
// ============================================================================

enum class PromptCacheRetention {
  NONE,
  SHORT,
  LONG,
  IN_MEMORY,
  H24
};

struct PromptCacheUsage {
  std::optional<int> input;
  std::optional<int> output;
  std::optional<int> cache_read;
  std::optional<int> cache_write;
  std::optional<int> total;
};

struct PromptCacheObservation {
  bool broke;
  std::optional<int> previous_cache_read;
  std::optional<int> cache_read;
};

struct PromptCacheInfo {
  std::optional<PromptCacheRetention> retention;
  std::optional<PromptCacheUsage> last_call_usage;
  std::optional<PromptCacheObservation> observation;
  std::optional<int64_t> last_cache_touch_at;
  std::optional<int64_t> expires_at;
};

// ============================================================================
// Context Engine Registry (对齐 OpenClaw context-engine/registry.ts)
// ============================================================================

/**
 * Registration result (对齐 ContextEngineRegistrationResult).
 */
struct ContextEngineRegistrationResult {
  bool ok;
  std::optional<std::string> existing_owner; // Set when ok=false
};

/**
 * Register a context engine implementation under the given id.
 * Simple registration without owner tracking.
 */
void register_context_engine(const std::string& id, ContextEngineFactory factory);

/**
 * Owner-aware registration (对齐 OpenClaw registerContextEngineForOwner).
 * Prevents cross-owner overwrite + protects core-owned default slot.
 */
ContextEngineRegistrationResult register_context_engine_for_owner(
    const std::string& id,
    ContextEngineFactory factory,
    const std::string& owner,
    bool allow_same_owner_refresh = false
);

/**
 * Return the factory for a registered engine, or nullptr.
 */
ContextEngineFactory get_context_engine_factory(const std::string& id);

/**
 * List all registered engine ids.
 */
std::vector<std::string> list_context_engine_ids();

/**
 * Resolve which ContextEngine to use based on plugin slot configuration.
 *
 * Resolution order:
 *   1. `config.plugins.slots.contextEngine` (explicit slot override)
 *   2. Default slot value ("legacy")
 *
 * Throws if the resolved engine id has no registered factory.
 */
std::shared_ptr<ContextEngine> resolve_context_engine(const std::string& config_json = "{}");

/**
 * Register the legacy context engine.
 * Call this at startup to make "legacy" available.
 */
void register_legacy_context_engine();

/**
 * Ensure context engines are initialized.
 * Registers built-in engines (legacy, etc.).
 */
void ensure_context_engines_initialized();

// ============================================================================
// Delegate Compaction Helper (对齐 OpenClaw context-engine/delegate.ts)
// ============================================================================

/**
 * Delegate compaction to the built-in runtime compaction path.
 * Third-party engines can call this from their compact() implementations.
 */
CompactResult delegate_compaction_to_runtime(
    const std::string& session_id,
    const std::string& session_file,
    const std::optional<int>& token_budget = std::nullopt,
    bool force = false,
    const std::optional<int>& current_token_count = std::nullopt,
    const std::optional<std::string>& custom_instructions = std::nullopt,
    const std::string& workspace_dir = ""
);

/**
 * Build a context-engine-ready system prompt addition from the active
 * memory plugin prompt path (对齐 buildMemorySystemPromptAddition).
 */
std::optional<std::string> build_memory_system_prompt_addition(
    const std::vector<std::string>& available_tools
);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Convert SubagentEndReason to string.
 */
std::string subagent_end_reason_to_string(SubagentEndReason reason);

/**
 * Convert string to SubagentEndReason.
 */
std::optional<SubagentEndReason> string_to_subagent_end_reason(const std::string& str);

/**
 * Convert PromptCacheRetention to string.
 */
std::string prompt_cache_retention_to_string(PromptCacheRetention retention);

}  // namespace context
}  // namespace hunterclaw
