#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace hunterclaw {
namespace context {

// ============================================================================
// Context Engine Plugin System (OpenClaw-compatible)
// ============================================================================

struct Message {
    std::string role;
    std::string content;
    std::map<std::string, core::JsonValue> metadata;
};

struct BootstrapResult {
    std::vector<Message> initial_messages;
    std::string system_prompt_addition;
};

struct IngestResult {
    bool success;
    std::string error;
};

struct AssembleResult {
    std::vector<Message> messages;
    std::string system_prompt_addition;
    int tokens_used;
};

struct CompactResult {
    bool compacted;
    int tokens_saved;
    std::string summary;
};

struct SubagentSpawnPreparation {
    std::vector<Message> inherited_context;
    std::map<std::string, core::JsonValue> resources;
    int ttl_ms;
};

// Context engine interface
class ContextEngine {
public:
    virtual ~ContextEngine() = default;
    
    // Engine info
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
    
    // Lifecycle hooks
    virtual core::Result<BootstrapResult> bootstrap(
        const std::string& session_id,
        const std::string& session_file
    ) { 
        return core::Result<BootstrapResult>::ok(BootstrapResult{}); 
    }
    
    virtual core::Result<IngestResult> ingest(
        const std::string& session_id,
        const Message& message,
        bool is_heartbeat = false
    ) = 0;
    
    virtual core::Result<IngestResult> ingest_batch(
        const std::string& session_id,
        const std::vector<Message>& messages
    ) {
        // Default: call ingest individually
        for (const auto& msg : messages) {
            auto result = ingest(session_id, msg);
            if (!result.is_ok()) return result;
        }
        return core::Result<IngestResult>::ok(IngestResult{true, ""});
    }
    
    // Context assembly (main hook)
    virtual core::Result<AssembleResult> assemble(
        const std::string& session_id,
        const std::vector<Message>& messages,
        int token_budget = -1
    ) = 0;
    
    // Compaction (summarization)
    virtual core::Result<CompactResult> compact(
        const std::string& session_id,
        const std::string& session_file,
        int token_budget = -1,
        bool force = false,
        float compaction_target = 0.5f
    ) {
        return core::Result<CompactResult>::ok(CompactResult{false, 0, ""});
    }
    
    // Subagent support
    virtual core::Result<SubagentSpawnPreparation> prepare_subagent_spawn(
        const std::string& parent_session_key,
        const std::string& child_session_key,
        int ttl_ms = -1
    ) {
        return core::Result<SubagentSpawnPreparation>::ok(SubagentSpawnPreparation{});
    }
    
    virtual core::Result<bool> on_subagent_ended(
        const std::string& child_session_key,
        const std::string& reason  // "deleted" | "completed" | "swept" | "released"
    ) {
        return core::Result<bool>::ok(true);
    }
    
    // Cleanup
    virtual void dispose() {}
};

// ============================================================================
// Built-in Engines
// ============================================================================

// Legacy engine (sliding window)
class LegacyEngine : public ContextEngine {
public:
    LegacyEngine(int max_messages = 100);
    
    std::string name() const override { return "legacy"; }
    std::string version() const override { return "1.0.0"; }
    
    core::Result<IngestResult> ingest(
        const std::string& session_id,
        const Message& message,
        bool is_heartbeat
    ) override;
    
    core::Result<AssembleResult> assemble(
        const std::string& session_id,
        const std::vector<Message>& messages,
        int token_budget
    ) override;
    
private:
    int max_messages_;
    std::map<std::string, std::vector<Message>> session_histories_;
};

// Lossless engine (preserve all history)
class LosslessClawEngine : public ContextEngine {
public:
    LosslessClawEngine();
    
    std::string name() const override { return "lossless-claw"; }
    std::string version() const override { return "1.0.0"; }
    
    core::Result<BootstrapResult> bootstrap(
        const std::string& session_id,
        const std::string& session_file
    ) override;
    
    core::Result<IngestResult> ingest(
        const std::string& session_id,
        const Message& message,
        bool is_heartbeat
    ) override;
    
    core::Result<AssembleResult> assemble(
        const std::string& session_id,
        const std::vector<Message>& messages,
        int token_budget
    ) override;
    
    core::Result<SubagentSpawnPreparation> prepare_subagent_spawn(
        const std::string& parent_session_key,
        const std::string& child_session_key,
        int ttl_ms
    ) override;
    
private:
    std::map<std::string, std::string> session_files_;
    std::map<std::string, std::vector<Message>> session_histories_;
};

// ============================================================================
// Engine Registry (Global Singleton)
// ============================================================================

using ContextEngineFactory = std::function<std::unique_ptr<ContextEngine>()>;

class ContextEngineRegistry {
public:
    // Get global instance
    static ContextEngineRegistry& instance();
    
    // Register engine factory
    void register_engine(const std::string& name, ContextEngineFactory factory);
    
    // Create engine instance
    core::Result<std::unique_ptr<ContextEngine>> create_engine(const std::string& name);
    
    // List available engines
    std::vector<std::string> list_engines() const;
    
private:
    ContextEngineRegistry();
    std::map<std::string, ContextEngineFactory> factories_;
};

// Helper: register engine at startup
template<typename T>
struct EngineRegistrar {
    EngineRegistrar(const std::string& name) {
        ContextEngineRegistry::instance().register_engine(name, []() {
            return std::make_unique<T>();
        });
    }
};

// Usage: static EngineRegistrar<MyEngine> _reg("my-engine");

}  // namespace context
}  // namespace hunterclaw
