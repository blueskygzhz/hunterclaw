#pragma once

#include "hunterclaw/tools/tool.h"
#include "hunterclaw/nodes/node_registry.h"
#include "hunterclaw/infra/http_client.h"
#include <memory>

namespace hunterclaw {
namespace tools {

/**
 * Camera Tool - 摄像头拍照和录视频
 * 对齐 OpenClaw nodes-tool 的 camera 能力
 * 
 * Actions:
 * - camera_snap: 拍照 (返回 JPG base64)
 * - camera_clip: 录视频 (返回 MP4 路径/base64)
 * - camera_list: 列出相机设备
 * - photos_latest: 获取最新照片
 */
class CameraTool : public Tool {
public:
    explicit CameraTool(
        const ToolContext& context,
        std::shared_ptr<nodes::NodeRegistry> node_registry
    );
    
    ~CameraTool() override = default;
    
    std::string name() const override { return "camera"; }
    
    std::string description() const override;
    
    Schema parameters() const override;
    
    ToolResult execute(const core::JsonValue& params) override;
    
    bool requires_approval() const override { return true; }  // Camera 需要审批
    
private:
    // === 各个 Action 的实现 ===
    
    /// camera_snap - 拍照
    ToolResult action_camera_snap(const ToolParams& params);
    
    /// camera_clip - 录视频
    ToolResult action_camera_clip(const ToolParams& params);
    
    /// camera_list - 列出相机设备
    ToolResult action_camera_list(const ToolParams& params);
    
    /// photos_latest - 获取最新照片
    ToolResult action_photos_latest(const ToolParams& params);
    
    // === 辅助函数 ===
    
    /// 选择目标节点 (iOS/Android)
    std::string select_camera_node();
    
    /// 调用节点命令
    core::Result<core::JsonValue> invoke_node_command(
        const std::string& node_id,
        const std::string& command,
        const core::JsonValue& command_params
    );
    
    ToolContext context_;
    std::shared_ptr<nodes::NodeRegistry> node_registry_;
    
    /// 创建错误结果
    ToolResult make_error(const std::string& msg) const;
    
    /// 创建 JSON 结果
    ToolResult make_json_result(const core::JsonValue& data) const;
};

} // namespace tools
} // namespace hunterclaw
