# Channel Monitor 对齐分析报告

## 📋 概述

本文档分析 HunterClaw Channel Monitor 的实现与 OpenClaw 的对齐情况。

---

## ✅ 当前实现状态

### 1. Channel Health Monitor 基础架构

**OpenClaw 实现位置**: `openclaw-analysis/src/gateway/channel-health-monitor.ts:76-200`

**HunterClaw 实现位置**: `HunterClaw/src/channels/manager.cc:279-465`

#### 核心组件对齐

| 组件 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| **监控配置** | `ChannelHealthMonitorDeps` | `ChannelHealthMonitorConfig` | ✅ 已对齐 |
| **监控启动** | `startChannelHealthMonitor()` | `start_health_monitor()` | ✅ 已对齐 |
| **监控停止** | `monitor.stop()` | `stop_health_monitor()` | ✅ 已对齐 |
| **监控循环** | `setInterval(runCheck, ...)` | `health_check_loop()` | ✅ 已对齐 |

---

## 🔍 详细功能对比

### 2.1 配置参数对齐

#### OpenClaw 配置

```typescript
const deps = {
  checkIntervalMs: 5 * 60_000,              // 5 分钟检查间隔
  cooldownCycles: 2,                         // 2 个冷却周期
  maxRestartsPerHour: 10,                    // 每小时最多重启 10 次
  timing: {
    monitorStartupGraceMs: 60_000,           // 监控器启动宽限期 60 秒
    channelConnectGraceMs: 120_000,          // Channel 连接宽限期 120 秒
    staleEventThresholdMs: 30 * 60_000,      // 事件过期阈值 30 分钟
  }
};
```

#### HunterClaw 配置

```cpp
struct ChannelHealthMonitorConfig {
    int64_t check_interval_ms = 5 * 60 * 1000;  // 5 分钟
    
    struct TimingConfig {
        int64_t monitor_startup_grace_ms = 60 * 1000;    // 60 秒
        int64_t channel_connect_grace_ms = 120 * 1000;   // 120 秒
        int64_t stale_event_threshold_ms = 30 * 60 * 1000;  // 30 分钟
    } timing;
    
    int cooldown_cycles = 2;        // 2 个冷却周期
    int max_restarts_per_hour = 10; // 每小时最大 10 次
};
```

**对齐度**: ✅ **100%** - 所有参数完全一致

---

### 2.2 健康检查流程对齐

#### OpenClaw 流程

```typescript
async function runCheck() {
  const now = Date.now();
  
  // 1. 启动宽限期检查
  if (now - startedAt < timing.monitorStartupGraceMs) {
    return;
  }
  
  // 2. 获取运行时快照
  const snapshot = channelManager.getRuntimeSnapshot();
  
  // 3. 遍历所有 Channel 账号
  for (const [channelId, accounts] of Object.entries(snapshot.channelAccounts)) {
    for (const [accountId, status] of Object.entries(accounts)) {
      // 4. 跳过手动停止
      if (channelManager.isManuallyStopped(channelId, accountId)) {
        continue;
      }
      
      // 5. 评估健康状态
      const health = evaluateChannelHealth(status, { ... });
      if (health.healthy) {
        continue;
      }
      
      // 6. 冷却期检查
      // 7. 重启频率限制
      // 8. 执行重启
      // 9. 记录重启事件
    }
  }
}
```

#### HunterClaw 流程

```cpp
void health_check_loop() {
    auto start_time = core::utils::now();
    int64_t start_time_ms = start_time.time_since_epoch().count() / 1000000;
    
    while (health_monitor_running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(health_config_.check_interval_ms));
        
        auto now = core::utils::now();
        int64_t now_ms = now.time_since_epoch().count() / 1000000;
        
        // 1. 启动宽限期检查
        if (now_ms - start_time_ms < health_config_.timing.monitor_startup_grace_ms) {
            continue;
        }
        
        // 2. 检查所有 Channel
        std::lock_guard<std::mutex> lock(stores_mutex_);
        for (const auto& [channel_id_str, store] : channel_stores_) {
            ChannelId channel_id = channel_id_str.c_str();
            check_channel_health(channel_id);
        }
    }
}
```

**对齐度**: ✅ **90%** - 核心流程一致,但实现细节有差异

---

## ⚠️ 发现的问题

### ❌ 问题 1: 缺少完整的健康评估逻辑

#### OpenClaw 有完整的 `evaluateChannelHealth` 函数:

```typescript
function evaluateChannelHealth(status, policy): ChannelHealth {
  const { channelId, now, staleEventThresholdMs, channelConnectGraceMs } = policy;
  
  // 未启用 → 跳过
  if (!status.enabled) {
    return { healthy: true, reason: 'disabled' };
  }
  
  // 未配置 → 跳过
  if (!status.configured) {
    return { healthy: true, reason: 'not configured' };
  }
  
  // 运行中但未连接 → 需要修复
  if (status.running && !status.connected) {
    const elapsed = now - status.started_at;
    if (elapsed < channelConnectGraceMs) {
      return { healthy: true, reason: 'connecting' };
    }
    return { healthy: false, reason: 'disconnected' };
  }
  
  // 已连接但消息过期 → 需要修复
  if (status.connected && status.last_message_at) {
    const idle = now - status.last_message_at;
    if (idle > staleEventThresholdMs) {
      return { healthy: false, reason: 'stale' };
    }
  }
  
  return { healthy: true, reason: 'ok' };
}
```

#### HunterClaw 当前实现:

```cpp
void check_channel_health(ChannelId channel_id) {
    auto& store = channel_stores_[std::string(channel_id)];
    
    for (const auto& [account_id, runtime] : store.runtimes) {
        // ✅ 跳过未启用/未配置
        if (!runtime.enabled || !runtime.configured) {
            continue;
        }
        
        // ⚠️ 检查消息过期 (但只是 warn,没有触发重启!)
        if (runtime.last_message_at.has_value()) {
            int64_t idle_ms = now_ms - runtime.last_message_at.value();
            if (idle_ms > health_config_.timing.stale_event_threshold_ms) {
                log()->warn("Channel {} account {} has stale connection ({} ms idle)",
                            channel_id, account_id, idle_ms);
                // ❌ 缺失: 应该触发重启!
            }
        }
        
        // ✅ 检查断连 (正确触发重启)
        if (runtime.running && !runtime.connected) {
            // ... 触发自动重启
        }
    }
}
```

**问题**: 
1. ❌ **没有实现完整的 `evaluate_channel_health` 函数**
2. ❌ **Stale 消息检测只记录 warn,没有触发重启**
3. ❌ **没有 `channelConnectGraceMs` 的连接宽限期检查**

---

### ❌ 问题 2: 缺少冷却期和频率限制

#### OpenClaw 实现:

```typescript
// 冷却期检查
const record = restartRecords.get(key);
if (record && now - record.lastRestartAt <= cooldownMs) {
  continue;  // ✅ 在冷却期内,跳过重启
}

// 重启频率限制
const restartsThisHour = (record?.restartsThisHour ?? [])
  .filter(r => now - r.at < ONE_HOUR_MS);
if (restartsThisHour.length >= maxRestartsPerHour) {
  log.warn(`[${channelId}:${accountId}] hit restart limit`);
  continue;  // ✅ 超过频率限制,跳过重启
}

// 记录重启
restartRecords.set(key, {
  lastRestartAt: now,
  restartsThisHour: [...restartsThisHour, { at: now }],
});
```

#### HunterClaw 实现:

```cpp
void check_channel_health(ChannelId channel_id) {
    // ...
    
    // ❌ 缺失: 没有冷却期检查!
    // ❌ 缺失: 没有重启频率限制!
    // ❌ 缺失: 没有 restartRecords 记录!
    
    if (runtime.running && !runtime.connected) {
        schedule_auto_restart(channel_id, account_id, attempts);
    }
}
```

**问题**: 
1. ❌ **没有实现冷却期机制** - 可能导致频繁重启
2. ❌ **没有实现重启频率限制** - 可能导致无限重启
3. ❌ **没有 `restartRecords` 数据结构** - 无法追踪重启历史

---

### ❌ 问题 3: 缺少手动停止检查

#### OpenClaw 实现:

```typescript
// 跳过手动停止的 Channel
if (channelManager.isManuallyStopped(channelId, accountId)) {
  continue;  // ✅ 不会自动重启手动停止的 Channel
}
```

#### HunterClaw 实现:

```cpp
void check_channel_health(ChannelId channel_id) {
    // ❌ 缺失: 没有检查 isManuallyStopped!
    // 这意味着即使用户手动停止,Health Monitor 也会尝试重启
}
```

**问题**: 
- ❌ **没有调用 `is_manually_stopped()` 检查** - 可能重启用户手动停止的 Channel

---

## 🎯 对齐建议

### 修复 1: 实现完整的健康评估函数

```cpp
struct ChannelHealthResult {
    bool healthy;
    std::string reason;
};

ChannelHealthResult evaluate_channel_health(
    const ChannelAccountSnapshot& runtime,
    ChannelId channel_id,
    int64_t now_ms,
    const ChannelHealthMonitorConfig& config
) {
    // 1. 未启用 → 健康 (跳过)
    if (!runtime.enabled) {
        return {true, "disabled"};
    }
    
    // 2. 未配置 → 健康 (跳过)
    if (!runtime.configured) {
        return {true, "not configured"};
    }
    
    // 3. 运行中但未连接 → 检查宽限期
    if (runtime.running && !runtime.connected) {
        if (runtime.started_at.has_value()) {
            int64_t elapsed = now_ms - runtime.started_at.value();
            if (elapsed < config.timing.channel_connect_grace_ms) {
                return {true, "connecting"};  // 在宽限期内
            }
        }
        return {false, "disconnected"};  // 超过宽限期仍未连接
    }
    
    // 4. 已连接但消息过期 → 不健康
    if (runtime.connected && runtime.last_message_at.has_value()) {
        int64_t idle = now_ms - runtime.last_message_at.value();
        if (idle > config.timing.stale_event_threshold_ms) {
            return {false, "stale"};  // 消息过期
        }
    }
    
    return {true, "ok"};
}
```

---

### 修复 2: 添加冷却期和频率限制

```cpp
struct RestartEvent {
    int64_t at;  // 重启时间戳 (ms)
};

struct RestartRecord {
    int64_t last_restart_at;
    std::vector<RestartEvent> restarts_this_hour;
};

void check_channel_health(ChannelId channel_id) {
    auto& store = channel_stores_[std::string(channel_id)];
    auto now_ms = core::utils::now().time_since_epoch().count() / 1000000;
    
    for (const auto& [account_id, runtime] : store.runtimes) {
        // 1. 跳过手动停止 (新增!)
        std::string key = std::string(channel_id) + ":" + account_id;
        {
            std::lock_guard<std::mutex> lock(manually_stopped_mutex_);
            if (manually_stopped_.find(key) != manually_stopped_.end()) {
                continue;
            }
        }
        
        // 2. 评估健康状态 (使用新函数!)
        auto health = evaluate_channel_health(runtime, channel_id, now_ms, health_config_);
        if (health.healthy) {
            continue;
        }
        
        // 3. 冷却期检查 (新增!)
        auto record_it = restart_records_.find(key);
        if (record_it != restart_records_.end()) {
            int64_t cooldown_ms = health_config_.cooldown_cycles * health_config_.check_interval_ms;
            int64_t last_restart_elapsed = now_ms - record_it->second.last_restart_at;
            if (last_restart_elapsed <= cooldown_ms) {
                continue;  // 在冷却期内
            }
        }
        
        // 4. 重启频率限制 (新增!)
        std::vector<RestartEvent> restarts_this_hour;
        if (record_it != restart_records_.end()) {
            for (const auto& event : record_it->second.restarts_this_hour) {
                if (now_ms - event.at < 60 * 60 * 1000) {  // 1 小时内
                    restarts_this_hour.push_back(event);
                }
            }
        }
        
        if (restarts_this_hour.size() >= health_config_.max_restarts_per_hour) {
            log()->warn("Channel {} account {} hit restart limit", channel_id, account_id);
            continue;
        }
        
        // 5. 执行重启
        log()->info("Channel {} account {} restarting (reason: {})",
                    channel_id, account_id, health.reason);
        
        if (runtime.running) {
            stop_channel(channel_id, account_id);
        }
        reset_restart_attempts(channel_id, account_id);
        start_channel(channel_id, account_id);
        
        // 6. 记录重启 (新增!)
        restarts_this_hour.push_back({now_ms});
        restart_records_[key] = {
            .last_restart_at = now_ms,
            .restarts_this_hour = std::move(restarts_this_hour)
        };
    }
}
```

---

### 修复 3: 添加 restartRecords 数据结构

```cpp
class ChannelManagerImpl {
private:
    // ... 已有字段
    
    // ✅ 新增: 重启记录 (对齐 OpenClaw)
    std::map<std::string, RestartRecord> restart_records_;
    mutable std::mutex restart_records_mutex_;
};
```

---

## 📊 对齐总结

| 功能 | OpenClaw | HunterClaw | 对齐度 | 状态 |
|------|----------|------------|--------|------|
| **基础架构** | ✅ | ✅ | 100% | ✅ 已对齐 |
| **配置参数** | ✅ | ✅ | 100% | ✅ 已对齐 |
| **监控循环** | ✅ | ✅ | 100% | ✅ 已对齐 |
| **启动宽限期** | ✅ | ✅ | 100% | ✅ 已对齐 |
| **健康评估函数** | ✅ | ❌ | 30% | ⚠️ **需要修复** |
| **连接宽限期** | ✅ | ❌ | 0% | ❌ **缺失** |
| **Stale 消息重启** | ✅ | ❌ | 0% | ❌ **缺失** |
| **手动停止检查** | ✅ | ❌ | 0% | ❌ **缺失** |
| **冷却期机制** | ✅ | ❌ | 0% | ❌ **缺失** |
| **重启频率限制** | ✅ | ❌ | 0% | ❌ **缺失** |
| **重启记录追踪** | ✅ | ❌ | 0% | ❌ **缺失** |

**总体对齐度**: ⚠️ **45%** 

---

## 🚨 风险评估

### 高风险

1. ❌ **无冷却期和频率限制** → 可能导致**无限重启循环**
2. ❌ **Stale 消息不触发重启** → Channel 可能**长期无响应**
3. ❌ **不检查手动停止** → 违反用户意图,**自动重启已停止的 Channel**

### 中风险

4. ⚠️ **缺少连接宽限期** → 可能在 Channel 正常启动过程中**误判为不健康**

---

## 🎯 推荐修复优先级

1. **P0 (Critical)**: 实现冷却期和频率限制 (防止无限重启)
2. **P0 (Critical)**: 添加手动停止检查 (修复用户体验问题)
3. **P1 (High)**: 实现完整的 `evaluate_channel_health` (修复 stale 消息问题)
4. **P1 (High)**: 添加连接宽限期 (避免误判)
5. **P2 (Medium)**: 添加 restartRecords 追踪 (增强可观测性)

---

## 📋 修复清单

- [ ] 实现 `evaluate_channel_health()` 函数
- [ ] 添加 `restart_records_` 数据结构
- [ ] 实现冷却期检查逻辑
- [ ] 实现重启频率限制
- [ ] 添加手动停止检查
- [ ] 添加连接宽限期检查
- [ ] 修复 Stale 消息重启触发
- [ ] 添加重启事件记录

---

## 🔄 下一步

1. **实现上述修复** (参考本文档的代码示例)
2. **添加单元测试** (验证冷却期、频率限制逻辑)
3. **集成测试** (模拟 Channel 失败场景)
4. **对齐文档** (更新 `CHANNEL_ADAPTER_OPENCLAW_STRICT_ALIGNMENT.md`)

---

**创建时间**: 2026-03-13  
**文档版本**: v1.0  
**状态**: 🔴 发现关键问题,需要修复
