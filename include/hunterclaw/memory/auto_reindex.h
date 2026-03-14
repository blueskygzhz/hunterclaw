#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <map>
#include <memory>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Auto-Reindex Detection (OpenClaw-compatible)
// ============================================================================

struct MemoryProviderInfo {
    std::string provider;          // "openai" | "gemini" | "mistral" | "local"
    std::string model;             // "text-embedding-3-small" | "gemini-embedding-2-preview"
    int output_dimensions = -1;    // Gemini 特殊支持
    std::string endpoint;          // Custom endpoint URL
};

struct ReindexReason {
    enum Type {
        None,
        ProviderChanged,
        ModelChanged,
        DimensionsChanged,
        EndpointChanged
    };
    
    Type type = None;
    std::string old_value;
    std::string new_value;
    
    std::string to_string() const {
        switch (type) {
            case ProviderChanged: return "Provider changed: " + old_value + " → " + new_value;
            case ModelChanged: return "Model changed: " + old_value + " → " + new_value;
            case DimensionsChanged: return "Dimensions changed: " + old_value + " → " + new_value;
            case EndpointChanged: return "Endpoint changed: " + old_value + " → " + new_value;
            default: return "No reindex needed";
        }
    }
};

// Auto-reindex detector
class AutoReindexDetector {
public:
    AutoReindexDetector(const std::string& state_file);
    
    // Check if reindex is needed
    core::Result<ReindexReason> check_reindex_needed(const MemoryProviderInfo& current_info);
    
    // Save current state
    core::Result<bool> save_state(const MemoryProviderInfo& info);
    
    // Load saved state
    core::Result<MemoryProviderInfo> load_state();
    
    // Clear state (after successful reindex)
    core::Result<bool> clear_state();
    
private:
    std::string state_file_;
    
    // Compare two provider infos
    ReindexReason compare_provider_info(
        const MemoryProviderInfo& old_info,
        const MemoryProviderInfo& new_info
    );
};

// Memory status (matches OpenClaw)
struct MemoryStatus {
    std::string backend = "builtin";  // "builtin" | "external"
    int files_count = 0;
    int chunks_count = 0;
    std::string provider = "none";
    std::string model;
    int output_dimensions = -1;
    std::string requested_provider;
    bool reindex_in_progress = false;
};

// Memory manager with auto-reindex
class MemoryManagerWithAutoReindex {
public:
    MemoryManagerWithAutoReindex(
        const std::string& data_dir,
        std::shared_ptr<AutoReindexDetector> detector
    );
    
    // Initialize memory (check reindex)
    core::Result<bool> initialize(const MemoryProviderInfo& provider_info);
    
    // Get status
    MemoryStatus status() const;
    
    // Trigger reindex
    core::Result<bool> reindex(bool force = false);
    
    // Sync (incremental index)
    core::Result<bool> sync(const std::string& reason = "manual");
    
private:
    std::string data_dir_;
    std::shared_ptr<AutoReindexDetector> detector_;
    MemoryStatus current_status_;
    MemoryProviderInfo current_provider_;
    
    bool needs_reindex(const MemoryProviderInfo& new_provider);
    core::Result<bool> do_full_reindex();
    core::Result<bool> do_incremental_sync();
};

// ============================================================================
// Gemini Dimensions Support (特殊处理)
// ============================================================================

namespace gemini_support {

// Gemini 支持的输出维度
constexpr int VALID_DIMENSIONS[] = {256, 512, 768, 1024, 1536, 2048};

// Check if dimensions are valid for Gemini
inline bool is_valid_dimensions(int dims) {
    for (int valid : VALID_DIMENSIONS) {
        if (dims == valid) return true;
    }
    return false;
}

// Get default dimensions for model
inline int get_default_dimensions(const std::string& model) {
    if (model.find("gemini-embedding") != std::string::npos) {
        return 768;  // Gemini default
    }
    return -1;  // Other providers use model's native dimensions
}

}  // namespace gemini_support

}  // namespace memory
}  // namespace hunterclaw
