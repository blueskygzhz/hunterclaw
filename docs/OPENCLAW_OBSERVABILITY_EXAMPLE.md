# OpenClaw 可观测性实现详解

本文档通过一个完整的例子说明 OpenClaw 是如何实现分布式追踪和可观测性的。

## 1. 架构概览

OpenClaw 采用**事件驱动 + OpenTelemetry**的可观测性架构:

```
业务代码 (emit events)
    ↓
诊断事件总线 (diagnostic-events)
    ↓
OpenTelemetry 插件 (diagnostics-otel)
    ↓
OTLP Exporter (HTTP/Protobuf)
    ↓
后端 (Jaeger/Prometheus/Grafana)
```

### 核心设计原则

1. **故障隔离**: 可观测性代码失败不影响业务流程
2. **事件解耦**: 业务代码只负责发送事件,不关心具体实现
3. **灵活订阅**: 插件通过事件订阅机制消费诊断数据
4. **标准协议**: 使用 OpenTelemetry 标准,兼容所有主流后端

---

## 2. 完整示例: Webhook 处理流程追踪

让我们通过一个实际例子看看 OpenClaw 如何追踪 Webhook 的接收和处理。

### 2.1 业务代码发送诊断事件

**文件: `src/logging/diagnostic.ts`**

```typescript
import { emitDiagnosticEvent } from "../infra/diagnostic-events.js";

// Webhook 接收时发送事件
export function logWebhookReceived(params: {
  channel: string;
  updateType?: string;
  chatId?: number | string;
}) {
  webhookStats.received += 1;
  webhookStats.lastReceived = Date.now();
  
  // 记录调试日志
  if (diag.isEnabled("debug")) {
    diag.debug(
      `webhook received: channel=${params.channel} type=${params.updateType ?? "unknown"} ` +
      `chatId=${params.chatId ?? "unknown"} total=${webhookStats.received}`
    );
  }
  
  // 发送诊断事件 (核心)
  emitDiagnosticEvent({
    type: "webhook.received",
    channel: params.channel,
    updateType: params.updateType,
    chatId: params.chatId,
  });
  
  markActivity();
}

// Webhook 处理完成时发送事件
export function logWebhookProcessed(params: {
  channel: string;
  updateType?: string;
  chatId?: number | string;
  durationMs?: number;
}) {
  webhookStats.processed += 1;
  
  if (diag.isEnabled("debug")) {
    diag.debug(
      `webhook processed: channel=${params.channel} type=${params.updateType ?? "unknown"} ` +
      `chatId=${params.chatId ?? "unknown"} duration=${params.durationMs ?? 0}ms`
    );
  }
  
  // 发送诊断事件 (包含耗时)
  emitDiagnosticEvent({
    type: "webhook.processed",
    channel: params.channel,
    updateType: params.updateType,
    chatId: params.chatId,
    durationMs: params.durationMs,
  });
  
  markActivity();
}

// Webhook 处理失败时发送事件
export function logWebhookError(params: {
  channel: string;
  updateType?: string;
  chatId?: number | string;
  error: string;
}) {
  webhookStats.errors += 1;
  
  diag.error(
    `webhook error: channel=${params.channel} type=${params.updateType ?? "unknown"} ` +
    `chatId=${params.chatId ?? "unknown"} error="${params.error}"`
  );
  
  // 发送错误事件
  emitDiagnosticEvent({
    type: "webhook.error",
    channel: params.channel,
    updateType: params.updateType,
    chatId: params.chatId,
    error: params.error,
  });
  
  markActivity();
}
```

**关键点**:
- 业务代码只负责调用 `emitDiagnosticEvent()`,不关心如何导出到后端
- 事件包含完整的上下文信息: channel、updateType、chatId、durationMs
- 所有可观测性代码都不会抛出异常影响主流程

---

### 2.2 事件类型定义

**文件: `src/infra/diagnostic-events.ts`**

```typescript
// 基础事件字段
type DiagnosticBaseEvent = {
  ts: number;      // 时间戳
  seq: number;     // 序列号
};

// Webhook 接收事件
export type DiagnosticWebhookReceivedEvent = DiagnosticBaseEvent & {
  type: "webhook.received";
  channel: string;
  updateType?: string;
  chatId?: number | string;
};

// Webhook 处理完成事件
export type DiagnosticWebhookProcessedEvent = DiagnosticBaseEvent & {
  type: "webhook.processed";
  channel: string;
  updateType?: string;
  chatId?: number | string;
  durationMs?: number;  // 处理耗时
};

// Webhook 错误事件
export type DiagnosticWebhookErrorEvent = DiagnosticBaseEvent & {
  type: "webhook.error";
  channel: string;
  updateType?: string;
  chatId?: number | string;
  error: string;        // 错误信息
};

// ... 还有其他事件类型 (model.usage, message.queued, session.state 等)
```

**设计亮点**:
- 所有事件都继承 `DiagnosticBaseEvent` (包含时间戳和序列号)
- 使用 TypeScript 类型系统确保事件结构正确
- 事件类型清晰,方便订阅者过滤和处理

---

### 2.3 OpenTelemetry 插件处理事件

**文件: `extensions/diagnostics-otel/src/service.ts`**

```typescript
import { trace, metrics, SpanStatusCode } from "@opentelemetry/api";
import { OTLPTraceExporter } from "@opentelemetry/exporter-trace-otlp-proto";
import { NodeSDK } from "@opentelemetry/sdk-node";

export function createDiagnosticsOtelService(): OpenClawPluginService {
  let sdk: NodeSDK | null = null;
  let unsubscribe: (() => void) | null = null;

  return {
    id: "diagnostics-otel",
    
    async start(ctx) {
      const cfg = ctx.config.diagnostics;
      const otel = cfg?.otel;
      
      // 检查配置是否启用
      if (!cfg?.enabled || !otel?.enabled) {
        return;
      }

      // 初始化 OpenTelemetry SDK
      const endpoint = normalizeEndpoint(otel.endpoint);
      const serviceName = otel.serviceName?.trim() || "openclaw";
      
      const traceExporter = new OTLPTraceExporter({
        url: resolveOtelUrl(endpoint, "v1/traces"),
        headers: otel.headers,
      });
      
      sdk = new NodeSDK({
        resource: resourceFromAttributes({
          [ATTR_SERVICE_NAME]: serviceName,
        }),
        traceExporter,
        sampler: new ParentBasedSampler({
          root: new TraceIdRatioBasedSampler(otel.sampleRate ?? 1.0),
        }),
      });
      
      await sdk.start();

      // 获取 Tracer 和 Meter
      const tracer = trace.getTracer("openclaw");
      const meter = metrics.getMeter("openclaw");

      // 创建 Metrics
      const webhookReceivedCounter = meter.createCounter("openclaw.webhook.received", {
        unit: "1",
        description: "Webhook requests received",
      });
      
      const webhookDurationHistogram = meter.createHistogram("openclaw.webhook.duration_ms", {
        unit: "ms",
        description: "Webhook processing duration",
      });
      
      const webhookErrorCounter = meter.createCounter("openclaw.webhook.error", {
        unit: "1",
        description: "Webhook processing errors",
      });

      // 订阅诊断事件并处理
      unsubscribe = onDiagnosticEvent((evt: DiagnosticEventPayload) => {
        try {
          switch (evt.type) {
            case "webhook.received":
              recordWebhookReceived(evt);
              break;
            case "webhook.processed":
              recordWebhookProcessed(evt);
              break;
            case "webhook.error":
              recordWebhookError(evt);
              break;
            // ... 其他事件类型
          }
        } catch (err) {
          // 可观测性代码失败不影响主流程
          ctx.logger.error(
            `diagnostics-otel: event handler failed (${evt.type}): ${formatError(err)}`
          );
        }
      });

      // 处理 webhook.received 事件
      const recordWebhookReceived = (
        evt: Extract<DiagnosticEventPayload, { type: "webhook.received" }>
      ) => {
        const attrs = {
          "openclaw.channel": evt.channel ?? "unknown",
          "openclaw.webhook": evt.updateType ?? "unknown",
        };
        
        // 记录 Counter 指标
        webhookReceivedCounter.add(1, attrs);
      };

      // 处理 webhook.processed 事件
      const recordWebhookProcessed = (
        evt: Extract<DiagnosticEventPayload, { type: "webhook.processed" }>
      ) => {
        const attrs = {
          "openclaw.channel": evt.channel ?? "unknown",
          "openclaw.webhook": evt.updateType ?? "unknown",
        };
        
        // 记录 Histogram 指标 (耗时分布)
        if (typeof evt.durationMs === "number") {
          webhookDurationHistogram.record(evt.durationMs, attrs);
        }
        
        // 创建 Span (追踪)
        const spanAttrs: Record<string, string | number> = { ...attrs };
        if (evt.chatId !== undefined) {
          spanAttrs["openclaw.chatId"] = String(evt.chatId);
        }
        
        // 创建带回溯时间的 Span
        const span = spanWithDuration("openclaw.webhook.processed", spanAttrs, evt.durationMs);
        span.end();
      };

      // 处理 webhook.error 事件
      const recordWebhookError = (
        evt: Extract<DiagnosticEventPayload, { type: "webhook.error" }>
      ) => {
        const attrs = {
          "openclaw.channel": evt.channel ?? "unknown",
          "openclaw.webhook": evt.updateType ?? "unknown",
        };
        
        // 记录错误 Counter
        webhookErrorCounter.add(1, attrs);
        
        // 创建错误 Span
        const redactedError = redactSensitiveText(evt.error);
        const spanAttrs: Record<string, string | number> = {
          ...attrs,
          "openclaw.error": redactedError,
        };
        
        if (evt.chatId !== undefined) {
          spanAttrs["openclaw.chatId"] = String(evt.chatId);
        }
        
        const span = tracer.startSpan("openclaw.webhook.error", {
          attributes: spanAttrs,
        });
        
        // 标记 Span 为失败状态
        span.setStatus({ 
          code: SpanStatusCode.ERROR, 
          message: redactedError 
        });
        
        span.end();
      };

      // 辅助函数: 创建带历史时间戳的 Span
      const spanWithDuration = (
        name: string,
        attributes: Record<string, string | number>,
        durationMs?: number,
      ) => {
        // 根据 durationMs 反推 startTime
        const startTime = typeof durationMs === "number" 
          ? Date.now() - Math.max(0, durationMs) 
          : undefined;
        
        const span = tracer.startSpan(name, {
          attributes,
          ...(startTime ? { startTime } : {}),
        });
        
        return span;
      };
    },
    
    async stop() {
      unsubscribe?.();
      unsubscribe = null;
      
      if (sdk) {
        await sdk.shutdown().catch(() => undefined);
        sdk = null;
      }
    },
  };
}
```

**设计亮点**:

1. **故障隔离**: 所有事件处理都包裹在 `try-catch` 中
2. **Metrics + Traces 双重可观测性**:
   - Counter: 统计 webhook 接收/错误次数
   - Histogram: 记录 webhook 处理耗时分布
   - Span: 追踪单次 webhook 的完整生命周期
3. **安全性**: 使用 `redactSensitiveText()` 过滤敏感信息 (API keys, tokens)
4. **回溯时间戳**: 通过 `spanWithDuration()` 创建历史 Span,准确反映实际执行时间

---

## 3. 在 Jaeger 中的追踪效果

假设收到一个 Telegram webhook,在 Jaeger 中会看到如下 Trace:

```
Trace: telegram-webhook-12345
├─ openclaw.webhook.processed (120ms)
│  ├─ openclaw.channel: telegram
│  ├─ openclaw.webhook: telegram-post
│  ├─ openclaw.chatId: 123456789
│  └─ status: OK
│
└─ openclaw.message.processed (2500ms)
   ├─ openclaw.channel: telegram
   ├─ openclaw.sessionId: sess_abc123
   ├─ openclaw.outcome: completed
   ├─ openclaw.chatId: 123456789
   └─ status: OK
```

如果处理失败,会看到:

```
Trace: telegram-webhook-error
└─ openclaw.webhook.error (0ms)
   ├─ openclaw.channel: telegram
   ├─ openclaw.webhook: telegram-post
   ├─ openclaw.chatId: 123456789
   ├─ openclaw.error: "Rate limit exceeded"
   └─ status: ERROR
```

---

## 4. Prometheus 指标示例

OpenTelemetry 插件还会导出 Prometheus 指标:

```prometheus
# Webhook 接收计数
openclaw_webhook_received_total{openclaw_channel="telegram",openclaw_webhook="telegram-post"} 1247

# Webhook 处理耗时分布
openclaw_webhook_duration_ms_bucket{openclaw_channel="telegram",openclaw_webhook="telegram-post",le="100"} 823
openclaw_webhook_duration_ms_bucket{openclaw_channel="telegram",openclaw_webhook="telegram-post",le="500"} 1201
openclaw_webhook_duration_ms_bucket{openclaw_channel="telegram",openclaw_webhook="telegram-post",le="1000"} 1247

# Webhook 错误计数
openclaw_webhook_error_total{openclaw_channel="telegram",openclaw_webhook="telegram-post"} 12
```

---

## 5. 核心实现模式总结

### 5.1 业务代码模式

```typescript
// 1. 在关键操作开始时发送事件
emitDiagnosticEvent({
  type: "operation.started",
  operationId: id,
  ...context,
});

// 2. 执行业务逻辑
const startTime = Date.now();
try {
  const result = await doWork();
  
  // 3. 成功时发送事件
  emitDiagnosticEvent({
    type: "operation.completed",
    operationId: id,
    durationMs: Date.now() - startTime,
    ...result,
  });
} catch (error) {
  // 4. 失败时发送事件
  emitDiagnosticEvent({
    type: "operation.error",
    operationId: id,
    error: error.message,
    durationMs: Date.now() - startTime,
  });
  throw error;
}
```

### 5.2 OpenTelemetry 插件模式

```typescript
// 1. 订阅诊断事件
unsubscribe = onDiagnosticEvent((evt) => {
  try {
    switch (evt.type) {
      case "operation.completed":
        // 2. 记录 Metrics
        operationCounter.add(1, { status: "success" });
        operationDuration.record(evt.durationMs, { status: "success" });
        
        // 3. 创建 Span
        const span = spanWithDuration(
          "operation.completed",
          { operationId: evt.operationId },
          evt.durationMs
        );
        span.end();
        break;
        
      case "operation.error":
        // 4. 记录错误 Metrics
        operationCounter.add(1, { status: "error" });
        
        // 5. 创建错误 Span
        const errorSpan = tracer.startSpan("operation.error", {
          attributes: { operationId: evt.operationId, error: evt.error },
        });
        errorSpan.setStatus({ code: SpanStatusCode.ERROR, message: evt.error });
        errorSpan.end();
        break;
    }
  } catch (err) {
    // 6. 可观测性代码失败不影响主流程
    logger.error(`observability failed: ${err}`);
  }
});
```

---

## 6. 对比 HunterClaw 的实现

### OpenClaw 的优势

✅ **事件解耦**: 业务代码通过事件总线发送数据,插件订阅消费  
✅ **插件化**: 可观测性逻辑独立为插件,易于扩展和测试  
✅ **标准化**: 使用 OpenTelemetry 标准,兼容所有主流后端  
✅ **安全性**: 自动过滤敏感信息,防止 API keys 泄露  
✅ **灵活配置**: 支持采样率、信号开关、自定义端点等

### HunterClaw 的当前实现

⚠️ **直接调用**: 业务代码直接调用 Tracer API  
⚠️ **紧耦合**: Tracer 与业务逻辑混合在一起  
⚠️ **扩展困难**: 添加新的可观测性功能需要修改业务代码

### 改进建议

HunterClaw 可以考虑引入类似的事件总线机制:

```cpp
// 业务代码
void ReactEngine::run(const std::string& user_input) {
    // 发送事件
    emit_diagnostic_event({
        .type = "react.started",
        .session_id = session_id,
        .input_length = user_input.size(),
    });
    
    auto start_time = std::chrono::steady_clock::now();
    try {
        auto result = execute_loop(user_input);
        
        emit_diagnostic_event({
            .type = "react.completed",
            .session_id = session_id,
            .duration_ms = calculate_duration(start_time),
            .steps = result.steps.size(),
        });
    } catch (...) {
        emit_diagnostic_event({
            .type = "react.error",
            .session_id = session_id,
            .error = current_exception_message(),
        });
        throw;
    }
}

// 可观测性插件
void ObservabilityPlugin::on_event(const DiagnosticEvent& evt) {
    if (evt.type == "react.completed") {
        // 创建 Span
        auto span = tracer.start_span("react.completed");
        span->set_attribute("session_id", evt.session_id);
        span->set_attribute("duration_ms", evt.duration_ms);
        span->end();
        
        // 记录 Metrics
        react_duration_histogram.record(evt.duration_ms);
    }
}
```

---

## 7. 关键设计原则

### 7.1 故障隔离 (Fault Isolation)

**原则**: 可观测性代码的失败不能影响业务流程

```typescript
// ✅ 正确示例
try {
  const span = tracer.startSpan("operation");
  span.setAttribute("key", value);
  span.end();
} catch (err) {
  // 记录日志但不抛出异常
  logger.error(`observability failed: ${err}`);
}

// ❌ 错误示例
const span = tracer.startSpan("operation");  // 可能抛出异常
span.setAttribute("key", value);              // 影响主流程
```

### 7.2 事件驱动架构 (Event-Driven)

**原则**: 业务代码和可观测性逻辑通过事件解耦

```
业务代码 --emit--> 事件总线 <--subscribe-- 可观测性插件
```

### 7.3 标准化协议 (Standard Protocols)

**原则**: 使用行业标准 (OpenTelemetry) 而非自定义协议

好处:
- 兼容所有 OTLP 兼容后端 (Jaeger, Tempo, Lightstep 等)
- 丰富的生态工具支持
- 社区维护,持续演进

### 7.4 安全优先 (Security First)

**原则**: 自动过滤敏感信息,防止数据泄露

```typescript
// 自动检测和脱敏 API keys, tokens, passwords
const redactedError = redactSensitiveText("Error: API key sk-1234567890abcdef failed");
// 输出: "Error: API key sk-123… failed"
```

---

## 8. 参考资源

- [OpenTelemetry 官方文档](https://opentelemetry.io/docs/)
- [Jaeger Tracing](https://www.jaegertracing.io/)
- [OpenClaw diagnostics-otel 插件源码](https://github.com/example/openclaw/tree/main/extensions/diagnostics-otel)
- [OTLP Specification](https://opentelemetry.io/docs/specs/otlp/)

---

## 总结

OpenClaw 的可观测性实现展示了一个成熟的企业级方案:

1. **业务代码**: 通过 `emitDiagnosticEvent()` 发送事件,简洁清晰
2. **事件总线**: 解耦业务逻辑和可观测性实现
3. **OpenTelemetry 插件**: 订阅事件并导出到后端,支持 Traces、Metrics、Logs
4. **故障隔离**: 可观测性失败不影响主流程
5. **标准协议**: 兼容所有 OpenTelemetry 后端

这种架构可扩展、易维护、安全可靠,是值得 HunterClaw 借鉴的优秀实践。
