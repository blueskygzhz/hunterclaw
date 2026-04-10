#pragma once

#include <string>
#include <vector>
#include <set>
#include <optional>

// 严格对齐 OpenClaw src/memory/manager-status-state.ts (105行)

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// ============================================================================
// StatusAggregateResult (对齐 manager-status-state.ts:65-73)
// ============================================================================
struct SourceCount {
    std::string source;
    int files = 0;
    int chunks = 0;
};

struct StatusAggregateResult {
    int files = 0;
    int chunks = 0;
    std::vector<SourceCount> source_counts;
};

// ============================================================================
// resolveInitialMemoryDirty (对齐 manager-status-state.ts:26-32)
// ============================================================================
bool resolve_initial_memory_dirty(bool has_memory_source, bool status_only, bool has_indexed_meta);

// ============================================================================
// StatusProviderInfo (对齐 manager-status-state.ts:34-63)
// ============================================================================
struct StatusProviderInfo {
    std::string provider;
    std::optional<std::string> model;
    std::string search_mode;  // "hybrid" | "fts-only"
};

struct ResolveStatusProviderParams {
    std::optional<std::string> provider_id;
    std::optional<std::string> provider_model;
    bool provider_initialized = false;
    std::string requested_provider;
    std::optional<std::string> configured_model;
};

StatusProviderInfo resolve_status_provider_info(const ResolveStatusProviderParams& params);

// ============================================================================
// collectMemoryStatusAggregate (对齐 manager-status-state.ts:65-104)
// ============================================================================
struct CollectStatusAggregateParams {
    SQLiteVectorStorage* db = nullptr;
    std::set<std::string> sources;
    std::string source_filter_sql;
    std::vector<std::string> source_filter_params;
};

StatusAggregateResult collect_memory_status_aggregate(const CollectStatusAggregateParams& params);

// ============================================================================
// SQL constant (对齐 manager-status-state.ts:21-24)
// ============================================================================
extern const char* MEMORY_STATUS_AGGREGATE_SQL;

} // namespace memory
} // namespace hunterclaw
