# OpenClaw Block Reply 配置详解

## 📖 概述

OpenClaw 通过 `agentDefaults` 配置部分提供了强大的 **Block Reply (块回复)** 功能，用于在工具调用前分段输出 AI 响应。

## 🎯 核心配置项

### 1. blockStreamingDefault

**作用**: 全局开关，控制块流式输出的默认行为

**类型**: `"off" | "on"`

**默认值**: 未明确指定时根据上下文决定

**配置位置**: `agentDefaults.blockStreamingDefault`

```yaml
agentDefaults:
  blockStreamingDefault: on  # 默认启用块流式输出
```

---

### 2. blockStreamingBreak ⭐ 核心

**作用**: 控制块输出的边界时机

**类型**: `"text_end" | "message_end"`

**配置位置**: `agentDefaults.blockStreamingBreak`

**详细说明**:

- **`text_end`** (默认推荐):
  - 在每个助手文本内容块结束时触发
  - **在工具调用之前**立即刷新缓冲区
  - 适合实时展示 AI 思考过程

- **`message_end`**:
  - 在整个助手消息结束时触发
  - 可能包含工具调用块
  - 适合需要完整消息后再输出的场景

```yaml
agentDefaults:
  # 推荐配置: 在文本块结束时立即输出
  blockStreamingBreak: text_end
  
  # 或者: 在整个消息结束时输出
  # blockStreamingBreak: message_end
```

**工作流程**:

```
text_end 模式:
AI 文本输出 → 达到边界 (工具调用前) → 触发 on_block_reply → 刷新缓冲区

message_end 模式:
AI 文本 + 工具调用 → 整个消息结束 → 触发 on_block_reply → 输出完整内容
```

---

### 3. blockStreamingChunk

**作用**: 软分块配置（字符数阈值和分块偏好）

**类型**: `BlockStreamingChunkConfig`

**配置位置**: `agentDefaults.blockStreamingChunk`

**字段说明**:

```typescript
type BlockStreamingChunkConfig = {
  minChars?: number;              // 最小字符数 (默认: 100)
  maxChars?: number;              // 最大字符数 (默认: 1000)
  breakPreference?: "paragraph" | "newline" | "sentence";
  // 分块偏好:
  //   - paragraph: 在段落边界分块 (推荐)
  //   - newline: 在换行符分块
  //   - sentence: 在句子边界分块
};
```

**配置示例**:

```yaml
agentDefaults:
  blockStreamingChunk:
    minChars: 50        # 最少累积 50 字符才触发
    maxChars: 500       # 最多累积 500 字符就强制触发
    breakPreference: paragraph  # 优先在段落边界分块
```

**分块逻辑**:

```
累积缓冲区字符 < minChars  → 继续累积
minChars ≤ 累积字符 < maxChars → 寻找分块边界 (paragraph/newline/sentence)
累积字符 ≥ maxChars  → 强制刷新
```

---

### 4. blockStreamingCoalesce

**作用**: 块回复合并配置（在发送前合并流式块）

**类型**: `BlockStreamingCoalesceConfig`

**配置位置**: `agentDefaults.blockStreamingCoalesce`

**字段说明**:

```typescript
type BlockStreamingCoalesceConfig = {
  minChars?: number;  // 最小字符数才合并
  maxChars?: number;  // 最大字符数限制
  idleMs?: number;    // 空闲等待时间 (毫秒) 才刷新
};
```

**配置示例**:

```yaml
agentDefaults:
  blockStreamingCoalesce:
    minChars: 100       # 至少 100 字符才合并
    maxChars: 1000      # 最多合并 1000 字符
    idleMs: 200         # 空闲 200ms 后刷新
```

**工作原理**:

```
流式块 1 → 累积
流式块 2 → 累积
...
空闲 200ms 或达到 maxChars → 刷新合并内容
```

**使用场景**: 减少网络请求，批量发送多个小块

---

## 📋 完整配置示例

### 示例 1: 默认推荐配置

```yaml
agentDefaults:
  # 启用块流式输出
  blockStreamingDefault: on
  
  # 在文本块结束时立即输出 (工具调用前)
  blockStreamingBreak: text_end
  
  # 软分块: 在段落边界分块，50-500 字符
  blockStreamingChunk:
    minChars: 50
    maxChars: 500
    breakPreference: paragraph
  
  # 合并配置: 空闲 250ms 后刷新
  blockStreamingCoalesce:
    minChars: 100
    maxChars: 1000
    idleMs: 250
```

### 示例 2: 低延迟配置 (快速响应)

```yaml
agentDefaults:
  blockStreamingDefault: on
  blockStreamingBreak: text_end
  
  blockStreamingChunk:
    minChars: 30        # 更小的阈值 → 更快输出
    maxChars: 200
    breakPreference: newline
  
  blockStreamingCoalesce:
    minChars: 50
    maxChars: 300
    idleMs: 100         # 更短的空闲时间
```

### 示例 3: 高质量输出配置 (更完整的段落)

```yaml
agentDefaults:
  blockStreamingDefault: on
  blockStreamingBreak: text_end
  
  blockStreamingChunk:
    minChars: 200       # 更大的阈值 → 更完整的段落
    maxChars: 1500
    breakPreference: paragraph
  
  blockStreamingCoalesce:
    minChars: 200
    maxChars: 2000
    idleMs: 500         # 等待更长时间
```

### 示例 4: 关闭块流式输出

```yaml
agentDefaults:
  blockStreamingDefault: off  # 完全关闭
```

---

## 🔄 与 HunterClaw 对应关系

| OpenClaw 配置 | HunterClaw 配置 | 说明 |
|--------------|----------------|------|
| `blockStreamingDefault` | `ReactConfig.block_reply.enabled` | 全局开关 |
| `blockStreamingBreak` | `ReactConfig.block_reply.break_mode` | 分块边界 |
| `blockStreamingChunk.minChars` | `ReactConfig.block_reply.min_chars` | 最小字符数 |
| `blockStreamingChunk.maxChars` | `ReactConfig.block_reply.max_chars` | 最大字符数 |
| `blockStreamingChunk.breakPreference` | `ReactConfig.block_reply.break_preference` | 分块偏好 |
| `blockStreamingCoalesce.idleMs` | - | HunterClaw 暂无直接对应 |

---

## 🎯 HunterClaw 等效配置

```cpp
// OpenClaw YAML 配置:
// agentDefaults:
//   blockStreamingDefault: on
//   blockStreamingBreak: text_end
//   blockStreamingChunk:
//     minChars: 50
//     maxChars: 500
//     breakPreference: paragraph

// HunterClaw C++ 配置:
react::ReactConfig config;

// 1. 启用块回复
config.block_reply.enabled = true;

// 2. 分块边界: text_end
config.block_reply.break_mode = react::ReactConfig::BlockReply::BreakMode::TextEnd;

// 3. 字符数阈值
config.block_reply.min_chars = 50;
config.block_reply.max_chars = 500;

// 4. 分块偏好: paragraph
config.block_reply.break_preference = 
    react::ReactConfig::BlockReply::BreakPreference::Paragraph;

auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
```

---

## 📊 配置影响对比

### 不同 `blockStreamingBreak` 的影响

```
输入: "帮我分析这个文件"

text_end 模式:
[块 1] "让我先读取文件内容..."
[工具调用] read_file("example.txt")
[块 2] "文件内容分析如下: ..."

message_end 模式:
[完整消息] "让我先读取文件内容... [工具调用: read_file] 文件内容分析如下: ..."
```

### 不同字符数阈值的影响

```
minChars=50, maxChars=200:
[块 1] "这是第一段话，大约 100 字符。"
[块 2] "这是第二段话，又是 100 字符。"
→ 输出频率: 高 (更实时)

minChars=200, maxChars=1000:
[块 1] "这是第一段话... 第二段话... 第三段话... (共 500 字符)"
→ 输出频率: 低 (更批量)
```

---

## 🔍 实际应用场景

### 场景 1: Telegram/Discord 聊天机器人

```yaml
agentDefaults:
  blockStreamingDefault: on
  blockStreamingBreak: text_end
  blockStreamingChunk:
    minChars: 100
    maxChars: 800
    breakPreference: paragraph
  blockStreamingCoalesce:
    idleMs: 300
```

**原因**: 
- 用户期望实时响应 (`text_end`)
- 段落分块可读性好 (`paragraph`)
- 合并小块减少消息数量 (`coalesce`)

---

### 场景 2: Web 应用流式输出

```yaml
agentDefaults:
  blockStreamingDefault: on
  blockStreamingBreak: text_end
  blockStreamingChunk:
    minChars: 30
    maxChars: 300
    breakPreference: newline
  blockStreamingCoalesce:
    idleMs: 100
```

**原因**:
- 快速响应提升体验 (小阈值)
- 换行分块适合代码输出
- 短空闲时间保证流畅度

---

### 场景 3: Email/报告生成

```yaml
agentDefaults:
  blockStreamingDefault: off  # 或使用 message_end
```

**原因**:
- 不需要实时输出
- 等待完整内容再发送

---

## 🛠️ 调试和监控

### 查看实际触发情况

在 HunterClaw 中:

```cpp
react::StreamCallbacks callbacks;

callbacks.on_block_reply = [](const std::string& block) {
    std::cout << "\n[块回复触发]"
              << " 长度: " << block.size() << " 字符"
              << " 时间: " << get_timestamp() << "\n"
              << block << "\n";
};
```

### 性能监控

```cpp
size_t block_count = 0;
size_t total_chars = 0;

callbacks.on_block_reply = [&block_count, &total_chars](const std::string& block) {
    block_count++;
    total_chars += block.size();
    
    std::cout << "[统计] 块数: " << block_count 
              << " 总字符: " << total_chars 
              << " 平均: " << (total_chars / block_count) << "\n";
};
```

---

## 📚 参考资料

### OpenClaw 源码位置

- **类型定义**: `src/config/types.agent-defaults.ts` (行 192-206)
- **基础类型**: `src/config/types.base.ts` (行 22-32)
- **Schema 验证**: `src/config/zod-schema.agent-defaults.ts`
- **实现逻辑**: `src/agents/pi-embedded-subscribe.handlers.messages.ts`

### HunterClaw 对应实现

- **类型定义**: `include/hunterclaw/react/react_engine.h` (行 116-131)
- **实现代码**: `src/react/react_engine.cc`
- **完整文档**: `docs/STREAMING_CALLBACKS_GUIDE.md`
- **示例代码**: `examples/streaming_callbacks_demo.cc`

---

## ✅ 快速决策表

| 需求 | blockStreamingBreak | minChars | maxChars | breakPreference |
|------|---------------------|----------|----------|-----------------|
| 最快响应 | `text_end` | 30 | 200 | `newline` |
| 平衡体验 | `text_end` | 100 | 500 | `paragraph` |
| 高质量输出 | `text_end` | 200 | 1000 | `paragraph` |
| 批量处理 | `message_end` | 500 | 2000 | `paragraph` |
| 关闭流式 | - | - | - | - |

---

## 🎉 总结

OpenClaw 的 `blockStreamingBreak` 配置核心价值：

1. **`text_end`**: 实时体验，工具调用前立即输出 ⭐
2. **`message_end`**: 完整输出，包含工具调用后的内容
3. **`blockStreamingChunk`**: 细粒度控制分块行为
4. **`blockStreamingCoalesce`**: 优化网络传输效率

**推荐默认配置**:

```yaml
agentDefaults:
  blockStreamingDefault: on
  blockStreamingBreak: text_end
  blockStreamingChunk:
    minChars: 100
    maxChars: 500
    breakPreference: paragraph
  blockStreamingCoalesce:
    minChars: 100
    maxChars: 1000
    idleMs: 250
```

这个配置在 **响应速度、可读性、网络效率** 之间取得了最佳平衡！🚀
