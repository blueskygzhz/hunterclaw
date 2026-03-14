#include "hunterclaw/tools/builtin_tools.h"
#include "hunterclaw/tools/camera_tool.h"
#include "hunterclaw/tools/tts_tool.h"
#include "hunterclaw/tools/audio_transcription_tool.h"
#include "hunterclaw/tools/image_analysis_tool.h"

namespace hunterclaw {
namespace tools {

// 注册多媒体工具
void BuiltinTools::register_media_tools(
    ToolManager& manager,
    const ToolContext& context,
    std::shared_ptr<nodes::NodeRegistry> node_registry,
    std::shared_ptr<http::HttpClient> http_client,
    std::shared_ptr<llm::LlmClient> llm_client
) {
    // 1. Camera Tool
    if (node_registry) {
        auto camera_tool = std::make_shared<CameraTool>(context, node_registry);
        manager.register_tool(camera_tool);
    }
    
    // 2. TTS Tool
    if (http_client) {
        auto tts_tool = std::make_shared<TtsTool>(context, http_client);
        manager.register_tool(tts_tool);
    }
    
    // 3. Audio Transcription Tool
    if (http_client) {
        auto audio_tool = std::make_shared<AudioTranscriptionTool>(context, http_client);
        manager.register_tool(audio_tool);
    }
    
    // 4. Image Analysis Tool
    if (llm_client) {
        auto image_tool = std::make_shared<ImageAnalysisTool>(context, llm_client);
        manager.register_tool(image_tool);
    }
}

} // namespace tools
} // namespace hunterclaw
