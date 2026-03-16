# OpenClaw Sessions 目录消息存储机制完全解析

## 🎯 核心发现

**OpenClaw sessions 目录里确实只存储工具输出和模型最终输出，但并不是"没有模型输出数据"！**

关键理解：**Assistant 消息一定会保存，但保存的内容取决于消息的结构。**

---

## 📦 消息存储完整机制

### 1. 消息保存入口

所有消息通过 `SessionManager.appendMessage()` 保存到 `.jsonl` 文件。

**关键守卫**: `session-tool-result-guard.ts:172`

```typescript
const guardedAppend = (message: AgentMessage) => {
    let nextMessage = message;
    const role = (message as { role?: unknown }).role;
    
    // ✅ Assistant 消息会被处理
    if (role === "assistant") {
        const sanitized = sanitizeToolCallInputs([message], {
            allowedToolNames: opts?.allowedToolNames,
        });
        
        // 如果 sanitize 后消息为空，才会被丢弃
        if (sanitized.length === 0) {
            if (pendingState.shouldFlushForSanitizedDrop()) {
                flushPendingToolResults();
            }
            return undefined;  // ⚠️ 仅在这种情况下不保存
        }
        nextMessage = sanitized[0];
    }
    
    // ✅ Tool result 消息会被保存
    if (nextRole === "toolResult") {
        // ... 处理逻辑 ...
        return originalAppend(persisted as never);
    }
    
    // ✅ 最终所有消息（包括 assistant）都会调用 originalAppend
    const finalMessage = applyBeforeWriteHook(persistMessage(nextMessage));
    if (!finalMessage) {
        return undefined;
    }
    return originalAppend(finalMessage as never);  // ← Assistant 消息在这里保存
};
```

---

## 🔍 消息保存逻辑详解

### Assistant 消息的三种情况

#### 情况 1: 纯文本回复（一定会保存）

```json
{
    "role": "assistant",
    "content": [
        {
            "type": "text",
            "text": "这是我的回答"
        }
    ]
}
```

**保存**: ✅ **会保存**  
**原因**: 经过 `sanitizeToolCallInputs()` 后，内容完整保留。

---

#### 情况 2: 工具调用消息（一定会保存）

```json
{
    "role": "assistant",
    "content": [
        {
            "type": "thinking",
            "thinking": "我需要使用 read_file 工具"
        },
        {
            "type": "tool_use",
            "id": "call_abc123",
            "name": "read_file",
            "input": {
                "filePath": "/path/to/file.cc"
            }
        }
    ]
}
```

**保存**: ✅ **会保存**  
**原因**: 
1. `tool_use` 块被保留
2. `thinking` 块也会保留（可选，取决于 reasoningLevel）

---

#### 情况 3: 完整的思考 + 工具 + 文本消息（一定会保存）

```json
{
    "role": "assistant",
    "content": [
        {
            "type": "thinking",
            "thinking": "用户想要查看文件"
        },
        {
            "type": "tool_use",
            "id": "call_abc123",
            "name": "read_file",
            "input": { "filePath": "/path/to/file.cc" }
        },
        {
            "type": "text",
            "text": "我已经读取了文件内容..."
        }
    ]
}
```

**保存**: ✅ **会保存完整的 content 数组**  
**原因**: 所有块都是有效内容，不会被过滤。

---

### Tool Result 消息（一定会保存）

```json
{
    "role": "toolResult",
    "content": [
        {
            "type": "tool_result",
            "tool_use_id": "call_abc123",
            "content": "#include <iostream>\nint main() { ... }"
        }
    ],
    "toolName": "read_file"
}
```

**保存**: ✅ **会保存**（可能会被截断）  
**截断逻辑**: `session-tool-result-guard.ts:198`

```typescript
// Apply hard size cap before persistence
const capped = capToolResultSize(persistMessage(normalizedToolResult));
```

**默认限制**: `HARD_MAX_TOOL_RESULT_CHARS`（定义在 `tool-result-truncation.ts`）

---

## ⚠️ 什么情况下 Assistant 消息会被丢弃？

### 情况 A: 消息完全为空

```typescript
if (sanitized.length === 0) {
    // 丢弃
    return undefined;
}
```

**示例**:
```json
{
    "role": "assistant",
    "content": []  // ← 空内容
}
```

---

### 情况 B: Tool call 被过滤后消息为空

```typescript
// 如果 tool_use 不在 allowedToolNames 中
if (!hasToolCallName(block, allowedToolNames)) {
    droppedToolCalls += 1;
    continue;  // ← 跳过这个块
}
```

**示例**:
```json
{
    "role": "assistant",
    "content": [
        {
            "type": "tool_use",
            "name": "unknown_tool"  // ← 不在允许列表中
        }
    ]
}
```

**结果**: 如果所有 `tool_use` 都被过滤，且没有其他内容块，消息会被丢弃。

---

### 情况 C: beforeMessageWriteHook 阻止

```typescript
const applyBeforeWriteHook = (msg: AgentMessage): AgentMessage | null => {
    if (!beforeWrite) {
        return msg;
    }
    const result = beforeWrite({ message: msg });
    if (result?.block) {
        return null;  // ← 阻止保存
    }
    return msg;
};
```

**用途**: 插件可以通过 hook 阻止某些消息保存。

---

## 📂 Session 文件格式 (.jsonl)

### 完整对话示例

```jsonl
{"version":"v1","timestamp":1710000000000}
{"role":"user","content":"请读取 /path/to/file.cc"}
{"role":"assistant","content":[{"type":"thinking","thinking":"用户想要查看文件"},{"type":"tool_use","id":"call_abc123","name":"read_file","input":{"filePath":"/path/to/file.cc"}}]}
{"role":"toolResult","content":[{"type":"tool_result","tool_use_id":"call_abc123","content":"#include <iostream>\nint main() { return 0; }"}],"toolName":"read_file"}
{"role":"assistant","content":[{"type":"text","text":"我已经读取了文件内容，这是一个简单的 C++ 程序..."}]}
```

**逐行解析**:

1. **Header**: 版本和时间戳
2. **User**: 用户输入
3. **Assistant (Tool Call)**: 模型决定调用工具
   - ✅ **包含 thinking 块**（推理过程）
   - ✅ **包含 tool_use 块**（工具调用）
4. **Tool Result**: 工具执行结果
   - ✅ **包含完整输出**
5. **Assistant (Final)**: 模型最终回复
   - ✅ **包含 text 块**（文本回复）

---

## 🔥 关键对比：为什么感觉"只有工具输出"？

### 视觉误解来源

当你查看 `.jsonl` 文件时，你会看到：

```jsonl
{"role":"toolResult","content":[...],"toolName":"read_file"}  ← 工具输出（大段文本）
{"role":"assistant","content":[{"type":"tool_use",...]]}      ← 模型输出（结构化）
{"role":"assistant","content":[{"type":"text","text":"..."}]} ← 模型输出（文本）
```

**为什么感觉"只有工具输出"？**

1. **Tool result 通常很长**（文件内容、命令输出）
2. **Assistant tool_use 很短**（只是工具调用结构）
3. **Assistant text 可能不长**（简短总结）

**但实际上**:
- ✅ Assistant 工具调用消息已保存
- ✅ Assistant 文本回复也已保存
- ✅ 只是视觉上 tool result 更显眼

---

## 🎨 前端如何聚合展示？

### 前端不是逐条显示 JSONL 的每一行！

**前端聚合机制**:

```typescript
// 1. 读取所有消息
const messages = readSessionFile(sessionFile);

// 2. 按角色分组
const groups = groupMessagesByRole(messages);

// 3. 提取内容块
for (const msg of assistantMessages) {
    const thinking = extractThinking(msg);      // 提取 thinking
    const toolCalls = extractToolCalls(msg);    // 提取 tool_use
    const text = extractText(msg);              // 提取 text
    
    // 4. 条件渲染
    if (showReasoning && thinking) {
        renderThinking(thinking);
    }
    if (toolCalls.length > 0) {
        renderToolCards(toolCalls);
    }
    if (text) {
        renderText(text);
    }
}
```

**UI 效果**:

```
┌─────────────────────────────────┐
│ 🤖 Assistant                     │
│                                 │
│ ╭─────────────────────────────╮ │  ← thinking（可选）
│ │ _Reasoning:_                │ │
│ │ _用户想要查看文件内容。_     │ │
│ ╰─────────────────────────────╯ │
│                                 │
│ ┌─────────────────────────────┐ │  ← tool_use（卡片）
│ │ 📄 read_file — View ✓       │ │
│ │ filePath: /path/to/file.cc  │ │
│ └─────────────────────────────┘ │
│                                 │
│ 我已经读取了文件内容，发现...    │  ← text（最终回复）
└─────────────────────────────────┘
```

---

## 📊 数据流完整图

```
┌─────────────────────────────────────────┐
│ LLM 生成 Assistant 消息                  │
│ {                                       │
│   "role": "assistant",                  │
│   "content": [                          │
│     { "type": "thinking", ... },        │
│     { "type": "tool_use", ... },        │
│     { "type": "text", ... }             │
│   ]                                     │
│ }                                       │
└─────────────────────────────────────────┘
            ↓
┌─────────────────────────────────────────┐
│ guardedAppend (session-tool-result-guard)│
│ - sanitizeToolCallInputs()              │
│ - applyBeforeWriteHook()                │
│ - persistMessage()                      │
└─────────────────────────────────────────┘
            ↓
┌─────────────────────────────────────────┐
│ originalAppend (SessionManager)         │
│ - 写入 .jsonl 文件                       │
│ - 每行一个 JSON 对象                     │
└─────────────────────────────────────────┘
            ↓
┌─────────────────────────────────────────┐
│ session.jsonl                           │
│ {"role":"assistant","content":[...]}    │  ← 完整保存
└─────────────────────────────────────────┘
            ↓
┌─────────────────────────────────────────┐
│ 前端读取并聚合                           │
│ - extractThinking()                     │
│ - extractToolCalls()                    │
│ - extractText()                         │
└─────────────────────────────────────────┘
            ↓
┌─────────────────────────────────────────┐
│ UI 渲染                                 │
│ - Reasoning 块（灰色背景）               │
│ - Tool 卡片（卡片样式）                  │
│ - Text 文本（Markdown）                  │
└─────────────────────────────────────────┘
```

---

## 🚀 HunterClaw 对齐建议

### 1. 后端保存逻辑

**当前状态**: ✅ 应该已经通过 emit_lifecycle_event() 保存

**验证检查**:
```cpp
// 确保在以下位置调用 appendMessage:
// 1. Agent 生成工具调用
json assistant_msg;
assistant_msg["role"] = "assistant";
assistant_msg["content"] = json::array();
assistant_msg["content"].push_back({
    {"type", "tool_use"},
    {"id", tool_call_id},
    {"name", tool_name},
    {"input", tool_args}
});
session_manager->appendMessage(assistant_msg);

// 2. Agent 生成最终回复
json final_msg;
final_msg["role"] = "assistant";
final_msg["content"] = json::array();
final_msg["content"].push_back({
    {"type", "text"},
    {"text", response_text}
});
session_manager->appendMessage(final_msg);

// 3. 工具执行完成
json tool_result;
tool_result["role"] = "toolResult";
tool_result["content"] = json::array();
tool_result["content"].push_back({
    {"type", "tool_result"},
    {"tool_use_id", tool_call_id},
    {"content", tool_output}
});
session_manager->appendMessage(tool_result);
```

---

### 2. 前端聚合逻辑

**需要实现**:

```typescript
// 提取逻辑
function extractThinking(message: Message): string | null {
    if (message.role !== "assistant") return null;
    
    const thinkingBlocks = message.content.filter(
        block => block.type === "thinking"
    );
    
    return thinkingBlocks.map(b => b.thinking).join("\n");
}

function extractToolCalls(message: Message): ToolCall[] {
    if (message.role !== "assistant") return [];
    
    return message.content.filter(
        block => block.type === "tool_use"
    );
}

function extractText(message: Message): string | null {
    if (message.role !== "assistant") return null;
    
    const textBlocks = message.content.filter(
        block => block.type === "text"
    );
    
    return textBlocks.map(b => b.text).join("\n");
}

// 渲染逻辑
function renderAssistantMessage(message: Message, showReasoning: boolean) {
    const thinking = extractThinking(message);
    const toolCalls = extractToolCalls(message);
    const text = extractText(message);
    
    return (
        <div className="chat-bubble">
            {showReasoning && thinking && (
                <div className="chat-thinking">
                    <em>Reasoning:</em>
                    {thinking.split('\n').map(line => (
                        <div><em>{line}</em></div>
                    ))}
                </div>
            )}
            {toolCalls.map(call => (
                <ToolCard {...call} />
            ))}
            {text && (
                <div className="chat-text">
                    {renderMarkdown(text)}
                </div>
            )}
        </div>
    );
}
```

---

## 🎉 总结

### OpenClaw 实际保存的内容

| 消息类型 | 保存内容 | 示例 |
|---------|---------|------|
| User | ✅ 完整保存 | `{"role":"user","content":"..."}` |
| **Assistant (Tool Call)** | ✅ **完整保存** | `{"role":"assistant","content":[{"type":"tool_use",...}]}` |
| **Assistant (Thinking)** | ✅ **完整保存** | `{"role":"assistant","content":[{"type":"thinking",...}]}` |
| **Assistant (Text)** | ✅ **完整保存** | `{"role":"assistant","content":[{"type":"text",...}]}` |
| Tool Result | ✅ 保存（可能截断） | `{"role":"toolResult","content":[...]}` |

### 视觉误解的原因

1. **Tool result 通常很长**，视觉上更显眼
2. **Assistant 消息结构化**，看起来不像"输出"
3. **前端聚合展示**，而不是逐行显示

### 正确理解

**OpenClaw sessions 目录包含：**
- ✅ 用户输入
- ✅ **模型工具调用决策**（Assistant tool_use）
- ✅ **模型推理过程**（Assistant thinking，可选）
- ✅ **模型文本回复**（Assistant text）
- ✅ 工具执行结果（Tool result）

**完整的对话历史都保存在 `.jsonl` 文件中，只是需要前端聚合展示！** 🚀
