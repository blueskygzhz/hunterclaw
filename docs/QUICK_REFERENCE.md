# Session Device ID - 快速参考

## 核心概念

**Device ID Session**: 基于设备 ID 自动识别和复用会话，无需用户登录。

## Session Key 格式

```
agent:{agentId}:device:{deviceId}
```

示例: `agent:main:device:my-laptop-001`

## Device ID 提取优先级

1. `X-Device-ID` Header
2. `X-OpenClaw-Device-ID` Header  
3. Bearer Token 中的 `device_id`
4. Cookie `openclaw_device_id`
5. Fallback: `computed-{SHA256(IP+UA)[0:16]}`

## HTTP 请求示例

### 方式 1: HTTP Header (推荐)

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "X-Device-ID: my-device" \
  -H "Content-Type: application/json" \
  -d '{"model":"claude-sonnet-4.5","messages":[...]}'
```

### 方式 2: Bearer Token

```bash
# Token = Base64({"device_id": "abc123"})
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Authorization: Bearer eyJkZXZpY2VfaWQiOiJhYmMxMjMifQ==" \
  -H "Content-Type: application/json" \
  -d '{"model":"claude-sonnet-4.5","messages":[...]}'
```

### 方式 3: Cookie

```bash
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Cookie: openclaw_device_id=my-device" \
  -H "Content-Type: application/json" \
  -d '{"model":"claude-sonnet-4.5","messages":[...]}'
```

## 配置

### 禁用自动重置

```cpp
// include/hunterclaw/sessions/session_manager.h
struct SessionConfig {
    bool daily_reset = false;           // ✅ 禁用每日重置
    std::optional<int> idle_minutes;    // ✅ nullopt = 禁用空闲超时
    int prune_after_days = 365;         // ✅ 365天后清理
    int max_entries = 10000;            // ✅ 最大10000个会话
    bool device_based_sessions = true;  // ✅ 启用 device-based
};
```

### JSON 配置

```json
{
  "sessions": {
    "reset": {"mode": "none"},
    "maintenance": {
      "pruneAfter": "365d",
      "maxEntries": 10000
    },
    "storage": {
      "deviceBased": true
    }
  }
}
```

## 核心 API

```cpp
// 提取 Device ID
std::string device_id = extract_device_id_from_request(req);

// 构造 Session Key
std::string session_key = build_device_session_key("main", device_id);

// 验证 Device ID
bool valid = is_valid_device_id(device_id);  // 8-64 字符, 字母数字和连字符

// 计算 Fallback Device ID
std::string computed = compute_stable_device_id(ip, user_agent);
```

## 验证

### 测试不同设备独立会话

```bash
# Device A
curl -H "X-Device-ID: device-A" ... -d '{"messages":[{"role":"user","content":"我叫Alice"}]}'

# Device B
curl -H "X-Device-ID: device-B" ... -d '{"messages":[{"role":"user","content":"我叫Bob"}]}'

# Device A 验证
curl -H "X-Device-ID: device-A" ... -d '{"messages":[{"role":"user","content":"我叫什么名字？"}]}'
# 应返回: Alice
```

### 测试会话持久化

```bash
# 第一次请求
curl -H "X-Device-ID: test" ...

# 等待 5 分钟
sleep 300

# 第二次请求 - 会话应该仍然存在
curl -H "X-Device-ID: test" ...
```

## 故障排除

### Session 被重置？

检查配置:
```bash
grep -E "(daily_reset|idle_minutes)" include/hunterclaw/sessions/session_manager.h
# 应显示: daily_reset = false
```

### Device ID 未识别？

开启 debug 日志:
```bash
export SPDLOG_LEVEL=debug
./hunterclaw_gateway | grep "DeviceID"
```

### Session 文件在哪？

```bash
ls ~/.hunterclaw/agents/main/sessions/
# 或
ls ~/.openclaw/agents/main/sessions/
```

## OpenClaw 对齐状态

| 特性 | 状态 |
|------|------|
| 禁用自动重置 | ✅ |
| Device ID 提取 | ✅ |
| Session Key 格式 | ✅ |
| Fallback 计算 | ✅ |
| 维护策略 | ✅ |

## 相关文档

- [完整实施指南](./SESSION_DEVICE_ID_GUIDE.md)
- [实施总结](../IMPLEMENTATION_SUMMARY.md)
- [对齐文档](../SESSION_DEVICE_ID_ALIGNMENT.md)
