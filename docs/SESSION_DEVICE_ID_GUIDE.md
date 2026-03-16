# Session Device ID 实施指南

## 概述

本文档描述如何在 HunterClaw 中实现基于 Device ID 的会话管理，严格对齐 OpenClaw 的实现规范。

## 核心特性

✅ **永久会话** - Device ID 不变则会话永久保留  
✅ **自动识别** - 无需用户登录，基于设备自动匹配  
✅ **禁用重置** - 不会因为空闲或跨天而重置会话  
✅ **多种提取** - 支持 Header、Token、Cookie、Fallback  
✅ **OpenClaw 对齐** - 完全遵循 OpenClaw 规范

## 架构设计

### 1. Session Key 格式

```
agent:{agentId}:device:{deviceId}
agent:{agentId}:device:{deviceId}:channel:{channelId}
```

示例：
```
agent:main:device:my-laptop-001
agent:main:device:computed-1a2b3c4d:channel:discord
```

### 2. Device ID 提取策略

优先级从高到低：

| 优先级 | 方式 | Header/Key | 示例 |
|--------|------|-----------|------|
| 1 | HTTP Header | `X-Device-ID` | `my-device-123` |
| 2 | HTTP Header | `X-OpenClaw-Device-ID` | `openclaw-device-abc` |
| 3 | Bearer Token | 解析 JWT/JSON | `{"device_id": "..."}` |
| 4 | Cookie | `openclaw_device_id` | `device=xyz` |
| 5 | Fallback | 计算 SHA256(IP+UA) | `computed-1a2b3c4d` |

### 3. 配置禁用自动重置

```cpp
// include/hunterclaw/sessions/session_manager.h
struct SessionConfig {
    bool daily_reset = false;           // ✅ 禁用每日重置
    std::optional<int> idle_minutes;    // ✅ nullopt = 禁用空闲超时
    int prune_after_days = 365;         // ✅ 1年后才清理
    int max_entries = 10000;            // ✅ 增大限制
    bool device_based_sessions = true;  // ✅ 启用 device-based
};
```

## 实施步骤

### 步骤 1: 修改 SessionConfig 默认值

文件: `include/hunterclaw/sessions/session_manager.h`

```cpp
struct SessionConfig {
    bool daily_reset = false;          // 改为 false
    std::optional<int> idle_minutes;   // 保持 nullopt
    int prune_after_days = 365;        // 30 → 365
    int max_entries = 10000;           // 500 → 10000
    bool device_based_sessions = true; // 新增字段
};
```

### 步骤 2: 实现 Device ID 工具

创建文件:
- `include/hunterclaw/gateway/session_device_id.h`
- `src/gateway/session_device_id.cc`

核心函数：
```cpp
std::string extract_device_id_from_request(const httplib::Request& req);
std::string compute_stable_device_id(const std::string& ip, const std::string& ua);
std::string build_device_session_key(const std::string& agent_id, const std::string& device_id);
bool is_valid_device_id(const std::string& device_id);
```

### 步骤 3: 修改 should_reset 逻辑

文件: `src/sessions/session_manager.cc`

```cpp
bool FileSessionManager::should_reset(const SessionEntry& session) const {
    // ✅ 完全禁用时直接返回 false
    if (!config_.daily_reset && !config_.idle_minutes.has_value()) {
        return false;
    }
    return is_idle(session) || is_past_daily_reset(session);
}
```

### 步骤 4: HTTP Gateway 集成

文件: `src/gateway/http_gateway.cc`

```cpp
#include "hunterclaw/gateway/session_device_id.h"

void HttpGateway::handle_chat_completions(...) {
    std::string session_key;
    
    // 方式 1: 显式 session key
    if (req.has_header("x-hunterclaw-session-key")) {
        session_key = req.get_header_value("x-hunterclaw-session-key");
    }
    // 方式 2: Device ID 自动构造
    else {
        std::string device_id = extract_device_id_from_request(req);
        if (is_valid_device_id(device_id)) {
            session_key = build_device_session_key("main", device_id);
        }
    }
    
    request.user = session_key;
}
```

## 使用示例

### 示例 1: 使用 HTTP Header

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "X-Device-ID: my-laptop" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### 示例 2: 使用 Bearer Token

```bash
# Token 格式: Base64({"device_id": "abc123", ...})
TOKEN=$(echo '{"device_id":"abc123"}' | base64)

curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### 示例 3: 使用 Cookie

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Cookie: openclaw_device_id=my-device-001" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### 示例 4: Fallback (自动计算)

```bash
# 不带 Device ID - 自动基于 IP + User-Agent 计算
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "User-Agent: MyApp/1.0" \
  -d '{
    "model": "claude-sonnet-4.5",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

## 测试验证

### 运行测试脚本

```bash
cd HunterClaw
./scripts/test-device-sessions.sh
```

### 手动验证

#### 1. 测试不同设备独立会话

```bash
# Device A
curl -H "X-Device-ID: device-A" ...
# 应该创建新会话 agent:main:device:device-A

# Device B
curl -H "X-Device-ID: device-B" ...
# 应该创建另一个会话 agent:main:device:device-B
```

#### 2. 测试会话复用

```bash
# 第一次请求
curl -H "X-Device-ID: device-A" -d '{"messages":[{"role":"user","content":"我叫Alice"}]}' ...

# 第二次请求
curl -H "X-Device-ID: device-A" -d '{"messages":[{"role":"user","content":"我叫什么名字？"}]}' ...
# 应该返回 "Alice"
```

#### 3. 测试禁用重置

```bash
# 初始请求
curl -H "X-Device-ID: device-C" ...

# 等待超过原 idle_minutes 时间
sleep 120

# 再次请求
curl -H "X-Device-ID: device-C" ...
# 会话应该仍然存在，没有被重置
```

#### 4. 验证 session 文件

```bash
# 查看 session 存储
ls -la ~/.openclaw/agents/main/sessions/

# 应该看到类似的文件:
# agent_main_device_device-A.json
# agent_main_device_device-B.json
```

## 配置文件

### 完整配置示例

`config/hunterclaw.json`:

```json
{
  "sessions": {
    "reset": {
      "mode": "none"
    },
    "maintenance": {
      "pruneAfter": "365d",
      "maxEntries": 10000,
      "mode": "warn"
    },
    "storage": {
      "perAgent": true,
      "deviceBased": true,
      "baseDir": "~/.hunterclaw/sessions"
    }
  },
  "gateway": {
    "http": {
      "host": "0.0.0.0",
      "port": 8080,
      "cors": {
        "enabled": true,
        "allowOrigin": "*",
        "allowMethods": "GET, POST, PUT, DELETE, OPTIONS",
        "allowHeaders": "Content-Type, Authorization, X-Device-ID, X-OpenClaw-Device-ID"
      }
    }
  }
}
```

## 编译构建

### 使用 Bazel

```bash
cd HunterClaw

# 编译
bazel build //src:hunterclaw_gateway

# 运行
./bazel-bin/src/hunterclaw_gateway --config config/hunterclaw.json
```

### 使用 CMake

```bash
cd HunterClaw
mkdir build && cd build

# 配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 运行
./hunterclaw_gateway
```

## 故障排除

### 问题 1: Session 仍然被重置

**症状**: 会话在空闲后消失

**排查**:
```bash
# 检查配置
grep -A 5 "sessions" config/hunterclaw.json

# 检查日志
tail -f ~/.hunterclaw/logs/gateway.log | grep "reset"
```

**解决方案**:
- 确认 `daily_reset = false`
- 确认 `idle_minutes` 为 `nullopt` 或未设置

### 问题 2: Device ID 未被识别

**症状**: 每次请求创建新会话

**排查**:
```bash
# 开启 debug 日志
export SPDLOG_LEVEL=debug

# 查看 device ID 提取日志
./hunterclaw_gateway | grep "DeviceID"
```

**解决方案**:
- 检查 HTTP Header 是否正确设置
- 验证 Device ID 格式（8-64字符，只包含字母数字和连字符）

### 问题 3: Session 文件未创建

**症状**: `~/.hunterclaw/sessions/` 目录为空

**排查**:
```bash
# 检查目录权限
ls -la ~/.hunterclaw/

# 检查写入权限
touch ~/.hunterclaw/sessions/test && rm ~/.hunterclaw/sessions/test
```

**解决方案**:
- 确保目录存在且有写权限
- 检查 `per_agent_storage` 配置

## OpenClaw 对齐清单

| 特性 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| 禁用重置 | `reset: { mode: "none" }` | `daily_reset = false; idle_minutes = nullopt` | ✅ |
| Device ID 提取 | HTTP Header / Token / Cookie | 同左 | ✅ |
| Session Key 格式 | `agent:{id}:device:{deviceId}` | 同左 | ✅ |
| Fallback Device ID | SHA256(IP+UA) | 同左 | ✅ |
| 维护策略 | `pruneAfter: 365d` | `prune_after_days = 365` | ✅ |
| 最大会话数 | `maxEntries: 10000` | `max_entries = 10000` | ✅ |
| Per-Agent Storage | 支持 | 支持 | ✅ |

## 参考资料

- [OpenClaw Session 文档](https://docs.openclaw.dev/sessions)
- [HunterClaw Session Manager 源码](../src/sessions/session_manager.cc)
- [Device ID 工具实现](../src/gateway/session_device_id.cc)
- [实施总结文档](../IMPLEMENTATION_SUMMARY.md)

## 常见问题

### Q1: 如何手动删除某个 device 的会话？

```bash
# 方式 1: 通过 API
curl -X DELETE http://localhost:8080/api/v1/sessions/agent:main:device:device-A

# 方式 2: 直接删除文件
rm ~/.hunterclaw/agents/main/sessions/agent_main_device_device-A.json
```

### Q2: Device ID 的长度限制是多少？

8-64 字符，只能包含字母数字和连字符 `-` 或下划线 `_`。

### Q3: 是否支持跨设备同步会话？

不支持。每个 Device ID 对应独立的会话。如需跨设备同步，建议使用统一的 Device ID 或实现单独的同步机制。

### Q4: 如何查看某个设备的会话历史？

```bash
# 通过 API
curl http://localhost:8080/api/v1/sessions/agent:main:device:device-A

# 直接查看文件
cat ~/.hunterclaw/agents/main/sessions/agent_main_device_device-A.json | jq '.'
```

### Q5: 是否可以临时启用会话重置？

可以。修改配置后重启服务：

```json
{
  "sessions": {
    "reset": {
      "mode": "idle",
      "idleMinutes": 60
    }
  }
}
```

## 贡献

如有问题或建议，请提交 Issue 或 Pull Request 到 [HunterClaw GitHub](https://github.com/hunterclaw/hunterclaw)。

## 许可证

本文档和相关代码遵循 MIT License。
