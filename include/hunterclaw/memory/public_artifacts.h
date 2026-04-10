#pragma once

// ============================================================================
// Public Artifacts — 严格对齐 OpenClaw public-artifacts.ts (105 行)
//
// 枚举 memory 系统的公开产物（MEMORY.md、memory/*.md、DREAMS.md、event-log），
// 供 UI 和插件系统发现和展示。
// ============================================================================

#include <string>
#include <vector>

namespace hunterclaw {
namespace memory {

/**
 * A discoverable memory artifact.
 * (对齐 public-artifacts.ts MemoryPluginPublicArtifact)
 */
struct MemoryPublicArtifact {
    std::string kind;          // "memory-root" | "daily-note" | "dream-report" | "event-log"
    std::string workspace_dir;
    std::string relative_path;
    std::string absolute_path;
    std::vector<std::string> agent_ids;
    std::string content_type;  // "markdown" | "json"
};

/**
 * List all public memory artifacts for the given workspace.
 * (对齐 public-artifacts.ts:90-104 listMemoryCorePublicArtifacts)
 */
std::vector<MemoryPublicArtifact> list_memory_public_artifacts(
    const std::string& workspace_dir,
    const std::vector<std::string>& agent_ids = {}
);

}  // namespace memory
}  // namespace hunterclaw
