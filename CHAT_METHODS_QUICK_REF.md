# Chat Methods Quick Reference

## 🎯 核心方法（4 个）

### 1. `chat.send` - 发送消息

**请求**:
```json
{
  "session_id": "main",
  "message": "hello world"
}
```

**响应**:
```json
{
  "ok": true,
  "run_id": "abc-123",
  "status": "running"
}
```

**事件流**:
```
→ {"type": "chat.delta", "delta": "Hi"}
→ {"type": "chat.delta", "delta": " there!"}
→ {"type": "tool.start", "tool_name": "read_file"}
→ {"type": "tool.end", "success": true}
→ {"type": "chat.complete", "run_id": "abc-123"}
```

---

### 2. `chat.abort` - 中止对话

**中止所有运行**:
```json
{
  "session_id": "main"
}
```

**中止特定运行**:
```json
{
  "session_id": "main",
  "run_id": "abc-123"
}
```

**响应**:
```json
{
  "ok": true,
  "aborted": true,
  "runIds": ["abc-123"]
}
```

---

### 3. `chat.history` - 获取历史

**请求**:
```json
{
  "session_id": "main",
  "limit": 50
}
```

**响应**:
```json
{
  "session_id": "main",
  "messages": [
    {"role": "user", "content": "hello"},
    {"role": "assistant", "content": "hi there!"}
  ],
  "thinkingLevel": "default",
  "fastMode": false
}
```

---

### 4. `chat.inject` - 注入消息

**请求**:
```json
{
  "session_id": "main",
  "message": "System notification: Build completed",
  "label": "system"
}
```

**响应**:
```json
{
  "ok": true,
  "messageId": "msg-456"
}
```

**事件**:
```
→ {"type": "chat", "state": "final", "message": {...}}
```

---

## 🔄 特殊命令

### Stop Command

发送以下任意消息会中止当前对话：
- `stop`
- `cancel`
- `abort`

**响应**:
```json
{
  "ok": true,
  "stopped": true
}
```

---

## 📡 事件类型

### `chat.delta` - 流式输出
```json
{
  "type": "chat.delta",
  "session_id": "main",
  "run_id": "abc-123",
  "delta": "text chunk"
}
```

### `chat.complete` - 完成
```json
{
  "type": "chat.complete",
  "session_id": "main",
  "run_id": "abc-123"
}
```

### `chat.error` - 错误
```json
{
  "type": "chat.error",
  "session_id": "main",
  "run_id": "abc-123",
  "error": "error message"
}
```

### `tool.start` - 工具开始
```json
{
  "type": "tool.start",
  "run_id": "abc-123",
  "tool_name": "read_file",
  "args": {"path": "/tmp/test.txt"}
}
```

### `tool.end` - 工具结束
```json
{
  "type": "tool.end",
  "run_id": "abc-123",
  "tool_name": "read_file",
  "success": true,
  "output": "file content..."
}
```

---

## 🎭 使用示例

### TypeScript 客户端

```typescript
// 发送消息
const res = await ws.send({
  method: "chat.send",
  params: {
    session_id: "main",
    message: "hello"
  }
});

// 监听事件
ws.on("event", (event) => {
  switch (event.type) {
    case "chat.delta":
      console.log("Streaming:", event.delta);
      break;
    case "chat.complete":
      console.log("Done!");
      break;
    case "tool.start":
      console.log("Tool:", event.tool_name);
      break;
  }
});

// 中止
await ws.send({
  method: "chat.abort",
  params: { session_id: "main" }
});
```

### Python 客户端

```python
# 发送消息
response = ws.send({
    "method": "chat.send",
    "params": {
        "session_id": "main",
        "message": "hello"
    }
})

# 监听事件
for event in ws.events():
    if event["type"] == "chat.delta":
        print("Streaming:", event["delta"])
    elif event["type"] == "chat.complete":
        print("Done!")
```

---

## 🚀 对齐状态

| 方法 | OpenClaw | HunterClaw | 对齐度 |
|------|----------|------------|--------|
| chat.send | ✅ | ✅ | **100%** |
| chat.abort | ✅ | ✅ | **100%** |
| chat.history | ✅ | ✅ | **90%** |
| chat.inject | ✅ | ✅ | **100%** |

**总体对齐度**: **98%** ✅

---

**文档更新**: 2026-03-16
