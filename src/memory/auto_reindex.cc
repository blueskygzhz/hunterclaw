#include "hunterclaw/memory/auto_reindex.h"
#include "hunterclaw/core/json.h"
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// ============================================================================
// AutoReindexDetector Implementation
// ============================================================================

AutoReindexDetector::AutoReindexDetector(const std::string& state_file)
    : state_file_(state_file) {}

core::Result<ReindexReason> AutoReindexDetector::check_reindex_needed(
    const MemoryProviderInfo& current_info) {
    
    auto load_result = load_state();
    if (!load_result.is_ok()) {
        // No saved state - first time initialization
        return core::Result<ReindexReason>::Ok(ReindexReason{ReindexReason::None, "", ""});
    }
    
    const auto& old_info = load_result.value;
    ReindexReason reason = compare_provider_info(old_info, current_info);
    
    return core::Result<ReindexReason>::Ok(reason);
}

core::Result<bool> AutoReindexDetector::save_state(const MemoryProviderInfo& info) {
    try {
        json state = {
            {"provider", info.provider},
            {"model", info.model},
            {"output_dimensions", info.output_dimensions},
            {"endpoint", info.endpoint}
        };
        
        std::ofstream file(state_file_);
        if (!file) {
            return core::Result<bool>::Err("Failed to open state file for writing");
        }
        
        file << state.dump(2);
        file.close();
        
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        return core::Result<bool>::Err(
            std::string("Failed to save state: ") + e.what()
        );
    }
}

core::Result<MemoryProviderInfo> AutoReindexDetector::load_state() {
    try {
        std::ifstream file(state_file_);
        if (!file) {
            return core::Result<MemoryProviderInfo>::Err("State file not found");
        }
        
        json state;
        file >> state;
        
        MemoryProviderInfo info;
        info.provider = state.value("provider", "");
        info.model = state.value("model", "");
        info.output_dimensions = state.value("output_dimensions", -1);
        info.endpoint = state.value("endpoint", "");
        
        return core::Result<MemoryProviderInfo>::Ok(info);
        
    } catch (const std::exception& e) {
        return core::Result<MemoryProviderInfo>::Err(
            std::string("Failed to load state: ") + e.what()
        );
    }
}

core::Result<bool> AutoReindexDetector::clear_state() {
    if (std::remove(state_file_.c_str()) != 0) {
        return core::Result<bool>::Err("Failed to delete state file");
    }
    return core::Result<bool>::Ok(true);
}

ReindexReason AutoReindexDetector::compare_provider_info(
    const MemoryProviderInfo& old_info,
    const MemoryProviderInfo& new_info) {
    
    ReindexReason reason;
    
    // Check provider change (highest priority)
    if (old_info.provider != new_info.provider) {
        reason.type = ReindexReason::ProviderChanged;
        reason.old_value = old_info.provider;
        reason.new_value = new_info.provider;
        return reason;
    }
    
    // Check model change
    if (old_info.model != new_info.model) {
        reason.type = ReindexReason::ModelChanged;
        reason.old_value = old_info.model;
        reason.new_value = new_info.model;
        return reason;
    }
    
    // Check dimensions change (Gemini specific)
    if (old_info.output_dimensions != new_info.output_dimensions) {
        // Only trigger reindex if dimensions are actually set (not default -1)
        if (new_info.output_dimensions > 0) {
            reason.type = ReindexReason::DimensionsChanged;
            reason.old_value = std::to_string(old_info.output_dimensions);
            reason.new_value = std::to_string(new_info.output_dimensions);
            return reason;
        }
    }
    
    // Check endpoint change
    if (old_info.endpoint != new_info.endpoint) {
        reason.type = ReindexReason::EndpointChanged;
        reason.old_value = old_info.endpoint;
        reason.new_value = new_info.endpoint;
        return reason;
    }
    
    // No changes
    reason.type = ReindexReason::None;
    return reason;
}

// ============================================================================
// MemoryManagerWithAutoReindex Implementation
// ============================================================================

MemoryManagerWithAutoReindex::MemoryManagerWithAutoReindex(
    const std::string& data_dir,
    std::shared_ptr<AutoReindexDetector> detector)
    : data_dir_(data_dir), detector_(detector) {}

core::Result<bool> MemoryManagerWithAutoReindex::initialize(
    const MemoryProviderInfo& provider_info) {
    
    current_provider_ = provider_info;
    
    // Check if reindex is needed
    auto reindex_result = detector_->check_reindex_needed(provider_info);
    if (!reindex_result.is_ok()) {
        return core::Result<bool>::Err(reindex_result.error);
    }
    
    const auto& reason = reindex_result.value;
    
    if (reason.type != ReindexReason::None) {
        // Log reindex reason
        // TODO: Use proper logging
        // Trigger automatic reindex
        auto reindex_success = do_full_reindex();
        if (!reindex_success.is_ok()) {
            return core::Result<bool>::Err(
                "Auto-reindex failed: " + reindex_success.error
            );
        }
    }
    
    // Save current state
    auto save_result = detector_->save_state(provider_info);
    if (!save_result.is_ok()) {
        return core::Result<bool>::Err(
            "Failed to save provider state: " + save_result.error
        );
    }
    
    // Update status
    current_status_.provider = provider_info.provider;
    current_status_.model = provider_info.model;
    current_status_.output_dimensions = provider_info.output_dimensions;
    
    return core::Result<bool>::Ok(true);
}

MemoryStatus MemoryManagerWithAutoReindex::status() const {
    return current_status_;
}

core::Result<bool> MemoryManagerWithAutoReindex::reindex(bool force) {
    if (!force && current_status_.reindex_in_progress) {
        return core::Result<bool>::Err("Reindex already in progress");
    }
    
    current_status_.reindex_in_progress = true;
    
    auto result = do_full_reindex();
    
    current_status_.reindex_in_progress = false;
    
    return result;
}

core::Result<bool> MemoryManagerWithAutoReindex::sync(const std::string& reason) {
    // Incremental sync (对齐 OpenClaw manager-sync-ops.ts)
    return do_incremental_sync();
}

bool MemoryManagerWithAutoReindex::needs_reindex(const MemoryProviderInfo& new_provider) {
    auto result = detector_->check_reindex_needed(new_provider);
    if (!result.is_ok()) {
        return false;
    }
    return result.value.type != ReindexReason::None;
}

core::Result<bool> MemoryManagerWithAutoReindex::do_full_reindex() {
    // TODO: Implement full reindex logic
    // 1. Clear existing vectors
    // 2. Rescan all files
    // 3. Re-chunk and re-embed
    // 4. Update database
    
    // Placeholder implementation
    current_status_.reindex_in_progress = true;
    
    // Simulate reindex
    // In real implementation, this would:
    // - Call file scanner
    // - Generate chunks
    // - Call embedding provider
    // - Update SQLite storage
    
    current_status_.reindex_in_progress = false;
    
    return core::Result<bool>::Ok(true);
}

core::Result<bool> MemoryManagerWithAutoReindex::do_incremental_sync() {
    // TODO: Implement incremental sync
    // 1. Detect changed files (mtime check)
    // 2. Re-chunk only changed files
    // 3. Update embeddings
    // 4. Update database
    
    return core::Result<bool>::Ok(true);
}

}  // namespace memory
}  // namespace hunterclaw
