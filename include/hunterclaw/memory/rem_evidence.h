#pragma once

// ============================================================================
// REM Evidence — 严格对齐 OpenClaw rem-evidence.ts (1078 行)
//
// REM (Rapid Eye Movement) 阶段证据评估系统。
// 分析 Markdown 记忆文件，提取事实、反思、候选记忆和持久化建议。
//
// 核心 API:
//   - previewGroundedRemForFile({relPath, content}) -> GroundedRemFilePreview
//   - previewGroundedRemMarkdown({workspaceDir, inputPaths}) -> GroundedRemPreviewResult
//   - scoreSection / scoreSnippet / scoreCandidateSnippet (内部评分函数)
//
// 信号维度 (对齐 rem-evidence.ts:4-57):
//   - Memory / Persistence / Explicit Preference
//   - Build / Incident / Logistics / Routing
//   - Externalization / Retry / Person-Pattern
//   - Monitoring / Transient / Situational / Metadata
// ============================================================================

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Types (对齐 rem-evidence.ts:66-104)
// ============================================================================

struct GroundedRemPreviewItem {
    std::string text;
    std::vector<std::string> refs;
};

struct GroundedRemCandidate {
    std::string text;
    std::vector<std::string> refs;
    std::string lean;  // "likely_durable" | "unclear" | "likely_situational"
};

struct GroundedRemFilePreview {
    std::string path;
    std::vector<GroundedRemPreviewItem> facts;
    std::vector<GroundedRemPreviewItem> reflections;
    std::vector<GroundedRemPreviewItem> memory_implications;
    std::vector<GroundedRemCandidate> candidates;
    std::string rendered_markdown;
};

struct GroundedRemPreviewResult {
    std::string workspace_dir;
    int scanned_files = 0;
    std::vector<GroundedRemFilePreview> files;
};

// ============================================================================
// Core API
// ============================================================================

/**
 * Preview grounded REM evidence for a single markdown file.
 * (对齐 rem-evidence.ts:680-1019 previewGroundedRemForFile)
 */
GroundedRemFilePreview preview_grounded_rem_for_file(
    const std::string& rel_path,
    const std::string& content
);

/**
 * Preview grounded REM evidence across multiple markdown files.
 * (对齐 rem-evidence.ts:1060-1077 previewGroundedRemMarkdown)
 */
GroundedRemPreviewResult preview_grounded_rem_markdown(
    const std::string& workspace_dir,
    const std::vector<std::string>& input_paths
);

}  // namespace memory
}  // namespace hunterclaw
