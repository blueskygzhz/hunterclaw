#pragma once

// 严格对齐 OpenClaw config/sessions/paths.ts (330行)
// Session 文件路径解析: transcript dir、store path、session file path

#include <optional>
#include <string>

namespace hunterclaw {
namespace sessions {

// ============================================================================
// Transcript Dirs (对齐 paths.ts:9-32)
// ============================================================================

/// Resolve the sessions directory for a given agent.
std::string resolve_agent_sessions_dir(
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& state_dir = std::nullopt
);

/// Resolve the default transcripts directory.
std::string resolve_session_transcripts_dir(
    const std::optional<std::string>& state_dir = std::nullopt
);

/// Resolve transcripts dir for a specific agent.
std::string resolve_session_transcripts_dir_for_agent(
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& state_dir = std::nullopt
);

// ============================================================================
// Store Path (对齐 paths.ts:34-36, 280-330)
// ============================================================================

/// Resolve the default session store path (sessions.json).
std::string resolve_default_session_store_path(
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& state_dir = std::nullopt
);

/// Resolve store path from config or default.
std::string resolve_store_path(
    const std::optional<std::string>& configured_store = std::nullopt,
    const std::optional<std::string>& agent_id = std::nullopt,
    const std::optional<std::string>& state_dir = std::nullopt
);

// ============================================================================
// Session File Path (对齐 paths.ts:38-278)
// ============================================================================

struct SessionFilePathOptions {
  std::optional<std::string> agent_id;
  std::optional<std::string> sessions_dir;
};

/// Resolve the JSONL transcript file path for a session.
std::string resolve_session_file_path(
    const std::string& session_id,
    const std::optional<std::string>& entry_session_file = std::nullopt,
    const SessionFilePathOptions& opts = {}
);

/// Resolve session transcript path (JSONL).
std::string resolve_session_transcript_path(
    const std::string& session_id,
    const SessionFilePathOptions& opts = {}
);

/// Resolve the agents dir from a session store path.
std::optional<std::string> resolve_agents_dir_from_session_store_path(
    const std::string& store_path
);

}  // namespace sessions
}  // namespace hunterclaw
