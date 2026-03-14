# 🦅 HunterClaw Channel Monitor Demo

完整展示 **Channel Monitor 内部实现机制** 的实战演示。

---

## 🎯 核心功能

### 1️⃣ **连接状态监控**
```
状态自动机 (5 种状态):
DISCONNECTED → CONNECTING → CONNECTED
       ↑           ↓            ↓
       └─────── RECONNECTING ←─┘
                    ↓
                 FAILED
```

**状态转换规则**:
- `DISCONNECTED` → 自动触发连接
- `CONNECTING` → 连接成功/失败
- `CONNECTED` → 心跳超时触发重连
- `RECONNECTING` → 指数退避后重试
- `FAILED` → 超过最大重连次数

---

### 2️⃣ **心跳检测机制**

```cpp
配置参数:
- heartbeat_interval: 5000ms    (心跳间隔)
- heartbeat_timeout: 10000ms    (心跳超时)
- max_consecutive_timeout: 3    (连续超时次数)

检测逻辑:
1. 每 5 秒发送一次心跳
2. 等待心跳响应 (最多 10 秒)
3. 连续 3 次超时 → 触发重连
4. 心跳正常 → 重置超时计数
```

**健康指标**:
- ✅ Heartbeat Sent (发送次数)
- ✅ Heartbeat Received (响应次数)
- ✅ Heartbeat Timeout (超时次数)
- ✅ Success Rate (成功率 %)

---

### 3️⃣ **自动重连策略 (指数退避)**

```cpp
重连延迟计算:
delay = base_delay * 2^(attempt - 1)

示例:
尝试 #1: 1s   (1000ms)
尝试 #2: 2s   (2000ms)
尝试 #3: 4s   (4000ms)
尝试 #4: 8s   (8000ms)
尝试 #5: 16s  (16000ms)
最大延迟: 30s  (cap)
```

**重连配置**:
- `max_reconnect_attempts`: 5 次
- `base_reconnect_delay`: 1000ms
- `max_reconnect_delay`: 30000ms

---

### 4️⃣ **事件通知系统**

```cpp
支持的事件类型:
- STATE_CHANGED       (状态改变)
- HEARTBEAT_SENT      (心跳发送)
- HEARTBEAT_RECEIVED  (心跳响应)
- HEARTBEAT_TIMEOUT   (心跳超时)
- RECONNECTING        (开始重连)
- RECONNECT_SUCCESS   (重连成功)
- RECONNECT_FAILED    (重连失败)
- ERROR_OCCURRED      (错误发生)
```

**事件回调**:
```cpp
monitor.set_event_callback([](const MonitorEventData& event) {
    // 处理事件: 日志记录、告警、指标上报
});
```

---

### 5️⃣ **多通道并发监控**

```cpp
多通道管理器:
- 独立线程 (每个通道一个监控线程)
- 互不干扰 (通道间隔离)
- 统一管理 (start_all, stop_all)
- 聚合状态 (print_all_status)

示例:
Discord  Monitor Thread  → [心跳检测] [状态管理] [自动重连]
Slack    Monitor Thread  → [心跳检测] [状态管理] [自动重连]
Telegram Monitor Thread  → [心跳检测] [状态管理] [自动重连]
```

---

## 🚀 快速开始

### 编译和运行
```bash
cd /data/workspace/QQMail/HunterClaw/examples
chmod +x run_channel_monitor_demo.sh
./run_channel_monitor_demo.sh
```

### 手动编译
```bash
g++ -std=c++17 -pthread -O2 channel_monitor_demo.cc -o channel_monitor_demo
./channel_monitor_demo
```

---

## 📊 Demo 场景

### **场景 1: 正常运行** (15 秒)
```
✅ 所有通道正常连接
✅ 心跳定期发送 (每 5 秒)
✅ 健康指标统计
```

### **场景 2: 网络故障** (20 秒)
```
🔥 模拟 Slack 通道网络故障
❌ 心跳连续超时
🔄 自动触发重连 (指数退避)
📊 重连尝试统计
```

### **场景 3: 网络恢复** (15 秒)
```
✅ 恢复 Slack 网络连接
🔄 自动重连成功
✅ 恢复正常心跳
📊 最终健康报告
```

---

## 🎨 输出示例

```
╔══════════════════════════════════════════════════════════╗
║ Channel: Discord                                         ║
╠══════════════════════════════════════════════════════════╣
║ State: CONNECTED                                         ║
║ Uptime: 42s                                              ║
╠══════════════════════════════════════════════════════════╣
║ Heartbeat Metrics:                                       ║
║   Sent: 8                                                ║
║   Received: 8                                            ║
║   Timeout: 0                                             ║
║   Success Rate: 100.0%                                   ║
╠══════════════════════════════════════════════════════════╣
║ Messages: 0 received                                     ║
║ Reconnects: 0                                            ║
║ Errors: 0                                                ║
╚══════════════════════════════════════════════════════════╝
```

---

## 🏗️ 核心实现类

### `ChannelMonitor` (核心监控器)
```cpp
class ChannelMonitor {
    // 状态管理
    ConnectionState state_;
    
    // 配置参数
    int heartbeat_interval_ms_;
    int heartbeat_timeout_ms_;
    int max_reconnect_attempts_;
    
    // 健康指标
    HealthMetrics metrics_;
    
    // 核心方法
    void monitor_loop();              // 主监控循环
    void handle_connected();          // 心跳检测
    void handle_reconnecting();       // 重连逻辑
    bool send_heartbeat();            // 发送心跳
    void change_state(State new_state); // 状态切换
};
```

### `MultiChannelMonitor` (多通道管理器)
```cpp
class MultiChannelMonitor {
    map<string, ChannelMonitor> monitors_;
    
    void add_channel(IChannel* channel);
    void start_all();
    void stop_all();
    void print_all_status();
};
```

---

## 🔍 关键技术点

### 1. **线程安全**
```cpp
- atomic<ConnectionState> state_      // 原子状态
- mutex + condition_variable          // 线程同步
- lock_guard<mutex>                   // RAII 锁
```

### 2. **事件驱动架构**
```cpp
- Event Callback 机制
- 状态变化通知
- 异步事件处理
```

### 3. **健康指标统计**
```cpp
- 心跳成功率 (Success Rate)
- 运行时长 (Uptime)
- 重连次数 (Reconnect Attempts)
- 错误计数 (Error Count)
```

### 4. **网络弹性设计**
```cpp
- 指数退避重连
- 超时检测
- 故障隔离
- 自动恢复
```

---

## 📚 扩展阅读

相关源码文件:
- `include/hunterclaw/channels/channel_monitor.h`
- `src/channels/channel_monitor.cc`
- `include/hunterclaw/gateway/gateway.h`
- `src/gateway/gateway.cc`

---

## 🎯 适用场景

✅ **多平台消息通道监控** (Discord, Slack, Telegram, WhatsApp)  
✅ **WebSocket 连接管理** (Gateway, Real-time Communication)  
✅ **分布式服务健康检查** (Microservices, Cloud Native)  
✅ **长连接保活机制** (Keep-alive, Heartbeat)  
✅ **网络故障自动恢复** (Resilience, Self-healing)  

---

**🦅 HunterClaw - Production-Grade C++ Multi-Channel Monitoring System**

完整演示 Channel Monitor 的内部实现机制,包括:
- 🎯 连接状态自动机
- 💓 心跳检测机制
- 🔄 指数退避重连
- 📊 健康指标统计
- 🎨 多通道并发监控

**Production-Ready!** 🚀
