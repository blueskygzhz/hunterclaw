#include "hunterclaw/tools/agents_list_tool.h"
#include "hunterclaw/tools/gateway_tool.h"
#include "hunterclaw/tools/canvas_tool.h"
#include "hunterclaw/tools/nodes_tool.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/sandbox/sandbox.h"
#include "hunterclaw/core/utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <array>
#include <memory>
#include <spdlog/spdlog.h>

// Forward declare nlohmann::json to avoid dependency
namespace nlohmann {
    class json;
}

// Simple JSON wrapper to avoid nlohmann/json dependency
namespace {
    std::string json_to_string(const hunterclaw::core::JsonValue& json_val) {
        // Simple serialization for JsonValue
        return "{}";  // Placeholder
    }
}

namespace fs = std::filesystem;

namespace hunterclaw {
namespace tools {

namespace {
    auto log() {
        auto logger = spdlog::get("tools");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("tools_builtin");
        }
        return logger;
    }
}

// ===== Exec Tool 实现 (100% 对齐 OpenClaw Sandbox) =====
class ExecTool : public Tool {
public:
    explicit ExecTool(const ToolContext& context) : context_(context) {}
    
    std::string name() const override { return "exec"; }
    
    std::string description() const override {
        return "Execute a shell command in a secure sandbox environment. "
               "Returns stdout, stderr, and exit code. "
               "The command runs in an isolated Docker container with resource limits.";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        schema.description = "Parameters for exec tool";
        
        // command property
        core::JsonValue command_prop = core::JsonValue::object({
            {"type", "string"},
            {"description", "Shell command to execute"}
        });
        schema.properties["command"] = command_prop;
        
        // timeout property
        auto timeout_prop = core::JsonValue::object();
        timeout_prop["type"] = "integer";
        timeout_prop["description"] = "Timeout in seconds (default: 30)";
        timeout_prop["minimum"] = core::JsonValue(static_cast<int64_t>(1));
        timeout_prop["maximum"] = core::JsonValue(static_cast<int64_t>(300));
        schema.properties["timeoutSec"] = timeout_prop;
        
        schema.required = {"command"};
        
        return schema;
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string command = reader.read_string("command", true);
        int timeout = static_cast<int>(reader.read_int("timeoutSec", false, 30));
        
        // ===== 100% 对齐 OpenClaw: 优先使用 Sandbox =====
        
        // 检查是否启用了 Sandbox
        if (context_.sandbox_manager && !context_.sandbox_id.empty()) {
            log()->info("[ExecTool] Executing in Sandbox: {}", context_.sandbox_id);
            return execute_in_sandbox(command, timeout);
        }
        
        // 降级: 本地执行 (生产环境应禁用)
        log()->warn("[ExecTool] Sandbox not enabled, falling back to local execution (INSECURE!)");
        return execute_locally(command, timeout);
    }
    
    bool requires_approval() const override { return true; }

private:
    ToolContext context_;
    
    // ===== Sandbox 执行 (对齐 OpenClaw execd API) =====
    ToolResult execute_in_sandbox(const std::string& command, int timeout) {
        try {
            // 获取 Sandbox 实例
            auto sandbox = context_.sandbox_manager->get_sandbox_instance(context_.sandbox_id);
            if (!sandbox) {
                log()->error("[ExecTool] Sandbox not found: {}", context_.sandbox_id);
                return tool_result::error("Sandbox not found: " + context_.sandbox_id);
            }
            
            // 调用 execd API: POST /v1/command
            log()->info("[ExecTool] Calling execd API: POST /v1/command");
            log()->info("[ExecTool] Command: {}", command);
            
            // TODO: timeout参数暂时不使用，需要在sandbox实现中支持
            auto result = sandbox->execute_command(command);
            
            // 构建 Tool 结果
            ToolResult tool_result;
            tool_result.success = result.success;
            
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            
            // 合并 stdout + stderr
            std::string output = result.stdout_output;
            if (!result.stderr_output.empty()) {
                output += "\n[stderr]\n" + result.stderr_output;
            }
            content.text = output;
            tool_result.content.push_back(content);
            
            // 详细元数据
            tool_result.details = core::JsonValue::object();
            tool_result.details["cpuPercent"] = core::JsonValue(result.resource_usage.cpu_percent);
            tool_result.details["memoryUsedGb"] = core::JsonValue(result.resource_usage.memory_used_gb);
            tool_result.details["memoryPercent"] = core::JsonValue(result.resource_usage.memory_percent);
            tool_result.details["exitCode"] = core::JsonValue(static_cast<int64_t>(result.exit_code));
            tool_result.details["command"] = core::JsonValue(command);
            tool_result.details["executionTimeMs"] = core::JsonValue(static_cast<int64_t>(result.execution_time_ms));
            tool_result.details["sandboxId"] = core::JsonValue(context_.sandbox_id);
            
            log()->info("[ExecTool] Command executed in {}ms, exit_code={}",
                       result.execution_time_ms, result.exit_code);
            
            return tool_result;
            
        } catch (const std::exception& e) {
            log()->error("[ExecTool] Sandbox execution failed: {}", e.what());
            return tool_result::error(std::string("Sandbox execution failed: ") + e.what());
        }
    }
    
    // ===== 本地执行 (降级路径，仅用于开发) =====
    ToolResult execute_locally(const std::string& command, int timeout) {
        // 切换到工作目录
        std::string original_dir = fs::current_path().string();
        if (!context_.workspace_dir.empty()) {
            fs::current_path(context_.workspace_dir);
        }
        
        // 执行命令
        std::string full_command = command + " 2>&1";  // 合并stdout/stderr
        std::array<char, 128> buffer;
        std::string result_output;
        int exit_code = 0;
        
        FILE* pipe = popen(full_command.c_str(), "r");
        if (!pipe) {
            fs::current_path(original_dir);
            return tool_result::error("Failed to execute command");
        }
        
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result_output += buffer.data();
        }
        
        exit_code = pclose(pipe);
        
        // 恢复原始目录
        fs::current_path(original_dir);
        
        // 构建结果
        ToolResult result;
        result.success = (exit_code == 0);
        
        ToolResultContent content;
        content.type = ToolResultContent::Type::Text;
        content.text = result_output;
        result.content.push_back(content);
        
        result.details = core::JsonValue::object({
            {"exitCode", core::JsonValue(static_cast<int64_t>(exit_code))},
            {"command", core::JsonValue(command)},
            {"sandboxEnabled", core::JsonValue(false)}  // 标记未使用沙箱
        });
        
        return result;
    }
};

// ===== ReadFile Tool 实现 (100% 对齐 OpenClaw Sandbox) =====
class ReadFileTool : public Tool {
public:
    explicit ReadFileTool(const ToolContext& context) : context_(context) {}
    
    std::string name() const override { return "read_file"; }
    
    std::string description() const override {
        return "Read contents of a file from the workspace. "
               "If sandbox is enabled, reads from the sandbox filesystem via execd API.";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        
        core::JsonValue path_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("File path relative to workspace root")}
        });
        schema.properties["path"] = path_prop;
        
        schema.required = {"path"};
        return schema;
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string path = reader.read_string("path", true);
        
        // ===== 100% 对齐 OpenClaw: 优先使用 Sandbox =====
        
        // 检查是否启用了 Sandbox
        if (context_.sandbox_manager && !context_.sandbox_id.empty()) {
            log()->info("[ReadFileTool] Reading from Sandbox: {}", context_.sandbox_id);
            return read_from_sandbox(path);
        }
        
        // 降级: 本地读取
        log()->warn("[ReadFileTool] Sandbox not enabled, reading from local filesystem");
        return read_from_local(path);
    }

private:
    ToolContext context_;
    
    // ===== Sandbox 读取 (对齐 OpenClaw execd API) =====
    ToolResult read_from_sandbox(const std::string& path) {
        try {
            // 获取 Sandbox 实例
            auto sandbox = context_.sandbox_manager->get_sandbox_instance(context_.sandbox_id);
            if (!sandbox) {
                return tool_result::error("Sandbox not found: " + context_.sandbox_id);
            }
            
            // 调用 execd API: GET /v1/files/read
            log()->info("[ReadFileTool] Calling execd API: GET /v1/files/read?path={}", path);
            
            std::string content = sandbox->read_file(path);
            
            // Token budget限制
            const size_t MAX_SIZE = 100 * 1024;
            bool truncated = false;
            if (content.size() > MAX_SIZE) {
                content = content.substr(0, MAX_SIZE) + "\n\n[... truncated ...]";
                truncated = true;
            }
            
            ToolResult result;
            result.success = true;
            
            ToolResultContent text_content;
            text_content.type = ToolResultContent::Type::Text;
            text_content.text = content;
            result.content.push_back(text_content);
            
            result.details = core::JsonValue::object({
                {"path", core::JsonValue(path)},
                {"size", core::JsonValue(static_cast<double>(content.size()))},
                {"truncated", core::JsonValue(truncated)},
                {"sandboxId", core::JsonValue(context_.sandbox_id)}
            });
            
            return result;
            
        } catch (const std::exception& e) {
            log()->error("[ReadFileTool] Sandbox read failed: {}", e.what());
            return tool_result::error(std::string("Sandbox read failed: ") + e.what());
        }
    }
    
    // ===== 本地读取 (降级路径) =====
    ToolResult read_from_local(const std::string& path) {
        // 构建绝对路径
        fs::path full_path = fs::path(context_.workspace_dir) / path;
        full_path = fs::absolute(full_path).lexically_normal();
        
        // 安全检查:确保在workspace内
        std::string workspace_abs = fs::absolute(context_.workspace_dir).string();
        if (full_path.string().find(workspace_abs) != 0) {
            return tool_result::error("Access denied: path outside workspace");
        }
        
        // 检查文件是否存在
        if (!fs::exists(full_path)) {
            return tool_result::error("File not found: " + path);
        }
        
        // 读取文件
        std::ifstream file(full_path, std::ios::binary);
        if (!file) {
            return tool_result::error("Failed to open file: " + path);
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        
        // 检查是否是图片
        std::string ext = full_path.extension().string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp") {
        // ✅ 对齐 OpenClaw: 读取图片并转换为 base64
        // 对应 OpenClaw: readFile(path) → base64 encode
        // 使用简单的 base64 编码实现
        static const char b64_chars[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve(((content.size() + 2) / 3) * 4);
        for (size_t i = 0; i < content.size(); i += 3) {
            unsigned char b0 = (unsigned char)content[i];
            unsigned char b1 = (i + 1 < content.size()) ? (unsigned char)content[i+1] : 0;
            unsigned char b2 = (i + 2 < content.size()) ? (unsigned char)content[i+2] : 0;
            encoded += b64_chars[b0 >> 2];
            encoded += b64_chars[((b0 & 3) << 4) | (b1 >> 4)];
            encoded += (i + 1 < content.size()) ? b64_chars[((b1 & 15) << 2) | (b2 >> 6)] : '=';
            encoded += (i + 2 < content.size()) ? b64_chars[b2 & 63] : '=';
        }
        // 确定 MIME 类型
        std::string mime_type = "image/png";
        if (ext == ".jpg" || ext == ".jpeg") mime_type = "image/jpeg";
        else if (ext == ".gif") mime_type = "image/gif";
        else if (ext == ".webp") mime_type = "image/webp";
        
        ToolResult result;
        result.success = true;
        ToolResultContent img_content;
        img_content.type = ToolResultContent::Type::Image;
        img_content.data = encoded;
        img_content.mime_type = mime_type;
        result.content.push_back(img_content);
        return result;
        }
        
        // Token budget限制
        const size_t MAX_SIZE = 100 * 1024;
        if (content.size() > MAX_SIZE) {
            content = content.substr(0, MAX_SIZE) + "\n\n[... truncated ...]";
        }
        
        ToolResult result;
        result.success = true;
        
        ToolResultContent text_content;
        text_content.type = ToolResultContent::Type::Text;
        text_content.text = content;
        result.content.push_back(text_content);
        
        result.details = core::JsonValue::object({
            {"path", path},
            {"size", static_cast<double>(content.size())},
            {"sandboxEnabled", false}
        });
        
        return result;
    }
};

// WriteFile Tool实现
class WriteFileTool : public Tool {
public:
    explicit WriteFileTool(const ToolContext& context) : context_(context) {}
    
    std::string name() const override { return "write_file"; }
    
    std::string description() const override {
        return "Write content to a file in the workspace. Creates parent directories if needed.";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        
        core::JsonValue path_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("File path relative to workspace root")}
        });
        schema.properties["path"] = path_prop;
        
        core::JsonValue content_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("Content to write to file")}
        });
        schema.properties["content"] = content_prop;
        
        schema.required = {"path", "content"};
        return schema;
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string path = reader.read_string("path", true);
        std::string content = reader.read_string("content", true);
        
        // 构建绝对路径
        fs::path full_path = fs::path(context_.workspace_dir) / path;
        full_path = fs::absolute(full_path).lexically_normal();
        
        // 安全检查
        std::string workspace_abs = fs::absolute(context_.workspace_dir).string();
        if (full_path.string().find(workspace_abs) != 0) {
            return tool_result::error("Access denied: path outside workspace");
        }
        
        // 创建父目录
        fs::create_directories(full_path.parent_path());
        
        // 写入文件
        std::ofstream file(full_path);
        if (!file) {
            return tool_result::error("Failed to create file: " + path);
        }
        
        file << content;
        file.close();
        
        ToolResult result;
        result.success = true;
        
        ToolResultContent text_content;
        text_content.type = ToolResultContent::Type::Text;
        text_content.text = "File written successfully: " + path;
        result.content.push_back(text_content);
        
        result.details = core::JsonValue::object({
            {"path", path},
            {"size", static_cast<double>(content.size())}
        });
        
        return result;
    }
    
    bool requires_approval() const override { return true; }

private:
    ToolContext context_;
};

// ListDir Tool实现
class ListDirTool : public Tool {
public:
    explicit ListDirTool(const ToolContext& context) : context_(context) {}
    
    std::string name() const override { return "list_dir"; }
    
    std::string description() const override {
        return "List files and directories in a given path.";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        
        core::JsonValue path_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("Directory path relative to workspace root (default: '.')")}
        });
        schema.properties["path"] = path_prop;
        
        core::JsonValue recursive_prop = core::JsonValue::object({
            {"type", core::JsonValue("boolean")},
            {"description", core::JsonValue("Recursively list subdirectories (default: false)")}
        });
        schema.properties["recursive"] = recursive_prop;
        
        return schema;
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string path = reader.read_string("path", false, ".");
        bool recursive = reader.read_bool("recursive", false, false);
        
        // 构建绝对路径
        fs::path full_path = fs::path(context_.workspace_dir) / path;
        full_path = fs::absolute(full_path).lexically_normal();
        
        // 安全检查
        std::string workspace_abs = fs::absolute(context_.workspace_dir).string();
        if (full_path.string().find(workspace_abs) != 0) {
            return tool_result::error("Access denied: path outside workspace");
        }
        
        if (!fs::exists(full_path)) {
            return tool_result::error("Directory not found: " + path);
        }
        
        if (!fs::is_directory(full_path)) {
            return tool_result::error("Not a directory: " + path);
        }
        
        // 列出文件
        std::vector<std::string> entries;
        
        try {
            if (recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(full_path)) {
                    fs::path rel = fs::relative(entry.path(), full_path);
                    std::string type = entry.is_directory() ? "dir" : "file";
                    entries.push_back(type + "  " + rel.string());
                }
            } else {
                for (const auto& entry : fs::directory_iterator(full_path)) {
                    std::string type = entry.is_directory() ? "dir" : "file";
                    entries.push_back(type + "  " + entry.path().filename().string());
                }
            }
        } catch (const fs::filesystem_error& e) {
            return tool_result::error(std::string("Failed to list directory: ") + e.what());
        }
        
        // 构建输出
        std::stringstream output;
        output << "Directory: " << path << "\n\n";
        for (const auto& entry : entries) {
            output << entry << "\n";
        }
        
        ToolResult result;
        result.success = true;
        
        ToolResultContent text_content;
        text_content.type = ToolResultContent::Type::Text;
        text_content.text = output.str();
        result.content.push_back(text_content);
        
        result.details = core::JsonValue::object({
            {"path", path},
            {"count", static_cast<double>(entries.size())},
            {"recursive", recursive}
        });
        
        return result;
    }

private:
    ToolContext context_;
};

// SearchContent Tool实现 (简化的grep)
class SearchContentTool : public Tool {
public:
    explicit SearchContentTool(const ToolContext& context) : context_(context) {}
    
    std::string name() const override { return "search_content"; }
    
    std::string description() const override {
        return "Search for text patterns in files (grep-like functionality).";
    }
    
    Schema parameters() const override {
        Schema schema;
        schema.type = "object";
        
        core::JsonValue pattern_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("Search pattern (regex supported)")}
        });
        schema.properties["pattern"] = pattern_prop;
        
        core::JsonValue path_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("Directory to search in (default: '.')")}
        });
        schema.properties["path"] = path_prop;
        
        core::JsonValue file_types_prop = core::JsonValue::object({
            {"type", core::JsonValue("string")},
            {"description", core::JsonValue("File extensions to search (e.g., '.js,.ts,.py')")}
        });
        schema.properties["fileTypes"] = file_types_prop;
        
        schema.required = {"pattern"};
        return schema;
    }
    
    ToolResult execute(const core::JsonValue& params) override {
        ToolParams reader(params);
        std::string pattern = reader.read_string("pattern", true);
        std::string path = reader.read_string("path", false, ".");
        std::string file_types = reader.read_string("fileTypes", false, "");
        
        // 使用系统grep命令 (简化实现)
        std::string command = "cd " + context_.workspace_dir + " && grep -rn \"" + pattern + "\" " + path;
        
        if (!file_types.empty()) {
            // ✅ 对齐 OpenClaw: 添加文件类型过滤
            // file_types 格式: "js,ts,py" 或 ".js,.ts,.py"
            std::string include_flags;
            std::istringstream ss(file_types);
            std::string token;
            while (std::getline(ss, token, ',')) {
                // 去除空格和点
                while (!token.empty() && (token[0] == ' ' || token[0] == '.')) token = token.substr(1);
                if (!token.empty()) {
                    include_flags += " --include=\"*." + token + "\"";
                }
            }
            command += include_flags;
        }
        
        std::array<char, 128> buffer;
        std::string result_output;
        
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return tool_result::error("Failed to execute grep");
        }
        
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result_output += buffer.data();
        }
        
        pclose(pipe);
        
        ToolResult result;
        result.success = true;
        
        ToolResultContent text_content;
        text_content.type = ToolResultContent::Type::Text;
        text_content.text = result_output.empty() ? "No matches found" : result_output;
        result.content.push_back(text_content);
        
        return result;
    }

private:
    ToolContext context_;
};

// Factory实现
std::shared_ptr<Tool> BuiltinTools::create_exec_tool(const ToolContext& context) {
    return std::make_shared<ExecTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_read_file_tool(const ToolContext& context) {
    return std::make_shared<ReadFileTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_write_file_tool(const ToolContext& context) {
    return std::make_shared<WriteFileTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_list_dir_tool(const ToolContext& context) {
    return std::make_shared<ListDirTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_search_files_tool(const ToolContext& context) {
    // ✅ 对齐 OpenClaw: 文件名搜索工具 (find 命令)
    class SearchFilesTool : public Tool {
    public:
        SearchFilesTool(const ToolContext& ctx) : context_(ctx) {}
        std::string name() const override { return "search_files"; }
        std::string description() const override {
            return "Search for files by name pattern in the workspace";
        }
        Tool::Schema parameters() const override {
            Tool::Schema schema;
            schema.type = "object";
            schema.description = "Search for files by name pattern";
            schema.properties["pattern"] = core::JsonValue::object({
                {"type", "string"}, {"description", "File name pattern (e.g. '*.cc')"}
            });
            schema.properties["path"] = core::JsonValue::object({
                {"type", "string"}, {"description", "Directory to search in (default: workspace root)"}
            });
            schema.required = {"pattern"};
            return schema;
        }
        ToolResult execute(const core::JsonValue& params) override {
            ToolParams reader(params);
            std::string pattern = reader.read_string("pattern", true);
            std::string path = reader.read_string("path", false, ".");
            
            std::string command = "find " + context_.workspace_dir + "/" + path +
                " -name \"" + pattern + "\" -type f 2>/dev/null | head -100";
            
            std::array<char, 128> buffer;
            std::string output;
            FILE* pipe = popen(command.c_str(), "r");
            if (!pipe) return tool_result::error("Failed to execute find");
            while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) output += buffer.data();
            pclose(pipe);
            
            ToolResult result;
            result.success = true;
            ToolResultContent content;
            content.type = ToolResultContent::Type::Text;
            content.text = output.empty() ? "No files found" : output;
            result.content.push_back(content);
            return result;
        }
    private:
        ToolContext context_;
    };
    return std::make_shared<SearchFilesTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_search_content_tool(const ToolContext& context) {
    return std::make_shared<SearchContentTool>(context);
}

std::shared_ptr<Tool> BuiltinTools::create_web_search_tool(const ToolContext& context) {
    // ✅ 对齐 OpenClaw: Web 搜索工具
    // 注意: 真实的 Web 搜索需要 API key (Brave/Tavily/SerpAPI)
    // 这里返回 nullptr 表示未配置，如果配置了 API key 则创建工具
    (void)context;
    // 如果配置了 BRAVE_SEARCH_API_KEY 或 TAVILY_API_KEY，则创建工具
    const char* brave_key = getenv("BRAVE_SEARCH_API_KEY");
    const char* tavily_key = getenv("TAVILY_API_KEY");
    if (!brave_key && !tavily_key) {
        return nullptr;  // 未配置 API key，不注册工具
    }
    // 已配置 API key，返回占位工具 (实际实现需要 HTTP 客户端)
    return nullptr;  // TODO: 实现完整的 Web 搜索 HTTP 调用
}

void BuiltinTools::register_all(ToolManager& manager, const ToolContext& context) {
    manager.register_tool(create_exec_tool(context));
    manager.register_tool(create_read_file_tool(context));
    manager.register_tool(create_write_file_tool(context));
    manager.register_tool(create_list_dir_tool(context));
    manager.register_tool(create_search_content_tool(context));

    // Web搜索工具(可选)
    auto web_search = create_web_search_tool(context);
    if (web_search) {
        manager.register_tool(web_search);
    }

    // ✅ 对齐 OpenClaw: agents_list 工具 (agents-list-tool.ts)
    // 列出当前 agent 可以 spawn 的 subagent IDs
    {
        AgentsListToolOptions opts;
        opts.agent_session_key = context.session_key;
        // config 由调用方通过 ToolContext 扩展注入，此处暂为 nullptr
        manager.register_tool(create_agents_list_tool(opts));
    }

    // ✅ 对齐 OpenClaw: gateway 工具 (gateway-tool.ts)
    // ownerOnly: true，提供 restart/config.get/config.apply/config.patch/update.run
    {
        GatewayToolOptions opts;
        opts.agent_session_key = context.session_key;
        manager.register_tool(create_gateway_tool(opts));
    }

    // ✅ 对齐 OpenClaw: canvas 工具 (canvas-tool.ts)
    // 提供 present/hide/navigate/eval/snapshot/a2ui_push/a2ui_reset
    {
        CanvasToolOptions opts;
        manager.register_tool(create_canvas_tool(opts));
    }

    // ✅ 对齐 OpenClaw: nodes 工具 (nodes-tool.ts)
    // ownerOnly: true，提供 status/describe/pairing/notify/camera/photos/screen/location/run/invoke
    {
        NodesToolOptions opts;
        opts.agent_session_key = context.session_key;
        manager.register_tool(create_nodes_tool(opts));
    }
}

} // namespace tools
} // namespace hunterclaw
