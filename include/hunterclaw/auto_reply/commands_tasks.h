#pragma once

// 严格对齐 OpenClaw: auto-reply/reply/commands-tasks.ts (150行)
// + auto-reply/reply/commands-status.ts (task lines only)
// + task-status-access.ts (11行)
// /tasks command handler + /status task line + status access helpers

#include "hunterclaw/tasks/task_registry.h"
#include "hunterclaw/tasks/task_executor.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Task Status Access (对齐 OC: task-status-access.ts)
// ============================================================================

/// List tasks for session key, suitable for status display
/// (对齐 OC: listTasksForSessionKeyForStatus)
std::vector<tasks::TaskRecord> list_tasks_for_session_key_for_status(
    tasks::TaskRegistry& registry,
    const std::string& session_key);

/// List tasks for agent id, suitable for status display
/// (对齐 OC: listTasksForAgentIdForStatus)
std::vector<tasks::TaskRecord> list_tasks_for_agent_id_for_status(
    tasks::TaskRegistry& registry,
    const std::string& agent_id);

// ============================================================================
// /tasks command (对齐 OC: auto-reply/reply/commands-tasks.ts)
// ============================================================================

/// Build /tasks reply text (对齐 OC: buildTasksText)
std::string build_tasks_text(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& agent_id);

// ============================================================================
// /status task lines (对齐 OC: auto-reply/reply/commands-status.ts:68-92)
// ============================================================================

/// Format session task line for /status output (对齐 OC: formatSessionTaskLine)
std::optional<std::string> format_session_task_line(
    tasks::TaskRegistry& registry,
    const std::string& session_key);

/// Format agent task counts line for /status output (对齐 OC: formatAgentTaskCountsLine)
std::optional<std::string> format_agent_task_counts_line(
    tasks::TaskRegistry& registry,
    const std::string& agent_id);

// ============================================================================
// Media Generation Task Status (对齐 OC: agents/media-generation-task-status-shared.ts)
// ============================================================================

/// Check if task is an active media generation task
bool is_active_media_generation_task(
    const tasks::TaskRecord& task,
    const std::string& task_kind);

/// Find active media generation task for session
std::optional<tasks::TaskRecord> find_active_media_generation_task_for_session(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& task_kind,
    const std::string& source_prefix);

/// Get media generation task provider id from source_id
std::optional<std::string> get_media_generation_task_provider_id(
    const tasks::TaskRecord& task,
    const std::string& source_prefix);

/// Build media generation task status text
std::string build_media_generation_task_status_text(
    const tasks::TaskRecord& task,
    const std::string& source_prefix,
    const std::string& noun_label,
    const std::string& tool_name,
    const std::string& completion_label,
    bool duplicate_guard = false);

/// Build active media generation task prompt context for session
std::optional<std::string> build_active_media_generation_task_prompt_context_for_session(
    tasks::TaskRegistry& registry,
    const std::string& session_key,
    const std::string& task_kind,
    const std::string& source_prefix,
    const std::string& noun_label,
    const std::string& tool_name,
    const std::string& completion_label);

// ============================================================================
// Video / Music generation wrappers (对齐 OC video/music-generation-task-status.ts)
// ============================================================================

static constexpr const char* VIDEO_GENERATION_TASK_KIND = "video_generation";
static constexpr const char* VIDEO_GENERATION_SOURCE_PREFIX = "video_generate";
static constexpr const char* MUSIC_GENERATION_TASK_KIND = "music_generation";
static constexpr const char* MUSIC_GENERATION_SOURCE_PREFIX = "music_generate";

} // namespace auto_reply
} // namespace hunterclaw
