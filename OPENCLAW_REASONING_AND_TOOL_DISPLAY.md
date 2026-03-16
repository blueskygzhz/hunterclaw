# OpenClaw 前端 Reasoning 和 Tool 执行展示机制

## 📋 核心机制概览

OpenClaw 前端通过 **内容聚合 + 条件渲染** 的方式展示 reasoning 和 tool 执行过程，而不是将每个事件单独展示。

---

## 🔄 Reasoning 展示机制

### 1. **配置层级**

OpenClaw 支持三个 reasoning 级别：

```typescript
// ui/src/ui/views/sessions.ts:61
const REASONING_LEVELS = ["", "off", "on", "stream"] as const;

// 对应功能:
// - "" (inherit): 继承默认配置
// - "off": 不显示 reasoning
// - "on": 显示 reasoning（块模式）
// - "stream": 流式显示 reasoning
```

### 2. **前端获取 Session 配置**

```typescript
// ui/src/ui/views/chat.ts:805
const activeSession = props.sessions?.sessions?.find((row) => row.key === props.sessionKey);
const reasoningLevel = activeSession?.reasoningLevel ?? "off";
const showReasoning = props.showThinking && reasoningLevel !== "off";
```

**关键逻辑**:
- 从 `activeSession` 中获取 `reasoningLevel`
- 结合全局开关 `props.showThinking` 决定是否显示
- **只有 `reasoningLevel !== "off"` 时才显示 reasoning**

---

### 3. **消息内容提取**

```typescript
// ui/src/ui/chat/message-extract.ts:41
export function extractThinking(message: unknown): string | null {
  const m = message as Record<string, unknown>;
  const content = m.content;
  const parts: string[] = [];
  
  // ✅ 提取所有 thinking 类型的 content block
  if (Array.isArray(content)) {
    for (const p of content) {
      const item = p as Record<string, unknown>;
      if (item.type === "thinking" && typeof item.thinking === "string") {
        const cleaned = item.thinking.trim();
        if (cleaned) {
          parts.push(cleaned);
        }
      }
    }
  }
  
  // ✅ 合并所有 thinking 块
  if (parts.length > 0) {
    return parts.join("\n");
  }
  
  // ⚠️ 向后兼容: 支持旧格式的 <think>...</think> 标签
  const rawText = extractRawText(message);
  if (!rawText) {
    return null;
  }
  const matches = [
    ...rawText.matchAll(/<\s*think(?:ing)?\s*>([\s\S]*?)<\s*\/\s*think(?:ing)?\s*>/gi),
  ];
  const extracted = matches.map((m) => (m[1] ?? "").trim()).filter(Boolean);
  return extracted.length > 0 ? extracted.join("\n") : null;
}
```

**支持的格式**:
1. **标准格式** (推荐): `content` 数组中的 `{ type: "thinking", thinking: "..." }`
2. **旧格式** (兼容): `<think>...</think>` 或 `<thinking>...</thinking>` 标签

---

### 4. **Reasoning 格式化**

```typescript
// ui/src/ui/chat/message-extract.ts:111
export function formatReasoningMarkdown(text: string): string {
  const trimmed = text.trim();
  if (!trimmed) {
    return "";
  }
  
  // ✅ 每行都转换为斜体
  const lines = trimmed
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => `_${line}_`);  // Markdown 斜体
  
  // ✅ 添加 "Reasoning:" 标题
  return lines.length ? ["_Reasoning:_", ...lines].join("\n") : "";
}
```

**渲染效果**:
```markdown
_Reasoning:_
_First, I need to understand the user's request._
_Then, I'll search for the relevant files._
_Finally, I'll provide a detailed response._
```

---

### 5. **消息渲染逻辑**

```typescript
// ui/src/ui/chat/grouped-render.ts:620
function renderGroupedMessage(
  message: unknown,
  opts: { isStreaming: boolean; showReasoning: boolean },
  onOpenSidebar?: (content: string) => void,
) {
  const m = message as Record<string, unknown>;
  const role = typeof m.role === "string" ? m.role : "unknown";
  
  // ✅ 提取 text 内容 (不包含 thinking)
  const extractedText = extractTextCached(message);
  
  // ✅ 仅当 showReasoning === true 且 role === "assistant" 时提取 thinking
  const extractedThinking =
    opts.showReasoning && role === "assistant" ? extractThinkingCached(message) : null;
  
  const markdownBase = extractedText?.trim() ? extractedText : null;
  const reasoningMarkdown = extractedThinking ? formatReasoningMarkdown(extractedThinking) : null;
  
  // ...
}
```

---

### 6. **Reasoning UI 渲染**

```typescript
// ui/src/ui/chat/grouped-render.ts:716
return html`
  <div class="${bubbleClasses}">
    ${renderMessageImages(images)}
    ${
      reasoningMarkdown
        ? html`<div class="chat-thinking">${unsafeHTML(
            toSanitizedMarkdownHtml(reasoningMarkdown),
          )}</div>`
        : nothing
    }
    ${
      markdown
        ? html`<div class="chat-text" dir="${detectTextDirection(markdown)}">${unsafeHTML(toSanitizedMarkdownHtml(markdown))}</div>`
        : nothing
    }
    ${hasToolCards ? renderCollapsedToolCards(toolCards, onOpenSidebar) : nothing}
  </div>
`;
```

**渲染顺序**:
1. 图片（如果有）
2. **Reasoning 块** (斜体，灰色背景) ← 只在 `showReasoning === true` 时显示
3. 主要文本内容
4. Tool 卡片（如果有）

---

## 🛠️ Tool 执行展示机制

### 1. **Tool 卡片提取**

```typescript
// ui/src/ui/chat/tool-cards.ts:10
export function extractToolCards(message: unknown): ToolCard[] {
  const m = message as Record<string, unknown>;
  const content = normalizeContent(m.content);
  const cards: ToolCard[] = [];

  // ✅ 提取 tool_call/tool_use 块
  for (const item of content) {
    const kind = (typeof item.type === "string" ? item.type : "").toLowerCase();
    const isToolCall =
      ["toolcall", "tool_call", "tooluse", "tool_use"].includes(kind) ||
      (typeof item.name === "string" && item.arguments != null);
    
    if (isToolCall) {
      cards.push({
        kind: "call",
        name: (item.name as string) ?? "tool",
        args: coerceArgs(item.arguments ?? item.args),
      });
    }
  }

  // ✅ 提取 tool_result 块
  for (const item of content) {
    const kind = (typeof item.type === "string" ? item.type : "").toLowerCase();
    if (kind !== "toolresult" && kind !== "tool_result") {
      continue;
    }
    const text = extractToolText(item);
    const name = typeof item.name === "string" ? item.name : "tool";
    cards.push({ kind: "result", name, text });
  }

  return cards;
}
```

**提取的信息**:
- **Tool Call**: `{ kind: "call", name: "read_file", args: { filePath: "..." } }`
- **Tool Result**: `{ kind: "result", name: "read_file", text: "File contents..." }`

---

### 2. **Tool 卡片 UI 组件**

```typescript
// ui/src/ui/chat/tool-cards.ts:51
export function renderToolCardSidebar(card: ToolCard, onOpenSidebar?: (content: string) => void) {
  const display = resolveToolDisplay({ name: card.name, args: card.args });
  const detail = formatToolDetail(display);
  const hasText = Boolean(card.text?.trim());

  const canClick = Boolean(onOpenSidebar);
  const handleClick = canClick
    ? () => {
        if (hasText) {
          onOpenSidebar!(formatToolOutputForSidebar(card.text!));
          return;
        }
        const info = `## ${display.label}\n\n${
          detail ? `**Command:** \`${detail}\`\n\n` : ""
        }*No output — tool completed successfully.*`;
        onOpenSidebar!(info);
      }
    : undefined;

  // ✅ 判断是否折叠显示
  const isShort = hasText && (card.text?.length ?? 0) <= TOOL_INLINE_THRESHOLD;
  const showCollapsed = hasText && !isShort;
  const showInline = hasText && isShort;
  const isEmpty = !hasText;

  return html`
    <div
      class="chat-tool-card ${canClick ? "chat-tool-card--clickable" : ""}"
      @click=${handleClick}
      role=${canClick ? "button" : nothing}
    >
      <div class="chat-tool-card__header">
        <div class="chat-tool-card__title">
          <span class="chat-tool-card__icon">${icons[display.icon]}</span>
          <span>${display.label}</span>
        </div>
        ${
          canClick
            ? html`<span class="chat-tool-card__action">${hasText ? "View" : ""} ${icons.check}</span>`
            : nothing
        }
      </div>
      ${detail ? html`<div class="chat-tool-card__detail">${detail}</div>` : nothing}
      ${
        isEmpty
          ? html`<div class="chat-tool-card__status-text muted">Completed</div>`
          : nothing
      }
      ${
        showCollapsed
          ? html`<div class="chat-tool-card__preview mono">${getTruncatedPreview(card.text!)}</div>`
          : nothing
      }
      ${showInline ? html`<div class="chat-tool-card__inline mono">${card.text}</div>` : nothing}
    </div>
  `;
}
```

**UI 状态**:
1. **空输出**: 显示 "Completed" ✅
2. **短输出** (< 阈值): 内联显示完整内容
3. **长输出** (> 阈值): 显示预览 + "View" 按钮
4. **点击卡片**: 在侧边栏打开完整输出

---

### 3. **Tool 消息折叠**

```typescript
// ui/src/ui/chat/grouped-render.ts:676
${
  isToolMessage
    ? html`
      <details class="chat-tool-msg-collapse">
        <summary class="chat-tool-msg-summary">
          <span class="chat-tool-msg-summary__icon">${icons.zap}</span>
          <span class="chat-tool-msg-summary__label">Tool output</span>
          ${
            toolSummaryLabel
              ? html`<span class="chat-tool-msg-summary__names">${toolSummaryLabel}</span>`
              : toolPreview
                ? html`<span class="chat-tool-msg-summary__preview">${toolPreview}</span>`
                : nothing
          }
        </summary>
        <div class="chat-tool-msg-body">
          ${renderMessageImages(images)}
          ${
            reasoningMarkdown
              ? html`<div class="chat-thinking">${unsafeHTML(
                  toSanitizedMarkdownHtml(reasoningMarkdown),
                )}</div>`
              : nothing
          }
          ${markdown ? html`<div class="chat-text">${unsafeHTML(toSanitizedMarkdownHtml(markdown))}</div>` : nothing}
          ${hasToolCards ? renderCollapsedToolCards(toolCards, onOpenSidebar) : nothing}
        </div>
      </details>
    `
    : html`...`
}
```

**折叠效果**:
- **折叠状态**: 显示 `⚡ Tool output — tool1, tool2, tool3`
- **展开状态**: 显示完整的工具输出 + 工具卡片

---

## 🎯 完整的消息渲染流程

```
┌────────────────────────────────────────────────────┐
│ 1. 用户发送消息                                      │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 2. Agent 处理 → 返回消息对象                         │
│    {                                               │
│      role: "assistant",                            │
│      content: [                                    │
│        { type: "thinking", thinking: "分析中..." }, │
│        { type: "tool_use", name: "read_file", ... },│
│        { type: "text", text: "找到文件..." }        │
│      ]                                             │
│    }                                               │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 3. 前端接收 → renderGroupedMessage()                │
│    - 检查 showReasoning 配置                        │
│    - extractThinkingCached() 提取 thinking         │
│    - extractTextCached() 提取 text                 │
│    - extractToolCards() 提取工具卡片                │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 4. 条件渲染                                         │
│    ✅ showReasoning === true                       │
│       → 渲染 <div class="chat-thinking">           │
│           _Reasoning:_                             │
│           _分析中..._                               │
│         </div>                                     │
│                                                    │
│    ✅ 总是渲染                                      │
│       → <div class="chat-text">                    │
│           找到文件...                               │
│         </div>                                     │
│                                                    │
│    ✅ hasToolCards === true                        │
│       → <div class="chat-tool-card">               │
│           📄 read_file — View ✓                    │
│         </div>                                     │
└────────────────────────────────────────────────────┘
```

---

## 📊 消息结构示例

### 完整的 Assistant 消息

```json
{
  "role": "assistant",
  "content": [
    {
      "type": "thinking",
      "thinking": "用户想要查看文件内容。我需要使用 read_file 工具。"
    },
    {
      "type": "tool_use",
      "id": "call_abc123",
      "name": "read_file",
      "arguments": {
        "filePath": "/path/to/file.cc"
      }
    },
    {
      "type": "text",
      "text": "我已经读取了文件内容，发现以下关键代码..."
    }
  ],
  "timestamp": 1710000000000
}
```

### Tool Result 消息

```json
{
  "role": "tool",
  "content": [
    {
      "type": "tool_result",
      "tool_call_id": "call_abc123",
      "name": "read_file",
      "text": "#include <iostream>\nint main() { ... }"
    }
  ],
  "timestamp": 1710000001000
}
```

---

## 🎨 UI 效果对比

### Reasoning OFF (`reasoningLevel: "off"`)

```
┌─────────────────────────────────┐
│ 🤖 Assistant                     │
│                                 │
│ 我已经读取了文件内容，发现以      │
│ 下关键代码...                    │
│                                 │
│ ┌─────────────────────────────┐ │
│ │ 📄 read_file — View ✓       │ │
│ │ filePath: /path/to/file.cc  │ │
│ └─────────────────────────────┘ │
└─────────────────────────────────┘
```

### Reasoning ON (`reasoningLevel: "on"` 或 `"stream"`)

```
┌─────────────────────────────────┐
│ 🤖 Assistant                     │
│                                 │
│ ╭─────────────────────────────╮ │
│ │ _Reasoning:_                │ │  ← 斜体，灰色背景
│ │ _用户想要查看文件内容。_     │ │
│ │ _我需要使用 read_file 工具。_│ │
│ ╰─────────────────────────────╯ │
│                                 │
│ 我已经读取了文件内容，发现以      │
│ 下关键代码...                    │
│                                 │
│ ┌─────────────────────────────┐ │
│ │ 📄 read_file — View ✓       │ │
│ │ filePath: /path/to/file.cc  │ │
│ └─────────────────────────────┘ │
└─────────────────────────────────┘
```

---

## 🔥 关键设计原则

### 1. **内容聚合，而非事件流**
- ❌ **不是**: 收到一个事件就显示一个 UI 元素
- ✅ **而是**: 聚合所有内容块，统一渲染

### 2. **条件渲染**
- Reasoning 块仅在 `showReasoning === true` 时显示
- Tool 卡片总是显示（但可折叠）
- 空内容不显示（返回 `nothing`）

### 3. **用户体验优化**
- **Reasoning**: 斜体 + 灰色背景，视觉区分
- **Tool 卡片**: 点击展开完整输出
- **Tool 消息**: `<details>` 折叠，避免干扰
- **流式更新**: 实时追加内容（`isStreaming: true`）

### 4. **向后兼容**
- 支持旧格式 `<think>...</think>` 标签
- 支持多种 tool 类型命名（`toolCall`, `tool_use`, etc.）

---

## 🚀 HunterClaw 实现建议

### 后端改动

#### 1. 发送 Thinking Content Block

```cpp
// src/agents/agent_instance.cc
json message;
message["role"] = "assistant";
message["content"] = json::array();

// ✅ 添加 thinking 块
if (has_reasoning) {
    json thinking_block;
    thinking_block["type"] = "thinking";
    thinking_block["thinking"] = reasoning_text;
    message["content"].push_back(thinking_block);
}

// ✅ 添加 tool_use 块
if (has_tool_call) {
    json tool_block;
    tool_block["type"] = "tool_use";
    tool_block["id"] = tool_call_id;
    tool_block["name"] = tool_name;
    tool_block["arguments"] = tool_args;
    message["content"].push_back(tool_block);
}

// ✅ 添加 text 块
json text_block;
text_block["type"] = "text";
text_block["text"] = response_text;
message["content"].push_back(text_block);
```

#### 2. 支持 Session 级 reasoningLevel 配置

```cpp
// Gateway 存储 session 配置
struct SessionConfig {
    std::string reasoning_level = "off";  // "off" | "on" | "stream"
    // ...
};
```

---

### 前端改动

#### 1. 获取 Session 配置

```typescript
const activeSession = sessions.find(s => s.key === sessionKey);
const reasoningLevel = activeSession?.reasoningLevel ?? "off";
const showReasoning = reasoningLevel !== "off";
```

#### 2. 提取 Thinking 内容

```typescript
function extractThinking(message: any): string | null {
  const content = message.content;
  if (!Array.isArray(content)) return null;
  
  const thinkingBlocks = content
    .filter(block => block.type === "thinking")
    .map(block => block.thinking)
    .filter(Boolean);
  
  return thinkingBlocks.length > 0 ? thinkingBlocks.join("\n") : null;
}
```

#### 3. 渲染 Reasoning 块

```typescript
{showReasoning && reasoning && (
  <div className="chat-thinking">
    <em>Reasoning:</em>
    {reasoning.split('\n').map((line, i) => (
      <div key={i}><em>{line}</em></div>
    ))}
  </div>
)}
```

#### 4. 渲染 Tool 卡片

```typescript
{toolCards.map(card => (
  <div className="chat-tool-card" onClick={() => openToolOutput(card)}>
    <div className="chat-tool-card__header">
      <span>{card.icon}</span>
      <span>{card.name}</span>
      <span>✓</span>
    </div>
    {card.detail && (
      <div className="chat-tool-card__detail">{card.detail}</div>
    )}
    {card.preview && (
      <div className="chat-tool-card__preview">{card.preview}</div>
    )}
  </div>
))}
```

---

## 📊 对齐度总结

| 组件 | OpenClaw 机制 | HunterClaw 状态 |
|------|--------------|----------------|
| Reasoning 配置 | Session 级 `reasoningLevel` | ❌ 缺失 |
| Thinking 块提取 | `extractThinkingCached()` | ❌ 缺失 |
| Thinking UI 渲染 | `<div class="chat-thinking">` | ❌ 缺失 |
| Tool 卡片提取 | `extractToolCards()` | ❌ 缺失 |
| Tool 卡片渲染 | `renderToolCardSidebar()` | ❌ 缺失 |
| 条件渲染逻辑 | `showReasoning && role === "assistant"` | ❌ 缺失 |
| 内容聚合 | 单个 message 包含多个 content 块 | ⚠️ 部分实现 |

---

## 🎉 总结

### OpenClaw 的核心设计

1. **内容聚合**: 一个消息包含多个 content 块（thinking, tool_use, text）
2. **条件渲染**: 根据 `reasoningLevel` 配置决定是否显示 thinking
3. **工具卡片**: 自动提取 tool_use/tool_result 并渲染为卡片
4. **用户体验**: 折叠、预览、点击展开、侧边栏查看

### HunterClaw 需要实现

1. ✅ **后端**: 发送标准 content 块（thinking, tool_use, text）
2. ✅ **后端**: 支持 Session 级 `reasoningLevel` 配置
3. ❌ **前端**: 实现 thinking 提取和渲染
4. ❌ **前端**: 实现 tool 卡片提取和渲染
5. ❌ **前端**: 实现条件渲染逻辑
6. ❌ **前端**: 添加 CSS 样式（thinking 斜体，卡片样式）

**完成后，HunterClaw 将提供与 OpenClaw 相同的专业 UI 体验！** 🚀
