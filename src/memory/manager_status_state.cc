#include "hunterclaw/memory/manager_status_state.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include <algorithm>

namespace hunterclaw {
namespace memory {

// ============================================================================
// SQL constant (对齐 manager-status-state.ts:21-24)
// ============================================================================
const char* MEMORY_STATUS_AGGREGATE_SQL =
    "SELECT 'files' AS kind, source, COUNT(*) as c FROM files WHERE 1=1__FILTER__ GROUP BY source "
    "UNION ALL "
    "SELECT 'chunks' AS kind, source, COUNT(*) as c FROM chunks WHERE 1=1__FILTER__ GROUP BY source";

// ============================================================================
// resolveInitialMemoryDirty (对齐 manager-status-state.ts:26-32)
// ============================================================================
bool resolve_initial_memory_dirty(bool has_memory_source, bool status_only, bool has_indexed_meta) {
    return has_memory_source && (status_only ? !has_indexed_meta : true);
}

// ============================================================================
// resolveStatusProviderInfo (对齐 manager-status-state.ts:34-63)
// ============================================================================
StatusProviderInfo resolve_status_provider_info(const ResolveStatusProviderParams& params) {
    StatusProviderInfo info;

    if (params.provider_id) {
        info.provider = *params.provider_id;
        info.model = params.provider_model;
        info.search_mode = "hybrid";
        return info;
    }

    if (params.provider_initialized) {
        info.provider = "none";
        info.search_mode = "fts-only";
        return info;
    }

    info.provider = params.requested_provider;
    info.model = params.configured_model;
    info.search_mode = "hybrid";
    return info;
}

// ============================================================================
// collectMemoryStatusAggregate (对齐 manager-status-state.ts:65-104)
// ============================================================================
StatusAggregateResult collect_memory_status_aggregate(const CollectStatusAggregateParams& params) {
    StatusAggregateResult result;
    if (!params.db) return result;

    // Initialize source counts
    std::map<std::string, SourceCount> by_source;
    for (const auto& source : params.sources) {
        by_source[source] = {source, 0, 0};
    }

    // Build SQL by replacing __FILTER__
    std::string sql = MEMORY_STATUS_AGGREGATE_SQL;
    std::string filter = params.source_filter_sql;
    size_t pos;
    while ((pos = sql.find("__FILTER__")) != std::string::npos) {
        sql.replace(pos, 10, filter);
    }

    auto stmt = params.db->prepare_statement(sql);
    if (!stmt.is_ok()) return result;

    // Bind source filter params twice (for files + chunks union)
    int idx = 1;
    for (const auto& p : params.source_filter_params) {
        sqlite3_bind_text(stmt.get(), idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& p : params.source_filter_params) {
        sqlite3_bind_text(stmt.get(), idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        std::string kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        std::string source = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        int count = sqlite3_column_int(stmt.get(), 2);

        auto& entry = by_source[source];
        entry.source = source;
        if (kind == "files") {
            entry.files = count;
            result.files += count;
        } else {
            entry.chunks = count;
            result.chunks += count;
        }
    }

    for (const auto& src : params.sources) {
        result.source_counts.push_back(by_source[src]);
    }
    return result;
}

} // namespace memory
} // namespace hunterclaw
