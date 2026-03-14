# 🦅 HunterClaw Channel Monitor 内部实现机制完整说明

本文档通过 Demo 完整展示 **Channel Monitor** 的内部实现机制。

---

## 📚 目录

1. [核心组件架构](#1-核心组件架构)
2. [连接状态自动机](#2-连接状态自动机)
3. [心跳检测机制](#3-心跳检测机制)
4. [自动重连策略](#4-自动重连策略指数退避)
5. [事件通知系统](#5-事件通知系统)
6. [多通道并发监控](#6-多通道并发监控)
7. [实际运行示例](#7-实际运行示例)
8. [核心代码实现](#8-核心代码实现)

---

## 1. 核心组件架构

### 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    ChannelMonitor                           │
│                                                             │
│  ┌───────────────┐  ┌─────────────────┐  ┌──────────────┐ │
│  │ State Machine │  │ Heartbeat Timer │  │ Event Queue  │ │
│  └───────┬───────┘  └────────┬────────┘  └──────┬───────┘ │
│          │                   │                   │          │
│  ┌───────▼───────────────────▼───────────────────▼──────┐  │
│  │              Monitor Thread (监控线程)               │  │
│  └──────────────────────────────────────────────────────┘  │
│          │                                                  │
│  ┌───────▼───────┐  ┌──────────────┐  ┌─────────────────┐ │
│  │ Health Metrics│  │ Reconnect    │  │ Thread Control  │ │
│  │ (健康指标)     │  │ Strategy     │  │ (线程控制)       │ │
│  └───────────────┘  └──────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 核心组件

| 组件 | 职责 | 实现方式 |
|------|------|----------|
| **State Machine** | 管理连接状态 | 5 种状态 + 转换规则 |
| **Heartbeat Timer** | 心跳定时器 | 每 5 秒发送一次 |
| **Event Queue** | 事件通知 | 回调函数机制 |
| **Monitor Thread** | 监控线程 | 独立线程运行 |
| **Health Metrics** | 健康指标 | 统计各项指标 |
| **Reconnect Strategy** | 重连策略 | 指数退避算法 |
| **Thread Control** | 线程控制 | start/stop/pause |

---

## 2. 连接状态自动机

### 状态转换图

```
    [DISCONNECTED]  ──────────────────┐
           │                          │
           │ 1. start()               │ 5. max_attempts
           │                          │    exceeded
           ▼                          │
    [CONNECTING] ───────────┐         │
           │                 │        │
           │ 2. success      │ 3. fail│
           │                 │        │
           ▼                 ▼        │
    [CONNECTED]    ────►  [RECONNECTING]
           │                 │        │
           │ 4. timeout      │        │
           └─────────────────┘        │
                                      ▼
                                  [FAILED]
```

### 状态说明

#### 🔴 DISCONNECTED (断开)
- **说明**: 初始状态,通道未连接
- **触发条件**: 监控器启动时
- **下一状态**: `CONNECTING`
- **动作**: 准备建立连接

#### 🟡 CONNECTING (连接中)
- **说明**: 正在尝试建立连接
- **触发条件**: `start()` 调用
- **下一状态**: 
  - 成功 → `CONNECTED`
  - 失败 → `RECONNECTING`
- **动作**: 调用 `channel->connect()`

#### 🟢 CONNECTED (已连接)
- **说明**: 连接正常,定期发送心跳
- **触发条件**: 连接成功
- **下一状态**: 
  - 心跳超时 → `RECONNECTING`
- **动作**: 
  - 每 5 秒发送心跳
  - 监控心跳响应
  - 统计健康指标

#### 🟠 RECONNECTING (重连中)
- **说明**: 连接断开,正在尝试重连
- **触发条件**: 
  - 连接失败
  - 心跳连续 3 次超时
- **下一状态**: 
  - 尝试次数 < max → `CONNECTING`
  - 尝试次数 >= max → `FAILED`
- **动作**: 
  - 指数退避等待
  - 增加重连计数

#### ⚫ FAILED (失败)
- **说明**: 最终失败状态,停止监控
- **触发条件**: 超过最大重连次数
- **下一状态**: 无 (终态)
- **动作**: 
  - 停止监控线程
  - 清理资源

---

## 3. 心跳检测机制

### 心跳时间线 (正常场景)

```
t=0s        t=5s        t=10s       t=15s       t=20s
 │           │           │           │           │
 ├───────────┼───────────┼───────────┼───────────┤
 │💓 Ping    │💓 Ping    │💓 Ping    │💓 Ping    │
 │✅ Pong    │✅ Pong    │✅ Pong    │✅ Pong    │
 │           │           │           │           │
 └─ OK ──────└─ OK ──────└─ OK ──────└─ OK ──────┘
```

### 心跳超时场景

```
t=0s        t=5s        t=10s       t=15s       t=20s
 │           │           │           │           │
 ├───────────┼───────────┼───────────┼───────────┤
 │💓 Ping    │💓 Ping    │💓 Ping    │💓 Ping    │
 │✅ Pong    │❌ Timeout │❌ Timeout │❌ Timeout │
 │           │  (1/3)    │  (2/3)    │  (3/3)    │
 └─ OK ──────└─ Warn ────└─ Alert ───└─ RECONNECT
```

### 心跳配置

| 参数 | 值 | 说明 |
|------|------|------|
| `heartbeat_interval` | 5000ms | 心跳发送间隔 |
| `heartbeat_timeout` | 10000ms | 心跳超时时间 |
| `max_consecutive_fails` | 3 次 | 连续失败触发重连 |

### 心跳检测流程

```cpp
bool send_heartbeat() {
    // 1. 发送心跳
    metrics_.heartbeat_sent++;
    log("[HEARTBEAT] 💓 发送心跳 #" + heartbeat_sent);
    
    // 2. 等待响应 (最多 10 秒)
    bool success = channel_->send_heartbeat();
    
    // 3. 处理结果
    if (success) {
        metrics_.heartbeat_received++;
        metrics_.heartbeat_timeout = 0;  // 重置超时计数
        log("[HEARTBEAT] ✅ 心跳响应 (成功率: " + success_rate + "%)");
    } else {
        metrics_.heartbeat_timeout++;
        log("[HEARTBEAT] 💔 心跳超时 (" + timeout + "/3)");
        
        // 连续 3 次超时 → 触发重连
        if (metrics_.heartbeat_timeout >= 3) {
            change_state(RECONNECTING);
        }
    }
    
    return success;
}
```

---

## 4. 自动重连策略 (指数退避)

### 指数退避算法

```cpp
delay = base_delay × 2^(attempt - 1)
delay = min(delay, max_delay)
```

### 重连时间线

```
Attempt #1:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             1s        🔄 Reconnect
             └───────►  CONNECTING

Attempt #2:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             2s        🔄 Reconnect
             └───────►  CONNECTING

Attempt #3:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             4s        🔄 Reconnect
             └───────►  CONNECTING

Attempt #4:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             8s        🔄 Reconnect
             └───────►  CONNECTING

Attempt #5:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             16s       🔄 Reconnect
             └───────►  CONNECTING

Attempt #6:  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
             30s (cap) ❌ Max Attempts Exceeded
             └───────►  FAILED
```

### 重连配置

| 参数 | 值 | 说明 |
|------|------|------|
| `max_reconnect_attempts` | 5 次 | 最大重连尝试次数 |
| `base_reconnect_delay` | 1000ms | 基础延迟 (第 1 次) |
| `max_reconnect_delay` | 30000ms | 最大延迟上限 |

### 重连策略优势

| 优势 | 说明 |
|------|------|
| ✅ **减轻服务器压力** | 避免频繁重连导致雪崩 |
| ✅ **应对临时抖动** | 短时间故障快速恢复 |
| ✅ **防止永久重连** | 最大次数限制,避免无限循环 |
| ✅ **符合标准实践** | 业界公认的最佳实践 (RFC, AWS, Kubernetes) |

---

## 5. 事件通知系统

### 事件类型

| 事件类型 | 触发时机 | 用途 |
|----------|----------|------|
| `STATE_CHANGED` | 状态改变 | 监控状态流转 |
| `HEARTBEAT_SENT` | 发送心跳 | 记录心跳日志 |
| `HEARTBEAT_RECEIVED` | 收到响应 | 确认连接正常 |
| `HEARTBEAT_TIMEOUT` | 心跳超时 | 告警通知 |
| `RECONNECTING` | 开始重连 | 记录重连尝试 |
| `RECONNECT_SUCCESS` | 重连成功 | 恢复正常通知 |
| `RECONNECT_FAILED` | 重连失败 | 错误告警 |
| `ERROR_OCCURRED` | 错误发生 | 错误日志 |

### 事件流

```
[ChannelMonitor]
      │
      │ emit_event(STATE_CHANGED)
      ▼
[EventQueue]
      │
      │ push(event)
      ▼
[EventCallback]
      │
      ├───► [Logger]           (日志记录)
      ├───► [Alerter]          (告警通知)
      ├───► [MetricsCollector] (指标上报)
      └───► [Dashboard]        (仪表板更新)
```

### 事件数据结构

```cpp
struct MonitorEventData {
    MonitorEvent event;           // 事件类型
    string channel_id;            // 通道 ID
    ConnectionState old_state;    // 旧状态
    ConnectionState new_state;    // 新状态
    string message;               // 消息内容
    timestamp timestamp;          // 事件时间
};
```

### 事件回调示例

```cpp
monitor.set_event_callback([](const MonitorEventData& event) {
    switch (event.event) {
        case MonitorEvent::STATE_CHANGED:
            log_state_change(event);
            break;
        case MonitorEvent::HEARTBEAT_TIMEOUT:
            send_alert("心跳超时: " + event.channel_id);
            break;
        case MonitorEvent::RECONNECT_SUCCESS:
            send_notification("重连成功: " + event.channel_id);
            break;
        // ...
    }
});
```

---

## 6. 多通道并发监控

### 架构图

```
┌─────────────────────────────────────────────────────────┐
│              MultiChannelMonitor (管理器)              │
│                                                         │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐       │
│  │ Discord    │  │ Slack      │  │ Telegram   │       │
│  │ Monitor    │  │ Monitor    │  │ Monitor    │  ...  │
│  └──────┬─────┘  └──────┬─────┘  └──────┬─────┘       │
│         │                │                │             │
│         ▼                ▼                ▼             │
│  ┌─────────────────────────────────────────────┐       │
│  │     ThreadPool (独立监控线程池)             │       │
│  └─────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────┘
```

### 并发特性

#### 1️⃣ 独立线程
- 每个通道一个监控线程
- 线程间完全隔离
- 避免互相干扰

#### 2️⃣ 资源隔离
- 独立的健康指标
- 独立的重连策略
- 独立的事件队列

#### 3️⃣ 统一管理
- `start_all()` / `stop_all()` 批量控制
- 聚合健康报告
- 全局事件处理

### 线程模型

```
Main Thread
    │
    ├── Discord Monitor Thread
    │   ├── State Machine Loop
    │   ├── Heartbeat Timer
    │   └── Event Emitter
    │
    ├── Slack Monitor Thread
    │   ├── State Machine Loop
    │   ├── Heartbeat Timer
    │   └── Event Emitter
    │
    └── Telegram Monitor Thread
        ├── State Machine Loop
        ├── Heartbeat Timer
        └── Event Emitter
```

---

## 7. 实际运行示例

### 场景 1: 正常运行 (15 秒)

```
t=0s:   [MONITOR] 🚀 启动监控: Slack
t=0.1s: [STATE] 🔄 状态变化: DISCONNECTED -> CONNECTING
t=0.2s: [CONNECT] 🔌 正在连接: Slack...
t=0.3s: [CONNECT] ✅ 连接成功: Slack
t=0.3s: [STATE] 🔄 状态变化: CONNECTING -> CONNECTED

t=5s:   [HEARTBEAT] 💓 发送心跳: Slack (#1)
t=5.1s: [HEARTBEAT] ✅ 心跳响应: Slack (成功率: 100.0%)

t=10s:  [HEARTBEAT] 💓 发送心跳: Slack (#2)
t=10.1s:[HEARTBEAT] ✅ 心跳响应: Slack (成功率: 100.0%)

t=15s:  [HEARTBEAT] 💓 发送心跳: Slack (#3)
t=15.1s:[HEARTBEAT] ✅ 心跳响应: Slack (成功率: 100.0%)
```

**健康报告**:

```
╔══════════════════════════════════════════════════════════╗
║ Channel: Slack                                           ║
╠══════════════════════════════════════════════════════════╣
║ State: CONNECTED                                         ║
║ Uptime: 15s                                              ║
╠══════════════════════════════════════════════════════════╣
║ Heartbeat Metrics:                                       ║
║   Sent: 3                                                ║
║   Received: 3                                            ║
║   Timeout: 0                                             ║
║   Success Rate: 100.0%                                   ║
╠══════════════════════════════════════════════════════════╣
║ Reconnects: 0                                            ║
║ Errors: 0                                                ║
╚══════════════════════════════════════════════════════════╝
```

### 场景 2: 网络故障 + 自动重连 (20 秒)

```
t=20s:  [NETWORK] 🔥 模拟网络故障: Slack

t=25s:  [HEARTBEAT] 💓 发送心跳: Slack (#4)
t=35s:  [HEARTBEAT] 💔 心跳超时: Slack (1/3)

t=40s:  [HEARTBEAT] 💓 发送心跳: Slack (#5)
t=50s:  [HEARTBEAT] 💔 心跳超时: Slack (2/3)

t=55s:  [HEARTBEAT] 💓 发送心跳: Slack (#6)
t=65s:  [HEARTBEAT] 💔 心跳超时: Slack (3/3)
t=65s:  [STATE] 🔄 状态变化: CONNECTED -> RECONNECTING

t=66s:  [RECONNECT] 🔄 尝试重连 #1 (延迟 1000ms): Slack
t=67s:  [CONNECT] 🔌 正在连接: Slack...
t=67.1s:[CONNECT] ❌ 连接失败: Slack

t=67.1s:[STATE] 🔄 状态变化: CONNECTING -> RECONNECTING
t=69s:  [RECONNECT] 🔄 尝试重连 #2 (延迟 2000ms): Slack
t=71s:  [CONNECT] 🔌 正在连接: Slack...
t=71.1s:[CONNECT] ❌ 连接失败: Slack
```

**健康报告**:

```
╔══════════════════════════════════════════════════════════╗
║ Channel: Slack                                           ║
╠══════════════════════════════════════════════════════════╣
║ State: RECONNECTING                                      ║
╠══════════════════════════════════════════════════════════╣
║ Heartbeat Metrics:                                       ║
║   Sent: 6                                                ║
║   Received: 3                                            ║
║   Timeout: 3                                             ║
║   Success Rate: 50.0%                                    ║
╠══════════════════════════════════════════════════════════╣
║ Reconnects: 2                                            ║
║ Errors: 3                                                ║
╚══════════════════════════════════════════════════════════╝
```

### 场景 3: 网络恢复 (15 秒)

```
t=90s:  [NETWORK] ✅ 恢复网络连接: Slack

t=95s:  [RECONNECT] 🔄 尝试重连 #3 (延迟 4000ms): Slack
t=99s:  [CONNECT] 🔌 正在连接: Slack...
t=99.1s:[CONNECT] ✅ 连接成功: Slack
t=99.1s:[STATE] 🔄 状态变化: CONNECTING -> CONNECTED

t=104s: [HEARTBEAT] 💓 发送心跳: Slack (#7)
t=104.1s:[HEARTBEAT] ✅ 心跳响应: Slack (成功率: 57.1%)

t=109s: [HEARTBEAT] 💓 发送心跳: Slack (#8)
t=109.1s:[HEARTBEAT] ✅ 心跳响应: Slack (成功率: 62.5%)
```

**健康报告**:

```
╔══════════════════════════════════════════════════════════╗
║ Channel: Slack                                           ║
╠══════════════════════════════════════════════════════════╣
║ State: CONNECTED                                         ║
║ Uptime: 10s                                              ║
╠══════════════════════════════════════════════════════════╣
║ Heartbeat Metrics:                                       ║
║   Sent: 8                                                ║
║   Received: 5                                            ║
║   Timeout: 0                                             ║
║   Success Rate: 62.5%                                    ║
╠══════════════════════════════════════════════════════════╣
║ Reconnects: 3                                            ║
║ Errors: 3                                                ║
╚══════════════════════════════════════════════════════════╝
```

---

## 8. 核心代码实现

### 8.1 主监控循环

```cpp
void ChannelMonitor::monitor_loop() {
    while (running_) {
        auto state = state_.load();
        
        switch (state) {
            case ConnectionState::DISCONNECTED:
                change_state(ConnectionState::CONNECTING);
                break;
                
            case ConnectionState::CONNECTING:
                handle_connecting();
                break;
                
            case ConnectionState::CONNECTED:
                handle_connected();
                break;
                
            case ConnectionState::RECONNECTING:
                handle_reconnecting();
                break;
                
            case ConnectionState::FAILED:
                running_ = false;
                break;
        }
        
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}
```

### 8.2 心跳检测

```cpp
void ChannelMonitor::handle_connected() {
    // 发送心跳
    bool heartbeat_ok = send_heartbeat();
    
    if (!heartbeat_ok) {
        metrics_.heartbeat_timeout++;
        
        // 连续超时 -> 进入重连
        if (metrics_.heartbeat_timeout >= 3) {
            change_state(ConnectionState::RECONNECTING);
        }
    } else {
        // 心跳正常,重置超时计数
        metrics_.heartbeat_timeout = 0;
    }
    
    // 等待下一次心跳
    wait_for(heartbeat_interval_ms_);
}
```

### 8.3 指数退避重连

```cpp
void ChannelMonitor::handle_reconnecting() {
    metrics_.reconnect_attempts++;
    
    if (metrics_.reconnect_attempts > max_reconnect_attempts_) {
        change_state(ConnectionState::FAILED);
        return;
    }
    
    // 指数退避: 1s, 2s, 4s, 8s, 16s
    int delay_ms = base_reconnect_delay_ms_ 
                   * (1 << (metrics_.reconnect_attempts - 1));
    delay_ms = min(delay_ms, 30000);  // 最大 30 秒
    
    // 等待后重连
    wait_for(delay_ms);
    change_state(ConnectionState::CONNECTING);
}
```

---

## 📚 相关文件

- **头文件**: `include/hunterclaw/channels/channel_monitor.h`
- **实现**: `src/channels/channel_monitor.cc`
- **Demo**: `examples/channel_monitor_demo.cc`
- **文档**: `examples/CHANNEL_MONITOR_DEMO.md`
- **架构说明**: `examples/channel_monitor_architecture.sh`

---

## ✅ 核心特性总结

| 特性 | 说明 |
|------|------|
| ✅ **连接状态自动机** | 5 种状态自动切换,清晰的转换规则 |
| ✅ **心跳检测机制** | 定期发送心跳 (5秒),超时检测 (10秒),连续失败触发重连 (3次) |
| ✅ **指数退避重连** | 1s, 2s, 4s, 8s, 16s,最大延迟 30秒,最大尝试 5 次 |
| ✅ **事件通知系统** | 8 种事件类型,回调函数机制,可扩展的事件处理 |
| ✅ **多通道并发监控** | 独立线程隔离,资源独立管理,统一控制接口 |
| ✅ **健康指标统计** | 心跳成功率,运行时长,重连次数,错误计数 |

---

**🦅 HunterClaw - Production-Grade Channel Monitoring System**

**Enterprise-Ready | High-Performance | Fault-Tolerant**

完整展示 Channel Monitor 内部实现机制! 🚀
