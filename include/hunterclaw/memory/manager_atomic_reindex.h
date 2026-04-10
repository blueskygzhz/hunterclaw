#pragma once

// ============================================================================
// Manager Atomic Reindex — 严格对齐 OpenClaw manager-atomic-reindex.ts (50 行)
//
// 原子性重建索引: move/swap/remove index files。
// ============================================================================

#include <string>
#include <functional>

namespace hunterclaw {
namespace memory {

/**
 * Move memory index files (db, wal, shm).
 * (对齐 manager-atomic-reindex.ts:4-17 moveMemoryIndexFiles)
 */
void move_memory_index_files(const std::string& source_base, const std::string& target_base);

/**
 * Remove memory index files (db, wal, shm).
 * (对齐 manager-atomic-reindex.ts:19-22 removeMemoryIndexFiles)
 */
void remove_memory_index_files(const std::string& base_path);

/**
 * Atomically swap memory index files.
 * (对齐 manager-atomic-reindex.ts:24-33 swapMemoryIndexFiles)
 */
void swap_memory_index_files(const std::string& target_path, const std::string& temp_path);

/**
 * Run an atomic reindex: build in temp, then swap.
 * (对齐 manager-atomic-reindex.ts:36-49 runMemoryAtomicReindex)
 */
void run_memory_atomic_reindex(
    const std::string& target_path,
    const std::string& temp_path,
    std::function<void()> build
);

}  // namespace memory
}  // namespace hunterclaw
