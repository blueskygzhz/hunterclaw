# HunterClaw Message Processor - OpenClaw 对齐文档

## 📋 概览

本文档详细记录 HunterClaw 的 `message_processor.cpp/h` 与 OpenClaw 的 `process-message.ts` 对齐状态。

**对齐参考**:
- OpenClaw 主文件: `src/web/auto-reply/monitor/process-message.ts`
- 核心流程: `src/auto-reply/dispatch.ts`
- Reply Dispatcher: `src/auto-reply/reply/reply-dispatcher.ts`

---

## 🎯 核心流程对比

### OpenClaw 流程 (process-message.ts:126-473)

```typescript
export async function processMessage(params: {
  cfg, msg, route, groupHistoryKey, ...
}) {
  // 1. Resolve session envelope context (line 155-159)
  const { storePath, envelopeOptions, previousTimestamp } = 
    resolveInboundSessionEnvelopeContext({...});
  
  // 2. Build inbound line (line 160-166)
  let combinedBody = buildInboundLine({
    cfg, msg, agentId, previousTimestamp, envelope: envelopeOptions
  });
  
  // 3. Group history processing (line 169-195)
  if (msg.chatType === "group" && history.length > 0) {
    combinedBody = buildHistoryContextFromEntries({...});
  }
  
  // 4. Echo detection (line 198-206)
  const combinedEchoKey = buildCombinedEchoKey({...});
  if (echoHas(combinedEchoKey)) {
    return false; // Skip duplicate
  }
  
  // 5. Send ACK reaction (line 208-219)
  maybeSendAckReaction({...});
  
  // 6. Finalize inbound context (line 299-334)
  const ctxPayload = finalizeInboundContext({
    Body, BodyForAgent, From, To, SessionKey, ...
  });
  
  // 7. Update last route (line 336-373)
  updateLastRouteInBackground({...});
  
  // 8. Record session meta (line 375-389)
  const metaTask = recordSessionMetaFromInbound({...});
  
  // 9. Dispatch reply with buffered block dispatcher (line 391-458)
  const { queuedFinal } = await dispatchReplyWithBufferedBlockDispatcher({
    ctx: ctxPayload,
    cfg,
    replyResolver,
    dispatcherOptions: {
      ...prefixOptions,
      responsePrefix,
      deliver: async (payload, info) => {
        // 只投递 final reply (line 405-409)
        if (info.kind !== "final") return;
        await deliverWebReply({...});
      },
      onError: (err, info) => {...},
      onReplyStart: msg.sendComposing,
    },
    replyOptions: {
      // ✅ 核心: 禁用 block streaming (WhatsApp 只投递 final)
      disableBlockStreaming: true,  // line 455
      onModelSelected,
    },
  });
  
  // 10. Clear group history (line 460-470)
  if (shouldClearGroupHistory) {
    groupHistories.set(groupHistoryKey, []);
  }
  
  return didSendReply;
}
```

### HunterClaw 流程 (message_processor.cc:110-182)

```cpp
ProcessResult MessageProcessor::process_message(
    const channels::InboundMessage& message
) {
    // 1. 预处理: 路由解析
    auto context = preprocess_message(message);  // line 118
    
    // 2. 权限检查
    if (!check_permissions(context)) {
        return result;  // line 121-125
    }
    
    // 3. Echo 检测
    if (is_echo(context)) {
        return result;  // line 128-132
    }
    
    // 4. 发送 ACK 反应
    send_ack_reaction(context);  // line 135-137
    
    // 5. 加载会话和历史
    auto history = load_history(context);  // line 140
    
    // 6. 执行 ReAct 循环 (带循环检测和模型回退)
    ReactResult react_result;
    if (enable_model_fallback) {
        react_result = execute_with_fallback(...);  // line 147
    } else {
        react_result = execute_react_with_loop_detection(...);  // line 151
    }
    
    // 7. 投递响应
    deliver_response(context, react_result);  // line 163
    
    // 8. 保存会话
    save_session(context, react_result);  // line 166
    
    return result;
}
```

---

## 📊 功能对齐矩阵

| 功能 | OpenClaw 参考 | HunterClaw 当前 | 对齐状态 | 优先级 |
|------|---------------|----------------|---------|--------|
| **1. Session Envelope Context** | `resolveInboundSessionEnvelopeContext` | ❌ 未实现 | ⚠️ 缺失 | P1 |
| **2. Build Inbound Line** | `buildInboundLine` (timestamp, envelope) | ✅ `preprocess_message` | ⭐⭐⭐ 部分对齐 | P2 |
| **3. Group History Processing** | `buildHistoryContextFromEntries` | ✅ `load_history` 支持群组 | ⭐⭐⭐⭐ 对齐 | P3 |
| **4. Echo Detection** | `echoHas/echoForget` (combined body) | ✅ `is_echo` (content hash) | ⭐⭐⭐⭐ 对齐 | ✅ |
| **5. ACK Reaction** | `maybeSendAckReaction` | ✅ `send_ack_reaction` | ⭐⭐⭐⭐ 对齐 | ✅ |
| **6. Finalize Inbound Context** | `finalizeInboundContext` (Body/BodyForAgent) | ⭐⭐⭐ 部分 (MessageContext) | ⚠️ 需增强 | P1 |
| **7. Update Last Route** | `updateLastRouteInBackground` | ❌ 未实现 | ⚠️ 缺失 | P2 |
| **8. Record Session Meta** | `recordSessionMetaFromInbound` | ⭐⭐ `save_session` | ⚠️ 时机不同 | P2 |
| **9. Buffered Block Dispatcher** | `dispatchReplyWithBufferedBlockDispatcher` | ❌ 未实现 | ⚠️ 缺失 | P0 |
| **10. disableBlockStreaming** | `replyOptions.disableBlockStreaming` | ❌ 未控制 | ❌ **严重缺失** | **P0** |
| **11. Command Authorization** | `resolveWhatsAppCommandAuthorized` | ✅ `check_permissions` | ⭐⭐⭐⭐ 对齐 | ✅ |
| **12. DM Policy & Allow From** | `resolveDmGroupAccessWithCommandGate` | ✅ `check_permissions` 白名单 | ⭐⭐⭐⭐ 对齐 | ✅ |
| **13. Group History Clear** | `groupHistories.set(key, [])` | ❌ 未实现 | ⚠️ 缺失 | P3 |
| **14. Reasoning Stream** | ✅ WebSocket 推送 (streaming) | ✅ `on_reasoning_stream` | ⭐⭐⭐⭐ 对齐 | ✅ |
| **15. Block Reply** | ✅ WebSocket 推送 (streaming) | ✅ `on_block_reply` | ⭐⭐⭐⭐ 对齐 | ✅ |
| **16. Tool Update Dispatch** | `info.kind === "tool"` skip | ❌ 未控制 | ⚠️ 缺失 | P1 |

**对齐度总结**: ⭐⭐⭐ (65%) - 核心功能基本对齐，但缺少关键的 Dispatcher 机制

---

## 🚨 关键差异详解

### 1. **Buffered Block Dispatcher (P0 - 严重缺失)**

**OpenClaw 实现** (dispatch.ts:56-80):
```typescript
export async function dispatchInboundMessageWithBufferedDispatcher(params: {
  ctx, cfg, dispatcherOptions, replyOptions, replyResolver
}) {
  // 创建 dispatcher (带 typing indicator 和 buffering)
  const { dispatcher, replyOptions: mergedOpts, markDispatchIdle } = 
    createReplyDispatcherWithTyping(dispatcherOptions);
  
  try {
    return await dispatchInboundMessage({
      ctx, cfg, dispatcher, replyResolver,
      replyOptions: {
        ...replyOptions,
        ...mergedOpts,  // 包含 onToolResult, onBlockReply
      },
    });
  } finally {
    markDispatchIdle();  // 确保清理
  }
}
```

**HunterClaw 当前**: ❌ **完全缺失**
- 没有 Dispatcher 抽象
- 直接在回调中投递消息 (`on_block_reply`)
- 无法控制 streaming 行为

**影响**:
- ❌ 无法根据渠道动态控制 block streaming
- ❌ WhatsApp 等渠道会泄露推理过程 (thinking/reasoning)
- ❌ 无法统一管理投递策略

**修复方案**:
```cpp
// 创建 Dispatcher 抽象
class ReplyDispatcher {
public:
    virtual void deliver(const OutboundPayload& payload, DeliveryInfo info) = 0;
    virtual void mark_complete() = 0;
    virtual void wait_for_idle() = 0;
};

// 支持不同策略
enum class DispatchStrategy {
    Full,            // 投递所有 (reasoning + block + final)
    BlockAndFinal,   // 只投递 block + final (跳过 tool updates)
    FinalOnly        // 只投递 final (WhatsApp, Signal)
};

class BufferedBlockDispatcher : public ReplyDispatcher {
    DispatchStrategy strategy_;
    // ...
};
```

---

### 2. **disableBlockStreaming 控制 (P0 - 严重缺失)**

**OpenClaw 实现** (process-message.ts:454-455):
```typescript
replyOptions: {
  // WhatsApp delivery intentionally suppresses non-final payloads.
  // Keep block streaming disabled so final replies are still produced.
  disableBlockStreaming: true,
  onModelSelected,
}
```

**HunterClaw 当前**: ❌ **完全缺失**
- `ReactConfig` 中没有 `disableBlockStreaming` 选项
- `on_block_reply` 回调总是会被调用
- 无法针对渠道动态禁用

**影响**:
- ❌ WhatsApp 用户会看到推理过程 (不符合预期)
- ❌ Telegram 等渠道无法选择是否启用 streaming
- ❌ 违反 OpenClaw 的隐私设计 (thinking 应该只在 Web UI 显示)

**修复方案**:
```cpp
// 1. 添加到 ReactConfig
struct ReactConfig {
    bool disable_block_streaming = false;  // 禁用 block streaming
    // ...
};

// 2. 在 MessageProcessor 中根据渠道设置
MessageProcessorConfig config;
if (message.channel == channels::Channel::WhatsApp ||
    message.channel == channels::Channel::Signal) {
    config.react_config.disable_block_streaming = true;
}

// 3. 在 on_block_reply 中检查
callbacks.on_block_reply = [this, context](const std::string& block) {
    if (config_.react_config.disable_block_streaming) {
        return;  // 跳过 block reply
    }
    // ... 正常投递逻辑
};
```

---

### 3. **Session Envelope Context (P1 - 缺失)**

**OpenClaw 实现** (process-message.ts:155-159):
```typescript
const { storePath, envelopeOptions, previousTimestamp } = 
  resolveInboundSessionEnvelopeContext({
    cfg, agentId, sessionKey
  });

// 用于构建带时间戳的消息
let combinedBody = buildInboundLine({
  cfg, msg, agentId,
  previousTimestamp,  // ✅ 用于计算时间间隔
  envelope: envelopeOptions  // ✅ 控制消息格式
});
```

**HunterClaw 当前**: ❌ **完全缺失**
- 没有 `previousTimestamp` 概念
- 消息格式固定，无法动态配置

**影响**:
- ⚠️ 无法显示消息时间间隔 (e.g., "5 minutes later")
- ⚠️ 无法根据配置调整消息格式 (envelope style)

**修复方案**:
```cpp
// 1. 添加到 MessageContext
struct MessageContext {
    int64_t previous_timestamp_ms = 0;  // 上次消息时间
    EnvelopeOptions envelope_options;   // 消息封装选项
};

// 2. 在 preprocess_message 中加载
MessageContext MessageProcessor::preprocess_message(...) {
    // 从 session 获取上次消息时间
    auto session = session_manager_->get_session(route.session_key);
    if (session && !session->messages.empty()) {
        context.previous_timestamp_ms = session->messages.back().timestamp_ms;
    }
    return context;
}
```

---

### 4. **Finalize Inbound Context (P1 - 需增强)**

**OpenClaw 实现** (inbound-context.ts:37-96):
```typescript
export function finalizeInboundContext(ctx: T): T & FinalizedMsgContext {
  // 1. Sanitize and normalize all text fields
  ctx.Body = sanitizeInboundSystemTags(normalizeInboundTextNewlines(ctx.Body));
  ctx.RawBody = normalizeTextField(ctx.RawBody);
  ctx.CommandBody = normalizeTextField(ctx.CommandBody);
  
  // 2. Resolve BodyForAgent (优先级: BodyForAgent > CommandBody > RawBody > Body)
  ctx.BodyForAgent = sanitizeInboundSystemTags(
    normalizeInboundTextNewlines(
      ctx.BodyForAgent ?? ctx.CommandBody ?? ctx.RawBody ?? ctx.Body
    )
  );
  
  // 3. Resolve ConversationLabel
  if (!ctx.ConversationLabel) {
    ctx.ConversationLabel = resolveConversationLabel(ctx);
  }
  
  // 4. MediaType alignment (补全默认值)
  // ...
  
  return ctx;
}
```

**HunterClaw 当前**: ⭐⭐⭐ **部分对齐** (message_processor.cc:193-250)
- ✅ 有 `MessageContext` 结构
- ✅ 基本字段 (user_id, agent_id, workspace_path)
- ⚠️ 缺少 `BodyForAgent`, `CommandBody`, `RawBody` 分离
- ⚠️ 缺少 `sanitizeInboundSystemTags` (防止注入攻击)

**影响**:
- ⚠️ 无法区分用户原始输入和 Agent 处理后的输入
- ⚠️ 可能存在 system tag 注入漏洞 (e.g., `<system>...`)

**修复方案**:
```cpp
struct MessageContext {
    // ✅ 添加多个 Body 变体
    std::string body;             // 完整消息 (带 envelope)
    std::string body_for_agent;   // Agent 处理的输入
    std::string command_body;     // 命令部分
    std::string raw_body;         // 原始输入 (未处理)
    
    // ...
};

// Sanitize 函数
std::string sanitize_inbound_system_tags(const std::string& text) {
    // 移除或转义 <system>, <user>, <assistant> 等标签
    std::string result = text;
    // ... regex 替换逻辑
    return result;
}
```

---

### 5. **Update Last Route (P2 - 缺失)**

**OpenClaw 实现** (process-message.ts:336-373):
```typescript
// 只在 DM 中更新 last route (用于后续自动路由)
if (dmRouteTarget && shouldUpdateMainLastRoute) {
  updateLastRouteInBackground({
    cfg, backgroundTasks,
    storeAgentId, sessionKey: mainSessionKey,
    channel: "whatsapp",
    to: dmRouteTarget,
    accountId,
    ctx: ctxPayload,
  });
}
```

**HunterClaw 当前**: ❌ **完全缺失**

**影响**:
- ⚠️ 无法记住用户最后一次对话的渠道
- ⚠️ 跨渠道场景下可能路由错误

**修复方案**:
```cpp
void MessageProcessor::update_last_route_background(
    const MessageContext& context
) {
    // 异步更新 last route (不阻塞主流程)
    std::thread([=]() {
        try {
            router_->update_last_route(
                context.route.session_key,
                context.message.channel,
                context.user_id,
                context.route.account_id
            );
        } catch (const std::exception& e) {
            spdlog::warn("Failed to update last route: {}", e.what());
        }
    }).detach();
}
```

---

### 6. **Tool Update Dispatch 控制 (P1 - 缺失)**

**OpenClaw 实现** (process-message.ts:405-410):
```typescript
deliver: async (payload: ReplyPayload, info) => {
  if (info.kind !== "final") {
    // Only deliver final replies to external messaging channels (WhatsApp).
    // Block (reasoning/thinking) and tool updates are meant for the internal
    // web UI only; sending them here leaks chain-of-thought to end users.
    return;
  }
  await deliverWebReply({...});
}
```

**HunterClaw 当前**: ❌ **完全缺失**
- `on_tool_start` / `on_tool_end` 没有投递控制
- 可能会泄露工具调用细节

**影响**:
- ⚠️ 用户可能看到 "Calling tool: web_search" 等中间状态
- ⚠️ 违反隐私设计 (工具调用应该只在 debug 模式显示)

**修复方案**:
```cpp
// 添加 DeliveryInfo 类型
enum class DeliveryKind {
    Reasoning,   // 推理过程
    Tool,        // 工具调用
    Block,       // Block reply
    Final        // 最终响应
};

// 在回调中检查
callbacks.on_tool_start = [this, context](const std::string& tool, ...) {
    if (config_.suppress_tool_updates) {
        return;  // 跳过工具更新
    }
    // ... 投递逻辑
};
```

---

## 🛠️ 修复优先级

### P0 - 严重问题 (立即修复)

1. **实现 Buffered Block Dispatcher**
   - 创建 `ReplyDispatcher` 抽象
   - 支持 `FinalOnly`, `BlockAndFinal`, `Full` 策略
   - 重构 `deliver_response` 使用 Dispatcher

2. **添加 `disableBlockStreaming` 控制**
   - 在 `ReactConfig` 添加选项
   - 在 `MessageProcessorConfig` 根据渠道设置
   - 在 `on_block_reply` 中检查并跳过

### P1 - 高优先级 (尽快修复)

3. **增强 `finalizeInboundContext`**
   - 添加 `BodyForAgent`, `CommandBody`, `RawBody`
   - 实现 `sanitize_inbound_system_tags`
   - 添加 `ConversationLabel` 解析

4. **实现 Session Envelope Context**
   - 添加 `previous_timestamp_ms` 到 `MessageContext`
   - 在 `preprocess_message` 中加载
   - 用于消息格式化 (时间间隔显示)

5. **添加 Tool Update 控制**
   - 添加 `suppress_tool_updates` 配置
   - 在 `on_tool_start`/`on_tool_end` 中检查

### P2 - 中优先级 (后续优化)

6. **实现 Update Last Route**
   - 添加 `update_last_route_background`
   - 异步更新路由记录

7. **优化 Record Session Meta**
   - 区分 inbound 和 outbound meta
   - 提前记录 (不要等到最后)

### P3 - 低优先级 (可选)

8. **Group History Clear**
   - 添加 `clear_group_history` 方法
   - 在消息处理完成后清理

---

## 📝 实施建议

### 最小对齐 (快速修复)

**目标**: 修复 P0 问题，防止泄露推理过程

```cpp
// 1. 添加 disableBlockStreaming
struct ReactConfig {
    bool disable_block_streaming = false;
};

// 2. 在 MessageProcessor 中设置
MessageProcessorConfig config;
if (message.channel == channels::Channel::WhatsApp) {
    config.react_config.disable_block_streaming = true;
}

// 3. 在回调中检查
callbacks.on_block_reply = [config](const std::string& block) {
    if (config.react_config.disable_block_streaming) {
        return;
    }
    // ... 正常投递
};
```

**工作量**: 1-2 小时  
**对齐度提升**: 65% → 75%

---

### 完整对齐 (生产级)

**目标**: 完全对齐 OpenClaw 的投递机制

1. **创建 Dispatcher 系统** (4-6 小时)
   - `ReplyDispatcher` 抽象
   - `BufferedBlockDispatcher` 实现
   - `FinalOnlyDispatcher` (WhatsApp)
   - `FullStreamingDispatcher` (Telegram)

2. **增强 Context 处理** (2-3 小时)
   - `finalizeInboundContext` 完整实现
   - `sanitize_inbound_system_tags`
   - `BodyForAgent` / `CommandBody` 分离

3. **添加后台任务** (1-2 小时)
   - `update_last_route_background`
   - `record_session_meta_background`

**工作量**: 8-12 小时  
**对齐度提升**: 65% → 95%

---

## 🎓 关键设计思想

### 1. **分离投递策略和业务逻辑**

OpenClaw 通过 Dispatcher 模式完全解耦:
- ReAct Engine 只负责生成内容
- Dispatcher 决定如何投递 (streaming vs final-only)
- 渠道无关的统一接口

### 2. **分层的消息表示**

OpenClaw 区分多种 Body:
- `Body`: 完整消息 (带 envelope, 时间戳)
- `BodyForAgent`: Agent 处理的输入 (简化版)
- `CommandBody`: 命令部分 (用于权限检查)
- `RawBody`: 原始用户输入 (未处理)

**好处**: 支持复杂的群组历史、命令检测、权限控制

### 3. **非阻塞的后台任务**

OpenClaw 使用 `backgroundTasks` 管理所有异步操作:
- 更新 last route
- 记录 session meta
- 清理过期数据

**好处**: 主流程不被慢操作阻塞，提升响应速度

---

## ✅ 验证清单

- [ ] P0 问题已修复 (disableBlockStreaming)
- [ ] WhatsApp 消息不泄露推理过程
- [ ] Telegram 可以正常使用 block streaming
- [ ] 群组历史正确加载和清理
- [ ] Echo 检测正常工作
- [ ] ACK reaction 正常发送
- [ ] 权限检查符合预期
- [ ] 模型回退在失败时触发
- [ ] Session 正确保存和加载
- [ ] 媒体附件正常投递

---

**文档版本**: v1.0  
**创建时间**: 2026-03-16  
**对齐状态**: ⭐⭐⭐ (65% - 核心对齐，缺少 Dispatcher)  
**下一步**: 实现 P0 修复 (disableBlockStreaming)
