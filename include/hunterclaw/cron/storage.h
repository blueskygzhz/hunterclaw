#pragma once

#include <string>
#include <vector>
#include <optional>
#include "hunterclaw/cron/types.h"

namespace hunterclaw {
namespace cron {

// Forward declarations
class CronStorage {
public:
    CronStorage(const std::string& base_dir);
    ~CronStorage();
    
    // Storage operations
    bool save(const std::string& id, const CronJob& job);
    std::optional<CronJob> load(const std::string& id);
    bool remove(const std::string& id);
    std::vector<CronJob> list_all();
    
    // Backup operations
    bool backup();
    bool restore(const std::string& backup_name);
    std::vector<std::string> list_backups();
    bool cleanup_old_backups(int keep_count = 10);

private:
    std::string base_dir_;
};

} // namespace cron
} // namespace hunterclaw
