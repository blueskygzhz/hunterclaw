# OpenClaw 前端 Lifecycle 事件聚合展示机制

## 📋 核心机制

OpenClaw 前端通过 **状态聚合 + 定时清理** 的方式展示 lifecycle 事件。

---

## 🔄 事件处理流程

### 1. **事件接收** (WebSocket)

```typescript
// ui/src/ui/app-tool-stream.ts:392
export function handleAgentEvent(host: ToolStreamHost, payload?: AgentEventPayload) {
  if (!payload) {
    return;
  }

  // 根据 stream 类型分发
  if (payload.stream === "compaction") {
    handleCompactionEvent(host as CompactionHost, payload);
    return;
  }

  if (payload.stream === "lifecycle" || payload.stream === "fallback") {
    handleLifecycleFallbackEvent(host as CompactionHost, payload);
    return;
  }

  if (payload.stream !== "tool") {
    return;
  }
  
  // ... 处理 tool 事件
}
```

---

### 2. **Compaction 事件聚合** (Start → End)

```typescript
// ui/src/ui/app-tool-stream.ts:273
export function handleCompactionEvent(host: CompactionHost, payload: AgentEventPayload) {
  const data = payload.data ?? {};
  const phase = typeof data.phase === "string" ? data.phase : "";

  // ✅ 清理现有定时器
  if (host.compactionClearTimer != null) {
    window.clearTimeout(host.compactionClearTimer);
    host.compactionClearTimer = null;
  }

  // ✅ Start 阶段：设置 active 状态
  if (phase === "start") {
    host.compactionStatus = {
      active: true,
      startedAt: Date.now(),
      completedAt: null,
    };
  } 
  // ✅ End 阶段：标记完成 + 启动清理定时器
  else if (phase === "end") {
    host.compactionStatus = {
      active: false,
      startedAt: host.compactionStatus?.startedAt ?? null,
      completedAt: Date.now(),
    };
    
    // 🔥 5 秒后自动清除状态
    host.compactionClearTimer = window.setTimeout(() => {
      host.compactionStatus = null;
      host.compactionClearTimer = null;
    }, COMPACTION_TOAST_DURATION_MS); // 5000ms
  }
}
```

**关键点**:
- **Start**: 立即显示 "Compacting context..." 提示
- **End**: 显示 "Context compacted" ✅，5 秒后自动消失

---

### 3. **前端渲染逻辑**

```typescript
// ui/src/ui/views/chat.ts:191
function renderCompactionIndicator(status: CompactionIndicatorStatus | null | undefined) {
  if (!status) {
    return nothing;
  }

  // ✅ 正在压缩中
  if (status.active) {
    return html`
      <div class="compaction-indicator compaction-indicator--active" 
           role="status" aria-live="polite">
        ${icons.loader} Compacting context...
      </div>
    `;
  }

  // ✅ 压缩完成（5 秒内显示）
  if (status.completedAt) {
    const elapsed = Date.now() - status.completedAt;
    if (elapsed < COMPACTION_TOAST_DURATION_MS) {
      return html`
        <div class="compaction-indicator compaction-indicator--complete" 
             role="status" aria-live="polite">
          ${icons.check} Context compacted
        </div>
      `;
    }
  }

  return nothing;
}
```

**视觉效果**:
- **Loading 状态**: 🔄 旋转图标 + "Compacting context..."
- **Complete 状态**: ✅ 勾号 + "Context compacted"（5 秒后消失）

---

## 🎯 完整的 UI 交互流程

```
┌────────────────────────────────────────────────────┐
│ 1. 后端发送 lifecycle 事件 (Start)                  │
│    { stream: "compaction", data: { phase: "start" }} │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 2. 前端接收事件 → handleCompactionEvent()           │
│    - 设置 host.compactionStatus.active = true       │
│    - 清除现有定时器                                  │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 3. UI 渲染 "Compacting context..." 提示             │
│    - 显示 loading 图标                              │
│    - 用户看到实时反馈                                │
└────────────────────────────────────────────────────┘
                   ↓
         [后端执行压缩操作...]
                   ↓
┌────────────────────────────────────────────────────┐
│ 4. 后端发送 lifecycle 事件 (End)                    │
│    { stream: "compaction", data: { phase: "end" }}   │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 5. 前端接收事件 → handleCompactionEvent()           │
│    - 设置 host.compactionStatus.active = false      │
│    - 设置 completedAt = Date.now()                  │
│    - 启动 5 秒定时器                                │
└────────────────────────────────────────────────────┘
                   ↓
┌────────────────────────────────────────────────────┐
│ 6. UI 渲染 "Context compacted" ✅                   │
│    - 显示勾号图标                                    │
│    - 用户知道操作已完成                              │
└────────────────────────────────────────────────────┘
                   ↓
            [等待 5 秒...]
                   ↓
┌────────────────────────────────────────────────────┐
│ 7. 定时器触发 → 清除状态                            │
│    - host.compactionStatus = null                   │
│    - UI 提示消失                                     │
└────────────────────────────────────────────────────┘
```

---

## 📦 数据结构定义

### CompactionStatus

```typescript
// ui/src/ui/app-tool-stream.ts:247
export type CompactionStatus = {
  active: boolean;          // 是否正在压缩
  startedAt: number | null; // 开始时间戳
  completedAt: number | null; // 完成时间戳
};
```

### CompactionHost

```typescript
// ui/src/ui/app-tool-stream.ts:263
type CompactionHost = ToolStreamHost & {
  compactionStatus?: CompactionStatus | null;  // 当前压缩状态
  compactionClearTimer?: number | null;        // 清理定时器 ID
  fallbackStatus?: FallbackStatus | null;      // Fallback 状态
  fallbackClearTimer?: number | null;          // Fallback 定时器
};
```

---

## 🎨 CSS 样式 (推测)

```css
/* 正在压缩 */
.compaction-indicator--active {
  background: #3b82f6; /* 蓝色 */
  animation: pulse 2s infinite;
}

/* 压缩完成 */
.compaction-indicator--complete {
  background: #10b981; /* 绿色 */
}

/* 基础样式 */
.compaction-indicator {
  position: fixed;
  bottom: 80px;
  right: 20px;
  padding: 12px 16px;
  border-radius: 8px;
  color: white;
  display: flex;
  align-items: center;
  gap: 8px;
  box-shadow: 0 4px 12px rgba(0,0,0,0.15);
  animation: slideInUp 0.3s ease-out;
}

@keyframes slideInUp {
  from {
    transform: translateY(100px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}
```

---

## 🔥 关键设计原则

### 1. **状态聚合**
- 不是每个 lifecycle 事件都单独显示
- 而是维护一个 **全局状态对象** (`compactionStatus`)
- 通过 Start/End 事件更新状态

### 2. **定时清理**
- 完成后不立即清除，显示 5 秒（给用户反馈）
- 使用 `setTimeout` 自动清理
- 避免 UI 闪烁

### 3. **异常安全**
- 新事件到来时，先清除旧定时器
- 防止多个定时器同时存在

### 4. **用户体验**
- **Loading 状态**: 让用户知道系统正在工作
- **Success 状态**: 让用户知道操作成功
- **自动消失**: 不需要用户手动关闭

---

## 🚀 HunterClaw 实现建议

### 后端改动 (✅ 已完成)

```cpp
// src/gateway/methods/chat.cc:128
agent->on_event([server, session_id](const agents::AgentEvent& evt) {
    if (evt.stream != agents::AgentEventStream::Lifecycle) {
        return;
    }
    
    json event_payload;
    event_payload["type"] = "agent.lifecycle";
    event_payload["session_id"] = session_id;
    event_payload["run_id"] = evt.run_id;
    event_payload["seq"] = evt.seq;
    event_payload["stream"] = "lifecycle";
    event_payload["timestamp"] = evt.timestamp_ms;
    event_payload["data"] = evt.data;
    
    core::EventFrame frame;
    frame.type = "agent";
    frame.payload = event_payload;
    
    server->broadcast(frame);
});
```

### 前端实现 (需要补充)

#### 1. **添加状态管理**

```typescript
interface LifecycleStatus {
  phase: 'start' | 'reasoning' | 'tool_execution' | 'end' | null;
  active: boolean;
  startedAt: number | null;
  completedAt: number | null;
}

class ChatComponent {
  private lifecycleStatus: LifecycleStatus | null = null;
  private lifecycleClearTimer: number | null = null;
  
  // ...
}
```

#### 2. **事件处理器**

```typescript
private handleLifecycleEvent(event: AgentEvent) {
  const phase = event.data?.phase as string;
  
  // 清理现有定时器
  if (this.lifecycleClearTimer) {
    clearTimeout(this.lifecycleClearTimer);
    this.lifecycleClearTimer = null;
  }
  
  if (phase === 'start') {
    this.lifecycleStatus = {
      phase: 'start',
      active: true,
      startedAt: Date.now(),
      completedAt: null,
    };
  } else if (phase === 'end') {
    this.lifecycleStatus = {
      phase: 'end',
      active: false,
      startedAt: this.lifecycleStatus?.startedAt ?? null,
      completedAt: Date.now(),
    };
    
    // 5 秒后清除
    this.lifecycleClearTimer = setTimeout(() => {
      this.lifecycleStatus = null;
      this.lifecycleClearTimer = null;
      this.requestUpdate();
    }, 5000);
  }
  
  this.requestUpdate(); // 触发 UI 更新
}
```

#### 3. **UI 渲染**

```typescript
private renderLifecycleIndicator() {
  if (!this.lifecycleStatus) {
    return nothing;
  }
  
  if (this.lifecycleStatus.active) {
    return html`
      <div class="lifecycle-indicator lifecycle-indicator--active">
        🔄 Agent is thinking...
      </div>
    `;
  }
  
  if (this.lifecycleStatus.completedAt) {
    const elapsed = Date.now() - this.lifecycleStatus.completedAt;
    if (elapsed < 5000) {
      return html`
        <div class="lifecycle-indicator lifecycle-indicator--complete">
          ✅ Agent completed
        </div>
      `;
    }
  }
  
  return nothing;
}
```

---

## 📊 对比：事件流 vs 状态聚合

| 方案 | 优点 | 缺点 |
|------|------|------|
| **每个事件都显示** | 实时性强 | UI 闪烁，信息过载 |
| **状态聚合** ✅ | UI 稳定，用户体验好 | 需要维护状态 |

**OpenClaw 选择状态聚合是正确的设计！**

---

## 🎯 总结

### OpenClaw 前端聚合机制核心要点:

1. ✅ **不显示每个 lifecycle 事件**
2. ✅ **维护全局状态对象**（compactionStatus）
3. ✅ **Start 阶段**: 显示 Loading 提示
4. ✅ **End 阶段**: 显示 Success 提示 + 5 秒后自动清除
5. ✅ **定时清理**: 防止 UI 永久占用
6. ✅ **异常安全**: 清理旧定时器

### HunterClaw 对齐度:

| 组件 | 状态 | 说明 |
|------|------|------|
| 后端事件发射 | ✅ 完成 | emit_lifecycle_event 已实现 |
| Gateway 事件注册 | ✅ 完成 | on_event 已注册 |
| WebSocket 广播 | ✅ 完成 | server->broadcast 已调用 |
| **前端状态聚合** | ❌ 缺失 | 需要实现 handleLifecycleEvent |
| **前端 UI 渲染** | ❌ 缺失 | 需要实现 renderLifecycleIndicator |

### 下一步:

1. 在前端添加 `LifecycleStatus` 状态管理
2. 实现 `handleLifecycleEvent()` 事件处理器
3. 实现 `renderLifecycleIndicator()` UI 渲染
4. 添加 CSS 样式（参考上面的示例）

**完成后，HunterClaw 将 100% 对齐 OpenClaw 的用户体验！** 🚀
