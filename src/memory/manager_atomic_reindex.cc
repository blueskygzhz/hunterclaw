#include "hunterclaw/memory/manager_atomic_reindex.h"
#include <filesystem>
#include <random>
#include <sstream>

// 严格对齐 OpenClaw memory/manager-atomic-reindex.ts (50 行)

namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(gen) << "-" << dist(gen);
    return oss.str();
}

// 对齐 manager-atomic-reindex.ts:4-17
void move_memory_index_files(const std::string& source_base, const std::string& target_base) {
    const std::string suffixes[] = {"", "-wal", "-shm"};
    for (const auto& suffix : suffixes) {
        auto source = source_base + suffix;
        auto target = target_base + suffix;
        std::error_code ec;
        fs::rename(source, target, ec);
        // Ignore ENOENT
    }
}

// 对齐 manager-atomic-reindex.ts:19-22
void remove_memory_index_files(const std::string& base_path) {
    const std::string suffixes[] = {"", "-wal", "-shm"};
    for (const auto& suffix : suffixes) {
        std::error_code ec;
        fs::remove(base_path + suffix, ec);
    }
}

// 对齐 manager-atomic-reindex.ts:24-33
void swap_memory_index_files(const std::string& target_path, const std::string& temp_path) {
    std::string backup_path = target_path + ".backup-" + generate_uuid();
    move_memory_index_files(target_path, backup_path);
    try {
        move_memory_index_files(temp_path, target_path);
    } catch (...) {
        move_memory_index_files(backup_path, target_path);
        throw;
    }
    remove_memory_index_files(backup_path);
}

// 对齐 manager-atomic-reindex.ts:36-49
void run_memory_atomic_reindex(
    const std::string& target_path,
    const std::string& temp_path,
    std::function<void()> build
) {
    try {
        build();
        swap_memory_index_files(target_path, temp_path);
    } catch (...) {
        remove_memory_index_files(temp_path);
        throw;
    }
}

}  // namespace memory
}  // namespace hunterclaw
