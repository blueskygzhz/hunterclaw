#include "hunterclaw/memory/public_artifacts.h"
#include "hunterclaw/memory/events.h"
#include <filesystem>
#include <algorithm>
#include <set>

// 严格对齐 OpenClaw extensions/memory-core/src/public-artifacts.ts (105 行)

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

// (对齐 public-artifacts.ts:8-15)
static bool path_exists(const std::string& input_path) {
    try {
        return fs::exists(input_path);
    } catch (...) {
        return false;
    }
}

// (对齐 public-artifacts.ts:17-31)
static std::vector<std::string> list_markdown_files_recursive(const std::string& root_dir) {
    std::vector<std::string> files;
    try {
        for (auto& entry : fs::recursive_directory_iterator(root_dir,
            fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".md") {
                    files.push_back(entry.path().string());
                }
            }
        }
    } catch (...) {}
    std::sort(files.begin(), files.end());
    return files;
}

// (对齐 public-artifacts.ts:90-104)
std::vector<MemoryPublicArtifact> list_memory_public_artifacts(
    const std::string& workspace_dir,
    const std::vector<std::string>& agent_ids
) {
    std::vector<MemoryPublicArtifact> artifacts;

    // (对齐 public-artifacts.ts:43-56) Check MEMORY.md / memory.md
    for (const auto& rel : {"MEMORY.md", "memory.md"}) {
        std::string abs_path = (fs::path(workspace_dir) / rel).string();
        if (path_exists(abs_path)) {
            artifacts.push_back({
                "memory-root",
                workspace_dir,
                rel,
                abs_path,
                agent_ids,
                "markdown"
            });
            break;  // Only include the first found
        }
    }

    // (对齐 public-artifacts.ts:58-69) Enumerate memory/*.md
    std::string memory_dir = (fs::path(workspace_dir) / "memory").string();
    for (const auto& abs_path : list_markdown_files_recursive(memory_dir)) {
        std::string rel_path = fs::relative(fs::path(abs_path), fs::path(workspace_dir)).string();
        std::replace(rel_path.begin(), rel_path.end(), '\\', '/');

        std::string kind = "daily-note";
        if (rel_path.find("memory/dreaming/") == 0 || rel_path.find("memory/.dreams/") == 0) {
            kind = "dream-report";
        }
        artifacts.push_back({kind, workspace_dir, rel_path, abs_path, agent_ids, "markdown"});
    }

    // (对齐 public-artifacts.ts:71-81) Event log
    std::string event_log = (fs::path(workspace_dir) / MEMORY_HOST_EVENT_LOG_RELATIVE_PATH).string();
    if (path_exists(event_log)) {
        std::string rel_path = fs::relative(fs::path(event_log), fs::path(workspace_dir)).string();
        std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
        artifacts.push_back({"event-log", workspace_dir, rel_path, event_log, agent_ids, "json"});
    }

    // (对齐 public-artifacts.ts:83-87) Deduplicate
    std::set<std::string> seen;
    std::vector<MemoryPublicArtifact> deduped;
    for (auto& a : artifacts) {
        std::string key = a.workspace_dir + "\0" + a.relative_path + "\0" + a.kind;
        if (!seen.count(key)) {
            seen.insert(key);
            deduped.push_back(std::move(a));
        }
    }
    return deduped;
}

}  // namespace memory
}  // namespace hunterclaw
