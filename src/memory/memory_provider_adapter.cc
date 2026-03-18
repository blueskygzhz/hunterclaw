#include "hunterclaw/memory/memory_provider_adapter.h"
#include "hunterclaw/memory/types.h"
#include <chrono>
#include <stdexcept>

namespace hunterclaw {
namespace memory {

MemoryProviderAdapter::MemoryProviderAdapter(
    std::shared_ptr<MemoryManager> memory_mgr
) : memory_mgr_(memory_mgr) {
    if (!memory_mgr_) {
        throw std::invalid_argument("MemoryManager cannot be null");
    }
}

// ============================================================================
// MemoryProvider interface implementation
// ============================================================================

core::Result<std::string> MemoryProviderAdapter::add_memory(
    const std::string& content,
    const std::map<std::string, std::string>& metadata
) {
    try {
        MemoryEntry entry;
        entry.id = "";  // Will be generated
        entry.content = content;
        entry.file_path = metadata.count("file_path") ? metadata.at("file_path") : "";
        entry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        auto result = memory_mgr_->add_entry(entry);
        if (!result.is_ok()) {
            return core::Result<std::string>::Err(result.error);
        }
        return core::Result<std::string>::Ok(entry.id);
    } catch (const std::exception& e) {
        return core::Result<std::string>::Err(std::string("add_memory failed: ") + e.what());
    }
}

core::Result<std::vector<MemorySearchResult>> MemoryProviderAdapter::search_memories(
    const std::string& query,
    const SearchOptions& options
) {
    try {
        auto result = memory_mgr_->search_memories(query, options);
        if (!result.is_ok()) {
            return core::Result<std::vector<MemorySearchResult>>::Err(result.error);
        }

        // Convert vector<Memory> to vector<MemorySearchResult>
        // Memory has: id, content, metadata, created_at, updated_at, embedding, score
        // MemorySearchResult has: path, start_line, end_line, score, snippet, source, citation
        std::vector<MemorySearchResult> search_results;
        search_results.reserve(result.get().size());
        for (const auto& mem : result.get()) {
            MemorySearchResult sr;
            // Map Memory fields to MemorySearchResult fields
            sr.path = mem.metadata.count("file_path") ? mem.metadata.at("file_path") : mem.id;
            sr.start_line = 0;
            sr.end_line = 0;
            sr.score = mem.score;
            sr.snippet = mem.content;
            sr.source = MemorySource::Memory;
            search_results.push_back(sr);
        }
        return core::Result<std::vector<MemorySearchResult>>::Ok(search_results);
    } catch (const std::exception& e) {
        return core::Result<std::vector<MemorySearchResult>>::Err(
            std::string("search_memories failed: ") + e.what()
        );
    }
}

core::Result<bool> MemoryProviderAdapter::remove_memory(const std::string& /*id*/) {
    // MemoryManager does not support direct entry removal
    // This would require direct access to the underlying MemoryProvider
    return core::Result<bool>::Err("remove_memory not supported via MemoryManager adapter");
}

core::Result<bool> MemoryProviderAdapter::update_memory(
    const std::string& /*id*/,
    const std::string& /*content*/,
    const std::map<std::string, std::string>& /*metadata*/
) {
    // MemoryManager does not support direct entry update
    return core::Result<bool>::Err("update_memory not supported via MemoryManager adapter");
}

core::Result<bool> MemoryProviderAdapter::clear_memories() {
    // MemoryManager does not support clear_all directly
    // Use reindex with empty workspace to effectively clear
    try {
        auto result = memory_mgr_->reindex("");
        if (!result.is_ok()) {
            return core::Result<bool>::Err(result.error);
        }
        return core::Result<bool>::Ok(true);
    } catch (const std::exception& e) {
        return core::Result<bool>::Err(std::string("clear_memories failed: ") + e.what());
    }
}

core::Result<Memory> MemoryProviderAdapter::get_memory(const std::string& id) {
    // MemoryManager does not support get by ID directly
    // Try to search by ID as a workaround
    try {
        SearchOptions opts;
        opts.max_results = 1;
        auto result = memory_mgr_->search_memories(id, opts);
        if (!result.is_ok()) {
            return core::Result<Memory>::Err(result.error);
        }
        const auto& memories = result.get();
        if (memories.empty()) {
            return core::Result<Memory>::Err("Memory not found: " + id);
        }
        const auto& mem = memories[0];
        Memory m;
        m.id = mem.id;
        m.content = mem.content;
        m.metadata = mem.metadata;
        m.created_at = mem.created_at;
        m.updated_at = mem.updated_at;
        m.score = mem.score;
        return core::Result<Memory>::Ok(m);
    } catch (const std::exception& e) {
        return core::Result<Memory>::Err(std::string("get_memory failed: ") + e.what());
    }
}

core::Result<MemoryStats> MemoryProviderAdapter::get_stats() {
    MemoryStats stats;
    stats.total_memories = 0;
    stats.total_size = 0;
    return core::Result<MemoryStats>::Ok(stats);
}

// ============================================================================
// Legacy convenience methods
// ============================================================================

core::Result<std::vector<MemoryEntry>> MemoryProviderAdapter::search(
    const std::string& query,
    int limit
) {
    try {
        SearchOptions opts;
        opts.max_results = limit;
        opts.min_score = 0.0;

        auto result = memory_mgr_->search_memories(query, opts);
        if (!result.is_ok()) {
            return core::Result<std::vector<MemoryEntry>>::Err(result.error);
        }

        std::vector<MemoryEntry> entries;
        entries.reserve(result.get().size());
        for (const auto& memory : result.get()) {
            MemoryEntry entry;
            entry.id = memory.id;
            entry.content = memory.content;
            entry.file_path = memory.metadata.count("file_path")
                ? memory.metadata.at("file_path") : "";
            entry.timestamp = memory.created_at;
            entries.push_back(entry);
        }
        return core::Result<std::vector<MemoryEntry>>::Ok(entries);
    } catch (const std::exception& e) {
        return core::Result<std::vector<MemoryEntry>>::Err(
            std::string("Search failed: ") + e.what()
        );
    }
}

core::Result<void> MemoryProviderAdapter::add_memory_entry(const MemoryEntry& entry) {
    try {
        return memory_mgr_->add_entry(entry);
    } catch (const std::exception& e) {
        return core::Result<void>::Err(std::string("Add memory failed: ") + e.what());
    }
}

core::Result<void> MemoryProviderAdapter::reindex() {
    try {
        return memory_mgr_->reindex();
    } catch (const std::exception& e) {
        return core::Result<void>::Err(std::string("Reindex failed: ") + e.what());
    }
}

} // namespace memory
} // namespace hunterclaw
