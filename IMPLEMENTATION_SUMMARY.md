# HunterClaw Session Management - OpenClaw 对齐实现

## 概述

严格对齐 OpenClaw 的 session 管理机制，实现：

1. **禁用自动重置** - Session 永久保留（除非显式删除）
2. **Device ID 匹配** - 基于设备 ID 自动识别和复用会话
3. **多种提取策略** - 支持 Header、Token、Cookie、Fallback 计算

## 核心修改

### 1. 禁用自动重置 ✅

#### `include/hunterclaw/sessions/session_manager.h`

```cpp
struct SessionConfig {
    // ✅ 默认禁用重置
    bool daily_reset = false;          // 原: true
    std::optional<int> idle_minutes;   // 原: 空, 现: nullopt (禁用)
    
    // ✅ 调整维护策略
    int prune_after_days = 365;        // 原: 30
    int max_entries = 10000;           // 原: 500
    
    // ✅ 启用 device-based sessions
    bool device_based_sessions = true;
};
```

#### `src/sessions/session_manager.cc`

```cpp
bool FileSessionManager::should_reset(const SessionEntry& session) const {
    // ✅ 完全禁用时直接返回 false
    if (!config_.daily_reset && !config_.idle_minutes.has_value()) {
        return false;
    }
    return is_idle(session) || is_past_daily_reset(session);
}
```

### 2. Device ID 工具 ✅

#### 新建文件

- `include/hunterclaw/gateway/session_device_id.h` - Device ID 工具头文件
- `src/gateway/session_device_id.cc` - Device ID 工具实现

#### 核心函数

```cpp
// 从 HTTP 请求提取 Device ID
std::string extract_device_id_from_request(const httplib::Request& req);

// 计算稳定的 Device ID (基于 IP + User-Agent)
std::string compute_stable_device_id(const std::string& ip, const std::string& ua);

// 构造 device-based session key
std::string build_device_session_key(
    const std::string& agent_id,
    const std::string& device_id,
    const std::optional<std::string>& channel_id = std::nullopt
);

// 验证 Device ID 格式
bool is_valid_device_id(const std::string& device_id);
```

#### 提取策略（优先级从高到低）

1. **HTTP Header**: `X-Device-ID` / `X-OpenClaw-Device-ID`
2. **Authorization Token**: 从 Bearer token 解析 `device_id`
3. **Cookie**: `openclaw_device_id` / `device_id`
4. **Fallback**: `computed-{SHA256(IP+UA)[0:16]}`

### 3. HTTP Gateway 集成 ✅

#### `src/gateway/http_gateway.cc`

```cpp
void HttpGateway::handle_chat_completions(...) {
    // 方式 1: 显式 session key (兼容旧版)
    if (req.has_header("x-hunterclaw-session-key")) {
        session_key = req.get_header_value("x-hunterclaw-session-key");
    }
    // 方式 2: Device ID 自动构造 (✅ 新增)
    else {
        std::string device_id = extract_device_id_from_request(req);
        if (is_valid_device_id(device_id)) {
            session_key = build_device_session_key("main", device_id);
        }
    }
    
    // 传递 session_key 到 LLM adapter
    request.user = session_key;
}
```

## Session Key 格式

严格对齐 OpenClaw 格式：

```
agent:main:device:{deviceId}
agent:main:device:{deviceId}:channel:{channelId}
```

示例：
```
agent:main:device:abc123
agent:main:device:computed-1a2b3c4d5e6f7890:channel:discord
```

## 配置文件

### `config/session-config-example.json`

```json
{
  "sessions": {
    "reset": {
      "mode": "none"  // ✅ 禁用自动重置
    },
    "maintenance": {
      "pruneAfter": "365d",
      "maxEntries": 10000
    },
    "storage": {
      "deviceBased": true  // ✅ 启用 device-based sessions
    }
  }
}
```

## 使用示例

### 1. 使用 HTTP Header 传递 Device ID

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: my-device-123" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### 2. 验证会话复用

```bash
# 第一次请求 - 创建会话
curl -H "X-Device-ID: device-A" http://localhost:8080/v1/chat/completions ...

# 第二次请求 - 复用会话
curl -H "X-Device-ID: device-A" http://localhost:8080/v1/chat/completions ...
```

### 3. 不同设备独立会话

```bash
# Device A
curl -H "X-Device-ID: device-A" ...

# Device B (独立会话)
curl -H "X-Device-ID: device-B" ...
```

## 验证清单

- [x] SessionConfig 默认禁用 daily_reset
- [x] SessionConfig 默认不设置 idle_minutes
- [x] should_reset() 完全禁用时返回 false
- [x] Device ID 提取工具实现
- [x] HTTP Gateway 集成 Device ID
- [x] Session Key 格式对齐 OpenClaw
- [x] 配置文件示例

## 对齐 OpenClaw 的关键点

| 特性 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| 禁用重置 | `reset: { mode: "none" }` | `daily_reset = false; idle_minutes = nullopt` | ✅ 已对齐 |
| Device ID | HTTP Header / Token | 同左 | ✅ 已对齐 |
| Session Key | `agent:{id}:device:{deviceId}` | 同左 | ✅ 已对齐 |
| 维护策略 | `pruneAfter: 365d` | `prune_after_days = 365` | ✅ 已对齐 |
| Per-Agent Storage | 支持 | 支持 | ✅ 已对齐 |

## 编译和测试

```bash
# 编译
cd HunterClaw
bazel build //src:hunterclaw_gateway

# 运行
./bazel-bin/src/hunterclaw_gateway

# 测试
curl -H "X-Device-ID: test-123" \
     -X POST http://localhost:8080/v1/chat/completions \
     -H "Content-Type: application/json" \
     -d '{"model":"claude-sonnet-4.5","messages":[{"role":"user","content":"test"}]}'
```

## 关键优势

✅ **永久会话** - Device ID 不变则会话永久保留  
✅ **自动识别** - 无需用户登录，基于设备自动匹配  
✅ **跨平台同步** - 同一设备的多个客户端共享会话  
✅ **向后兼容** - 保留显式 session key 支持  
✅ **OpenClaw 对齐** - 完全遵循 OpenClaw 规范
