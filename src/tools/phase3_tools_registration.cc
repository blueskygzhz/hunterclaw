#include "hunterclaw/tools/tool.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/tools/screen_record_tool.h"
#include "hunterclaw/tools/location_tool.h"
#include "hunterclaw/tools/video_understanding_tool.h"
#include "hunterclaw/tools/edge_tts_provider.h"
#include "hunterclaw/tools/gemini_vision_provider.h"

namespace hunterclaw {
namespace tools {

/**
 * Register Phase 3 Extended Media Tools
 * 
 * This registers the extended multimedia capabilities:
 * - Screen Recording
 * - Location Services
 * - Edge TTS (FREE)
 * - Video Understanding
 * - Google Gemini Vision
 */
void register_phase3_tools(
    ToolManager& tool_manager,
    std::shared_ptr<nodes::NodeRegistry> node_registry,
    std::shared_ptr<infra::HttpClient> http_client,
    std::shared_ptr<llm::LLMClient> llm_client,
    std::shared_ptr<ApprovalManager> approval_manager
) {
    // Register Screen Record Tool
    auto screen_record_tool = std::make_shared<ScreenRecordTool>(
        node_registry,
        approval_manager
    );
    tool_manager.register_tool(screen_record_tool);
    
    // Register Location Tool
    auto location_tool = std::make_shared<LocationTool>(
        node_registry,
        approval_manager
    );
    tool_manager.register_tool(location_tool);
    
    // Register Video Understanding Tool
    auto video_tool = std::make_shared<VideoUnderstandingTool>(
        http_client,
        llm_client
    );
    tool_manager.register_tool(video_tool);
    
    // Note: Edge TTS and Gemini Vision are providers, not standalone tools
    // They are integrated into existing TTS Tool and Image Analysis Tool
    // See: tts_tool.cc and image_analysis_tool.cc for integration
}

// Helper functions for creating providers (not part of BuiltinTools class)
// These are used by other tools that need TTS or vision capabilities

/**
 * Create Edge TTS Provider
 * 
 * Returns a configured Edge TTS provider instance.
 * Edge TTS is FREE and requires no API key!
 */
std::shared_ptr<EdgeTTSProvider> create_edge_tts_provider(
    std::shared_ptr<infra::HttpClient> http_client
) {
    return std::make_shared<EdgeTTSProvider>(http_client);
}

/**
 * Create Gemini Vision Provider
 * 
 * Returns a configured Gemini Vision provider instance.
 * Requires GEMINI_API_KEY environment variable.
 */
std::shared_ptr<GeminiVisionProvider> create_gemini_vision_provider(
    std::shared_ptr<infra::HttpClient> http_client
) {
    // Get API key from environment
    const char* api_key_env = std::getenv("GEMINI_API_KEY");
    std::string api_key = api_key_env ? api_key_env : "";
    
    if (api_key.empty()) {
        // Log warning
        fprintf(stderr, "Warning: GEMINI_API_KEY not set. Gemini Vision will not work.\n");
    }
    
    return std::make_shared<GeminiVisionProvider>(http_client, api_key);
}

} // namespace tools
} // namespace hunterclaw
