#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <set>

namespace hunterclaw {
namespace tools {

// 🎯 Tool Profile Types (对齐 OpenClaw tool-catalog.ts)
enum class ToolProfileId {
    MINIMAL,     // 最小工具集
    CODING,      // 编码工具集
    MESSAGING,   // 消息工具集
    FULL         // 完整工具集
};

// Tool Profile Policy
struct ToolProfilePolicy {
    std::vector<std::string> allow;
    std::vector<std::string> deny;
};

// Tool Profile Option
struct ToolProfileOption {
    std::string id;
    std::string label;
    std::string description;
};

// Core Tool Definition (对齐 OpenClaw)
struct CoreToolDefinition {
    std::string id;
    std::string label;
    std::string description;
    std::string section_id;
    std::vector<ToolProfileId> profiles;
    bool include_in_openclaw_group = false;
};

// Tool Catalog Entry
struct ToolCatalogEntry {
    std::string id;
    std::string label;
    std::string description;
    std::string source;  // "core" or "plugin"
    std::string plugin_id;
    bool optional = false;
    std::vector<std::string> default_profiles;
    
    core::JsonValue to_json() const;
};

// Tool Catalog Group
struct ToolCatalogGroup {
    std::string id;
    std::string label;
    std::string source;  // "core" or "plugin"
    std::string plugin_id;
    std::vector<ToolCatalogEntry> tools;
    
    core::JsonValue to_json() const;
};

// Tool Section (分类)
struct CoreToolSection {
    std::string id;
    std::string label;
    std::vector<CoreToolDefinition> tools;
};

// 🎯 Tool Catalog Manager (核心类)
class ToolCatalog {
public:
    static std::shared_ptr<ToolCatalog> create();
    
    // Get all tool sections
    std::vector<CoreToolSection> list_sections() const;
    
    // Get tools for a specific profile
    std::vector<std::string> get_tools_for_profile(ToolProfileId profile_id) const;
    
    // Get profile options
    std::vector<ToolProfileOption> get_profile_options() const;
    
    // Resolve profiles for a tool
    std::vector<std::string> resolve_tool_profiles(const std::string& tool_id) const;
    
    // Build catalog response (for Gateway RPC)
    core::JsonValue build_catalog(
        const std::string& agent_id,
        bool include_plugins = true
    ) const;
    
    // Check if tool is in profile
    bool is_tool_in_profile(const std::string& tool_id, ToolProfileId profile_id) const;
    
    // Get tool section by ID
    core::Result<CoreToolSection> get_section(const std::string& section_id) const;
    
    // Register plugin tool
    void register_plugin_tool(const ToolCatalogEntry& entry);
    
private:
    ToolCatalog();
    void init_core_tools();
    void init_sections();
    
    std::map<std::string, CoreToolDefinition> core_tools_;
    std::map<std::string, CoreToolSection> sections_;
    std::vector<ToolCatalogEntry> plugin_tools_;
};

// 🎯 Core Tool Sections (11 个分类 - 对齐 OpenClaw)
const std::vector<std::pair<std::string, std::string>> CORE_TOOL_SECTION_ORDER = {
    {"fs", "Files"},
    {"runtime", "Runtime"},
    {"web", "Web"},
    {"memory", "Memory"},
    {"sessions", "Sessions"},
    {"ui", "UI"},
    {"messaging", "Messaging"},
    {"automation", "Automation"},
    {"nodes", "Nodes"},
    {"agents", "Agents"},
    {"media", "Media"}
};

// 🎯 Core Tool Definitions (完整定义 - 对齐 OpenClaw)
const std::vector<CoreToolDefinition> CORE_TOOL_DEFINITIONS = {
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Files Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"read", "read", "Read file contents", "fs", {ToolProfileId::CODING}, false},
    {"write", "write", "Create or overwrite files", "fs", {ToolProfileId::CODING}, false},
    {"edit", "edit", "Make precise edits", "fs", {ToolProfileId::CODING}, false},
    {"apply_patch", "apply_patch", "Patch files (OpenAI)", "fs", {ToolProfileId::CODING}, false},
    {"list_dir", "list_dir", "List directory contents", "fs", {ToolProfileId::CODING}, false},
    {"search_files", "search_files", "Search files by pattern", "fs", {ToolProfileId::CODING}, false},
    {"delete", "delete", "Delete files", "fs", {ToolProfileId::CODING}, false},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Runtime Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"exec", "exec", "Run shell commands", "runtime", {ToolProfileId::CODING}, false},
    {"process", "process", "Manage background processes", "runtime", {ToolProfileId::CODING}, false},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Web Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"web_search", "web_search", "Search the web", "web", {ToolProfileId::CODING}, true},
    {"web_fetch", "web_fetch", "Fetch web content", "web", {ToolProfileId::CODING}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Memory Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"memory_search", "memory_search", "Semantic memory search", "memory", {ToolProfileId::CODING}, true},
    {"memory_get", "memory_get", "Read memory files", "memory", {ToolProfileId::CODING}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Sessions Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"sessions_list", "sessions_list", "List all sessions", "sessions", {ToolProfileId::CODING, ToolProfileId::MESSAGING}, true},
    {"sessions_history", "sessions_history", "Get session history", "sessions", {ToolProfileId::CODING, ToolProfileId::MESSAGING}, true},
    {"sessions_send", "sessions_send", "Send to session", "sessions", {ToolProfileId::CODING, ToolProfileId::MESSAGING}, true},
    {"sessions_spawn", "sessions_spawn", "Spawn sub-agent", "sessions", {ToolProfileId::CODING}, true},
    {"subagents", "subagents", "Manage sub-agents", "sessions", {ToolProfileId::CODING}, true},
    {"session_status", "session_status", "Session status", "sessions", {ToolProfileId::MINIMAL, ToolProfileId::CODING, ToolProfileId::MESSAGING}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // UI Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"browser", "browser", "Control web browser", "ui", {}, true},
    {"canvas", "canvas", "Control canvases", "ui", {}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Messaging Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"message", "message", "Send messages", "messaging", {ToolProfileId::MESSAGING}, true},
    {"discord_send", "discord_send", "Send Discord messages", "messaging", {ToolProfileId::MESSAGING}, true},
    {"slack_send", "slack_send", "Send Slack messages", "messaging", {ToolProfileId::MESSAGING}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Automation Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"cron", "cron", "Schedule tasks", "automation", {ToolProfileId::CODING}, true},
    {"gateway", "gateway", "Gateway control", "automation", {}, false},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Nodes Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"nodes_list", "nodes_list", "List all nodes", "nodes", {ToolProfileId::CODING}, true},
    {"nodes_invoke", "nodes_invoke", "Invoke node command", "nodes", {ToolProfileId::CODING}, true},
    {"nodes_status", "nodes_status", "Node status", "nodes", {ToolProfileId::CODING}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Agents Section
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"agents_list", "agents_list", "List all agents", "agents", {}, true},
    
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    // Media Section (HunterClaw 扩展)
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    {"camera_snap", "camera_snap", "Take photo", "media", {}, true},
    {"tts", "tts", "Text-to-speech", "media", {}, true},
    {"audio_transcription", "audio_transcription", "Transcribe audio", "media", {}, true},
    {"image_analysis", "image_analysis", "Analyze images", "media", {}, true},
    {"video_understanding", "video_understanding", "Understand video", "media", {}, true},
    {"screen_record", "screen_record", "Record screen", "media", {}, true},
    {"location", "location", "Get location", "media", {}, true},
    {"pdf", "pdf", "PDF operations", "media", {}, true}
};

// Profile Options (4 个 Profile - 对齐 OpenClaw)
const std::vector<ToolProfileOption> PROFILE_OPTIONS = {
    {"minimal", "Minimal", "Essential tools only"},
    {"coding", "Coding", "Development tools"},
    {"messaging", "Messaging", "Communication tools"},
    {"full", "Full", "All available tools"}
};

} // namespace tools
} // namespace hunterclaw
