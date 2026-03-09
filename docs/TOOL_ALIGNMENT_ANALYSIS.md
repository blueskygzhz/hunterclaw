# HunterClaw vs OpenClaw Tool 执行流程对齐分析

## 🎯 执行概要

**对齐目标**: 将 HunterClaw 的 Tool 执行流程 100% 对齐 OpenClaw

**当前对齐度**: **~40%**

**主要差距**:
1. ❌ 缺少 Tool Hook 系统 (before/after)
2. ❌ 缺少 Tool 循环检测机制
3. ⚠️ Tool 权限控制不完整
4. ⚠️ Tool 结果处理简化
5. ❌ 缺少 Stream 输出支持
6. ❌ 缺少 Tool Policy 管道

---

## 📊 详细对比

### 1. Tool 定义和注册

| 功能 | OpenClaw | HunterClaw | 对齐状态 |
|------|----------|------------|---------|
| **Tool 接口** | `AnyAgentTool` | `Tool` 基类 | ✅ 对齐 |
| **参数 Schema** | JSON Schema | `Tool::Schema` | ✅ 对齐 |
| **ownerOnly** | `tool.ownerOnly` | `Tool::owner_only()` | ✅ 对齐 |
| **Tool 注册** | `ToolManager` | `ToolManager` | ✅ 对齐 |
| **Plugin 工具** | `resolvePluginTools()` | ❌ 缺失 | ⚠️ 部分缺失 |

**OpenClaw 特性**:
```typescript
export type AnyAgentTool = AgentTool<any, unknown> & {
  ownerOnly?: boolean;
};

// 多种工具来源
- 核心工具 (read, write, exec...)
- OpenClaw 工具 (browser, canvas...)
- 插件工具 (通过 resolvePluginTools)
- MCP 工具 (通过 mcporter)
```

**HunterClaw 现状**:
```cpp
class Tool {
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual Schema parameters() const = 0;
    virtual ToolResult execute(const core::JsonValue& params) = 0;
    virtual bool requires_approval() const { return false; }
    virtual bool owner_only() const { return false; }
};

// 工具来源
✅ 内置工具 (BuiltinTools)
✅ MCP 工具 (McpToolAdapter)
❌ 插件工具系统 (未实现)
```

---

### 2. Tool 调用流程

#### 2.1 OpenClaw 完整流程

```typescript
execute: async (toolCallId, params, signal, onUpdate) => {
  // 1. Before Hook (插件可修改参数或阻止执行)
  const hookOutcome = await runBeforeToolCallHook({
    toolName, params, toolCallId, ctx
  });
  if (hookOutcome.blocked) {
    throw new Error(hookOutcome.reason);
  }
  
  // 2. 循环检测
  const loopResult = detectToolCallLoop(state, toolName, params, config);
  if (loopResult.stuck && loopResult.level === "critical") {
    throw new Error(loopResult.message);
  }
  
  // 3. 参数规范化
  const normalizedParams = normalizeToolParams(hookOutcome.params);
  
  // 4. 执行工具
  const rawResult = await tool.execute(
    toolCallId, normalizedParams, signal, onUpdate
  );
  
  // 5. 结果规范化
  const result = normalizeToolExecutionResult({
    toolName, result: rawResult
  });
  
  // 6. 图片消毒
  if (hasImages(result)) {
    await sanitizeToolResultImages(result, toolName, limits);
  }
  
  // 7. After Hook (日志记录、审计)
  await emitHook("after_tool_call", {
    toolName, params, result, durationMs
  });
  
  // 8. 记录循环状态
  recordToolCallOutcome(state, { toolName, params, result, toolCallId });
  
  return result;
}
```

**关键特性**:
- ✅ **Before Hook**: 插件可修改参数或阻止执行
- ✅ **循环检测**: 防止 Agent 陷入重复调用
- ✅ **参数规范化**: snake_case ↔ camelCase 转换
- ✅ **结果规范化**: 统一结果格式
- ✅ **图片消毒**: 自动压缩和降采样
- ✅ **After Hook**: 事件发射、日志记录
- ✅ **循环记录**: 跟踪工具调用历史
- ✅ **Stream 支持**: `onUpdate` 回调

#### 2.2 HunterClaw 当前流程

```cpp
ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params
) {
    // 1. 查找工具
    auto tool = get_tool(name);
    if (!tool) {
        return tool_result::error("Tool not found: " + name);
    }
    
    // 2. 执行工具
    try {
        return tool->execute(params);
    } catch (const std::exception& e) {
        return tool_result::error(
            std::string("Tool execution failed: ") + e.what()
        );
    }
}
```

**缺失功能**:
- ❌ **Before Hook**
- ❌ **循环检测**
- ❌ **参数规范化**
- ❌ **结果规范化**
- ❌ **图片消毒**
- ❌ **After Hook**
- ❌ **循环记录**
- ❌ **Stream 支持**

---

### 3. Tool Hook 系统

#### 3.1 OpenClaw Hook 架构

```typescript
// Hook 类型
export type PluginHookName =
  | "before_tool_call"
  | "after_tool_call"
  | "tool_result_persist"
  | ...;

// Before Hook
export async function runBeforeToolCallHook(args: {
  toolName: string;
  params: unknown;
  toolCallId?: string;
  ctx?: HookContext;
}): Promise<HookOutcome>

type HookOutcome = 
  | { blocked: true; reason: string }
  | { blocked: false; params: unknown };

// After Hook (通过插件系统)
api.on("after_tool_call", async (event, ctx) => {
  // event: { toolName, params, result, error, durationMs }
});
```

**使用场景**:
1. **参数修改**: 自动填充默认值、转换格式
2. **权限检查**: 动态权限验证
3. **速率限制**: 限制工具调用频率
4. **日志记录**: 记录所有工具调用
5. **审计**: 合规性审计
6. **度量**: 性能监控

#### 3.2 HunterClaw 需要实现

```cpp
// 目标架构
class ToolManager {
public:
    // Hook 注册
    using BeforeToolCallHook = std::function<
        core::Result<core::JsonValue>(  // 返回修改后的 params 或错误
            const std::string& tool_name,
            const core::JsonValue& params,
            const ToolContext& ctx
        )
    >;
    
    using AfterToolCallHook = std::function<
        void(
            const std::string& tool_name,
            const core::JsonValue& params,
            const ToolResult& result,
            double duration_ms
        )
    >;
    
    void register_before_hook(BeforeToolCallHook hook);
    void register_after_hook(AfterToolCallHook hook);

private:
    std::vector<BeforeToolCallHook> before_hooks_;
    std::vector<AfterToolCallHook> after_hooks_;
};
```

---

### 4. Tool 循环检测

#### 4.1 OpenClaw 循环检测机制

**检测器类型**:
```typescript
export type LoopDetectorKind =
  | "generic_repeat"           // 通用重复: 相同调用 N 次
  | "known_poll_no_progress"   // 无进展轮询: 相同结果 N 次
  | "global_circuit_breaker"   // 全局断路器: 总调用超限
  | "ping_pong";               // 乒乓模式: A→B→A→B...
```

**配置**:
```typescript
{
  loopDetection: {
    enabled: true,
    warningThreshold: 10,      // 警告阈值
    criticalThreshold: 20,     // 严重阈值 (阻止执行)
    globalCircuitBreakerThreshold: 30,
    historySize: 30,           // 历史记录窗口
    detectors: {
      genericRepeat: true,
      knownPollNoProgress: true,
      pingPong: true,
    },
  },
}
```

**检测逻辑**:
```typescript
export function detectToolCallLoop(
  state: SessionState,
  toolName: string,
  params: unknown,
  config?: ToolLoopDetectionConfig
): LoopDetectionResult

type LoopDetectionResult =
  | { stuck: false }
  | {
      stuck: true;
      level: "warning" | "critical";
      detector: LoopDetectorKind;
      count: number;
      message: string;
      pairedToolName?: string;  // ping-pong 模式
    };
```

**工具调用哈希**:
```typescript
// 基于工具名 + 参数
function hashToolCall(toolName: string, params: unknown): string {
  return `${toolName}:${digestStable(params)}`;
}

// 基于工具名 + 参数 + 结果 (无进展检测)
function hashToolOutcome(
  toolName: string,
  params: unknown,
  result: unknown,
  error: unknown
): string | undefined
```

#### 4.2 HunterClaw 需要实现

```cpp
// 目标实现
namespace hunterclaw {
namespace tools {

// 循环检测器类型
enum class LoopDetectorKind {
    GenericRepeat,           // 通用重复
    KnownPollNoProgress,     // 无进展轮询
    GlobalCircuitBreaker,    // 全局断路器
    PingPong                 // 乒乓模式
};

// 检测结果
struct LoopDetectionResult {
    bool stuck = false;
    std::string level;       // "warning" 或 "critical"
    LoopDetectorKind detector;
    int count = 0;
    std::string message;
    std::string paired_tool_name;  // ping-pong 配对工具
};

// 循环检测器
class ToolLoopDetector {
public:
    LoopDetectionResult detect(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolLoopDetectionConfig& config
    );
    
    void record_call(
        const std::string& tool_name,
        const core::JsonValue& params,
        const std::string& tool_call_id
    );
    
    void record_outcome(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result
    );

private:
    // 调用历史 (滑动窗口)
    std::deque<ToolCallRecord> call_history_;
    
    // 检测算法
    std::optional<LoopDetectionResult> detect_generic_repeat();
    std::optional<LoopDetectionResult> detect_poll_no_progress();
    std::optional<LoopDetectionResult> detect_global_circuit_breaker();
    std::optional<LoopDetectionResult> detect_ping_pong();
};

} // namespace tools
} // namespace hunterclaw
```

---

### 5. Tool 权限控制

#### 5.1 OpenClaw 权限系统

**多层权限架构**:

```
┌──────────────────────────────────┐
│   1. Tool Profile                │ 预定义工具集
│   (minimal/coding/messaging/full)│
├──────────────────────────────────┤
│   2. Provider Profile            │ 按 LLM Provider 策略
│   (claude/gemini/...)            │
├──────────────────────────────────┤
│   3. Global Policy               │ 全局 allow/deny
│   (tools.allow/tools.deny)      │
├──────────────────────────────────┤
│   4. Global Provider Policy      │ 全局按 Provider 策略
│   (tools.byProvider)            │
├──────────────────────────────────┤
│   5. Agent Policy                │ Agent 级别策略
│   (agents.*.tools.allow)        │
├──────────────────────────────────┤
│   6. Agent Provider Policy       │ Agent 按 Provider 策略
│   (agents.*.tools.byProvider)   │
├──────────────────────────────────┤
│   7. Group Policy                │ 群组级别策略
├──────────────────────────────────┤
│   8. Sandbox Policy              │ 沙箱策略
├──────────────────────────────────┤
│   9. Subagent Policy             │ 子 Agent 策略
└──────────────────────────────────┘
```

**策略管道**:
```typescript
export function applyToolPolicyPipeline(params: {
  tools: AnyAgentTool[];
  toolMeta: (tool: AnyAgentTool) => { pluginId: string } | undefined;
  warn: (message: string) => void;
  steps: ToolPolicyPipelineStep[];
}): AnyAgentTool[]

// 策略步骤
type ToolPolicyPipelineStep = {
  name: string;
  policy: ToolPolicyLike;
  priority?: number;
};
```

**Tool Profile**:
```typescript
const CORE_TOOL_PROFILES = {
  minimal: {
    allow: ["session_status"]
  },
  coding: {
    allow: [
      "read", "write", "edit", "apply_patch",
      "exec", "process",
      "web_search", "web_fetch",
      // ...
    ]
  },
  messaging: {
    allow: ["message", "sessions_list", ...]
  },
  full: {}  // 无限制
};
```

**Tool 分组**:
```typescript
export const CORE_TOOL_GROUPS = {
  "group:fs": ["read", "write", "edit", "apply_patch"],
  "group:runtime": ["exec", "process"],
  "group:web": ["web_search", "web_fetch"],
  "group:memory": ["memory_search", "memory_get"],
  // ...
};
```

#### 5.2 HunterClaw 当前状态

```cpp
// 简单的 owner_only 检查
class Tool {
    virtual bool owner_only() const { return false; }
};

// 没有策略管道
// 没有 Tool Profile
// 没有 Tool 分组
// 没有多层权限控制
```

**需要实现**:
```cpp
// Tool 策略系统
namespace hunterclaw {
namespace tools {

// Tool Profile
enum class ToolProfile {
    Minimal,     // 最小工具集
    Coding,      // 编程工具集
    Messaging,   // 消息工具集
    Full         // 完整工具集
};

// Tool 策略
struct ToolPolicy {
    std::vector<std::string> allow;  // 允许列表
    std::vector<std::string> deny;   // 拒绝列表
};

// Tool 策略管道
class ToolPolicyPipeline {
public:
    void add_step(const std::string& name, const ToolPolicy& policy);
    std::vector<std::shared_ptr<Tool>> apply(
        std::vector<std::shared_ptr<Tool>> tools
    );
};

// Tool 分组
class ToolGroups {
public:
    static std::vector<std::string> expand(const std::string& group);
    // "group:fs" => ["read", "write", "edit", "apply_patch"]
};

} // namespace tools
} // namespace hunterclaw
```

---

### 6. Tool 结果处理

#### 6.1 OpenClaw 结果格式

```typescript
type AgentToolResult<T> = {
  content: Array<
    | { type: "text"; text: string }
    | { type: "image"; data: string; mimeType: string }
  >;
  details?: T;
};
```

**多模态支持**:
```typescript
// 文本结果
{
  content: [{ type: "text", text: "File contents..." }],
  details: { path: "main.cc", size: 1024 }
}

// 图片结果
{
  content: [
    { type: "text", text: "MEDIA:/path/to/image.png" },
    { type: "image", data: "base64...", mimeType: "image/png" }
  ],
  details: { path: "/path/to/image.png", width: 1920, height: 1080 }
}

// 混合结果
{
  content: [
    { type: "text", text: "Screenshot captured" },
    { type: "image", data: "base64...", mimeType: "image/png" },
    { type: "text", text: "Additional info" }
  ],
  details: { ... }
}
```

**图片消毒**:
```typescript
export async function sanitizeToolResultImages(
  result: AgentToolResult<unknown>,
  label: string,
  opts: ImageSanitizationLimits = {}
): Promise<AgentToolResult<unknown>>

// 限制
{
  maxDimensionPx: 2000,  // 最大尺寸
  maxBytes: 5 * 1024 * 1024,  // 5MB
  quality: 0.9,  // 压缩质量
}
```

#### 6.2 HunterClaw 当前实现

```cpp
struct ToolResultContent {
    enum class Type { Text, Image, Json };
    Type type;
    std::string text;       // type=Text/Json
    std::string data;       // type=Image (base64)
    std::string mime_type;  // type=Image
};

struct ToolResult {
    std::vector<ToolResultContent> content;
    core::JsonValue details;
    bool success = true;
    std::string error;
};
```

**已支持**: ✅ 多模态 content  
**缺失**: ❌ 图片消毒、❌ 结果规范化

---

### 7. Stream 输出支持

#### 7.1 OpenClaw Stream 机制

```typescript
execute: async (toolCallId, params, signal, onUpdate) => {
  // Stream 进度更新
  if (onUpdate) {
    onUpdate({
      type: "progress",
      message: "Processing...",
      progress: 0.5
    });
  }
  
  // Stream 部分结果
  if (onUpdate) {
    onUpdate({
      type: "partial",
      content: [{ type: "text", text: "Partial output..." }]
    });
  }
  
  // 最终结果
  return result;
}
```

**使用场景**:
- 长时间执行的工具 (exec, web_search)
- 实时反馈 (进度条、状态更新)
- 流式输出 (逐行输出命令结果)

#### 7.2 HunterClaw 需要实现

```cpp
// Tool 执行回调
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

struct ToolUpdate {
    enum class Type { Progress, Partial, Status };
    Type type;
    std::string message;
    double progress = 0.0;  // 0.0 ~ 1.0
    std::optional<ToolResultContent> partial_content;
};

class Tool {
    virtual ToolResult execute(
        const core::JsonValue& params,
        ToolUpdateCallback on_update = nullptr  // Stream 回调
    ) = 0;
};
```

---

## 🎯 对齐优先级

### P0 (立即实施)

1. **Tool Hook 系统** - 核心扩展机制
2. **Tool 循环检测** - 防止 Agent 卡死
3. **Tool 策略管道** - 细粒度权限控制

### P1 (短期)

4. **Tool 结果规范化** - 统一结果格式
5. **图片消毒** - 图片处理优化
6. **Stream 输出支持** - 实时反馈

### P2 (可选)

7. **Tool Profile** - 预定义工具集
8. **Tool 分组** - 工具组管理
9. **Tool Display** - UI 显示配置

---

## 📋 实施计划

### 阶段 1: Hook 系统 (3天)

**新增文件**:
- `include/hunterclaw/tools/tool_hooks.h` (150行)
- `src/tools/tool_hooks.cc` (300行)

**核心实现**:
```cpp
class ToolHookManager {
public:
    using BeforeHook = std::function<
        core::Result<core::JsonValue>(
            const std::string&, const core::JsonValue&, const ToolContext&
        )
    >;
    using AfterHook = std::function<
        void(const std::string&, const core::JsonValue&, 
             const ToolResult&, double)
    >;
    
    void register_before_hook(const std::string& name, BeforeHook hook);
    void register_after_hook(const std::string& name, AfterHook hook);
    
    core::Result<core::JsonValue> run_before_hooks(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolContext& ctx
    );
    
    void run_after_hooks(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result,
        double duration_ms
    );
};
```

**集成到 ToolManager**:
```cpp
ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params
) {
    auto start = std::chrono::steady_clock::now();
    
    // 1. Before Hooks
    auto hook_result = hook_manager_->run_before_hooks(name, params, context_);
    if (hook_result.is_err()) {
        return tool_result::error(hook_result.error);
    }
    auto modified_params = hook_result.value;
    
    // 2. 执行工具
    auto tool = get_tool(name);
    if (!tool) {
        return tool_result::error("Tool not found: " + name);
    }
    
    ToolResult result;
    try {
        result = tool->execute(modified_params);
    } catch (const std::exception& e) {
        result = tool_result::error(std::string("Tool execution failed: ") + e.what());
    }
    
    // 3. After Hooks
    auto end = std::chrono::steady_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
    hook_manager_->run_after_hooks(name, modified_params, result, duration_ms);
    
    return result;
}
```

---

### 阶段 2: 循环检测 (2天)

**新增文件**:
- `include/hunterclaw/tools/loop_detector.h` (200行)
- `src/tools/loop_detector.cc` (500行)

**核心实现**:
```cpp
class ToolLoopDetector {
public:
    struct Config {
        bool enabled = true;
        int warning_threshold = 10;
        int critical_threshold = 20;
        int global_circuit_breaker_threshold = 30;
        int history_size = 30;
        
        struct Detectors {
            bool generic_repeat = true;
            bool known_poll_no_progress = true;
            bool ping_pong = true;
        } detectors;
    };
    
    LoopDetectionResult detect(
        const std::string& tool_name,
        const core::JsonValue& params
    );
    
    void record_call(
        const std::string& tool_name,
        const core::JsonValue& params,
        const std::string& tool_call_id
    );
    
    void record_outcome(
        const std::string& tool_name,
        const core::JsonValue& params,
        const ToolResult& result
    );
    
private:
    struct CallRecord {
        std::string tool_name;
        std::string params_hash;
        std::string outcome_hash;
        int64_t timestamp;
        std::string tool_call_id;
    };
    
    std::deque<CallRecord> history_;
    Config config_;
    
    // 检测算法
    std::optional<LoopDetectionResult> detect_generic_repeat();
    std::optional<LoopDetectionResult> detect_poll_no_progress();
    std::optional<LoopDetectionResult> detect_global_circuit_breaker();
    std::optional<LoopDetectionResult> detect_ping_pong();
    
    // 哈希函数
    std::string hash_params(const core::JsonValue& params);
    std::string hash_outcome(const ToolResult& result);
};
```

**集成到 ToolManager**:
```cpp
ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params
) {
    // 1. 循环检测
    if (loop_detector_) {
        auto loop_result = loop_detector_->detect(name, params);
        if (loop_result.stuck) {
            if (loop_result.level == "critical") {
                return tool_result::error(loop_result.message);
            } else {
                // 警告但不阻止
                log_->warn("Loop warning for {}: {}", name, loop_result.message);
            }
        }
        
        // 记录调用
        loop_detector_->record_call(name, params, generate_tool_call_id());
    }
    
    // 2. Before Hooks...
    // 3. 执行工具...
    
    // 4. 记录结果
    if (loop_detector_) {
        loop_detector_->record_outcome(name, params, result);
    }
    
    // 5. After Hooks...
    
    return result;
}
```

---

### 阶段 3: 策略管道 (2天)

**新增文件**:
- `include/hunterclaw/tools/tool_policy.h` (250行)
- `src/tools/tool_policy.cc` (400行)

**核心实现**:
```cpp
// Tool Profile
enum class ToolProfile {
    Minimal,
    Coding,
    Messaging,
    Full
};

// Tool 策略
struct ToolPolicy {
    std::vector<std::string> allow;
    std::vector<std::string> deny;
    
    bool is_allowed(const std::string& tool_name) const;
};

// Tool 策略步骤
struct ToolPolicyStep {
    std::string name;
    ToolPolicy policy;
    int priority = 0;
};

// Tool 策略管道
class ToolPolicyPipeline {
public:
    void add_step(const ToolPolicyStep& step);
    
    std::vector<std::shared_ptr<Tool>> apply(
        const std::vector<std::shared_ptr<Tool>>& tools,
        std::function<void(const std::string&)> warn_callback = nullptr
    );
    
private:
    std::vector<ToolPolicyStep> steps_;
    
    bool is_tool_allowed(
        const std::string& tool_name,
        const std::vector<ToolPolicyStep>& steps
    );
};

// Tool 分组
class ToolGroups {
public:
    static void register_group(
        const std::string& name,
        const std::vector<std::string>& tools
    );
    
    static std::vector<std::string> expand(const std::string& spec);
    // "group:fs" => ["read", "write", "edit", "apply_patch"]
    // "group:*" => 所有组
    
private:
    static std::map<std::string, std::vector<std::string>> groups_;
};

// Tool Profile 定义
class ToolProfiles {
public:
    static ToolPolicy get_profile(ToolProfile profile);
    
    static void register_builtin_profiles();
    // minimal, coding, messaging, full
};
```

**集成到 ToolManager**:
```cpp
class ToolManager {
public:
    // 应用策略
    void apply_policy_pipeline(const ToolPolicyPipeline& pipeline);
    
    // 设置 Profile
    void set_tool_profile(ToolProfile profile);
    
private:
    std::shared_ptr<ToolPolicyPipeline> policy_pipeline_;
};
```

---

### 阶段 4: 结果优化 (2天)

**图片消毒**:
```cpp
namespace hunterclaw {
namespace tools {

struct ImageSanitizationLimits {
    int max_dimension_px = 2000;
    int max_bytes = 5 * 1024 * 1024;  // 5MB
    double quality = 0.9;
};

// 图片消毒
core::Result<ToolResult> sanitize_tool_result_images(
    const ToolResult& result,
    const std::string& label,
    const ImageSanitizationLimits& limits = {}
);

} // namespace tools
} // namespace hunterclaw
```

**结果规范化**:
```cpp
// 规范化工具结果
ToolResult normalize_tool_result(
    const std::string& tool_name,
    const ToolResult& raw_result
);

// 确保 content 不为空
// 添加默认 details
// 验证图片格式
```

---

### 阶段 5: Stream 支持 (1天)

**Stream 回调**:
```cpp
// Tool Update
struct ToolUpdate {
    enum class Type { Progress, Partial, Status };
    Type type;
    std::string message;
    double progress = 0.0;
    std::optional<ToolResultContent> partial_content;
};

// Tool 回调
using ToolUpdateCallback = std::function<void(const ToolUpdate&)>;

// Tool 接口更新
class Tool {
    virtual ToolResult execute(
        const core::JsonValue& params,
        ToolUpdateCallback on_update = nullptr
    ) = 0;
};

// ToolManager 传递回调
ToolResult ToolManager::execute_tool(
    const std::string& name,
    const core::JsonValue& params,
    ToolUpdateCallback on_update
);
```

---

## 📊 完成后对齐度

| 功能 | OpenClaw | HunterClaw (目标) | 状态 |
|------|----------|-------------------|------|
| Tool Hook 系统 | ✅ | 🎯 | 阶段1 |
| 循环检测 | ✅ | 🎯 | 阶段2 |
| 策略管道 | ✅ | 🎯 | 阶段3 |
| Tool Profile | ✅ | 🎯 | 阶段3 |
| Tool 分组 | ✅ | 🎯 | 阶段3 |
| 图片消毒 | ✅ | 🎯 | 阶段4 |
| 结果规范化 | ✅ | 🎯 | 阶段4 |
| Stream 输出 | ✅ | 🎯 | 阶段5 |
| 参数规范化 | ✅ | 🎯 | 阶段1 |
| 多模态结果 | ✅ | ✅ | 已支持 |

**完成后对齐度**: **100%** ✅

---

## 🚀 快速开始示例

### 使用 Hook 系统

```cpp
// 注册 Before Hook (参数验证)
tool_manager->register_before_hook("validation", 
    [](const std::string& tool_name, const core::JsonValue& params, const ToolContext& ctx) {
        if (tool_name == "exec") {
            // 检查是否在允许列表中
            if (!is_command_allowed(params["command"])) {
                return core::Result<core::JsonValue>::Err("Command not allowed");
            }
        }
        return core::Result<core::JsonValue>::Ok(params);
    }
);

// 注册 After Hook (日志记录)
tool_manager->register_after_hook("logging",
    [](const std::string& tool_name, const core::JsonValue& params, 
       const ToolResult& result, double duration_ms) {
        log->info("Tool {} executed in {:.2f}ms", tool_name, duration_ms);
    }
);
```

### 使用循环检测

```cpp
// 配置循环检测
ToolLoopDetector::Config config;
config.enabled = true;
config.warning_threshold = 10;
config.critical_threshold = 20;

auto loop_detector = std::make_shared<ToolLoopDetector>(config);
tool_manager->set_loop_detector(loop_detector);

// 自动检测并阻止循环
// AI 重复调用 read_file 20 次 => 自动阻止
```

### 使用策略管道

```cpp
// 设置 Tool Profile
tool_manager->set_tool_profile(ToolProfile::Coding);
// => 只允许 read, write, exec, web_search 等编程工具

// 或者自定义策略
ToolPolicy custom_policy;
custom_policy.allow = {"read", "write", "list_dir"};
custom_policy.deny = {"exec"};  // 禁止命令执行

ToolPolicyPipeline pipeline;
pipeline.add_step({"custom", custom_policy, 10});
tool_manager->apply_policy_pipeline(pipeline);
```

---

## 📝 总结

通过 5 个阶段的实施,HunterClaw 的 Tool 执行流程将 **100% 对齐 OpenClaw**,获得:

1. ✅ **Hook 系统** - 灵活的扩展机制
2. ✅ **循环检测** - 防止 Agent 卡死
3. ✅ **策略管道** - 细粒度权限控制
4. ✅ **结果优化** - 图片消毒、结果规范化
5. ✅ **Stream 支持** - 实时反馈

**预计工时**: 约 10 天  
**优先级**: P0 (核心功能)

详细实现请参考本报告的实施计划章节! 🎉
