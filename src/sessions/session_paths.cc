// 严格对齐 OpenClaw config/sessions/paths.ts (330行)
#include "hunterclaw/sessions/session_paths.h"
#include <filesystem>
#include <algorithm>
#include <cstdlib>

namespace fs = std::filesystem;

namespace hunterclaw {
namespace sessions {

static const std::string DEFAULT_AGENT_ID = "main";

static std::string normalize_agent_id(const std::string& raw) {
  std::string r = raw;
  std::transform(r.begin(), r.end(), r.begin(), ::tolower);
  auto s = r.find_first_not_of(" \t\n\r");
  if (s == std::string::npos) return DEFAULT_AGENT_ID;
  auto e = r.find_last_not_of(" \t\n\r");
  r = r.substr(s, e - s + 1);
  return r.empty() ? DEFAULT_AGENT_ID : r;
}

static std::string resolve_state_dir(const std::optional<std::string>& override_dir) {
  if (override_dir && !override_dir->empty()) return *override_dir;
  const char* env = std::getenv("OPENCLAW_STATE_DIR");
  if (env && env[0]) return env;
  const char* home = std::getenv("HOME");
  if (!home) home = std::getenv("USERPROFILE");
  return home ? (fs::path(home) / ".openclaw").string() : ".openclaw";
}

// 对齐 paths.ts:9-17
std::string resolve_agent_sessions_dir(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& state_dir
) {
  std::string root = resolve_state_dir(state_dir);
  std::string id = normalize_agent_id(agent_id.value_or(DEFAULT_AGENT_ID));
  return (fs::path(root) / "agents" / id / "sessions").string();
}

std::string resolve_session_transcripts_dir(const std::optional<std::string>& state_dir) {
  return resolve_agent_sessions_dir(DEFAULT_AGENT_ID, state_dir);
}

std::string resolve_session_transcripts_dir_for_agent(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& state_dir
) {
  return resolve_agent_sessions_dir(agent_id, state_dir);
}

// 对齐 paths.ts:34-36
std::string resolve_default_session_store_path(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& state_dir
) {
  return (fs::path(resolve_agent_sessions_dir(agent_id, state_dir)) / "sessions.json").string();
}

// 对齐 paths.ts:280-330
std::string resolve_store_path(
    const std::optional<std::string>& configured_store,
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& state_dir
) {
  if (configured_store && !configured_store->empty()) {
    std::string store = *configured_store;
    auto s = store.find_first_not_of(" \t");
    if (s != std::string::npos) {
      store = store.substr(s);
      auto e = store.find_last_not_of(" \t");
      store = store.substr(0, e + 1);
    }
    if (!store.empty()) {
      // Expand ~ prefix
      if (store[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) store = std::string(home) + store.substr(1);
      }
      return fs::path(store).string();
    }
  }
  return resolve_default_session_store_path(agent_id, state_dir);
}

// 对齐 paths.ts:38-278 (simplified)
static std::string sanitize_session_id_for_filename(const std::string& id) {
  std::string safe;
  for (char c : id) {
    if (std::isalnum(c) || c == '-' || c == '_') safe += c;
  }
  return safe.empty() ? "unknown" : safe;
}

std::string resolve_session_file_path(
    const std::string& session_id,
    const std::optional<std::string>& entry_session_file,
    const SessionFilePathOptions& opts
) {
  // If entry already has a session file, use it
  if (entry_session_file && !entry_session_file->empty()) {
    std::string f = *entry_session_file;
    auto s = f.find_first_not_of(" \t");
    if (s != std::string::npos) {
      f = f.substr(s);
      auto e = f.find_last_not_of(" \t");
      f = f.substr(0, e + 1);
      if (!f.empty()) return f;
    }
  }
  // Compute from session ID
  return resolve_session_transcript_path(session_id, opts);
}

std::string resolve_session_transcript_path(
    const std::string& session_id,
    const SessionFilePathOptions& opts
) {
  std::string sessions_dir;
  if (opts.sessions_dir && !opts.sessions_dir->empty()) {
    sessions_dir = *opts.sessions_dir;
  } else {
    sessions_dir = resolve_agent_sessions_dir(opts.agent_id);
  }
  std::string filename = sanitize_session_id_for_filename(session_id) + ".jsonl";
  return (fs::path(sessions_dir) / filename).string();
}

std::optional<std::string> resolve_agents_dir_from_session_store_path(
    const std::string& store_path
) {
  fs::path abs_path = fs::absolute(store_path);
  if (abs_path.filename() != "sessions.json") return std::nullopt;
  // Expected: .../agents/<agentId>/sessions/sessions.json
  fs::path sessions_dir = abs_path.parent_path();           // sessions/
  fs::path agent_dir = sessions_dir.parent_path();           // <agentId>/
  fs::path agents_dir = agent_dir.parent_path();             // agents/
  if (agents_dir.filename() != "agents") return std::nullopt;
  return agents_dir.string();
}

}  // namespace sessions
}  // namespace hunterclaw
