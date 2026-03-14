#ifndef HUNTERCLAW_TOOLS_TERMINAL_TOOL_H_
#define HUNTERCLAW_TOOLS_TERMINAL_TOOL_H_

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace hunterclaw {
namespace tools {

// ===== Terminal Tool - 100% 对齐 OpenClaw terminal-tool.ts =====
//
// 功能对齐:
// 1. ✅ 所有 Action (7个): status, start, stop, tabs, execute, input, resize
// 2. ✅ 持久化会话: 长期运行的 terminal 会话,跨多次工具调用
// 3. ✅ 多标签页支持: 同时管理多个 terminal 实例
// 4. ✅ 增量输出: 实时获取新的输出行 (lastLine 过滤)
// 5. ✅ 窗口调整: resize 终端窗口大小
// 6. ✅ 3 种 Target 模式: sandbox, host, node (自动路由)
// 7. ✅ Shell 自动检测: 根据系统自动选择 bash/zsh/sh
// 8. ✅ 工作目录管理: 每个会话独立的 cwd
// 9. ✅ 环境变量传递: 自定义环境变量
// 10. ✅ 外部内容安全: 输出标记为 untrusted
//
// OpenClaw 原始路径: src/agents/tools/terminal-tool.ts (480 行)

class TerminalTool : public Tool {
public:
    explicit TerminalTool(const ToolContext& context);
    
    std::string name() const override;
    std::string description() const override;
    Schema parameters() const override;
    ToolResult execute(const core::JsonValue& params) override;
    bool requires_approval() const override;

private:
    ToolContext context_;
    
    // ===== Terminal 会话管理 =====
    
    struct TerminalSession {
        std::string session_id;
        std::string shell;           // bash, zsh, sh
        std::string cwd;              // 工作目录
        int rows;                     // 终端行数
        int cols;                     // 终端列数
        std::vector<std::string> output_lines;  // 历史输出
        bool is_running;
        int64_t created_at;
        int64_t last_activity;
    };
    
    // 解析 Terminal 目标 (sandbox vs host vs node)
    std::optional<std::string> resolve_terminal_base_url(
        const std::string& target,
        const std::string& sandbox_bridge_url
    ) const;
    
    struct TerminalNodeTarget {
        std::string node_id;
        std::string label;
    };
    
    std::optional<TerminalNodeTarget> resolve_terminal_node_target(
        const std::string& requested_node,
        const std::string& target,
        const std::string& sandbox_bridge_url
    ) const;
    
    // ===== Terminal Actions =====
    
    // status - 获取所有 Terminal 会话状态
    ToolResult action_status(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target
    );
    
    // start - 启动新的 Terminal 会话
    ToolResult action_start(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target,
        const std::string& shell,
        const std::string& cwd,
        const core::JsonValue& env,
        int rows,
        int cols
    );
    
    // stop - 停止指定 Terminal 会话
    ToolResult action_stop(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target,
        const std::string& session_id
    );
    
    // tabs - 列出所有 Terminal 标签页
    ToolResult action_tabs(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target
    );
    
    // execute - 在指定会话执行命令
    ToolResult action_execute(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target,
        const std::string& session_id,
        const std::string& command,
        int timeout_ms,
        int last_line
    );
    
    // input - 向 Terminal 发送原始输入 (不自动添加回车)
    ToolResult action_input(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target,
        const std::string& session_id,
        const std::string& text
    );
    
    // resize - 调整 Terminal 窗口大小
    ToolResult action_resize(
        const std::optional<std::string>& base_url,
        const std::optional<TerminalNodeTarget>& node_target,
        const std::string& session_id,
        int rows,
        int cols
    );
    
    // ===== HTTP 请求辅助 =====
    
    core::JsonValue http_get(const std::string& url) const;
    core::JsonValue http_post(const std::string& url, const core::JsonValue& body) const;
    core::JsonValue http_delete(const std::string& url) const;
    
    // Node 代理请求
    core::JsonValue call_node_terminal_proxy(
        const std::string& node_id,
        const std::string& method,
        const std::string& path,
        const core::JsonValue& query,
        const core::JsonValue& body,
        int timeout_ms
    ) const;
    
    // ===== 安全辅助 =====
    
    // 包装外部内容 (标记为 untrusted)
    std::string wrap_terminal_output(
        const std::string& output,
        const std::string& session_id
    ) const;
    
    // 格式化 Terminal 输出 (增量输出,过滤重复行)
    ToolResult format_terminal_output(
        const std::vector<std::string>& output_lines,
        int last_line,
        const std::string& session_id,
        const std::string& cwd,
        int exit_code
    ) const;
};

// ===== Terminal Tool Factory =====

class TerminalToolFactory {
public:
    static std::shared_ptr<Tool> create(const ToolContext& context);
};

} // namespace tools
} // namespace hunterclaw

#endif // HUNTERCLAW_TOOLS_TERMINAL_TOOL_H_
