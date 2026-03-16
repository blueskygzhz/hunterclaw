# HunterClaw MCP 快速使用指南

## 1. 基础使用

### 1.1 启用 MCP 工具

```cpp
#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/tools/tool.h"

// 配置 MCP
hunterclaw::mcp::McporterConfig config;
config.enabled = true;
config.start_daemon = true;               // 启动 daemon (推荐)
config.server_name = "linear";           // 默认 server
config.timeout_ms = 30000;               // 30秒超时
config.allowed_servers = {"linear", "github"};  // 白名单

// 创建 ToolManager 并启用 MCP
auto ctx = hunterclaw::tools::ToolContext{};
auto tool_manager = hunterclaw::tools::ToolManager::create(ctx);
tool_manager->enable_mcp(config);

// 刷新 MCP 工具列表
auto result = tool_manager->refresh_mcp_tools();
if (result.is_ok()) {
    std::cout << "注册了 " << result.value << " 个 MCP 工具\n";
}
```

---

## 2. 调用 MCP 工具

### 2.1 通过 ToolManager 调用

```cpp
// 列出所有工具
auto tools = tool_manager->list_tools();
for (const auto& name : tools) {
    std::cout << "- " << name << "\n";
}

// 构造参数 (使用 JsonValue)
hunterclaw::core::JsonValue params = hunterclaw::core::JsonValue::object({
    {"team", "ENG"},
    {"limit", 10},
    {"state", "open"}
});

// 执行工具
auto exec_result = tool_manager->execute_tool("linear.list_issues", params);

if (exec_result.success) {
    // 处理结果
    for (const auto& content : exec_result.content) {
        if (content.type == hunterclaw::tools::ToolResultContent::Type::Json) {
            std::cout << "JSON结果:\n" << content.text << "\n";
        }
    }
} else {
    std::cerr << "执行失败: " << exec_result.error << "\n";
}
```

### 2.2 直接使用 McporterClient

```cpp
#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/json_converter.h"

using hunterclaw::mcp::converters::JsonConverter;

// 创建客户端
auto client = hunterclaw::mcp::McporterClient::create(config);

// 确保 daemon 启动
auto ensure_result = client->ensure_daemon_started();
if (ensure_result.is_err()) {
    std::cerr << "启动失败: " << ensure_result.error << "\n";
    return;
}

// 调用工具
hunterclaw::core::JsonValue args = hunterclaw::core::JsonValue::object({
    {"url", "https://github.com/openai/gpt-4"},
    {"depth", 2}
});

auto call_result = client->call_tool("fetch.scrape", args);

if (call_result.is_ok() && call_result.value.success) {
    // 转换结果为 nlohmann::json (便于处理)
    auto json_result = JsonConverter::to_nlohmann(call_result.value.structured_content);
    std::cout << JsonConverter::stringify(json_result, 2) << "\n";
}
```

---

## 3. 工具发现

### 3.1 列出所有 MCP servers

```cpp
auto servers_result = client->list_servers();

if (servers_result.is_ok()) {
    for (const auto& server : servers_result.value) {
        std::cout << "Server: " << server.name << "\n";
        std::cout << "  Command: " << server.command << "\n";
        std::cout << "  Lifecycle: " << server.lifecycle << "\n";
    }
}
```

### 3.2 列出指定 server 的工具

```cpp
auto tools_result = client->list_tools("linear", true);  // with_schema = true

if (tools_result.is_ok()) {
    for (const auto& tool : tools_result.value) {
        std::cout << "工具: " << tool.name << "\n";
        std::cout << "  描述: " << tool.description << "\n";
        
        // 解析 schema
        auto schema_json = JsonConverter::to_nlohmann(tool.input_schema);
        if (schema_json.contains("properties")) {
            std::cout << "  参数:\n";
            for (auto& [key, prop] : schema_json["properties"].items()) {
                std::cout << "    - " << key;
                if (prop.contains("type")) {
                    std::cout << " (" << prop["type"] << ")";
                }
                std::cout << "\n";
            }
        }
    }
}
```

### 3.3 发现所有工具 (跨所有 servers)

```cpp
auto all_tools_result = client->discover_all_tools();

if (all_tools_result.is_ok()) {
    std::cout << "发现 " << all_tools_result.value.size() << " 个工具\n";
    
    // 按 server 分组
    std::map<std::string, int> tools_by_server;
    for (const auto& tool : all_tools_result.value) {
        tools_by_server[tool.server_name]++;
    }
    
    for (const auto& [server, count] : tools_by_server) {
        std::cout << "  " << server << ": " << count << " 个工具\n";
    }
}
```

---

## 4. 配置管理

### 4.1 添加新的 MCP server

```cpp
hunterclaw::core::JsonValue options = hunterclaw::core::JsonValue::object({
    {"lifecycle", "keep-alive"}  // 或 "per-request"
});

auto add_result = client->add_server(
    "my-server",
    "npx -y my-mcp-server@latest",
    options
);

if (add_result.is_ok()) {
    std::cout << "服务器添加成功\n";
}
```

### 4.2 删除 MCP server

```cpp
auto remove_result = client->remove_server("my-server");
```

### 4.3 导入配置文件

```cpp
auto import_result = client->import_config("/path/to/mcporter-config.json");
```

### 4.4 获取当前配置

```cpp
auto config_result = client->get_config();

if (config_result.is_ok()) {
    auto config_json = JsonConverter::to_nlohmann(config_result.value);
    std::cout << JsonConverter::stringify(config_json, 2) << "\n";
}
```

---

## 5. Daemon 管理

### 5.1 手动控制 daemon

```cpp
// 启动 daemon
client->start_daemon();

// 检查状态
auto running_result = client->is_daemon_running();
if (running_result.is_ok()) {
    std::cout << "Daemon 运行中: " << (running_result.value ? "是" : "否") << "\n";
}

// 重启 daemon
client->restart_daemon();

// 停止 daemon
client->stop_daemon();
```

### 5.2 自动管理 (推荐)

```cpp
// 设置 config.start_daemon = true
// ensure_daemon_started() 会自动管理生命周期
auto ensure_result = client->ensure_daemon_started();
```

---

## 6. 健康检查

### 6.1 检查 mcporter 是否已安装

```cpp
if (hunterclaw::mcp::McporterClient::is_mcporter_installed()) {
    std::cout << "mcporter 已安装\n";
} else {
    std::cout << "mcporter 未安装,请运行: npm install -g mcporter\n";
}
```

### 6.2 获取版本

```cpp
auto version_result = client->get_version();
if (version_result.is_ok()) {
    std::cout << "mcporter 版本: " << version_result.value << "\n";
}
```

### 6.3 健康检查

```cpp
auto health_result = client->health_check();
if (health_result.is_ok() && health_result.value) {
    std::cout << "mcporter 状态正常\n";
}
```

---

## 7. 高级用法

### 7.1 批量调用工具 (并发)

```cpp
std::vector<hunterclaw::mcp::McporterCallArgs> calls = {
    {"linear.list_issues", args1, "json", 30000},
    {"github.list_repos", args2, "json", 30000},
    {"slack.send_message", args3, "json", 10000}
};

// 当前串行实现,后续会改为并发
auto results = client->call_tools(calls);

if (results.is_ok()) {
    for (size_t i = 0; i < results.value.size(); ++i) {
        const auto& result = results.value[i];
        if (result.success) {
            std::cout << "调用 " << calls[i].server_and_tool << " 成功\n";
        } else {
            std::cout << "调用 " << calls[i].server_and_tool << " 失败: " 
                      << result.error << "\n";
        }
    }
}
```

### 7.2 自定义工具适配器

```cpp
#include "hunterclaw/mcp/mcp_tool_adapter.h"

// 创建工具定义
hunterclaw::mcp::McpToolDefinition definition;
definition.name = "my-server.my-tool";
definition.server_name = "my-server";
definition.tool_name = "my-tool";
definition.description = "自定义工具";

// 构造 input schema
hunterclaw::core::JsonValue schema = hunterclaw::core::JsonValue::object({
    {"type", "object"},
    {"properties", hunterclaw::core::JsonValue::object({
        {"param1", hunterclaw::core::JsonValue::object({
            {"type", "string"},
            {"description", "参数1"}
        })}
    })},
    {"required", hunterclaw::core::JsonValue::array()}
});
schema["required"].push_back("param1");
definition.input_schema = schema;

// 创建适配器
auto adapter = std::make_shared<hunterclaw::mcp::McpToolAdapter>(client, definition);

// 手动注册到 ToolManager
tool_manager->register_tool(adapter);
```

### 7.3 使用 JsonConverter 处理复杂 JSON

```cpp
#include "hunterclaw/mcp/json_converter.h"

using hunterclaw::mcp::converters::JsonConverter;

// 解析 JSON 字符串
std::string json_str = R"({
    "users": [
        {"name": "Alice", "age": 30},
        {"name": "Bob", "age": 25}
    ]
})";

auto j = JsonConverter::parse_safe(json_str);
auto jv = JsonConverter::from_nlohmann(j);

// 访问嵌套数据
if (jv.is_object() && jv.contains("users")) {
    std::cout << "用户列表:\n";
    // 注意: JsonValue 当前不支持数组索引,需要通过 nlohmann::json
    auto users_json = JsonConverter::to_nlohmann(jv["users"]);
    
    for (const auto& user : users_json) {
        std::cout << "  - " << user["name"] << " (" << user["age"] << "岁)\n";
    }
}

// 美化输出
std::cout << JsonConverter::stringify(j, 2) << "\n";
```

---

## 8. 错误处理

### 8.1 检查 Result 类型

```cpp
auto result = client->call_tool("linear.list_issues", args);

if (result.is_err()) {
    // 操作失败 (连接错误、daemon 未启动等)
    std::cerr << "操作错误: " << result.error << "\n";
} else {
    // 操作成功,但工具可能失败
    const auto& mcp_result = result.value;
    
    if (mcp_result.success) {
        // 工具执行成功
        std::cout << "成功: " << mcp_result.structured_content << "\n";
    } else {
        // 工具执行失败
        std::cerr << "工具错误: " << mcp_result.error << "\n";
        std::cerr << "标准错误: " << mcp_result.stderr_output << "\n";
    }
}
```

### 8.2 超时处理

```cpp
// 设置短超时
config.timeout_ms = 5000;  // 5秒

auto result = client->call_tool("slow-tool", args);
if (result.is_err() || !result.value.success) {
    // 可能是超时
    std::cerr << "可能超时: " << result.error << "\n";
}
```

---

## 9. 最佳实践

### 9.1 推荐配置

```cpp
// 生产环境配置
hunterclaw::mcp::McporterConfig prod_config;
prod_config.enabled = true;
prod_config.start_daemon = true;          // ✅ 启用 daemon (减少冷启动)
prod_config.timeout_ms = 30000;          // ✅ 适中的超时
prod_config.allowed_servers = {           // ✅ 使用白名单
    "linear", "github", "slack"
};

// 开发环境配置
hunterclaw::mcp::McporterConfig dev_config;
dev_config.enabled = true;
dev_config.start_daemon = false;         // ⚠️ 冷启动模式 (调试方便)
dev_config.timeout_ms = 60000;           // ⚠️ 更长的超时 (便于调试)
// 不设置白名单 (允许所有 servers)
```

### 9.2 错误恢复策略

```cpp
auto call_with_retry = [&](const std::string& tool, const auto& args, int max_retries = 3) {
    for (int i = 0; i < max_retries; ++i) {
        auto result = client->call_tool(tool, args);
        
        if (result.is_ok() && result.value.success) {
            return result;
        }
        
        if (i < max_retries - 1) {
            std::cerr << "重试 " << (i + 1) << "/" << max_retries << "...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    return hunterclaw::core::Result<hunterclaw::mcp::McporterResult>::Err(
        "重试 " + std::to_string(max_retries) + " 次后仍失败"
    );
};
```

### 9.3 日志配置

```cpp
#include <spdlog/spdlog.h>

// 设置 MCP 日志级别
auto mcp_logger = spdlog::get("mcp");
if (!mcp_logger) {
    mcp_logger = spdlog::stdout_logger_mt("mcp");
}

// 生产环境: 只记录警告和错误
mcp_logger->set_level(spdlog::level::warn);

// 开发环境: 记录所有调试信息
mcp_logger->set_level(spdlog::level::debug);
```

---

## 10. 常见问题

### Q1: mcporter 未安装

**错误:**
```
Failed to start daemon: Command not found: mcporter
```

**解决:**
```bash
npm install -g mcporter
```

### Q2: Daemon 启动失败

**错误:**
```
Failed to start daemon: Address already in use
```

**解决:**
```bash
# 停止现有 daemon
mcporter daemon stop

# 或直接使用 HunterClaw
client->restart_daemon();
```

### Q3: 工具调用超时

**错误:**
```
mcporter call failed: Timeout after 30000ms
```

**解决:**
```cpp
// 增加超时时间
config.timeout_ms = 60000;  // 60秒

// 或针对单个调用
hunterclaw::mcp::McporterCallArgs call_args;
call_args.timeout_ms = 120000;  // 2分钟
```

### Q4: Schema 解析失败

**错误:**
```
Failed to parse tools: JSON parse error
```

**解决:**
```cpp
// 使用 parse_safe() 替代直接解析
auto j = JsonConverter::parse_safe(json_str);
if (j.is_null()) {
    std::cerr << "JSON 无效\n";
}
```

---

## 11. 示例项目

### 完整示例: 列出 Linear issues

```cpp
#include "hunterclaw/mcp/mcporter_client.h"
#include "hunterclaw/mcp/json_converter.h"
#include <iostream>

int main() {
    using namespace hunterclaw::mcp;
    using hunterclaw::mcp::converters::JsonConverter;
    
    // 1. 配置
    McporterConfig config;
    config.enabled = true;
    config.start_daemon = true;
    config.allowed_servers = {"linear"};
    
    // 2. 创建客户端
    auto client = McporterClient::create(config);
    
    // 3. 确保 daemon 启动
    auto ensure_result = client->ensure_daemon_started();
    if (ensure_result.is_err()) {
        std::cerr << "启动失败: " << ensure_result.error << "\n";
        return 1;
    }
    
    // 4. 构造参数
    hunterclaw::core::JsonValue args = hunterclaw::core::JsonValue::object({
        {"team", "ENG"},
        {"limit", 5},
        {"state", "open"}
    });
    
    // 5. 调用工具
    auto result = client->call_tool("linear.list_issues", args);
    
    if (result.is_err() || !result.value.success) {
        std::cerr << "调用失败: " << result.error << "\n";
        return 1;
    }
    
    // 6. 解析结果
    auto json_result = JsonConverter::to_nlohmann(result.value.structured_content);
    
    std::cout << "Linear Issues:\n";
    if (json_result.contains("issues") && json_result["issues"].is_array()) {
        for (const auto& issue : json_result["issues"]) {
            std::cout << "  - [" << issue["id"] << "] " 
                      << issue["title"] << "\n";
        }
    }
    
    return 0;
}
```

**编译运行:**
```bash
bazel build //examples:linear_issues
./bazel-bin/examples/linear_issues
```

---

## 12. 参考资源

- **mcporter 官方文档:** http://mcporter.dev
- **Model Context Protocol:** https://modelcontextprotocol.io
- **HunterClaw MCP 对齐报告:** `OPENCLAW_ALIGNMENT_REPORT.md`
- **HunterClaw MCP 构建报告:** `MCP_BUILD_SUCCESS.md`

---

**快速开始清单:**

- [ ] 安装 mcporter: `npm install -g mcporter`
- [ ] 配置 MCP servers: `mcporter config add <server> --command "<command>"`
- [ ] 编译 HunterClaw MCP: `bazel build //src:hunterclaw_mcp`
- [ ] 创建客户端并启用 daemon
- [ ] 调用第一个 MCP 工具

**状态:** ✅ **就绪!**
