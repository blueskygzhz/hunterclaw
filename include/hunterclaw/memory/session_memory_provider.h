#pragma once

#include <string>
#include <optional>
#include <memory>

namespace hunterclaw {
namespace memory {

// Configuration for session memory export
// Used by DeltaTrigger to write session JSONL files
struct SessionMemoryConfig {
    bool enabled = false;
    std::optional<std::string> export_dir;  // Directory to write session JSONL files
};

// Provider interface for session memory operations
// Aligns with OpenClaw session delta tracking in manager-sync-ops.ts
class SessionMemoryProvider {
public:
    virtual ~SessionMemoryProvider() = default;

    // Returns the current session memory configuration
    virtual const SessionMemoryConfig& config() const = 0;
};

// Simple concrete implementation backed by a config struct
class SimpleSessionMemoryProvider : public SessionMemoryProvider {
public:
    explicit SimpleSessionMemoryProvider(SessionMemoryConfig cfg)
        : config_(std::move(cfg)) {}

    const SessionMemoryConfig& config() const override {
        return config_;
    }

private:
    SessionMemoryConfig config_;
};

} // namespace memory
} // namespace hunterclaw
