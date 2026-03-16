# OpenClaw `verboseDefault` 和 `reasoningDefault` 代码处理流程

本文档详细说明 OpenClaw 中 `verboseDefault: "full"` 和 `reasoningDefault: "on"` 两个配置的完整代码处理逻辑。

---

## 目录

1. [配置定义](#1-配置定义)
2. [verboseDefault 处理流程](#2-verbosedefault-处理流程)
3. [reasoningDefault 处理流程](#3-reasoningdefault-处理流程)
4. [HunterClaw 对齐建议](#4-hunterclaw-对齐建议)

---

## 1. 配置定义

### 1.1 配置文件位置

**文件**: `src/config/types.agent-defaults.ts`

```typescript
export type AgentDefaultsConfig = {
  // ... 其他配置 ...
  
  /** Default verbose level when no /verbose directive is present. */
  verboseDefault?: "off" | "on" | "full";
  
  // ... 其他配置 ...
};
```

**文件**: `src/config/zod-schema.agent-defaults.ts`

```typescript
export const AgentDefaultsSchema = z.object({
  // ... 其他配置 ...
  
  verboseDefault: z.union([
    z.literal("off"), 
    z.literal("on"), 
    z.literal("full")
  ]).optional(),
  
  // ... 其他配置 ...
});
```

---

## 2. verboseDefault 处理流程

### 2.1 配置解析（3 层优先级）

**文件**: `src/auto-reply/reply/get-reply-directives.ts:356-359`

```typescript
// 优先级 1: 消息中的内联指令（/verbose full）
// 优先级 2: 会话级别覆盖（用户发送 /verbose full 设置会话）
// 优先级 3: 全局默认配置（verboseDefault）
const resolvedVerboseLevel =
  directives.verboseLevel ??                                    // 优先级 1
  (sessionEntry?.verboseLevel as VerboseLevel | undefined) ??  // 优先级 2
  (agentCfg?.verboseDefault as VerboseLevel | undefined);      // 优先级 3 ✅
```

**解析顺序**:
1. **内联指令**: 消息中包含 `/verbose full` → 只对该消息生效
2. **会话覆盖**: 用户单独发送 `/verbose full` → 对整个会话生效
3. **全局默认**: 配置文件中 `verboseDefault: "full"` → 对所有会话生效

---

### 2.2 工具输出控制（Gateway 层）

**文件**: `src/gateway/server-chat.ts:501-521`

```typescript
// 解析工具事件的 verbose 级别
const resolveToolVerboseLevel = (runId: string, sessionKey?: string) => {
  // 1. 从运行时上下文获取
  const runContext = getAgentRunContext(runId);
  const runVerbose = normalizeVerboseLevel(runContext?.verboseLevel);
  if (runVerbose) {
    return runVerbose;
  }
  
  // 2. 从会话数据获取
  if (!sessionKey) return "off";
  try {
    const { cfg, entry } = loadSessionEntry(sessionKey);
    const sessionVerbose = normalizeVerboseLevel(entry?.verboseLevel);
    if (sessionVerbose) {
      return sessionVerbose;
    }
    
    // 3. 从全局配置获取 ✅
    const defaultVerbose = normalizeVerboseLevel(cfg.agents?.defaults?.verboseDefault);
    return defaultVerbose ?? "off";
  } catch {
    return "off";
  }
};
```

---

### 2.3 工具输出过滤（Gateway 广播）

**文件**: `src/gateway/server-chat.ts:539-551`

```typescript
// 根据 verbose 级别过滤工具输出
const isToolEvent = evt.stream === "tool";
const toolVerbose = isToolEvent ? resolveToolVerboseLevel(evt.runId, sessionKey) : "off";

// 构建工具 payload: 
// - verbose !== "full" 时，删除 result/partialResult
// - verbose === "full" 时，保留完整输出 ✅
const toolPayload =
  isToolEvent && toolVerbose !== "full"
    ? (() => {
        const data = evt.data ? { ...evt.data } : {};
        delete data.result;         // 删除工具输出 ❌
        delete data.partialResult;  // 删除增量输出 ❌
        return sessionKey
          ? { ...eventForClients, sessionKey, data }
          : { ...eventForClients, data };
      })()
    : agentPayload;  // 保留完整输出 ✅
```

**关键逻辑**:
- `verbose === "off"`: 不显示工具调用 ❌
- `verbose === "on"`: 只显示工具名称和参数 ⚠️
- `verbose === "full"`: 显示完整工具输出 ✅

---

### 2.4 TUI 显示层（终端 UI）

**文件**: `src/tui/tui-event-handlers.ts:286-316`

```typescript
// TUI 处理工具事件
if (evt.stream === "tool") {
  const verbose = state.sessionInfo.verboseLevel ?? "off";
  const allowToolEvents = verbose !== "off";
  const allowToolOutput = verbose === "full";  // ✅ 关键判断
  
  if (!allowToolEvents) {
    return;  // verbose === "off" 时完全跳过 ❌
  }
  
  const data = evt.data ?? {};
  const phase = asString(data.phase, "");
  const toolCallId = asString(data.toolCallId, "");
  const toolName = asString(data.name, "tool");
  
  if (phase === "start") {
    // 显示工具调用开始（verbose === "on" 和 "full" 都显示）
    chatLog.startTool(toolCallId, toolName, data.args);
  } else if (phase === "update") {
    // 显示工具输出增量更新（只在 verbose === "full" 时显示）✅
    if (!allowToolOutput) {
      return;  // verbose === "on" 时跳过 ❌
    }
    chatLog.updateToolResult(toolCallId, data.partialResult, {
      partial: true,
    });
  } else if (phase === "result") {
    // 显示工具最终结果
    if (allowToolOutput) {
      // verbose === "full": 显示完整输出 ✅
      chatLog.updateToolResult(toolCallId, data.result, {
        isError: Boolean(data.isError),
      });
    } else {
      // verbose === "on": 只显示成功/失败状态 ⚠️
      chatLog.updateToolResult(toolCallId, { content: [] }, { 
        isError: Boolean(data.isError) 
      });
    }
  }
  tui.requestRender();
  return;
}
```

---

### 2.5 完整流程图（verboseDefault）

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 配置解析                                                    │
│    src/auto-reply/reply/get-reply-directives.ts:356-359       │
├─────────────────────────────────────────────────────────────┤
│ resolvedVerboseLevel =                                        │
│   directives.verboseLevel ??          // 优先级 1: 内联指令   │
│   sessionEntry?.verboseLevel ??       // 优先级 2: 会话覆盖   │
│   agentCfg?.verboseDefault            // 优先级 3: 全局配置 ✅ │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 工具输出控制                                                │
│    src/gateway/server-chat.ts:501-551                         │
├─────────────────────────────────────────────────────────────┤
│ toolVerbose = resolveToolVerboseLevel(runId, sessionKey)     │
│                                                               │
│ if (toolVerbose !== "full") {                                │
│   delete data.result         // 删除工具输出 ❌                │
│   delete data.partialResult  // 删除增量输出 ❌                │
│ }                                                             │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. TUI 显示层                                                  │
│    src/tui/tui-event-handlers.ts:286-316                      │
├─────────────────────────────────────────────────────────────┤
│ const allowToolOutput = (verbose === "full")  ✅              │
│                                                               │
│ if (phase === "start") {                                     │
│   chatLog.startTool(...)      // 显示工具调用                 │
│ }                                                             │
│                                                               │
│ if (phase === "result" && allowToolOutput) {                 │
│   chatLog.updateToolResult(data.result)  // 显示工具输出 ✅    │
│ }                                                             │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 终端输出                                                    │
├─────────────────────────────────────────────────────────────┤
│ 🔧 read_file: /path/to/file.py                               │
│ 📄 工具输出:                                                   │
│ def hello():                                                  │
│     print("Hello, World!")                                    │
│ ✅                                                            │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. reasoningDefault 处理流程

### 3.1 配置解析（无 reasoningDefault 配置）

**注意**: OpenClaw **没有** `reasoningDefault` 配置项！

推理默认行为由**模型能力**决定：

**文件**: `src/auto-reply/reply/get-reply-directives.ts:409-419`

```typescript
// 当用户没有显式设置 reasoning 时，根据模型能力自动启用
// 注意: 这里没有 reasoningDefault 配置 ❌
let resolvedReasoningLevel: ReasoningLevel =
  directives.reasoningLevel ??                              // 优先级 1: 内联指令
  (sessionEntry?.reasoningLevel as ReasoningLevel | undefined) ??  // 优先级 2: 会话覆盖
  "off";  // 优先级 3: 默认关闭

const reasoningExplicitlySet =
  directives.reasoningLevel !== undefined ||
  (sessionEntry?.reasoningLevel !== undefined && sessionEntry?.reasoningLevel !== null);

const thinkingActive = resolvedThinkLevelWithDefault !== "off";

// 如果用户没有设置 reasoning，且 thinking 未激活，
// 则根据模型能力自动启用 reasoning ✅
if (!reasoningExplicitlySet && resolvedReasoningLevel === "off" && !thinkingActive) {
  resolvedReasoningLevel = await modelState.resolveDefaultReasoningLevel();
}
```

---

### 3.2 模型能力检测

**文件**: `src/agents/model-selection.ts:669-681`

```typescript
/** 
 * Default reasoning level when session/directive do not set it: 
 * "on" if model supports reasoning, else "off". 
 */
export function resolveReasoningDefault(params: {
  provider: string;
  model: string;
  catalog?: ModelCatalogEntry[];
}): "on" | "off" {
  const key = modelKey(params.provider, params.model);
  const candidate = params.catalog?.find(
    (entry) =>
      (entry.provider === params.provider && entry.id === params.model) ||
      (entry.provider === key && entry.id === params.model),
  );
  
  // 检查模型目录中的 reasoning 属性 ✅
  return candidate?.reasoning === true ? "on" : "off";
}
```

**关键逻辑**:
- 查找模型目录（`ModelCatalogEntry[]`）
- 检查 `reasoning: true` 属性
- 如果支持推理 → 返回 `"on"` ✅
- 否则 → 返回 `"off"` ❌

---

### 3.3 推理内容输出（Agent 层）

**文件**: `src/agents/pi-embedded-subscribe.handlers.messages.ts:345-425`

```typescript
// 决定是否发送推理内容
const shouldEmitReasoning = Boolean(
  ctx.state.includeReasoning &&      // reasoning 已启用 ✅
  formattedReasoning &&              // 有推理内容
  onBlockReply &&                    // 有 block reply 回调
  formattedReasoning !== ctx.state.lastReasoningSent,  // 内容未重复
);

// 决定推理发送时机
const shouldEmitReasoningBeforeAnswer =
  shouldEmitReasoning && 
  ctx.state.blockReplyBreak === "message_end" &&  // message_end 模式下先发推理
  !addedDuringMessage;

// 发送推理内容的函数
const maybeEmitReasoning = () => {
  if (!shouldEmitReasoning || !formattedReasoning) {
    return;
  }
  ctx.state.lastReasoningSent = formattedReasoning;
  
  // 调用 onBlockReply 发送推理 ✅
  emitBlockReplySafely({ 
    text: formattedReasoning,  // 格式化的推理内容
    isReasoning: true          // 标记为推理内容 ✅
  });
};

// 时序控制
if (shouldEmitReasoningBeforeAnswer) {
  maybeEmitReasoning();  // message_end 模式: 先发推理，再发答案
}

// ... 发送答案内容 ...

if (!shouldEmitReasoningBeforeAnswer) {
  maybeEmitReasoning();  // text_end 模式: 先发答案，再发推理
}

// 流式推理（仅 Telegram）
if (ctx.state.streamReasoning && rawThinking) {
  ctx.emitReasoningStream(rawThinking);  // 流式更新推理内容
}
```

---

### 3.4 推理内容格式化

**文件**: `src/agents/pi-embedded-utils.ts:278`

```typescript
// Show reasoning in italics (cursive) for markdown-friendly surfaces (Discord, etc.).
// 为支持 Markdown 的渠道（Discord 等）显示斜体推理内容

// 推理内容前缀: "Reasoning:\n_<content>_"
// 例如: "Reasoning:\n_I need to analyze the file first..._"
```

---

### 3.5 完整流程图（reasoningDefault）

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 配置解析                                                    │
│    src/auto-reply/reply/get-reply-directives.ts:360-419       │
├─────────────────────────────────────────────────────────────┤
│ resolvedReasoningLevel =                                      │
│   directives.reasoningLevel ??        // 优先级 1: 内联指令   │
│   sessionEntry?.reasoningLevel ??     // 优先级 2: 会话覆盖   │
│   "off"                               // 优先级 3: 默认关闭   │
│                                                               │
│ if (!reasoningExplicitlySet && !thinkingActive) {            │
│   // 根据模型能力自动启用 ✅                                   │
│   resolvedReasoningLevel = modelState.resolveDefaultReasoningLevel() │
│ }                                                             │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 模型能力检测                                                │
│    src/agents/model-selection.ts:669-681                      │
├─────────────────────────────────────────────────────────────┤
│ resolveReasoningDefault({ provider, model, catalog }) {      │
│   const entry = catalog.find(...)                            │
│   return entry?.reasoning === true ? "on" : "off"  ✅         │
│ }                                                             │
│                                                               │
│ 检查模型目录中的 reasoning 属性                                │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. 推理内容生成                                                │
│    src/agents/pi-embedded-subscribe.handlers.messages.ts      │
├─────────────────────────────────────────────────────────────┤
│ shouldEmitReasoning = Boolean(                               │
│   ctx.state.includeReasoning &&    // reasoning 已启用 ✅      │
│   formattedReasoning &&            // 有推理内容              │
│   onBlockReply                     // 有回调                 │
│ )                                                             │
│                                                               │
│ if (shouldEmitReasoning) {                                   │
│   emitBlockReplySafely({                                     │
│     text: formattedReasoning,      // 推理内容                │
│     isReasoning: true              // 标记为推理 ✅            │
│   })                                                          │
│ }                                                             │
└─────────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. 终端输出                                                    │
├─────────────────────────────────────────────────────────────┤
│ Reasoning:                                                    │
│ _I need to analyze the file first, then extract the key      │
│ information..._                                               │
│                                                               │
│ 让我先读取文件内容...                                           │
│ 🔧 read_file: README.md                                       │
│ ...                                                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. HunterClaw 对齐建议

### 4.1 verboseDefault 对齐

#### C++ 配置结构

**文件**: `include/hunterclaw/react/react_engine.h`

```cpp
struct ReactConfig {
    // ✅ 已有配置
    struct BlockReply {
        bool enabled = true;
        bool disable_streaming = false;  // 已实现
        
        // ... 其他配置 ...
    } block_reply;
    
    // ✅ 新增: Verbose 配置
    enum class VerboseLevel {
        Off,   // 不显示工具信息
        On,    // 只显示工具名称和参数
        Full   // 显示工具名称、参数和输出 ✅
    };
    VerboseLevel verbose_default = VerboseLevel::Off;
};
```

#### 处理逻辑实现

**文件**: `src/react/message_processor.cc`

```cpp
// 1. 配置解析（优先级处理）
VerboseLevel resolve_verbose_level(
    const channels::InboundMessage& message,
    const SessionEntry* session,
    const ReactConfig& config
) {
    // 优先级 1: 消息内联指令
    if (message.has_directive("verbose")) {
        return parse_verbose_directive(message.get_directive("verbose"));
    }
    
    // 优先级 2: 会话覆盖
    if (session && session->has_verbose_override()) {
        return session->verbose_level;
    }
    
    // 优先级 3: 全局配置 ✅
    return config.verbose_default;
}

// 2. 工具输出过滤
callbacks.on_tool_result = [this, verbose_level](
    const std::string& tool_name,
    const json& args,
    const json& result,
    bool is_error
) {
    if (verbose_level == VerboseLevel::Off) {
        return;  // 不显示工具信息 ❌
    }
    
    // 显示工具调用
    spdlog::info("🔧 {}: {}", tool_name, args.dump());
    
    if (verbose_level == VerboseLevel::Full) {
        // 显示工具输出 ✅
        spdlog::info("📄 工具输出:\n{}", result.dump(2));
    } else {
        // verbose_level == VerboseLevel::On
        // 只显示成功/失败状态 ⚠️
        spdlog::info("✅ 工具执行{}", is_error ? "失败" : "成功");
    }
};
```

---

### 4.2 reasoningDefault 对齐

#### C++ 配置结构

**文件**: `include/hunterclaw/react/react_engine.h`

```cpp
struct ReactConfig {
    // ✅ 已有: Reasoning 模式配置
    enum class ReasoningMode {
        Off,      // 关闭推理输出
        Final,    // 只在最后发送完整推理
        Stream    // 流式发送推理 (OpenClaw 默认)
    };
    ReasoningMode reasoning_mode = ReasoningMode::Stream;
    
    // ✅ 新增: 推理默认级别（根据模型能力自动启用）
    // 注意: 这不是直接的 "reasoningDefault" 配置
    // 而是根据模型能力自动检测
    bool auto_enable_reasoning_for_capable_models = true;
};

// ✅ 新增: 模型能力检测
struct ModelCapabilities {
    std::string provider;
    std::string model;
    bool supports_reasoning = false;  // 关键属性 ✅
    bool supports_tool_use = true;
    // ... 其他能力 ...
};
```

#### 处理逻辑实现

**文件**: `src/react/message_processor.cc`

```cpp
// 1. 模型能力检测
bool supports_reasoning(
    const std::string& provider,
    const std::string& model
) {
    // 查找模型目录
    auto catalog = load_model_catalog();
    auto it = std::find_if(catalog.begin(), catalog.end(),
        [&](const ModelCapabilities& cap) {
            return cap.provider == provider && cap.model == model;
        }
    );
    
    // 检查 reasoning 属性 ✅
    return (it != catalog.end() && it->supports_reasoning);
}

// 2. 推理级别解析
ReasoningMode resolve_reasoning_mode(
    const channels::InboundMessage& message,
    const SessionEntry* session,
    const ReactConfig& config,
    const std::string& provider,
    const std::string& model
) {
    // 优先级 1: 消息内联指令
    if (message.has_directive("reasoning")) {
        return parse_reasoning_directive(message.get_directive("reasoning"));
    }
    
    // 优先级 2: 会话覆盖
    if (session && session->has_reasoning_override()) {
        return session->reasoning_mode;
    }
    
    // 优先级 3: 根据模型能力自动启用 ✅
    if (config.auto_enable_reasoning_for_capable_models) {
        if (supports_reasoning(provider, model)) {
            return ReasoningMode::Stream;  // 自动启用 ✅
        }
    }
    
    // 默认关闭
    return ReasoningMode::Off;
}

// 3. 推理内容发送
callbacks.on_reasoning = [this](const std::string& reasoning_text) {
    if (reasoning_mode == ReasoningMode::Off) {
        return;  // 不发送推理 ❌
    }
    
    // 格式化推理内容
    std::string formatted = "Reasoning:\n_" + reasoning_text + "_";
    
    if (reasoning_mode == ReasoningMode::Stream) {
        // 流式发送推理 ✅
        queue::DeliveryRequest delivery;
        delivery.channel = context.message.channel;
        delivery.to = context.message.sender_id;
        
        queue::OutboundPayload payload;
        payload.text = formatted;
        payload.is_reasoning = true;  // 标记为推理内容 ✅
        
        delivery_queue_->enqueue(delivery, {payload});
        spdlog::debug("[MessageProcessor] Reasoning delivered: {} chars", 
            reasoning_text.length());
    } else {
        // ReasoningMode::Final: 缓存推理，最后一起发送
        buffered_reasoning_ += reasoning_text;
    }
};
```

---

### 4.3 完整对齐清单

| 功能 | OpenClaw 实现 | HunterClaw 当前状态 | 对齐建议 |
|------|--------------|-------------------|---------|
| **verboseDefault 配置** | ✅ `agents.defaults.verboseDefault` | ❌ 未实现 | P0: 添加 `VerboseLevel verbose_default` |
| **3 级 verbose 模式** | ✅ off/on/full | ❌ 未实现 | P0: 实现 3 级模式 |
| **工具输出过滤** | ✅ full 时保留输出 | ❌ 未实现 | P0: 根据 verbose 级别过滤 |
| **会话级别覆盖** | ✅ `/verbose full` | ❌ 未实现 | P1: 支持会话覆盖 |
| **模型能力检测** | ✅ 根据 catalog 自动启用 | ❌ 未实现 | P1: 添加模型能力检测 |
| **reasoning 自动启用** | ✅ 支持推理的模型自动启用 | ⚠️ 手动配置 | P1: 自动检测并启用 |
| **推理内容格式化** | ✅ "Reasoning:\n_..._" | ⚠️ 简单输出 | P2: 统一格式化 |
| **推理时序控制** | ✅ before/after answer | ⚠️ 固定顺序 | P2: 支持时序控制 |

---

## 5. 关键代码文件索引

### verboseDefault 相关

1. **配置定义**: `src/config/types.agent-defaults.ts:189`
2. **配置解析**: `src/auto-reply/reply/get-reply-directives.ts:356-359`
3. **工具输出控制**: `src/gateway/server-chat.ts:501-551`
4. **TUI 显示**: `src/tui/tui-event-handlers.ts:286-316`
5. **会话覆盖**: `src/sessions/level-overrides.ts:6-31`

### reasoningDefault 相关

1. **模型能力检测**: `src/agents/model-selection.ts:669-681`
2. **推理级别解析**: `src/auto-reply/reply/get-reply-directives.ts:409-419`
3. **推理内容发送**: `src/agents/pi-embedded-subscribe.handlers.messages.ts:345-425`
4. **推理格式化**: `src/agents/pi-embedded-utils.ts:278`
5. **推理流式更新**: `src/agents/pi-embedded-subscribe.ts:556-627`

---

## 6. 总结

### verboseDefault 核心逻辑

1. **3 层优先级**: 内联指令 > 会话覆盖 > 全局配置 ✅
2. **3 级模式**: off (不显示) / on (只显示名称) / full (显示输出) ✅
3. **输出过滤**: Gateway 层根据 verbose 级别过滤工具输出 ✅
4. **显示层**: TUI 根据 verbose 级别渲染工具卡片 ✅

### reasoningDefault 核心逻辑

1. **无直接配置**: OpenClaw 没有 `reasoningDefault` 配置项 ⚠️
2. **模型驱动**: 根据模型目录中的 `reasoning: true` 属性自动启用 ✅
3. **优先级**: 内联指令 > 会话覆盖 > 模型能力检测 ✅
4. **格式化**: "Reasoning:\n_<content>_" (斜体 Markdown) ✅
5. **时序控制**: message_end 模式下先发推理，text_end 模式下后发 ✅

---

**创建时间**: 2026-03-16  
**OpenClaw 版本**: latest  
**对齐状态**: 📋 文档完成，等待实现
