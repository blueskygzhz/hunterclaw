// DEPRECATED: SessionMemoryProvider as a standalone class does not exist in openclaw.
//
// openclaw alignment:
// - Session memory is handled INTERNALLY by MemoryIndexManager (manager.ts):
//   * sessionsDirty / sessionDeltas / sessionWarm track session state
//   * warmSession() triggers sync on session start
//   * ensureSessionListener() subscribes to session events
//   * sync({ reason: "session-start" }) indexes session JSONL files
//
// - Session JSONL files are parsed by session-files.ts (session_files.h in C++)
//   * buildSessionEntry() reads .jsonl and extracts text
//   * listSessionFilesForAgent() lists all session files
//
// - There is NO separate SessionMemoryProvider class in openclaw.
//
// Migration:
//   Before: SessionMemoryProvider::export_session_messages(session_key, messages)
//   After:  MemoryIndexManager::sync({ reason: "session-start", session_files: [path] })
//
// This file is kept as a compatibility shim only.
#pragma once
#include "hunterclaw/memory/session_files.h"
#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// Session memory configuration (aligns with ResolvedQmdSessionConfig in openclaw)
struct SessionMemoryConfig {
    bool enabled = false;
    std::optional<std::string> export_dir;   // Session transcripts directory
    std::optional<int> retention_days;       // How long to keep session files
};

// SessionMemoryProvider is a thin wrapper around session_files functions.
// It does NOT implement MemorySearchManager (openclaw has no such class).
// Use MemoryIndexManager for actual memory search.
class SessionMemoryProvider {
public:
    SessionMemoryProvider(
        const std::string& agent_id,
        const SessionMemoryConfig& config
    );

    // List all session files for this agent
    core::Result<std::vector<std::string>> list_session_files() const;

    // Build a session entry from a JSONL file path
    core::Result<SessionFileEntry> build_entry(const std::string& abs_path) const;

    // Prune old session files beyond retention_days
    core::Result<void> prune_old_sessions();

    const SessionMemoryConfig& config() const { return config_; }
    const std::string& agent_id() const { return agent_id_; }

private:
    std::string agent_id_;
    SessionMemoryConfig config_;
};

} // namespace memory
} // namespace hunterclaw
