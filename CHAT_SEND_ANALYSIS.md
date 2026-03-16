# HunterClaw chat.send 实现分析

## 📊 核心对比

### HunterClaw 当前实现 (❌ 未完成)

```cpp
// src/gateway/methods/chat.cc:19-49
core::Result<json> method_chat_send(...) {
    // 1. 参数验证
    if (!params.contains("session_id") || !params.contains("message")) {
        return Error();
    }
    
    // 2. 获取或创建 Agent
    auto agent = agent_pool->get_or_create_agent(session_id);
    
    // 3. ❌ 只返回确认，没有实际执行！
    return Ok({
        "session_id": session_id,
        "agent_id": agent->context().agent_id,
        "message": "Message received"
    });
}
```

**问题：** 只是创建了 Agent 实例，**没有实际执行对话逻辑**！

---

### OpenClaw 完整实现 (✅ 7 步完整流程)

```typescript
// src/gateway/server-methods/chat.ts

async function handleChatSend(params) {
    // ============================================
    // Step 1: 参数验证和清理
    // ============================================
    const message = sanitizeChatSendMessageInput(params.message);
    
    // Stop Command 检测
    if (isChatStopCommandText(message)) {
        await abortChatRunById(params.sessionId);
        return { ok: true, stopped: true };
    }
    
    // ============================================
    // Step 2: Session 解析和加载
    // ============================================
    const sessionKey = params.sessionKey ?? "main";
    const agentId = resolveSessionAgentId(sessionKey);
    
    // 加载或创建 Session Entry
    const entry = await loadSessionEntry(sessionKey, agentId);
    
    // ============================================
    // Step 3: 构造 Agent 运行上下文
    // ============================================
    const runId = generateUuid();
    const runContext = {
        sessionId: entry.sessionId,
        sessionKey,
        agentId,
        runId,
        workspace: entry.workspace,
        model: resolveSessionModelRef(entry),
        thinkingMode: entry.thinkingMode ?? "auto",
        temperature: entry.temperature ?? 0.7,
    };
    
    // ============================================
    // Step 4: 写入用户消息到 Transcript
    // ============================================
    const userMessageId = generateUuid();
    await appendMessageToTranscript({
        sessionFilePath: resolveSessionFilePath(sessionKey, agentId),
        message: {
            id: userMessageId,
            role: "user",
            content: message,
            timestamp: Date.now(),
            attachments: normalizeAttachments(params.attachments),
        },
    });
    
    // ============================================
    // Step 5: 启动 Agent 执行 (异步)
    // ============================================
    const agentProcess = spawn("pi-coding-agent", [
        "run",
        "--session-key", sessionKey,
        "--agent-id", agentId,
        "--run-id", runId,
        "--message", message,
    ]);
    
    // 注册 Abort Controller (支持中止)
    registerAbortController(runId, agentProcess);
    
    // ============================================
    // Step 6: 实时流式输出 (WebSocket)
    // ============================================
    agentProcess.stdout.on("data", (chunk) => {
        // 解析 Agent 输出
        const delta = parseDelta(chunk);
        
        // 广播到所有订阅的客户端
        broadcastToSession(sessionKey, {
            type: "chat.delta",
            runId,
            delta: delta.text,
            timestamp: Date.now(),
        });
    });
    
    // ============================================
    // Step 7: 完成后写入 Assistant 消息
    // ============================================
    agentProcess.on("exit", async (code) => {
        const finalText = getFinalText(runId);
        
        await appendMessageToTranscript({
            sessionFilePath: resolveSessionFilePath(sessionKey, agentId),
            message: {
                id: generateUuid(),
                role: "assistant",
                content: finalText,
                timestamp: Date.now(),
                runId,
            },
        });
        
        // 广播完成事件
        broadcastToSession(sessionKey, {
            type: "chat.complete",
            runId,
            text: finalText,
        });
    });
    
    // ============================================
    // 立即返回 (不等待执行完成)
    // ============================================
    return {
        ok: true,
        sessionId: entry.sessionId,
        runId,
        status: "running",
    };
}
```

---

## 🎯 实际调用例子

### 例子 1: 用户在 WebChat 发送 "hello"

#### 请求

```json
{
  "method": "chat.send",
  "params": {
    "session_key": "main",
    "message": "hello"
  }
}
```

#### OpenClaw 处理流程

```typescript
// 1. 参数验证
message = sanitize("hello")  // "hello"

// 2. 加载 Session
sessionKey = "main"
agentId = "main"  // 从 sessionKey 解析
entry = loadSessionEntry("main", "main")
// entry = {
//   sessionId: "sess-abc123",
//   workspace: "/home/user/project",
//   model: "claude-sonnet-4",
//   thinkingMode: "auto"
// }

// 3. 生成 runId
runId = "run-xyz789"

// 4. 写入用户消息到 transcript.jsonl
// ~/.openclaw/agents/main/sessions/sess-abc123/transcript.jsonl
{
  "id": "msg-user-001",
  "role": "user",
  "content": "hello",
  "timestamp": 1710000000000
}

// 5. 启动 Agent
spawn("pi-coding-agent", [
  "run",
  "--session-key", "main",
  "--agent-id", "main",
  "--run-id", "run-xyz789",
  "--message", "hello"
])

// 6. Agent 开始流式输出
// stdout: {"type": "delta", "text": "Hi"}
// 广播到客户端: {"type": "chat.delta", "runId": "run-xyz789", "delta": "Hi"}

// stdout: {"type": "delta", "text": " there!"}
// 广播: {"type": "chat.delta", "runId": "run-xyz789", "delta": " there!"}

// 7. Agent 完成
finalText = "Hi there! How can I help you today?"

// 写入 assistant 消息到 transcript.jsonl
{
  "id": "msg-asst-002",
  "role": "assistant",
  "content": "Hi there! How can I help you today?",
  "timestamp": 1710000005000,
  "runId": "run-xyz789"
}

// 广播完成事件
{"type": "chat.complete", "runId": "run-xyz789", "text": "..."}
```

#### HunterClaw 当前处理 (❌ 不完整)

```cpp
// 1. 参数验证
session_id = "main"
message = "hello"

// 2. 获取或创建 Agent
agent = agent_pool->get_or_create_agent("main");
// agent 实例已创建，但...

// 3. ❌ 只返回确认，没有实际执行
return {
  "session_id": "main",
  "agent_id": "main",
  "message": "Message received"
};

// ❌ Agent 没有开始思考
// ❌ 没有写入 transcript
// ❌ 没有流式输出
// ❌ 客户端只收到确认，没有实际回复
```

---

### 例子 2: 用户发送 "stop" 中止运行

#### OpenClaw 处理

```typescript
// 检测到 Stop Command
if (isChatStopCommandText("stop")) {
  // 查找正在运行的 runId
  const runId = chatRunRegistry.peek(sessionId);
  
  // 中止 Agent 进程
  await abortChatRunById(runId);
  // 发送 SIGTERM → Agent 优雅退出
  
  // 返回确认
  return { ok: true, stopped: true };
}
```

#### HunterClaw 处理 (❌ 未实现)

```cpp
// ❌ 没有 Stop Command 检测
// ❌ 没有 Abort Controller
// 只会把 "stop" 当成普通消息处理
```

---

## 📊 对齐分析

| 功能 | OpenClaw | HunterClaw | 对齐度 |
|------|----------|------------|--------|
| **1. 参数验证** | ✅ 完整 (Zod Schema) | ✅ 基础验证 | **70%** |
| **2. 消息清理** | ✅ NFC + 控制字符过滤 | ❌ 未实现 | **0%** |
| **3. Stop Command** | ✅ 支持 | ❌ 未实现 | **0%** |
| **4. Session 管理** | ✅ 磁盘持久化 | ✅ 内存 + 磁盘 | **90%** |
| **5. Agent 执行** | ✅ 子进程 + 流式输出 | ❌ 未实现 | **0%** |
| **6. Transcript 写入** | ✅ JSONL 格式 | ❌ 未实现 | **0%** |
| **7. WebSocket 广播** | ✅ 实时流式 | ❌ 未实现 | **0%** |
| **8. Abort 支持** | ✅ SIGTERM | ❌ 未实现 | **0%** |
| **总体对齐度** | - | - | **20%** |

---

## ⚠️ 待实现功能

### 高优先级 (核心功能)

1. **Agent 执行逻辑** (最关键！)
   ```cpp
   // 需要集成 ReAct Engine
   auto react_engine = agent->react_engine();
   react_engine->run_async(message, [&](const Delta& delta) {
       // 流式输出回调
       broadcast_delta(session_id, delta.text);
   });
   ```

2. **Transcript 写入**
   ```cpp
   // 写入用户消息
   transcript->append_user_message(message_id, message);
   
   // 写入 assistant 消息
   transcript->append_assistant_message(message_id, final_text, run_id);
   ```

3. **WebSocket 流式广播**
   ```cpp
   server->broadcast_to_session(session_id, {
       {"type", "chat.delta"},
       {"runId", run_id},
       {"delta", delta_text}
   });
   ```

### 中优先级

4. **Stop Command 检测**
   ```cpp
   if (is_stop_command(message)) {
       abort_chat_run(session_id);
       return Ok({"stopped", true});
   }
   ```

5. **消息清理**
   ```cpp
   std::string sanitize_message(const std::string& raw) {
       // NFC 归一化 + 控制字符过滤
   }
   ```

6. **Abort Controller**
   ```cpp
   class AbortController {
       void register_run(const std::string& run_id, std::shared_ptr<Process> proc);
       void abort(const std::string& run_id);  // 发送 SIGTERM
   };
   ```

---

## 🔧 建议的实现步骤

### Phase 1: 最小可用版 (MVP)

```cpp
core::Result<json> method_chat_send(...) {
    // 1. 参数验证 (已有)
    
    // 2. 获取 Agent (已有)
    auto agent = agent_pool->get_or_create_agent(session_id);
    
    // 3. 【新增】启动 ReAct Engine
    auto react_engine = agent->react_engine();
    std::string run_id = generate_uuid();
    
    react_engine->run_async(message, run_id, [&](const Delta& delta) {
        // 4. 【新增】流式输出
        server->broadcast_delta(session_id, run_id, delta.text);
    });
    
    // 5. 立即返回
    return Ok({
        {"session_id", session_id},
        {"run_id", run_id},
        {"status", "running"}
    });
}
```

### Phase 2: 完整功能

- ✅ Transcript 持久化
- ✅ Stop Command
- ✅ Abort Controller
- ✅ 消息清理和验证

---

## 📝 总结

### 当前状态
- ✅ **AgentPool 基础设施**：完全对齐 OpenClaw (100%)
- ✅ **Session 管理**：内存 + 磁盘双层查询 (90%)
- ❌ **chat.send 执行**：只有框架，没有实际执行逻辑 (20%)

### 核心缺失
**HunterClaw 的 `method_chat_send()` 只是一个空壳，需要集成：**
1. ReAct Engine 执行
2. 流式输出广播
3. Transcript 持久化

### 下一步
实现 `react_engine->run_async()`，这是整个对话系统的核心！
