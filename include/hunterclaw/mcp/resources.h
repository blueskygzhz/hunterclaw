#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/mcp/server.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// Resource Readers
// ============================================================================

/// Resource content reader
class ResourceReader {
public:
    virtual ~ResourceReader() = default;
    
    /// Read resource by URI
    virtual core::Result<std::string> read(const std::string& uri) = 0;
    
    /// Check if URI is supported
    virtual bool supports(const std::string& uri) = 0;
};

/// File system resource reader (file:// URIs)
class FileResourceReader : public ResourceReader {
public:
    explicit FileResourceReader(const std::vector<std::string>& allowed_roots = {});
    
    core::Result<std::string> read(const std::string& uri) override;
    bool supports(const std::string& uri) override;

private:
    std::vector<std::string> allowed_roots_;
    
    core::Result<std::string> read_file(const std::string& path);
    bool is_path_allowed(const std::string& path);
};

/// Memory resource reader (hunterclaw:// URIs)
class MemoryResourceReader : public ResourceReader {
public:
    using MemoryQuery = std::function<core::Result<std::vector<std::string>>(const std::string&)>;
    
    explicit MemoryResourceReader(MemoryQuery query_func);
    
    core::Result<std::string> read(const std::string& uri) override;
    bool supports(const std::string& uri) override;

private:
    MemoryQuery query_func_;
    
    std::string parse_query(const std::string& uri);
};

// ============================================================================
// Prompt Generators
// ============================================================================

/// Dynamic prompt template
struct PromptTemplate {
    std::string name;
    std::string description;
    std::vector<std::string> argument_names;
    std::function<std::string(const std::map<std::string, std::string>&)> generator;
};

/// Prompt generator
class PromptGenerator {
public:
    /// Register prompt template
    void register_template(const PromptTemplate& tmpl);
    
    /// List available prompts
    std::vector<MCPPrompt> list_prompts();
    
    /// Get prompt with arguments
    core::Result<std::string> get_prompt(
        const std::string& name,
        const std::map<std::string, std::string>& args = {}
    );

private:
    std::map<std::string, PromptTemplate> templates_;
};

// ============================================================================
// Complete MCP Server Implementation
// ============================================================================

/// HunterClaw MCP Server with resource/prompt support
class HunterClawMCPServer : public MCPServer {
public:
    HunterClawMCPServer();
    
    /// Add resource reader
    void add_resource_reader(std::shared_ptr<ResourceReader> reader);
    
    /// Add prompt template
    void add_prompt_template(const PromptTemplate& tmpl);
    
    // MCPServer interface
    std::vector<MCPTool> list_tools() override;
    std::vector<MCPResource> list_resources() override;
    std::vector<MCPPrompt> list_prompts() override;
    
    core::Result<JsonValue> call_tool(
        const std::string& name,
        const JsonValue& args
    ) override;
    
    core::Result<std::string> read_resource(const std::string& uri) override;
    
    core::Result<std::string> get_prompt(
        const std::string& name,
        const std::map<std::string, std::string>& args
    ) override;

private:
    std::vector<std::shared_ptr<ResourceReader>> resource_readers_;
    PromptGenerator prompt_generator_;
    
    // Tool implementations
    core::Result<JsonValue> tool_run_agent(const JsonValue& args);
    core::Result<JsonValue> tool_list_skills(const JsonValue& args);
    core::Result<JsonValue> tool_execute_skill(const JsonValue& args);
};

} // namespace mcp
} // namespace hunterclaw
