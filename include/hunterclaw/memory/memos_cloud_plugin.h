#pragma once

// ============================================================================
// MemOS Cloud Memory Plugin
//
// OpenClaw alignment:
//   This is a third-party memory plugin (kind="memory") that provides a
//   MemorySearchManager implementation backed by MemOS Cloud API.
//
//   In OpenClaw, memory plugins are OpenClawPluginDefinition objects with
//   kind="memory". They register a MemorySearchManager via the plugin API.
//
//   Reference: https://github.com/MemTensor/MemOS-Cloud-OpenClaw-Plugin
//
// There is NO MemoryProvider abstract class in OpenClaw.
// The contract is MemorySearchManager (types.ts / types.h).
// ============================================================================

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/plugin.h"
#include "hunterclaw/core/result.h"
#include <memory>
#include <string>
#include <map>

namespace hunterclaw {
namespace memory {

// ============================================================================
// MemOSCloudManager
//
// Implements MemorySearchManager backed by MemOS Cloud API.
// This is the C++ equivalent of the MemOS Cloud OpenClaw plugin's manager.
//
// Configuration (plugin_config keys):
//   "api_key"      - MemOS Cloud API Key (required)
//   "workspace_id" - Workspace ID (required)
//   "base_url"     - API Base URL (default: "https://api.memos.cloud")
// ============================================================================
class MemOSCloudManager : public MemorySearchManager {
public:
    MemOSCloudManager(
        const std::string& api_key,
        const std::string& workspace_id,
        const std::string& base_url = "https://api.memos.cloud"
    );

    ~MemOSCloudManager() override;

    // MemorySearchManager interface (对齐 types.ts:61-81)
    std::vector<MemorySearchResult> search(
        const std::string& query,
        const SearchOptions& opts = {}
    ) override;

    ReadFileResult read_file(const ReadFileParams& params) override;

    MemoryProviderStatus status() override;

    void sync(const SyncParams& params = {}) override;

    MemoryEmbeddingProbeResult probe_embedding_availability() override;

    bool probe_vector_availability() override;

    void close() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// MemOS Cloud Plugin Definition
//
// Register this plugin via MemoryPluginRegistry to make MemOS Cloud available
// as a memory backend.
//
// Usage:
//   MemoryPluginRegistry::instance().register_plugin(
//       make_memos_cloud_plugin_definition()
//   );
// ============================================================================
MemoryPluginDefinition make_memos_cloud_plugin_definition();

} // namespace memory
} // namespace hunterclaw
