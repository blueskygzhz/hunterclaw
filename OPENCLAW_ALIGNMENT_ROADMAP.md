# HunterClaw OpenClaw 对齐路线图

## 📊 当前状态评估 (2026-03-16)

### ✅ 已完成模块

| 模块 | 对齐状态 | 文件 |
|------|---------|------|
| **Verbose & Reasoning 配置** | 100% | `react/verbose_reasoning_config.{h,cc}` |
| **模型目录管理** | 100% | `react/model_catalog.{h,cc}` |
| **ReAct 引擎核心** | 95% | `react/react_engine.{h,cc}` |
| **消息处理器** | 90% | `react/message_processor.{h,cc}` |
| **循环检测** | 95% | `react/loop_detection.{h,cc}` |
| **权限检查** | 95% | `message_processor.cc:272-368` |
| **Echo 检测** | 100% | `message_processor.cc:370-402` |
| **ACK 反应** | 100% | `message_processor.cc:404-430` |
| **Block Reply** | 95% | `react_engine.cc:538-582` |
| **Reasoning Stream** | 95% | `react_engine.cc:483-534` |

### 🚧 部分完成模块

| 模块 | 对齐状态 | 缺失功能 |
|------|---------|---------|
| **MessageProcessor** | 90% | - 缺少 verbose_reasoning_config 集成<br>- 缺少 directive 解析 |
| **Gateway** | 70% | - 缺少工具输出过滤<br>- 缺少客户端 verbose 支持 |
| **SessionManager** | 70% | - 缺少会话级别覆盖<br>- 缺少指令存储 |
| **Agent Scope** | 85% | - 工作空间管理基本完成<br>- 需要权限策略完善 |

### ❌ 未实现模块

| 模块 | 优先级 | 说明 |
|------|-------|------|
| **TUI 集成** | P1 | 根据 verbose 级别渲染工具输出 |
| **Command Detection** | P1 | 解析 `/verbose`, `/reasoning` 指令 |
| **配置文件支持** | P2 | YAML/JSON 配置加载 |

---

## 🎯 对齐路线图

### Phase 1: 核心集成 (P0) - 立即执行

#### 1.1 MessageProcessor 集成 verbose_reasoning_config

**目标**: 在消息处理流程中使用 verbose 和 reasoning 配置

**文件**:
- `src/react/message_processor.cc`
- `include/hunterclaw/react/message_processor.h`

**变更点**:

```cpp
// message_processor.cc:110 - process_message()

ReactResult MessageProcessor::process_message(...) {
    // ========== 1. 解析 directives ==========
    MessageDirectives directives = parse_message_directives(message);
    
    // ========== 2. 获取会话覆盖 ==========
    auto session_result = session_manager_->get_or_create_session(...);
    SessionLevelOverrides* session_overrides = nullptr;
    if (session_result.has_value()) {
        session_overrides = &session_result.value().level_overrides;
    }
    
    // ========== 3. 加载 agent defaults ==========
    auto agent_config = session_manager_->get_agent_config(context.agent_id);
    AgentDefaultsConfig defaults = agent_config.value().agent_defaults;
    
    // ========== 4. 获取模型能力 ==========
    auto model_caps = ModelCatalog::instance().get_capabilities(
        llm_client_->get_provider(), 
        llm_client_->get_model()
    );
    
    // ========== 5. 解析配置 ==========
    auto resolved = resolve_verbose_reasoning_config(
        directives,
        session_overrides,
        defaults,
        model_caps,
        false  // thinking_active
    );
    
    // ========== 6. 应用到 ReactConfig ==========
    config_.react_config.verbose_level = resolved.verbose_level;
    config_.react_config.reasoning_mode = [&]() {
        switch (resolved.reasoning_level) {
            case ReasoningLevel::Off: return ReactConfig::ReasoningMode::Off;
            case ReasoningLevel::On: return ReactConfig::ReasoningMode::Final;
            case ReasoningLevel::Stream: return ReactConfig::ReasoningMode::Stream;
        }
    }();
    
    // ... 继续执行 ReAct ...
}
```

**测试**:
```bash
# 验证 verbose 级别解析
./hunterclaw_test --test=message_processor.verbose_resolution

# 验证 reasoning 自动启用
./hunterclaw_test --test=message_processor.reasoning_auto_enable
```

---

#### 1.2 Gateway 层工具输出过滤

**目标**: 在事件广播前根据 verbose 级别过滤工具输出

**文件**:
- `src/gateway/gateway.cc`
- `src/gateway/methods/chat.cc` (新建)

**实现**:

```cpp
// gateway.cc - 事件广播前过滤

void Gateway::broadcast_tool_event(
    const std::string& session_key,
    const ToolEventData& event_data
) {
    // ========== 1. 解析 verbose 级别 ==========
    VerboseLevel verbose = resolve_tool_verbose_level(session_key);
    
    // ========== 2. 过滤工具输出 ==========
    ToolEventData filtered_data = event_data;
    filter_tool_event_data(filtered_data, verbose);
    
    // ========== 3. 广播给客户端 ==========
    nlohmann::json payload;
    payload["type"] = "tool_event";
    payload["data"] = filtered_data.to_json();
    
    connection_pool_.broadcast_to_session(session_key, payload);
}

VerboseLevel Gateway::resolve_tool_verbose_level(
    const std::string& session_key
) {
    // 从 SessionManager 获取会话
    auto session = session_manager_->get_session(session_key);
    if (!session.has_value()) {
        return VerboseLevel::Off;
    }
    
    // 检查会话覆盖
    if (session->level_overrides.verbose_level.has_value()) {
        return *session->level_overrides.verbose_level;
    }
    
    // 检查 agent defaults
    auto agent_config = session_manager_->get_agent_config(session->agent_id);
    if (agent_config.has_value() && 
        agent_config->agent_defaults.verbose_default.has_value()) {
        return *agent_config->agent_defaults.verbose_default;
    }
    
    return VerboseLevel::Off;
}
```

**OpenClaw 参考**:
- `src/gateway/server-chat.ts:501-551` - 解析 verbose 级别
- `src/gateway/server-chat.ts:539-551` - 过滤工具输出

---

#### 1.3 会话级别覆盖和内联指令解析

**目标**: 支持 `/verbose full` 和 `/reasoning on` 命令

**文件**:
- `src/auto_reply/command_detection.cc` (已存在,需增强)
- `include/hunterclaw/sessions/sessions.h` (增加字段)

**实现**:

**a) 扩展 SessionEntry 结构**:

```cpp
// include/hunterclaw/sessions/sessions.h

struct SessionEntry {
    std::string session_key;
    std::string agent_id;
    std::vector<SessionMessage> messages;
    
    // ✅ 新增: 会话级别覆盖
    SessionLevelOverrides level_overrides;
    
    // ... 其他字段 ...
};

struct SessionLevelOverrides {
    std::optional<VerboseLevel> verbose_level;
    std::optional<ReasoningLevel> reasoning_level;
    std::optional<ThinkingLevel> thinking_level;
};
```

**b) 指令检测和解析**:

```cpp
// src/auto_reply/command_detection.cc

MessageDirectives CommandDetection::parse_directives(
    const std::string& text
) {
    MessageDirectives directives;
    
    // ========== 1. 检测 /verbose 指令 ==========
    std::regex verbose_re(R"(/verbose\s+(off|on|full))");
    std::smatch match;
    
    if (std::regex_search(text, match, verbose_re)) {
        std::string level = match[1].str();
        
        if (level == "off") {
            directives.verbose_level = VerboseLevel::Off;
        } else if (level == "on") {
            directives.verbose_level = VerboseLevel::On;
        } else if (level == "full") {
            directives.verbose_level = VerboseLevel::Full;
        }
        
        directives.is_directive_only = (text.find_first_not_of(" /verboseoful\n") == std::string::npos);
    }
    
    // ========== 2. 检测 /reasoning 指令 ==========
    std::regex reasoning_re(R"(/reasoning\s+(off|on|stream))");
    
    if (std::regex_search(text, match, reasoning_re)) {
        std::string level = match[1].str();
        
        if (level == "off") {
            directives.reasoning_level = ReasoningLevel::Off;
        } else if (level == "on") {
            directives.reasoning_level = ReasoningLevel::On;
        } else if (level == "stream") {
            directives.reasoning_level = ReasoningLevel::Stream;
        }
    }
    
    return directives;
}

// ========== 3. 更新会话覆盖 ==========
void CommandDetection::apply_session_overrides(
    const MessageDirectives& directives,
    sessions::SessionEntry& session
) {
    if (directives.verbose_level.has_value()) {
        session.level_overrides.verbose_level = directives.verbose_level;
        
        // 持久化到数据库
        session_manager_->update_session(session);
    }
    
    if (directives.reasoning_level.has_value()) {
        session.level_overrides.reasoning_level = directives.reasoning_level;
        session_manager_->update_session(session);
    }
}
```

**OpenClaw 参考**:
- `src/sessions/level-overrides.ts:6-31` - 会话覆盖定义
- `src/auto-reply/reply/get-reply-directives.ts:97-165` - 指令解析

---

### Phase 2: 高级功能 (P1) - 短期目标

#### 2.1 SessionManager 完整对齐

**目标**: 完善会话管理,支持覆盖、压缩、元数据

**文件**:
- `src/sessions/session_manager.cc`
- `include/hunterclaw/sessions/sessions.h`

**新增功能**:

1. **会话级别覆盖持久化**
   - 存储到 SQLite 的 sessions 表
   - 添加字段: `verbose_override`, `reasoning_override`

2. **会话压缩 (Compaction)**
   - 已在 `message_processor.cc:801-881` 有基础实现
   - 需要集成 CompactionManager

3. **会话元数据**
   - 记录首次/最后消息时间
   - 记录 token 使用统计

**OpenClaw 参考**:
- `src/sessions/compaction.ts` - 会话压缩
- `src/sessions/session-db.ts` - 数据库操作

---

#### 2.2 TUI 集成

**目标**: 根据 verbose 级别渲染工具输出

**文件**:
- `src/tui/tui.cc` (新建)
- `include/hunterclaw/tui/tui.h` (新建)

**实现**:

```cpp
// tui.cc - 处理工具事件

void TUI::handle_tool_event(const ToolEventData& data) {
    VerboseLevel verbose = current_session_->verbose_level;
    
    switch (data.phase) {
        case ToolPhase::Start:
            // verbose !== "off" 时显示工具调用
            if (verbose != VerboseLevel::Off) {
                print_tool_start(data.tool_name, data.args);
            }
            break;
            
        case ToolPhase::Update:
            // verbose === "full" 时显示增量输出
            if (verbose == VerboseLevel::Full) {
                print_tool_partial_result(data.partial_result);
            }
            break;
            
        case ToolPhase::Result:
            // verbose === "full" 时显示完整输出
            if (verbose == VerboseLevel::Full) {
                print_tool_result(data.result, data.is_error);
            } else if (verbose == VerboseLevel::On) {
                // 只显示成功/失败状态
                print_tool_status(data.tool_name, !data.is_error);
            }
            break;
    }
}
```

**OpenClaw 参考**:
- `src/tui/tui-event-handlers.ts:286-316` - TUI 事件处理

---

#### 2.3 Agent Scope 完善

**目标**: 完善工作空间管理和权限策略

**当前状态**: 已在 `message_processor.cc:236-269` 有基础实现

**待完善**:

1. **工作空间隔离**
   - 确保每个 agent 有独立的工作目录
   - 限制工具只能访问 agent 工作空间

2. **Owner-only 工具**
   - 标记某些工具为 owner-only (如 `execute_command`)
   - 在工具调用时检查权限

3. **工作空间路径规范化**
   - 支持相对路径解析
   - 防止路径遍历攻击

**实现示例**:

```cpp
// tools/tool_manager.cc

bool ToolManager::check_tool_permission(
    const std::string& tool_name,
    const std::string& user_id,
    bool is_owner
) {
    // 加载工具定义
    auto tool = get_tool(tool_name);
    if (!tool) {
        return false;
    }
    
    // 检查 owner-only 标记
    if (tool->owner_only && !is_owner) {
        spdlog::warn("[ToolManager] Tool '{}' requires owner permission", tool_name);
        return false;
    }
    
    return true;
}
```

**OpenClaw 参考**:
- `src/agents/agent-scope.ts` - Agent 工作空间
- `src/auto-reply/permissions/permission-check.ts` - 权限检查

---

### Phase 3: 体验优化 (P2) - 中期目标

#### 3.1 配置文件支持

**目标**: 从 YAML/JSON 加载 `verboseDefault` 等配置

**文件**:
- `src/config/config_system.cc`

**实现**:

```cpp
// config_system.cc

struct AgentDefaultsConfig {
    std::optional<VerboseLevel> verbose_default;
    bool auto_enable_reasoning_for_capable_models = true;
    
    static AgentDefaultsConfig from_yaml(const YAML::Node& node) {
        AgentDefaultsConfig config;
        
        if (node["verboseDefault"]) {
            std::string verbose = node["verboseDefault"].as<std::string>();
            if (verbose == "off") {
                config.verbose_default = VerboseLevel::Off;
            } else if (verbose == "on") {
                config.verbose_default = VerboseLevel::On;
            } else if (verbose == "full") {
                config.verbose_default = VerboseLevel::Full;
            }
        }
        
        if (node["autoEnableReasoningForCapableModels"]) {
            config.auto_enable_reasoning_for_capable_models = 
                node["autoEnableReasoningForCapableModels"].as<bool>();
        }
        
        return config;
    }
};
```

---

#### 3.2 渠道适配

**目标**: 根据渠道特性调整行为

**已实现**: `message_processor.cc:118-135` - 动态禁用 block streaming

**待完善**:

1. **Telegram 优化**
   - 支持 `ReasoningLevel::Stream`
   - Markdown 格式化

2. **Discord 优化**
   - Markdown 推理格式
   - 代码块语法高亮

3. **WhatsApp 优化**
   - 禁用 block streaming (已实现)
   - 禁用推理流式更新

**实现示例**:

```cpp
// message_processor.cc

void MessageProcessor::apply_channel_optimizations(
    const channels::InboundMessage& message,
    MessageProcessorConfig& config
) {
    if (message.channel == "telegram") {
        // Telegram 支持 Markdown 和流式更新
        config.react_config.reasoning_mode = ReactConfig::ReasoningMode::Stream;
        config.react_config.block_reply.enabled = true;
    } else if (message.channel == "whatsapp") {
        // WhatsApp 不支持消息更新
        config.react_config.block_reply.disable_streaming = true;
        config.react_config.reasoning_mode = ReactConfig::ReasoningMode::Final;
    }
}
```

---

#### 3.3 监控和日志

**目标**: 记录配置解析和 reasoning 自动启用

**实现**:

```cpp
// message_processor.cc

void MessageProcessor::log_configuration_resolution(
    const ResolvedConfig& resolved,
    const MessageDirectives& directives,
    const SessionLevelOverrides* session
) {
    spdlog::info("[MessageProcessor] Configuration resolved:");
    spdlog::info("  Verbose: {} (source: {})",
        verbose_level_to_string(resolved.verbose_level),
        directives.verbose_level.has_value() ? "directive" :
        (session && session->verbose_level.has_value() ? "session" : "default"));
    
    spdlog::info("  Reasoning: {} (source: {})",
        reasoning_level_to_string(resolved.reasoning_level),
        directives.reasoning_level.has_value() ? "directive" :
        (session && session->reasoning_level.has_value() ? "session" :
        (resolved.auto_enabled ? "model_capability" : "default")));
    
    if (resolved.auto_enabled) {
        spdlog::info("  ✅ Reasoning auto-enabled for model: {} (supports_reasoning=true)",
            resolved.model_id);
    }
}
```

---

## 📦 交付物清单

### 已完成 (✅)

1. ✅ `react/verbose_reasoning_config.{h,cc}` - 配置解析
2. ✅ `react/model_catalog.{h,cc}` - 模型目录
3. ✅ `tests/react/verbose_reasoning_config_test.cc` - 单元测试
4. ✅ `VERBOSE_REASONING_IMPLEMENTATION.md` - 使用指南
5. ✅ `VERBOSE_REASONING_SUMMARY.md` - 实现总结
6. ✅ `VERBOSE_REASONING_QUICK_REF.md` - 快速参考

### 待完成 (🚧)

#### Phase 1 (P0)
7. 🚧 `message_processor.cc` 集成 (本次实现)
8. 🚧 `gateway.cc` 工具输出过滤 (本次实现)
9. 🚧 `command_detection.cc` 指令解析 (本次实现)
10. 🚧 `sessions.h` 会话覆盖存储 (本次实现)

#### Phase 2 (P1)
11. ⏳ `session_manager.cc` 完整对齐
12. ⏳ `tui.cc` TUI 集成
13. ⏳ `tool_manager.cc` 权限检查增强

#### Phase 3 (P2)
14. ⏳ `config_system.cc` 配置文件支持
15. ⏳ 渠道适配完善
16. ⏳ 监控和日志增强

---

## 🧪 测试策略

### 单元测试

```bash
# 1. Verbose & Reasoning 配置
./hunterclaw_test --test=verbose_reasoning_config.*

# 2. MessageProcessor 集成
./hunterclaw_test --test=message_processor.verbose_integration
./hunterclaw_test --test=message_processor.reasoning_auto_enable

# 3. Gateway 过滤
./hunterclaw_test --test=gateway.tool_event_filtering

# 4. Command Detection
./hunterclaw_test --test=command_detection.directive_parsing
```

### 集成测试

```bash
# 1. 端到端测试 (/verbose full)
./hunterclaw --channel=telegram --test-mode << EOF
/verbose full
Read file README.md
EOF

# 2. Reasoning 自动启用测试
./hunterclaw --model=claude-opus-4 --test-mode << EOF
Analyze this code
EOF

# 3. 会话覆盖测试
./hunterclaw --test-mode << EOF
/verbose full
/reasoning on
Continue with these settings
EOF
```

---

## 📊 进度追踪

| Phase | 进度 | 完成时间 |
|-------|------|---------|
| Phase 1 (P0) | 20% → 100% | 2026-03-16 (今天) |
| Phase 2 (P1) | 0% → 80% | 2026-03-17 |
| Phase 3 (P2) | 0% → 50% | 2026-03-18 |

---

## 🎯 成功指标

### Phase 1 完成标准

- ✅ MessageProcessor 调用 `resolve_verbose_reasoning_config()`
- ✅ Gateway 根据 verbose 级别过滤工具输出
- ✅ 支持 `/verbose full` 指令并持久化到会话
- ✅ 通过所有单元测试
- ✅ 端到端测试通过

### Phase 2 完成标准

- ✅ SessionManager 完整对齐 OpenClaw
- ✅ TUI 正确渲染工具输出
- ✅ Owner-only 工具权限检查

### Phase 3 完成标准

- ✅ 支持从 YAML 加载配置
- ✅ 所有渠道优化完成
- ✅ 监控和日志完善

---

## 📚 参考文档

### OpenClaw 核心文件索引

#### Verbose 相关
1. `src/config/types.agent-defaults.ts:189` - 配置定义
2. `src/auto-reply/reply/get-reply-directives.ts:356` - 解析逻辑
3. `src/gateway/server-chat.ts:539-551` - 输出过滤
4. `src/tui/tui-event-handlers.ts:286-316` - TUI 渲染

#### Reasoning 相关
5. `src/agents/model-selection.ts:669-681` - 模型能力检测
6. `src/auto-reply/reply/get-reply-directives.ts:409` - 自动启用
7. `src/agents/pi-embedded-subscribe.ts:556` - 流式处理

#### Session 相关
8. `src/sessions/level-overrides.ts:6-31` - 会话覆盖
9. `src/sessions/compaction.ts` - 会话压缩
10. `src/sessions/session-db.ts` - 数据库操作

---

**创建时间**: 2026-03-16  
**最后更新**: 2026-03-16  
**状态**: 🚀 Phase 1 执行中
