#include "hunterclaw/core/supervisor_registry.h"
#include <algorithm>
#include <chrono>

namespace hunterclaw {

SupervisorRegistry::SupervisorRegistry(const RegistryConfig& config)
    : config_(config) {}

core::Result<void> SupervisorRegistry::Add(const core::RunRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  // 检查是否已存在
  if (records_.find(record.run_id) != records_.end()) {
    return core::Result<void>::Err("RunRecord with run_id '" + record.run_id + "' already exists");
  }
  
  // 添加记录
  records_[record.run_id] = record;
  
  // 检查是否需要自动修剪
  CheckAutoPrune();
  
  return core::Result<void>::Ok();
}

core::Result<core::RunRecord> SupervisorRegistry::Update(
    const std::string& run_id,
    std::function<void(core::RunRecord&)> updater) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return core::Result<core::RunRecord>::Err("RunRecord with run_id '" + run_id + "' not found");
  }
  
  // 执行更新
  updater(it->second);
  
  return core::Result<core::RunRecord>::Ok(it->second);
}

core::Result<void> SupervisorRegistry::Remove(const std::string& run_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return core::Result<void>::Err("RunRecord with run_id '" + run_id + "' not found");
  }
  
  records_.erase(it);
  return core::Result<void>::Ok();
}

std::optional<core::RunRecord> SupervisorRegistry::Get(const std::string& run_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  auto it = records_.find(run_id);
  if (it == records_.end()) {
    return std::nullopt;
  }
  
  return it->second;
}

std::vector<core::RunRecord> SupervisorRegistry::Query(const QueryOptions& options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  std::vector<core::RunRecord> results;
  results.reserve(records_.size());
  
  // 收集符合条件的记录
  for (const auto& [id, record] : records_) {
    // 按 scope_key 过滤
    if (options.scope_key && record.scope_key != *options.scope_key) {
      continue;
    }
    
    // 按 run_id 过滤
    if (options.run_id && record.run_id != *options.run_id) {
      continue;
    }
    
    // 按状态过滤
    if (options.state && record.state != *options.state) {
      continue;
    }
    
    // 按活跃状态过滤
    if (options.active_only && !IsActive(record)) {
      continue;
    }
    
    results.push_back(record);
  }
  
  // 排序(按创建时间)
  std::sort(results.begin(), results.end(), 
      [ascending = options.ascending](const core::RunRecord& a, const core::RunRecord& b) {
        return ascending ? (a.started_at_ms < b.started_at_ms) 
                        : (a.started_at_ms > b.started_at_ms);
      });
  
  // 限制返回数量
  if (options.limit && results.size() > *options.limit) {
    results.resize(*options.limit);
  }
  
  return results;
}

std::vector<core::RunRecord> SupervisorRegistry::GetAll() const {
  return Query();
}

std::vector<core::RunRecord> SupervisorRegistry::GetActive() const {
  QueryOptions options;
  options.active_only = true;
  return Query(options);
}

std::vector<core::RunRecord> SupervisorRegistry::GetByScope(
    const std::string& scope_key) const {
  QueryOptions options;
  options.scope_key = scope_key;
  return Query(options);
}

std::vector<core::RunRecord> SupervisorRegistry::GetActiveByScope(
    const std::string& scope_key) const {
  QueryOptions options;
  options.scope_key = scope_key;
  options.active_only = true;
  return Query(options);
}

size_t SupervisorRegistry::Prune() {
  std::lock_guard<std::mutex> lock(mutex_);
  
  // 如果记录数未超过上限,无需修剪
  if (records_.size() <= config_.max_records) {
    return 0;
  }
  
  // 收集所有记录并按时间排序
  std::vector<std::pair<std::string, int64_t>> records_with_time;
  records_with_time.reserve(records_.size());
  
  for (const auto& [id, record] : records_) {
    records_with_time.push_back(std::make_pair(id, record.started_at_ms));
  }
  
  // 按时间降序排序(最新的在前)
  std::sort(records_with_time.begin(), records_with_time.end(),
      [](const auto& a, const auto& b) {
        return a.second > b.second;
      });
  
  // 删除最旧的记录
  size_t to_remove = records_with_time.size() - config_.max_records;
  size_t removed = 0;
  
  for (size_t i = config_.max_records; i < records_with_time.size(); ++i) {
    records_.erase(records_with_time[i].first);
    ++removed;
  }
  
  return removed;
}

void SupervisorRegistry::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  records_.clear();
}

size_t SupervisorRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

size_t SupervisorRegistry::ActiveCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  size_t count = 0;
  for (const auto& [_, record] : records_) {
    if (IsActive(record)) {
      ++count;
    }
  }
  
  return count;
}

void SupervisorRegistry::CheckAutoPrune() {
  // 注意: 调用者必须持有锁
  if (!config_.auto_prune) {
    return;
  }
  
  if (records_.size() > config_.prune_threshold) {
    // 临时释放锁并调用 Prune
    // 注意: Prune 会重新获取锁
    mutex_.unlock();
    Prune();
    mutex_.lock();
  }
}

bool SupervisorRegistry::IsActive(const core::RunRecord& record) {
  return record.state == core::RunState::Starting ||
         record.state == core::RunState::Running ||
         record.state == core::RunState::Exiting;
}

// 全局注册表单例
SupervisorRegistry& GetGlobalRegistry() {
  static SupervisorRegistry registry;
  return registry;
}

}  // namespace hunterclaw
