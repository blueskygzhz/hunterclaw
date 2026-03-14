#include "hunterclaw/tools/camera_tool.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/base64.h"
#include <algorithm>

namespace hunterclaw {
namespace tools {

using namespace hunterclaw::core;

CameraTool::CameraTool(
    const ToolContext& context,
    std::shared_ptr<nodes::NodeRegistry> node_registry
) : context_(context), node_registry_(node_registry) {}

std::string CameraTool::description() const {
    return R"(Camera control tool for taking photos and recording videos.
    
Actions:
- camera_snap: Take a photo
- camera_clip: Record a video clip
- camera_list: List available cameras
- photos_latest: Get latest photos from gallery

Platforms: iOS, Android, macOS)";
}

Tool::Schema CameraTool::parameters() const {
    Schema schema;
    schema.type = "object";
    schema.description = "Camera tool parameters";
    
    // action (required)
    JsonValue action_prop = JsonValue::object();
    action_prop["type"] = "string";
    action_prop["enum"] = JsonValue::array({
        "camera_snap", "camera_clip", "camera_list", "photos_latest"
    });
    action_prop["description"] = "Camera action to perform";
    schema.properties["action"] = action_prop;
    schema.required.push_back("action");
    
    // node (optional)
    JsonValue node_prop = JsonValue::object();
    node_prop["type"] = "string";
    node_prop["description"] = "Target node ID (auto-select if not provided)";
    schema.properties["node"] = node_prop;
    
    // facing (optional, for camera_snap/camera_clip)
    JsonValue facing_prop = JsonValue::object();
    facing_prop["type"] = "string";
    facing_prop["enum"] = JsonValue::array({"front", "back", "both"});
    facing_prop["description"] = "Camera facing: front/back/both (snap only)";
    schema.properties["facing"] = facing_prop;
    
    // maxWidth (optional)
    JsonValue max_width_prop = JsonValue::object();
    max_width_prop["type"] = "number";
    max_width_prop["default"] = 1600;
    max_width_prop["description"] = "Maximum image width in pixels";
    schema.properties["maxWidth"] = max_width_prop;
    
    // quality (optional)
    JsonValue quality_prop = JsonValue::object();
    quality_prop["type"] = "number";
    quality_prop["minimum"] = 0.0;
    quality_prop["maximum"] = 1.0;
    quality_prop["default"] = 0.95;
    quality_prop["description"] = "Image quality (0.0-1.0)";
    schema.properties["quality"] = quality_prop;
    
    // durationMs (optional, for camera_clip)
    JsonValue duration_prop = JsonValue::object();
    duration_prop["type"] = "number";
    duration_prop["default"] = 3000;
    duration_prop["maximum"] = 60000;
    duration_prop["description"] = "Video duration in milliseconds (max 60s)";
    schema.properties["durationMs"] = duration_prop;
    
    // includeAudio (optional, for camera_clip)
    JsonValue audio_prop = JsonValue::object();
    audio_prop["type"] = "boolean";
    audio_prop["default"] = true;
    audio_prop["description"] = "Include audio in video recording";
    schema.properties["includeAudio"] = audio_prop;
    
    // limit (optional, for photos_latest)
    JsonValue limit_prop = JsonValue::object();
    limit_prop["type"] = "number";
    limit_prop["default"] = 1;
    limit_prop["minimum"] = 1;
    limit_prop["maximum"] = 20;
    limit_prop["description"] = "Number of photos to retrieve (1-20)";
    schema.properties["limit"] = limit_prop;
    
    return schema;
}

ToolResult CameraTool::execute(const JsonValue& params) {
    ToolParams tp(params);
    
    std::string action = tp.read_string("action", true);
    if (action.empty()) {
        return tool_result::error("Missing required parameter: action");
    }
    
    // 路由到对应的 action
    if (action == "camera_snap") {
        return action_camera_snap(tp);
    } else if (action == "camera_clip") {
        return action_camera_clip(tp);
    } else if (action == "camera_list") {
        return action_camera_list(tp);
    } else if (action == "photos_latest") {
        return action_photos_latest(tp);
    }
    
    return tool_result::error("Unknown action: " + action);
}

// ========== Action 实现 ==========

ToolResult CameraTool::action_camera_snap(const ToolParams& params) {
    Logger::info("[CameraTool] camera_snap start");
    
    // 1. 读取参数
    std::string node_id = params.read_string("node");
    if (node_id.empty()) {
        node_id = select_camera_node();
        if (node_id.empty()) {
            return tool_result::error("No camera-capable node found");
        }
    }
    
    std::string facing = params.read_string("facing", false, "back");
    int max_width = static_cast<int>(params.read_number("maxWidth", false, 1600));
    double quality = params.read_number("quality", false, 0.95);
    
    // 2. 构建命令参数
    JsonValue command_params = JsonValue::object();
    command_params["facing"] = facing;
    command_params["maxWidth"] = max_width;
    command_params["quality"] = quality;
    
    // 3. 调用节点
    auto result = invoke_node_command(node_id, "camera.snap", command_params);
    if (!result) {
        return tool_result::error("camera.snap failed: " + result.error());
    }
    
    // 4. 解析结果
    auto& payload = result.value();
    
    // 支持 "both" facing - 返回多张图片
    if (facing == "both" && payload.is_array()) {
        std::vector<ToolResultContent> contents;
        for (const auto& item : payload.as_array()) {
            if (item.has("data") && item["data"].is_string()) {
                std::string base64_data = item["data"].as_string();
                std::string mime = item.get("mime_type", "image/jpeg").as_string();
                
                ToolResultContent content;
                content.type = ToolResultContent::Type::Image;
                content.data = base64_data;
                content.mime_type = mime;
                contents.push_back(content);
            }
        }
        
        Logger::info("[CameraTool] camera_snap success: " + 
                     std::to_string(contents.size()) + " photos");
        
        return ToolResult{.content = contents, .success = true};
    }
    
    // 单张图片
    if (!payload.has("data") || !payload["data"].is_string()) {
        return tool_result::error("Invalid camera.snap response: missing data");
    }
    
    std::string base64_data = payload["data"].as_string();
    std::string mime = payload.get("mime_type", "image/jpeg").as_string();
    
    Logger::info("[CameraTool] camera_snap success: " + 
                 std::to_string(base64_data.size()) + " bytes");
    
    return tool_result::image(base64_data, mime);
}

ToolResult CameraTool::action_camera_clip(const ToolParams& params) {
    Logger::info("[CameraTool] camera_clip start");
    
    // 1. 读取参数
    std::string node_id = params.read_string("node");
    if (node_id.empty()) {
        node_id = select_camera_node();
        if (node_id.empty()) {
            return tool_result::error("No camera-capable node found");
        }
    }
    
    std::string facing = params.read_string("facing", false, "back");
    if (facing == "both") {
        return tool_result::error("camera_clip does not support 'both' facing");
    }
    
    int duration_ms = static_cast<int>(params.read_number("durationMs", false, 3000));
    duration_ms = std::min(duration_ms, 60000);  // 最大 60 秒
    
    bool include_audio = params.read_bool("includeAudio", false, true);
    
    // 2. 构建命令参数
    JsonValue command_params = JsonValue::object();
    command_params["facing"] = facing;
    command_params["durationMs"] = duration_ms;
    command_params["includeAudio"] = include_audio;
    
    // 3. 调用节点
    auto result = invoke_node_command(node_id, "camera.clip", command_params);
    if (!result) {
        return tool_result::error("camera.clip failed: " + result.error());
    }
    
    // 4. 解析结果 - 可能是文件路径或 base64
    auto& payload = result.value();
    
    if (payload.has("path") && payload["path"].is_string()) {
        // 返回文件路径
        std::string path = payload["path"].as_string();
        Logger::info("[CameraTool] camera_clip success: " + path);
        
        return tool_result::text("Video saved to: " + path);
    }
    
    if (payload.has("data") && payload["data"].is_string()) {
        // 返回 base64 (不常见,视频太大)
        std::string base64_data = payload["data"].as_string();
        std::string mime = payload.get("mime_type", "video/mp4").as_string();
        
        Logger::info("[CameraTool] camera_clip success: " + 
                     std::to_string(base64_data.size()) + " bytes");
        
        ToolResultContent content;
        content.type = ToolResultContent::Type::Image;  // 复用 Image 类型
        content.data = base64_data;
        content.mime_type = mime;
        
        return ToolResult{.content = {content}, .success = true};
    }
    
    return tool_result::error("Invalid camera.clip response");
}

ToolResult CameraTool::action_camera_list(const ToolParams& params) {
    Logger::info("[CameraTool] camera_list start");
    
    // 1. 选择节点
    std::string node_id = params.read_string("node");
    if (node_id.empty()) {
        node_id = select_camera_node();
        if (node_id.empty()) {
            return tool_result::error("No camera-capable node found");
        }
    }
    
    // 2. 调用节点
    auto result = invoke_node_command(node_id, "camera.list", JsonValue::object());
    if (!result) {
        return tool_result::error("camera.list failed: " + result.error());
    }
    
    // 3. 返回相机列表
    Logger::info("[CameraTool] camera_list success");
    return tool_result::json(result.value());
}

ToolResult CameraTool::action_photos_latest(const ToolParams& params) {
    Logger::info("[CameraTool] photos_latest start");
    
    // 1. 读取参数
    std::string node_id = params.read_string("node");
    if (node_id.empty()) {
        node_id = select_camera_node();
        if (node_id.empty()) {
            return tool_result::error("No camera-capable node found");
        }
    }
    
    int limit = static_cast<int>(params.read_number("limit", false, 1));
    limit = std::max(1, std::min(limit, 20));
    
    int max_width = static_cast<int>(params.read_number("maxWidth", false, 1600));
    double quality = params.read_number("quality", false, 0.95);
    
    // 2. 构建命令参数
    JsonValue command_params = JsonValue::object();
    command_params["limit"] = limit;
    command_params["maxWidth"] = max_width;
    command_params["quality"] = quality;
    
    // 3. 调用节点
    auto result = invoke_node_command(node_id, "photos.latest", command_params);
    if (!result) {
        return tool_result::error("photos.latest failed: " + result.error());
    }
    
    // 4. 解析结果 - 返回图片数组
    auto& payload = result.value();
    if (!payload.is_array()) {
        return tool_result::error("Invalid photos.latest response: not an array");
    }
    
    std::vector<ToolResultContent> contents;
    for (const auto& item : payload.as_array()) {
        if (item.has("data") && item["data"].is_string()) {
            std::string base64_data = item["data"].as_string();
            std::string mime = item.get("mime_type", "image/jpeg").as_string();
            
            ToolResultContent content;
            content.type = ToolResultContent::Type::Image;
            content.data = base64_data;
            content.mime_type = mime;
            contents.push_back(content);
        }
    }
    
    Logger::info("[CameraTool] photos_latest success: " + 
                 std::to_string(contents.size()) + " photos");
    
    return ToolResult{.content = contents, .success = true};
}

// ========== 辅助函数 ==========

std::string CameraTool::select_camera_node() {
    if (!node_registry_) {
        Logger::error("[CameraTool] node_registry is null");
        return "";
    }
    
    // 获取所有在线节点
    auto nodes = node_registry_->list_nodes();
    
    // 优先选择 iOS/Android 节点
    for (const auto& node : nodes) {
        auto node_info = node_registry_->get_node(node);
        if (!node_info) continue;
        
        std::string platform = node_info.value().platform;
        if (platform == "ios" || platform == "android") {
            // 检查是否有 camera 能力
            auto caps = node_info.value().capabilities;
            if (std::find(caps.begin(), caps.end(), "camera") != caps.end()) {
                Logger::info("[CameraTool] selected node: " + node + " (" + platform + ")");
                return node;
            }
        }
    }
    
    // 备选: macOS 节点 (有些 Mac 也有摄像头)
    for (const auto& node : nodes) {
        auto node_info = node_registry_->get_node(node);
        if (!node_info) continue;
        
        std::string platform = node_info.value().platform;
        if (platform == "macos") {
            auto caps = node_info.value().capabilities;
            if (std::find(caps.begin(), caps.end(), "camera") != caps.end()) {
                Logger::info("[CameraTool] selected node: " + node + " (" + platform + ")");
                return node;
            }
        }
    }
    
    Logger::error("[CameraTool] no camera-capable node found");
    return "";
}

Result<JsonValue> CameraTool::invoke_node_command(
    const std::string& node_id,
    const std::string& command,
    const JsonValue& command_params
) {
    if (!node_registry_) {
        return Result<JsonValue>::error("node_registry is null");
    }
    
    // 调用 node_registry_->invoke
    auto result = node_registry_->invoke(node_id, command, command_params);
    if (!result) {
        return Result<JsonValue>::error("Node invoke failed: " + result.error());
    }
    
    return Result<JsonValue>::ok(result.value());
}

} // namespace tools
} // namespace hunterclaw
