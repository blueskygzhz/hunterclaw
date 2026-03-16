# OpenClaw 严格对齐：Assistant Delta 处理机制

## 🎯 核心发现

**OpenClaw 中不存在简单的 `on_assistant_delta` 回调！**

Assistant 的流式输出是通过 **`message_update` 事件** + **Block Chunking** 机制处理的。

---

## 📊 OpenClaw 的完整流式架构

### 1. 事件流转

```typescript
// openclaw-analysis/src/agents/pi-embedded-subscribe.handlers.messages.ts:78-253

handleMessageUpdate(ctx, evt) {
    // 1. 提取 delta
    const delta = evt.assistantMessageEvent?.delta || "";
    
    // 2. 累积到 buffer
    ctx.state.deltaBuffer += chunk;
    
    // 3. Block Chunker 处理（可选）
    if (ctx.blockChunker) {
        ctx.blockChunker.append(chunk);  // ← 按段落/代码块切分
    } else {
        ctx.state.blockBuffer += chunk;   // ← 简单累积
    }
    
    // 4. 剥离 <think> 标签
    const cleaned = ctx.stripBlockTags(ctx.state.deltaBuffer, ...);
    
    // 5. 计算增量并发射 WebSocket 事件
    const deltaText = cleanedText.slice(previousCleaned.length);
    if (deltaText) {
        emitAgentEvent({
            runId: ctx.params.runId,
            stream: "assistant",
            data: {
                text: cleanedText,    // 完整文本
                delta: deltaText      // 增量文本
            }
        });
    }
}
```

### 2. 关键组件

| 组件 | 职责 | OpenClaw 位置 |
|------|------|--------------|
| `deltaBuffer` | 累积所有 token | `state.deltaBuffer` |
| `blockBuffer` | 累积可见内容 | `state.blockBuffer` |
| `blockChunker` | 按段落/代码块切分 | `EmbeddedBlockChunker` |
| `stripBlockTags` | 移除 `<think>` 标签 | `stripBlockTags()` |
| `emitAgentEvent` | 发射 WebSocket 事件 | `emitAgentEvent()` |

---

## ❌ HunterClaw 当前实现（不对齐）

```cpp
// ❌ 当前实现: 过于简单
callbacks.on_assistant_delta = [this](const std::string& text) {
    spdlog::trace("[AgentInstance] Delta: {}", text);  // 仅日志
};
```

**问题**：
1. 没有累积 delta 到 buffer
2. 没有 block chunking 机制
3. 没有 WebSocket 实时推送
4. 不符合 OpenClaw 架构

---

## ✅ 严格对齐方案

### 方案 1: 完整对齐（推荐）

实现完整的 Block Chunking + WebSocket 推送：

```cpp
// ========== 添加状态变量 ==========
class AgentInstance {
private:
    std::string delta_buffer_;              // 累积所有 delta
    std::string block_buffer_;              // 累积可见内容
    std::string last_streamed_assistant_;   // 上次发送的完整文本
    std::unique_ptr<BlockChunker> block_chunker_;  // Block 切分器
};

// ========== 对齐实现 ==========
callbacks.on_assistant_delta = [this](const std::string& delta) {
    // 1. 累积到 buffer (对齐 OpenClaw)
    delta_buffer_ += delta;
    
    // 2. Block Chunker 处理（可选）
    if (block_chunker_) {
        block_chunker_->append(delta);
        
        // 排空可发送的块
        auto chunks = block_chunker_->drain(/*force=*/false);
        for (const auto& chunk : chunks) {
            emit_block_chunk(chunk);
        }
    } else {
        block_buffer_ += delta;
    }
    
    // 3. 剥离 <think> 标签
    std::string cleaned = strip_block_tags(delta_buffer_);
    
    // 4. 计算增量
    std::string delta_text;
    if (cleaned.find(last_streamed_assistant_) == 0) {
        delta_text = cleaned.substr(last_streamed_assistant_.length());
    } else {
        delta_text = cleaned;
    }
    
    last_streamed_assistant_ = cleaned;
    
    // 5. 发射 WebSocket 事件 (对齐 OpenClaw)
    if (!delta_text.empty()) {
        emit_lifecycle_event(AgentLifecyclePhase::Responding, {
            {"stream", "assistant"},
            {"text", cleaned},      // 完整文本
            {"delta", delta_text}   // 增量文本
        });
    }
    
    spdlog::trace("[AgentInstance] Assistant delta: buffer_len={}, delta_len={}", 
                 cleaned.length(), delta_text.length());
};
```

### 方案 2: 简化对齐（最小改动）

如果暂时不需要 Block Chunking，至少实现基础的流式推送：

```cpp
// ========== 添加状态变量 ==========
class AgentInstance {
private:
    std::string assistant_buffer_;           // 累积所有 assistant 输出
    std::string last_streamed_assistant_;    // 上次发送的文本
    std::chrono::steady_clock::time_point last_emit_time_;
    const std::chrono::milliseconds min_emit_interval_{100};  // 100ms 节流
};

// ========== 简化实现 ==========
callbacks.on_assistant_delta = [this](const std::string& delta) {
    // 1. 累积
    assistant_buffer_ += delta;
    
    // 2. 节流检查 (避免过度推送)
    auto now = std::chrono::steady_clock::now();
    if (now - last_emit_time_ < min_emit_interval_) {
        return;  // 跳过本次发送
    }
    
    // 3. 计算增量
    std::string delta_text;
    if (assistant_buffer_.find(last_streamed_assistant_) == 0) {
        delta_text = assistant_buffer_.substr(last_streamed_assistant_.length());
    } else {
        delta_text = assistant_buffer_;
    }
    
    // 4. 发射事件
    if (!delta_text.empty()) {
        emit_lifecycle_event(AgentLifecyclePhase::Responding, {
            {"stream", "assistant"},
            {"text", assistant_buffer_},
            {"delta", delta_text}
        });
        
        last_streamed_assistant_ = assistant_buffer_;
        last_emit_time_ = now;
    }
};
```

### 方案 3: 保持现状（最保守）

如果确定不需要实时 assistant 流式推送，保持现状也是合理的：

```cpp
// ✅ 当前实现已经符合"不发射事件"的设计原则
callbacks.on_assistant_delta = [this](const std::string& text) {
    spdlog::trace("[AgentInstance] Delta: {}", text);
};

// 理由:
// 1. assistant 输出最终会在 on_step/on_end 中完整保存
// 2. 避免高频 WebSocket 推送的性能问题
// 3. 用户体验仍然流畅（reasoning 已实时推送）
```

---

## 🔍 OpenClaw 源码对照表

| OpenClaw 组件 | 对应文件 | HunterClaw 实现位置 |
|--------------|---------|-------------------|
| `handleMessageUpdate` | `pi-embedded-subscribe.handlers.messages.ts:78` | `agent_instance.cc:524` |
| `deltaBuffer` | `pi-embedded-subscribe.ts:49` | 需要添加 |
| `blockBuffer` | `pi-embedded-subscribe.ts:50` | 需要添加 |
| `EmbeddedBlockChunker` | `pi-embedded-block-chunker.ts` | 需要实现 |
| `stripBlockTags` | `pi-embedded-subscribe.ts:368` | 需要实现 |
| `emitAgentEvent` | `pi-embedded-subscribe.handlers.messages.ts:219` | `emit_lifecycle_event` |

---

## 📋 实现检查清单

### 完整对齐（推荐）

- [ ] 添加 `delta_buffer_` 成员变量
- [ ] 添加 `block_buffer_` 成员变量
- [ ] 添加 `last_streamed_assistant_` 成员变量
- [ ] 实现 `BlockChunker` 类（参考 `EmbeddedBlockChunker`）
- [ ] 实现 `strip_block_tags()` 函数
- [ ] 在 `on_assistant_delta` 中累积 buffer
- [ ] 实现增量计算逻辑
- [ ] 发射 WebSocket 事件
- [ ] 添加单元测试

### 简化对齐（最小改动）

- [ ] 添加 `assistant_buffer_` 成员变量
- [ ] 添加 `last_streamed_assistant_` 成员变量
- [ ] 实现节流机制（100ms 间隔）
- [ ] 在 `on_assistant_delta` 中累积 buffer
- [ ] 实现增量计算逻辑
- [ ] 发射 WebSocket 事件

### 保持现状

- [x] 已实现：仅日志记录
- [x] 已确认：不影响核心功能
- [ ] 文档说明：为什么不发射事件

---

## 🎨 Block Chunking 机制详解

OpenClaw 的 `EmbeddedBlockChunker` 按以下规则切分：

```typescript
// openclaw-analysis/src/agents/pi-embedded-block-chunker.ts

class EmbeddedBlockChunker {
    append(chunk: string) {
        this.buffer += chunk;
    }
    
    drain(force: boolean): string[] {
        // 1. 按段落切分（两个换行符）
        // 2. 检测代码块边界（```）
        // 3. 避免在代码块中间切分
        // 4. 保持至少 N 个字符的块大小
        // 5. 返回可发送的完整块
    }
}
```

**切分示例**：

```
输入流:
"Hello\nWorld\n\n```python\ndef foo():\n    pass\n```\n\nDone"

输出块:
1. "Hello\nWorld"          ← 段落1
2. "```python\ndef foo():\n    pass\n```"  ← 代码块（完整）
3. "Done"                  ← 段落2
```

**好处**：
- 避免在代码块中间截断
- 段落完整性
- 更好的前端渲染体验

---

## 🚀 推荐实施步骤

### 第一阶段：简化对齐（1-2 天）

1. 添加 buffer 变量
2. 实现节流机制
3. 发射基础事件
4. 测试 WebSocket 推送

### 第二阶段：完整对齐（3-5 天）

1. 实现 `BlockChunker` 类
2. 实现 `strip_block_tags`
3. 集成 Block Chunking
4. 完善测试用例

### 第三阶段：优化（持续）

1. 性能调优
2. 内存优化
3. 边界情况处理

---

## 📝 总结

| 方案 | 对齐程度 | 开发量 | 推荐度 |
|------|---------|--------|--------|
| 完整对齐 | ⭐⭐⭐⭐⭐ | 3-5 天 | ⭐⭐⭐⭐ |
| 简化对齐 | ⭐⭐⭐ | 1-2 天 | ⭐⭐⭐⭐⭐ |
| 保持现状 | ⭐ | 0 天 | ⭐⭐ |

**推荐**：先实施**简化对齐**，验证效果后再考虑**完整对齐**。

---

## 🔗 相关文件

- **OpenClaw 核心逻辑**: `openclaw-analysis/src/agents/pi-embedded-subscribe.handlers.messages.ts`
- **Block Chunker**: `openclaw-analysis/src/agents/pi-embedded-block-chunker.ts`
- **HunterClaw 当前实现**: `src/agents/agent_instance.cc:524-526`
- **对齐说明文档**: `WHY_ASSISTANT_DELTA_NOT_EMIT_LIFECYCLE.md`

---

**结论**：当前的"仅日志"实现是**过度简化**的，严格对齐 OpenClaw 需要实现完整的 Buffer + Block Chunking + WebSocket 推送机制。
