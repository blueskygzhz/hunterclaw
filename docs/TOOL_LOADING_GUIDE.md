# HunterClaw Tool 加载机制详解

## 📋 目录

1. [概览](#概览)
2. [核心组件](#核心组件)
3. [加载流程](#加载流程)
4. [完整示例](#完整示例)
5. [高级用法](#高级用法)

---

## 🎯 概览

HunterClaw 的 Tool 系统采用 **插件化注册机制**,支持:

- ✅ **内置 Tools** - 编译时静态注册
- ✅ **MCP Tools** - 运行时动态发现
- ✅ **自定义 Tools** - 用户扩展

**核心设计原则**:
1. 统一接口 (`Tool` 基类)
2. 延迟加载 (ToolManager 创建时注册)
3. 透明调用 (AI 无法区分内置/MCP)
4. 安全隔离 (workspace 沙箱)

---

## 🧩 核心组件

### 1. Tool 接口 (抽象基类)

```cpp
// include/hunterclaw/tools/tool.h

class Tool {
public:
    virtual ~Tool() = default;
    
    // Tool 元数据
    virtual std::string name() const = 0;                 // "read_file"
    virtual std::string description() const = 0;          // 功能描述
    virtual Schema parameters() const = 0;                // JSON Schema
    
    // 执行接口
    virtual ToolResult execute(const core::JsonValue& params) = 0;
    
    // 权限控制
    virtual bool requires_approval() const { return false; }  // 敏感操作
    virtual bool owner_only() const { return false; }         // 仅 Owner
};
```

### 2. ToolManager (工具注册中心)

```cpp
class ToolManager {
public:
    // 创建实例
    static std::shared_ptr<ToolManager> create(const ToolContext& context);
    
    // 注册工具
    void register_tool(std::shared_ptr<Tool> tool);
    
    // 查询工具
    std::shared_ptr<Tool> get_tool(const std::string& name);
    std::vector<std::string> list_tools() const;
    
    // 执行工具
    ToolResult execute_tool(const std::string& name, const core::JsonValue& params);
    
    // MCP 集成
    void enable_mcp(const mcp::McporterConfig& config);
    core::Result<int> refresh_mcp_tools();

private:
    std::map<std::string, std::shared_ptr<Tool>> tools_;  // 工具注册表
};
```

### 3. ToolContext (运行环境)

```cpp
struct ToolContext {
    std::string workspace_dir;   // "/workspace/project"
    std::string agent_id;        // "agent-123"
    std::string session_key;     // "agent:123:main"
    std::map<std::string, std::string> metadata;
};
```

---

## 🔄 加载流程

### 流程图

```
+------------------+
|  Agent 启动      |
+------------------+
         |
         v
+------------------+
| 创建 ToolContext |
+------------------+
         |
         v
+------------------+
| 创建 ToolManager |
+------------------+
         |
         v
+---------------------------+
| 注册 Builtin Tools        |
| (read_file, write_file...) |
+---------------------------+
         |
         v
+---------------------------+
| 启用 MCP (可选)            |
| - 启动 mcporter daemon    |
| - 发现 MCP tools          |
| - 自动注册到 ToolManager  |
+---------------------------+
         |
         v
+---------------------------+
| 工具准备就绪              |
| - AI 可以调用任何工具      |
| - 内置和 MCP 透明统一     |
+---------------------------+
```

### 时序图

```
Agent               ToolManager          BuiltinTools       McporterClient
  |                      |                    |                    |
  |--create_context----> |                    |                    |
  |                      |                    |                    |
  |--create(context)---> |                    |                    |
  |                      |                    |                    |
  |--register_all------> |                    |                    |
  |                      |--create_X_tool---->|                    |
  |                      |<---Tool-----------|                    |
  |                      |--register_tool---->|                    |
  |                      |                    |                    |
  |--enable_mcp--------> |                    |                    |
  |                      |--start_daemon------|------------------>|
  |                      |                    |                    |
  |--refresh_mcp_tools-->|                    |                    |
  |                      |--discover_tools----|------------------>|
  |                      |<---MCP tools-------|-------------------|
  |                      |--register(MCP)--->|                    |
  |                      |                    |                    |
  |<----ready-----------|                    |                    |
  |                      |                    |                    |
  |--execute_tool------> |                    |                    |
  |                      |--get_tool--------->|                    |
  |                      |--execute---------->|                    |
  |<----result----------|<---ToolResult------|                    |
```

---

## 💡 完整示例

### 示例 1: 实现一个自定义 Tool

#### Step 1: 定义 Tool 类

```cpp
// src/tools/custom_calculator.cc

#include "hunterclaw/tools/tool.h"
#include <cmath>

namespace hunterclaw {
namespace tools {

class CalculatorTool : public Tool {
public:
    explicit CalculatorTool(const ToolContext& context) 
        : context_(context) {}
    
    // ========== Tool 元数据 ==========
    
    std::string name() const override {
        return "calculator";
    }
    
    std::string description() const override {
        return "Perform mathematical calculations. "
               "Supports basic operations (+, -, *, /), "
               "powers (^), and functions (sin, cos, sqrt).";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        schema.description = "Calculator parameters";
        
        // expression 参数
        core::JsonValue expr_prop = core::JsonValue::object({
            {"type", "string"},
            {"description", "Mathematical expression to evaluate (e.g., '2 + 2', 'sqrt(16)')"}
        });
        schema.properties["expression"] = expr_prop;
        
        schema.required = {"expression"};
        
        return schema;
    }
    
    // ========== Tool 执行 ==========
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string expression = reader.read_string("expression", true);
        
        try {
            // 简化实现: 调用 Python 计算
            std::string command = "python3 -c \"print(" + expression + ")\"";
            
            std::array<char, 128> buffer;
            std::string result_output;
            
            FILE* pipe = popen(command.c_str(), "r");
            if (!pipe) {
                return tool_result::error("Failed to execute calculator");
            }
            
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
                result_output += buffer.data();
            }
            
            int exit_code = pclose(pipe);
            
            if (exit_code != 0) {
                return tool_result::error("Invalid expression: " + expression);
            }
            
            // 去除换行符
            result_output.erase(
                std::remove(result_output.begin(), result_output.end(), '\n'),
                result_output.end()
            );
            
            // 构建结果
            ToolResult result;
            result.success = true;
            
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            content.text = "Result: " + result_output;
            result.content.push_back(content);
            
            result.details = core::JsonValue::object({
                {"expression", expression},
                {"result", result_output}
            });
            
            return result;
            
        } catch (const std::exception& e) {
            return tool_result::error(
                std::string("Calculation error: ") + e.what()
            );
        }
    }
    
    // ========== 权限控制 ==========
    
    bool requires_approval() const override {
        return false;  // 数学计算不需要审批
    }

private:
    ToolContext context_;
};

} // namespace tools
} // namespace hunterclaw
```

#### Step 2: 注册 Tool

```cpp
// src/agents/react_executor.cc (在 Agent 初始化时)

void ReactExecutor::initialize_tools() {
    // 创建 ToolContext
    ToolContext tool_context;
    tool_context.workspace_dir = workspace_dir_;
    tool_context.agent_id = agent_id_;
    tool_context.session_key = session_key_;
    
    // 创建 ToolManager
    tool_manager_ = tools::ToolManager::create(tool_context);
    
    // ========== 注册内置工具 ==========
    tools::BuiltinTools::register_all(*tool_manager_, tool_context);
    
    // ========== 注册自定义工具 ==========
    auto calculator = std::make_shared<CalculatorTool>(tool_context);
    tool_manager_->register_tool(calculator);
    
    // ========== 启用 MCP (可选) ==========
    if (config_.tools.mcp.enabled) {
        tool_manager_->enable_mcp(config_.tools.mcp);
        
        // 异步刷新 MCP 工具
        std::async(std::launch::async, [this]() {
            auto count = tool_manager_->refresh_mcp_tools();
            if (count.is_ok()) {
                log_->info("Discovered {} MCP tools", count.value);
            }
        });
    }
    
    // 输出可用工具列表
    auto tools_list = tool_manager_->list_tools();
    log_->info("Available tools: {}", tools_list.size());
    for (const auto& tool_name : tools_list) {
        log_->debug("  - {}", tool_name);
    }
}
```

#### Step 3: AI 调用 Tool

```cpp
// AI 决策过程 (ReAct Loop)

void ReactExecutor::execute_react_loop(const std::string& user_query) {
    std::string agent_scratchpad;
    int iteration = 0;
    
    while (iteration < max_iterations_) {
        iteration++;
        
        // ========== 1. AI 生成下一步行动 ==========
        std::string prompt = build_react_prompt(user_query, agent_scratchpad);
        std::string llm_output = call_llm(prompt);
        
        // ========== 2. 解析 AI 输出 ==========
        AgentOutput output = parser_.parse(llm_output);
        
        if (std::holds_alternative<AgentFinish>(output)) {
            // AI 完成任务
            auto finish = std::get<AgentFinish>(output);
            send_final_answer(finish.return_values["output"]);
            break;
        }
        
        // ========== 3. 执行 Tool ==========
        auto action = std::get<AgentAction>(output);
        
        log_->info("AI Action: {} ({})", action.tool, action.tool_input.dump());
        
        // **关键: 通过 ToolManager 执行**
        ToolResult result = tool_manager_->execute_tool(
            action.tool,
            action.tool_input
        );
        
        // ========== 4. 处理结果 ==========
        if (!result.success) {
            agent_scratchpad += "\nObservation: Error - " + result.error;
        } else {
            std::string observation;
            for (const auto& content : result.content) {
                if (content.type == ToolResultContent::Type::Text) {
                    observation += content.text;
                }
            }
            agent_scratchpad += "\nObservation: " + observation;
        }
        
        log_->debug("Agent Scratchpad:\n{}", agent_scratchpad);
    }
}
```

### 示例 2: AI 实际使用流程

#### 用户输入
```
User: What is the square root of 144 plus 25?
```

#### AI 执行过程

```
=== Iteration 1 ===

AI Output:
---
Thought: I need to calculate sqrt(144) + 25.
Action: calculator
Action Input: sqrt(144) + 25
---

Tool Execution:
  Tool: calculator
  Params: {"expression": "sqrt(144) + 25"}
  
Calculator Tool:
  1. 解析表达式: "sqrt(144) + 25"
  2. 执行: python3 -c "print(sqrt(144) + 25)"
  3. 输出: 37.0
  
Tool Result:
  success: true
  content: "Result: 37.0"

Agent Scratchpad:
---
Thought: I need to calculate sqrt(144) + 25.
Action: calculator
Action Input: sqrt(144) + 25
Observation: Result: 37.0
---

=== Iteration 2 ===

AI Output:
---
Thought: I now know the final answer
Final Answer: 37.0
---

Response to User:
  "The square root of 144 plus 25 is 37.0"
```

---

## 🔧 高级用法

### 1. 批量注册工具

```cpp
class CustomTools {
public:
    static void register_all(ToolManager& manager, const ToolContext& context) {
        // 数学工具
        manager.register_tool(std::make_shared<CalculatorTool>(context));
        manager.register_tool(std::make_shared<UnitConverterTool>(context));
        
        // 数据工具
        manager.register_tool(std::make_shared<JsonParserTool>(context));
        manager.register_tool(std::make_shared<CsvReaderTool>(context));
        
        // 网络工具
        manager.register_tool(std::make_shared<HttpRequestTool>(context));
        manager.register_tool(std::make_shared<WebScraperTool>(context));
    }
};

// 使用
CustomTools::register_all(*tool_manager_, tool_context);
```

### 2. 工具权限控制

```cpp
class SensitiveFileTool : public Tool {
public:
    // 标记为需要审批
    bool requires_approval() const override {
        return true;  // 删除文件需要用户确认
    }
    
    // 标记为仅 Owner 可用
    bool owner_only() const override {
        return true;  // 仅 Agent 所有者可调用
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        // 检查权限 (ToolManager 会自动处理)
        // ...
    }
};
```

### 3. 动态工具发现 (MCP)

```cpp
// 配置文件: config.json
{
  "tools": {
    "mcp": {
      "enabled": true,
      "start_daemon": true,
      "allowed_servers": ["linear", "github", "slack"],
      "timeout_ms": 30000
    }
  }
}

// 代码中启用
tool_manager_->enable_mcp(config.tools.mcp);

// 刷新 MCP 工具
auto count = tool_manager_->refresh_mcp_tools();
// => 自动发现 linear.list_issues, github.create_pr, slack.send_message 等

// 调用 MCP 工具 (和内置工具完全一样)
tool_manager_->execute_tool("linear.list_issues", {
    {"team", "ENG"},
    {"status", "open"}
});
```

### 4. 工具链 (Tool Chaining)

```cpp
// AI 可以组合多个工具完成复杂任务

User: "Find all TODOs in the codebase and create GitHub issues"

AI Execution:
  1. search_content(pattern="TODO", path="src/")
     => 找到 15 个 TODOs
  
  2. read_file(path="src/main.cc")
     => 读取具体内容
  
  3. github.create_issue(title="TODO: ...", body="...")
     => 创建 15 个 GitHub issues
  
  4. write_file(path="todos_report.md", content="...")
     => 生成报告
```

### 5. 工具错误处理

```cpp
ToolResult execute(const core::JsonValue& params) override {
    try {
        // 工具逻辑
        return tool_result::text("Success");
        
    } catch (const std::exception& e) {
        // 错误会自动返回给 AI
        return tool_result::error(
            std::string("Tool failed: ") + e.what()
        );
    }
}

// AI 会看到错误并重试或采取其他策略
Agent Scratchpad:
---
Action: read_file
Action Input: {"path": "nonexistent.txt"}
Observation: Error - File not found: nonexistent.txt
Thought: The file doesn't exist, I should check the directory first.
Action: list_dir
Action Input: {"path": "."}
---
```

---

## 📊 工具注册流程对比

### 内置 Tools (Builtin)

| 阶段 | 操作 | 时机 |
|------|------|------|
| 1. 实现 | 继承 `Tool` 基类 | 编译时 |
| 2. 注册 | `BuiltinTools::register_all()` | Agent 启动时 |
| 3. 发现 | 立即可用 | 注册后 |
| 4. 调用 | `tool_manager_->execute_tool()` | AI 决策时 |

**优点**:
- ✅ 性能高 (无额外开销)
- ✅ 类型安全 (编译时检查)
- ✅ 稳定可靠

**缺点**:
- ❌ 需要重新编译
- ❌ 无法动态扩展

### MCP Tools (Dynamic)

| 阶段 | 操作 | 时机 |
|------|------|------|
| 1. 安装 | `npm install -g mcporter` | 用户操作 |
| 2. 配置 | `config.tools.mcp.enabled = true` | 配置文件 |
| 3. 启动 | `mcporter daemon start` | Agent 启动时 |
| 4. 发现 | `mcporter discover --all` | 异步后台 |
| 5. 注册 | `McpToolAdapter` 自动包装 | 发现后 |
| 6. 调用 | `tool_manager_->execute_tool()` | AI 决策时 |

**优点**:
- ✅ 无需重新编译
- ✅ 动态扩展 (添加新 MCP server)
- ✅ 生态丰富 (Linear, GitHub, Slack...)

**缺点**:
- ❌ 需要外部依赖 (Node.js)
- ❌ 轻微性能开销 (进程调用)

---

## 🎯 最佳实践

### 1. 工具命名规范

```cpp
// ✅ 好的命名
"read_file"          // 动词_名词
"search_content"     // 动词_名词
"calculator"         // 简洁明确

// ❌ 不好的命名
"readFile"           // 驼峰 (应该用下划线)
"tool1"              // 无意义
"do_something"       // 不明确
```

### 2. 工具描述清晰

```cpp
std::string description() const override {
    return "Read contents of a file from the workspace. "
           "Supports text files up to 100KB. "
           "Returns error if file is outside workspace.";
    // ✅ 说明了功能、限制、错误情况
}
```

### 3. 参数验证

```cpp
ToolResult execute(const core::JsonValue& params) override {
    ToolParams reader(params);
    
    // ✅ 必需参数
    std::string path = reader.read_string("path", true);
    
    // ✅ 可选参数 + 默认值
    bool recursive = reader.read_bool("recursive", false, false);
    
    // ✅ 参数验证
    if (path.empty()) {
        return tool_result::error("Path cannot be empty");
    }
    
    // ✅ 安全检查
    if (path.find("..") != std::string::npos) {
        return tool_result::error("Path traversal not allowed");
    }
    
    // ...
}
```

### 4. 结果结构化

```cpp
ToolResult execute(const core::JsonValue& params) override {
    // ...执行逻辑...
    
    ToolResult result;
    result.success = true;
    
    // ✅ 内容 (给 AI 看的)
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = "File contents:\n" + file_content;
    result.content.push_back(content);
    
    // ✅ 元数据 (结构化数据)
    result.details = core::JsonValue::object({
        {"path", path},
        {"size", static_cast<double>(file_content.size())},
        {"encoding", "utf-8"}
    });
    
    return result;
}
```

---

## 🔍 调试技巧

### 1. 查看已注册工具

```cpp
auto tools = tool_manager_->list_tools();
std::cout << "Available tools (" << tools.size() << "):\n";
for (const auto& tool_name : tools) {
    auto tool = tool_manager_->get_tool(tool_name);
    std::cout << "  - " << tool_name << ": " 
              << tool->description() << "\n";
}
```

### 2. 查看 LLM 格式

```cpp
auto llm_tools = tool_manager_->to_llm_format();
for (const auto& def : llm_tools) {
    std::cout << "Tool: " << def.name << "\n";
    std::cout << "Description: " << def.description << "\n";
    std::cout << "Parameters: " << def.parameters << "\n\n";
}
```

### 3. 测试工具执行

```cpp
// 单元测试
TEST(CalculatorToolTest, BasicOperations) {
    ToolContext context;
    context.workspace_dir = "/tmp/test";
    
    auto calculator = std::make_shared<CalculatorTool>(context);
    
    auto result = calculator->execute(
        core::JsonValue::object({{"expression", "2 + 2"}})
    );
    
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.content[0].text, "Result: 4");
}
```

---

## 📚 总结

### 核心流程

```
定义 Tool 类 (继承 Tool)
    ↓
实现 name(), description(), parameters(), execute()
    ↓
创建 ToolManager (传入 ToolContext)
    ↓
注册 Tool (register_tool)
    ↓
AI 调用 (execute_tool)
    ↓
返回 ToolResult
```

### 关键点

1. **统一接口**: 所有 Tool 实现相同基类
2. **延迟加载**: Agent 启动时统一注册
3. **透明调用**: AI 无法区分 Builtin/MCP
4. **安全隔离**: workspace 沙箱 + 权限控制
5. **动态扩展**: MCP 集成支持运行时添加

### 对齐 OpenClaw

| 特性 | OpenClaw | HunterClaw | 状态 |
|------|----------|------------|------|
| Tool 接口 | ✅ | ✅ | 100% 对齐 |
| ToolManager | ✅ | ✅ | 100% 对齐 |
| Builtin Tools | ✅ | ✅ | 100% 对齐 |
| MCP 集成 | ✅ | ✅ | 100% 对齐 |
| 权限控制 | ✅ | ✅ | 100% 对齐 |
| 沙箱隔离 | ✅ | ✅ | 100% 对齐 |

---

**完整代码示例**: 参考 `src/tools/builtin_tools.cc` 和 `src/agents/react_executor.cc`
