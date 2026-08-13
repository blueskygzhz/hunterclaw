#ifndef HUNTERCLAW_TOOLS_BROWSER_TOOL_H_
#define HUNTERCLAW_TOOLS_BROWSER_TOOL_H_

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <memory>
#include <optional>

namespace hunterclaw {
namespace tools {

// ===== Browser Tool - 严格对齐 OpenClaw browser-tool.ts =====
//
// 功能对齐:
// 1. ✅ 所有 Action (20个): doctor, status, start, stop, profiles, importprofile,
//                          tabs, open, focus, close, snapshot, screenshot, navigate,
//                          console, pdf, download, waitfordownload, upload, dialog, act
// 2. ✅ 所有 Snapshot 格式: aria, ai (snapshotFormat) + efficient mode + refs(role|aria)
// 3. ✅ Browser Act 操作 (14个): batch, click, clickCoords, type, press, hover,
//                              scrollIntoView, drag, select, fill, resize, wait, evaluate, close
// 4. ✅ 3 种 Target 模式: sandbox, host, node (自动路由到节点代理)
// 5. ✅ Chrome Extension Relay 支持: profile 接管现有 Chrome 标签页
// 6. ✅ 外部内容安全包装: snapshot/console/tabs 结果标记为 untrusted
// 7. ✅ 错误恢复: Chrome stale targetId 自动重试
// 8. ✅ Node 代理透明路由: gateway.nodes.browser 策略支持
// 9. ✅ 文件上传: file-chooser hook (路径安全检查)
// 10. ✅ Dialog 处理: alert/confirm/prompt hook
// 11. ✅ System profile import: importprofile (browser/systemProfile/into/domains)
// 12. ✅ Download: download / waitfordownload
//
// OpenClaw 原始路径: extensions/browser/src/browser-tool.ts
//   + browser-tool.schema.ts + browser-tool-description.ts

class BrowserTool : public Tool {
public:
    explicit BrowserTool(const ToolContext& context);
    
    std::string name() const override;
    std::string description() const override;
    Schema parameters() const override;
    ToolResult execute(const core::JsonValue& params) override;
    bool requires_approval() const override;

private:
    ToolContext context_;
    
    // ===== Browser 连接管理 =====
    
    // 解析 Browser Base URL (sandbox vs host)
    std::optional<std::string> resolve_browser_base_url(
        const std::string& target,
        const std::string& sandbox_bridge_url
    ) const;
    
    // 解析 Node 目标 (自动路由到远程 Node)
    struct BrowserNodeTarget {
        std::string node_id;
        std::string label;
    };
    
    std::optional<BrowserNodeTarget> resolve_browser_node_target(
        const std::string& requested_node,
        const std::string& target,
        const std::string& sandbox_bridge_url
    ) const;
    
    // ===== Browser 状态管理 =====
    
    ToolResult action_status(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_start(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_stop(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_profiles(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target
    );

    // ✅ 对齐 OpenClaw: doctor action (browser 健康诊断)
    ToolResult action_doctor(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );

    // ✅ 对齐 OpenClaw: importprofile action (system profile import，仅 host)
    ToolResult action_importprofile(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target
    );
    
    // ===== Browser 标签页管理 =====
    
    ToolResult action_tabs(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_open(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile,
        const std::string& target_url
    );
    
    ToolResult action_focus(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile,
        const std::string& target_id
    );
    
    ToolResult action_close(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile,
        const std::string& target_id
    );
    
    ToolResult action_navigate(
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile,
        const std::string& target_url,
        const std::string& target_id
    );
    
    // ===== Browser 观察 =====
    
    ToolResult action_snapshot(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_screenshot(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_console(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_pdf(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );

    // ✅ 对齐 OpenClaw: download / waitfordownload action
    ToolResult action_download(
        const std::string& action,
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    // ===== Browser 交互 =====
    
    ToolResult action_act(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_upload(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    ToolResult action_dialog(
        const core::JsonValue& params,
        const std::optional<std::string>& base_url,
        const std::optional<BrowserNodeTarget>& node_target,
        const std::string& profile
    );
    
    // ===== HTTP 请求辅助 =====
    
    core::JsonValue http_get(const std::string& url) const;
    core::JsonValue http_post(const std::string& url, const core::JsonValue& body) const;
    core::JsonValue http_delete(const std::string& url) const;
    
    // Node 代理请求
    core::JsonValue call_node_browser_proxy(
        const std::string& node_id,
        const std::string& method,
        const std::string& path,
        const core::JsonValue& query,
        const core::JsonValue& body,
        int timeout_ms
    ) const;
    
    // ===== 安全辅助 =====
    
    // 包装外部内容 (标记为 untrusted)
    std::string wrap_external_content(
        const std::string& content,
        const std::string& source,
        const std::string& kind
    ) const;
    
    // 格式化 Tabs 结果 (安全包装)
    ToolResult format_tabs_result(const std::vector<core::JsonValue>& tabs) const;
    
    // 创建错误结果
    ToolResult make_error(const std::string& msg) const;
    
    // 创建文本结果
    ToolResult make_text_result(const std::string& text) const;
    
    // 创建 JSON 结果
    ToolResult make_json_result(const core::JsonValue& data) const;
};

// ===== Browser Tool Factory =====

class BrowserToolFactory {
public:
    static std::shared_ptr<Tool> create(const ToolContext& context);
};

} // namespace tools
} // namespace hunterclaw

#endif // HUNTERCLAW_TOOLS_BROWSER_TOOL_H_
