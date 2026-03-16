# 为什么 on_assistant_delta 不下发 lifecycle_event？

## 🎯 问题
```cpp
callbacks.on_assistant_delta = [this](const std::string& text) {
    spdlog::trace("[AgentInstance] Delta: {}", text);
};
```

这个回调为什么只记录日志，不发射 `lifecycle_event`？

---

## ✅ 答案：这是正确的设计！

### 核心原因

**`on_assistant_delta` 是高频流式更新回调，不应该触发 lifecycle 事件。**

---

## 📊 设计对比分析

### 1. Reasoning Stream（推理流）

```cpp
callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
    std::string formatted = format_reasoning_message_openclaw(reasoning);
    
    // ✅ 发射 lifecycle 事件
    emit_lifecycle_event(AgentLifecyclePhase::Reasoning, {
        {"stream", "thinking"},
        {"text", formatted},
        {"delta", delta}
    });
};
```

**为什么需要发射事件？**
- Reasoning 是**用户可见的阶段性输出**
- 前端需要实时显示"AI 正在思考..."
- WebSocket 客户端需要接收思考过程

---

### 2. Assistant Delta（助手增量输出）

```cpp
callbacks.on_assistant_delta = [this](const std::string& text) {
    spdlog::trace("[AgentInstance] Delta: {}", text);  // ← 仅日志
};
```

**为什么不发射事件？**

#### 原因 1: 频率过高，会造成性能问题

- **Reasoning**: 通常每步 1-2 次，低频
- **Assistant Delta**: **每个 token 触发一次**，极高频

示例：一个 500 token 的回复会触发 **500 次回调**！

如果每次都发射 lifecycle_event：
```cpp
// ❌ 错误设计示例
callbacks.on_assistant_delta = [this](const std::string& text) {
    emit_lifecycle_event(AgentLifecyclePhase::Responding, {
        {"delta", text}
    });
};
// 结果: 500 次 WebSocket 推送，CPU 100%，延迟增加
```

#### 原因 2: 职责分离

在 ReAct 架构中：

| 回调 | 职责 | 是否发射事件 |
|------|------|-------------|
| `on_reasoning_stream` | 阶段性思考输出 | ✅ 是 |
| `on_assistant_delta` | 低级流式数据 | ❌ 否 |
| `on_block_reply` | 完整语义块 | ✅ 是（如果需要）|
| `on_tool_start` | 工具执行开始 | ✅ 是 |
| `on_tool_end` | 工具执行结束 | ✅ 是 |

#### 原因 3: 数据不完整

单个 `delta` 可能是：
- 半个单词: `"Hel"`
- 标点符号: `","`
- Unicode 片段: `"\xE2"`

发送这些碎片给前端**没有意义**，应该等待：
- **完整块** (`on_block_reply`)
- **完整消息** (`on_step` 或 `on_end`)

---

## 🔄 HunterClaw 的完整流式架构

```
┌─────────────────────────────────────────────────────────────┐
│                   LLM 流式输出                               │
└────────────┬────────────────────────────────────────────────┘
             │
             ├─→ on_reasoning_stream (低频, 思考块)
             │   └─→ emit_lifecycle_event ✅
             │       └─→ WebSocket → 前端显示 "Reasoning: ..."
             │
             ├─→ on_assistant_delta (极高频, token 级别)
             │   └─→ spdlog::trace ✅ (仅日志)
             │   └─→ ❌ 不发射事件 (避免性能问题)
             │
             ├─→ on_block_reply (中频, 段落块)
             │   └─→ 可选: emit_lifecycle_event
             │       └─→ WebSocket → 前端增量渲染
             │
             └─→ on_step (低频, 完整步骤)
                 └─→ 最终消息保存到 session
                 └─→ 前端显示完整回复
```

---

## 🎨 OpenClaw 的对标实现

查看 OpenClaw 源码:
```typescript
// openclaw-analysis/src/agents/pi-embedded-subscribe.ts:556-586

const emitReasoningStream = (text: string) => {
    // ✅ Reasoning 发射 WebSocket 事件
    emitAgentEvent({
        runId: params.runId,
        stream: "thinking",
        data: { text: formatted, delta }
    });
};

// ❌ Assistant delta 不直接发射事件
// 而是通过 block chunking 机制批量处理
```

**OpenClaw 的策略**:
1. Assistant delta → 累积到 `blockChunker`
2. `blockChunker` → 按段落/代码块切分
3. `on_block_reply` → 发射 **批量事件**

---

## 💡 如果你需要实时流式输出给用户？

### 正确方案 1: 使用 `on_block_reply`

```cpp
callbacks.on_block_reply = [this](const std::string& block) {
    // ✅ 按语义块发送（段落、代码块）
    emit_lifecycle_event(AgentLifecyclePhase::Responding, {
        {"type", "block"},
        {"content", block}
    });
};
```

### 正确方案 2: 自定义节流机制

```cpp
class ThrottledDeltaEmitter {
private:
    std::string buffer_;
    std::chrono::steady_clock::time_point last_emit_;
    const std::chrono::milliseconds min_interval_{100};  // 100ms 最小间隔

public:
    void on_delta(const std::string& text) {
        buffer_ += text;
        
        auto now = std::chrono::steady_clock::now();
        if (now - last_emit_ >= min_interval_) {
            emit_lifecycle_event(AgentLifecyclePhase::Responding, {
                {"delta", buffer_}
            });
            buffer_.clear();
            last_emit_ = now;
        }
    }
};

// 使用
auto emitter = std::make_shared<ThrottledDeltaEmitter>();
callbacks.on_assistant_delta = [emitter](const std::string& text) {
    emitter->on_delta(text);
};
```

### 错误方案: 直接转发每个 delta

```cpp
// ❌ 错误示例 - 会导致性能问题
callbacks.on_assistant_delta = [this](const std::string& text) {
    emit_lifecycle_event(AgentLifecyclePhase::Responding, {
        {"delta", text}
    });
};
// 问题:
// - 500 token 回复 = 500 次 WebSocket 发送
// - CPU 使用率暴增
// - WebSocket 拥塞
// - 前端渲染卡顿
```

---

## 📝 总结

| 问题 | 答案 |
|------|------|
| **为什么不发射事件？** | 频率过高，会造成性能问题 |
| **那如何实时推送？** | 使用 `on_block_reply` 或自定义节流 |
| **日志够用吗？** | 够用！`spdlog::trace` 用于调试，正常不输出 |
| **OpenClaw 怎么做的？** | 使用 block chunking 批量处理 |

---

## 🔗 相关文件

- **HunterClaw 实现**: `src/agents/agent_instance.cc:524`
- **OpenClaw 对标**: `openclaw-analysis/src/agents/pi-embedded-subscribe.ts:556-586`
- **流式架构文档**: `STREAMING_ARCHITECTURE.md`

---

## 🚀 下一步优化建议

如果需要更细粒度的实时反馈，可以考虑：

1. **实现 Block Chunking**（对标 OpenClaw）
   - 按段落/代码块切分
   - 减少事件频率 10-100 倍

2. **添加节流机制**
   - 设置最小发送间隔（如 100ms）
   - 批量累积 delta

3. **使用 SSE (Server-Sent Events)**
   - 比 WebSocket 更轻量
   - 适合单向推送场景

---

**结论**: 当前设计是正确的！`on_assistant_delta` 是低级回调，不应该直接触发 lifecycle 事件。
