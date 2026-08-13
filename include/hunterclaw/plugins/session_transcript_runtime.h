#pragma once
// ✅ 严格对齐 OpenClaw:
//   src/plugin-sdk/session-transcript-runtime.ts
//   src/plugin-sdk/session-transcript-memory-hit.ts
//
// 提供 plugin 读取当前 session transcript 的运行时 API。
// HunterClaw 底层为 JSONL transcript（transcript_io.cc），语义与 OpenClaw 的
// SQLite session-accessor 等价：读取 events / 投影可见消息 / 读取最新 assistant 文本。

#include "hunterclaw/core/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace plugins {

// ============================================================================
// Memory Hit Key (对齐 session-transcript-memory-hit.ts)
// ============================================================================

/// 对齐 OpenClaw: SessionTranscriptMemoryHitKey (string, "transcript:{agentId}:{sessionId}")
using SessionTranscriptMemoryHitKey = std::string;

/// 对齐 OpenClaw: SessionTranscriptMemoryHitKeyParams
struct SessionTranscriptMemoryHitKeyParams {
  std::string agent_id;
  std::string session_id;
};

/// 对齐 OpenClaw: SessionTranscriptIdentity
struct SessionTranscriptIdentity {
  std::string agent_id;
  SessionTranscriptMemoryHitKey memory_key;
  std::string session_id;
  std::string session_key;
};

/// 对齐 OpenClaw: SessionTranscriptMemoryHitIdentity
struct SessionTranscriptMemoryHitIdentity {
  std::string agent_id;
  SessionTranscriptMemoryHitKey key;
  std::string session_id;
};

/// 对齐 OpenClaw: Record<string, SessionEntry> 的简化视图
///   sessionKey -> sessionId
using SessionTranscriptStore = std::map<std::string, std::string>;

/// 对齐 OpenClaw: ResolveSessionTranscriptMemoryHitKeyParams
struct ResolveSessionTranscriptMemoryHitKeyParams {
  bool include_synthetic_fallback = true;
  SessionTranscriptMemoryHitKey key;
  SessionTranscriptStore store;
};

// ============================================================================
// Read Params (对齐 session-transcript-runtime.ts)
// ============================================================================

/// 对齐 OpenClaw: SessionTranscriptReadParams
struct SessionTranscriptReadParams {
  std::optional<std::string> agent_id;
  std::optional<std::string> env;
  bool hydrate_skill_prompt_refs = false;
  std::string session_id;
  std::string session_key;
  std::optional<std::string> store_path;
  std::optional<std::string> thread_id;
};

// ============================================================================
// Transcript Event / Message Entry (对齐 session-transcript-runtime.ts)
// ============================================================================

/// 对齐 OpenClaw: SessionTranscriptEvent (每条 JSONL 行解析后的对象)
using SessionTranscriptEvent = core::JsonValue;

/// 对齐 OpenClaw: SessionTranscriptMessageEntry
struct SessionTranscriptMessageEntry {
  std::string entry_id;
  std::optional<std::string> parent_id;  // null -> nullopt
  int64_t seq = 0;
  core::JsonValue message;
  std::string role;
  std::optional<std::string> created_at;
  std::optional<std::string> idempotency_key;
};

/// 对齐 OpenClaw: SessionTranscriptTarget
struct SessionTranscriptTarget {
  std::string target_kind = "runtime-session";
  std::string agent_id;
  SessionTranscriptMemoryHitKey memory_key;
  std::string session_id;
  std::string session_key;
};

/// 对齐 OpenClaw: LatestTranscriptAssistantText
struct LatestTranscriptAssistantText {
  std::string text;
  std::optional<std::string> message_id;
  std::optional<std::string> entry_id;
  std::optional<std::string> updated_at;
};

// ============================================================================
// Memory Hit Key 工具 (对齐 session-transcript-memory-hit.ts)
// ============================================================================

/// 对齐 OpenClaw: formatSessionTranscriptMemoryHitKey
SessionTranscriptMemoryHitKey format_session_transcript_memory_hit_key(
    const SessionTranscriptMemoryHitKeyParams& params);

/// 对齐 OpenClaw: parseSessionTranscriptMemoryHitKey
std::optional<SessionTranscriptMemoryHitIdentity> parse_session_transcript_memory_hit_key(
    const std::string& key);

/// 对齐 OpenClaw: resolveSessionTranscriptMemoryHitKeyToSessionKeys
std::vector<std::string> resolve_session_transcript_memory_hit_key_to_session_keys(
    const ResolveSessionTranscriptMemoryHitKeyParams& params);

// ============================================================================
// 目标解析 (对齐 session-transcript-runtime.ts)
// ============================================================================

/// 对齐 OpenClaw: resolveSessionTranscriptIdentity
core::Result<SessionTranscriptIdentity> resolve_session_transcript_identity(
    const SessionTranscriptReadParams& params);

/// 对齐 OpenClaw: resolveSessionTranscriptTarget
core::Result<SessionTranscriptTarget> resolve_session_transcript_target(
    const SessionTranscriptReadParams& params);

// ============================================================================
// 读取 API (对齐 session-transcript-runtime.ts)
// ============================================================================

/// 对齐 OpenClaw: readSessionTranscriptEvents
core::Result<std::vector<SessionTranscriptEvent>> read_session_transcript_events(
    const SessionTranscriptReadParams& params);

/// 对齐 OpenClaw: readVisibleSessionTranscriptMessageEntries
core::Result<std::vector<SessionTranscriptMessageEntry>>
read_visible_session_transcript_message_entries(
    const SessionTranscriptReadParams& params);

/// 对齐 OpenClaw: readLatestAssistantTextByIdentity
core::Result<std::optional<LatestTranscriptAssistantText>>
read_latest_assistant_text_by_identity(
    const SessionTranscriptReadParams& params);

}  // namespace plugins
}  // namespace hunterclaw
