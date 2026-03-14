/**
 * HunterClaw 流式输出回调使用示例
 * 演示 on_block_callback 和其他流式回调的实际应用
 */

#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/tools/tool_manager.h"
#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/gateway/connection_pool.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

using namespace hunterclaw;

// ============================================================================
// 示例 1: 基础流式输出到控制台
// ============================================================================

void example_basic_streaming() {
    std::cout << "\n=== 示例 1: 基础流式输出 ===\n";
    
    // 创建 ReactEngine
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    llm_config.api_key = std::getenv("ANTHROPIC_API_KEY");
    
    auto llm_client = llm::LLMClient::create(llm_config);
    auto engine = react::ReactEngine::create(tool_mgr, llm_client);
    
    // 配置流式回调
    react::StreamCallbacks callbacks;
    
    // 1. 助手输出流 (实时打字效果)
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    
    // 2. 块回复 (在工具调用前刷新)
    callbacks.on_block_reply = [](const std::string& block) {
        std::cout << "\n--- [块输出] ---\n" << block << "\n--- [结束] ---\n";
    };
    
    // 3. 工具调用提示
    callbacks.on_tool_start = [](const std::string& name, const core::JsonValue& args) {
        std::cout << "\n🔧 [工具调用] " << name << "\n";
    };
    
    callbacks.on_tool_end = [](const std::string& name, const tools::ToolResult& result) {
        std::cout << "✅ [工具完成] " << name 
                  << " (状态: " << (result.ok ? "成功" : "失败") << ")\n";
    };
    
    // 执行
    auto result = engine->run("帮我分析当前目录的文件结构", {}, callbacks);
    
    std::cout << "\n\n最终输出: " << result.final_output << "\n";
}

// ============================================================================
// 示例 2: WebSocket 实时推送
// ============================================================================

void example_websocket_streaming(const std::string& session_id) {
    std::cout << "\n=== 示例 2: WebSocket 实时推送 ===\n";
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    
    react::ReactConfig config;
    config.block_reply.enabled = true;
    config.block_reply.min_chars = 100;
    config.block_reply.max_chars = 500;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    // 配置回调推送到 WebSocket
    react::StreamCallbacks callbacks;
    callbacks.session_id = session_id;
    
    // 推理流
    callbacks.on_reasoning_stream = [session_id](const std::string& reasoning) {
        core::JsonValue payload = core::JsonValue::object({
            {"type", "reasoning"},
            {"content", reasoning}
        });
        
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.reasoning",
            payload
        );
    };
    
    // 助手输出
    callbacks.on_assistant_delta = [session_id](const std::string& text) {
        core::JsonValue payload = core::JsonValue::object({
            {"text", text}
        });
        
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.assistant_delta",
            payload
        );
    };
    
    // 块回复 (重点: on_block_callback 对应物)
    callbacks.on_block_reply = [session_id](const std::string& block) {
        core::JsonValue payload = core::JsonValue::object({
            {"content", block},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        });
        
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.block_reply",
            payload
        );
        
        std::cout << "[推送块回复] 长度: " << block.size() << " 字符\n";
    };
    
    // 工具调用
    callbacks.on_tool_start = [session_id](const std::string& name, const core::JsonValue& args) {
        core::JsonValue payload = core::JsonValue::object({
            {"tool_name", name},
            {"args", args}
        });
        
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.tool_start",
            payload
        );
    };
    
    callbacks.on_tool_end = [session_id](const std::string& name, const tools::ToolResult& result) {
        core::JsonValue payload = core::JsonValue::object({
            {"tool_name", name},
            {"success", result.ok},
            {"output", result.output}
        });
        
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.tool_end",
            payload
        );
    };
    
    // 生命周期
    callbacks.on_start = [session_id]() {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.start",
            core::JsonValue::object({{"status", "running"}})
        );
    };
    
    callbacks.on_end = [session_id]() {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.end",
            core::JsonValue::object({{"status", "completed"}})
        );
    };
    
    callbacks.on_error = [session_id](const std::string& error) {
        gateway::ConnectionPool::instance().send_event(
            session_id,
            "agent.error",
            core::JsonValue::object({{"error", error}})
        );
    };
    
    // 执行
    auto result = engine->run("帮我查找项目中的所有 TODO 注释", {}, callbacks);
}

// ============================================================================
// 示例 3: 进度跟踪和统计
// ============================================================================

void example_progress_tracking() {
    std::cout << "\n=== 示例 3: 进度跟踪 ===\n";
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    auto engine = react::ReactEngine::create(tool_mgr, llm_client);
    
    // 统计数据
    struct Stats {
        size_t total_chars = 0;
        size_t total_tokens = 0;
        size_t block_count = 0;
        size_t tool_calls = 0;
        std::chrono::system_clock::time_point start_time;
    } stats;
    
    stats.start_time = std::chrono::system_clock::now();
    
    react::StreamCallbacks callbacks;
    
    // 字符统计
    callbacks.on_assistant_delta = [&stats](const std::string& text) {
        stats.total_chars += text.size();
        stats.total_tokens += text.size() / 4;  // 粗略估计
        
        std::cout << "\r进度: " << stats.total_chars << " 字符 / "
                  << stats.total_tokens << " tokens (估算)" << std::flush;
    };
    
    // 块计数
    callbacks.on_block_reply = [&stats](const std::string& block) {
        stats.block_count++;
        std::cout << "\n[第 " << stats.block_count << " 块] "
                  << block.size() << " 字符\n";
    };
    
    // 工具调用计数
    callbacks.on_tool_start = [&stats](const std::string& name, const core::JsonValue& args) {
        stats.tool_calls++;
        std::cout << "\n[工具 #" << stats.tool_calls << "] " << name << "\n";
    };
    
    // 步骤信息
    callbacks.on_step = [](const react::ReactStep& step) {
        std::cout << "\n[步骤 " << step.step_number << "] "
                  << step.tool_calls.size() << " 个工具调用\n";
    };
    
    // 完成统计
    callbacks.on_end = [&stats]() {
        auto end_time = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            end_time - stats.start_time
        ).count();
        
        std::cout << "\n\n=== 执行统计 ===\n"
                  << "总字符数: " << stats.total_chars << "\n"
                  << "总 tokens (估算): " << stats.total_tokens << "\n"
                  << "块数量: " << stats.block_count << "\n"
                  << "工具调用: " << stats.tool_calls << "\n"
                  << "总耗时: " << duration << " 秒\n";
    };
    
    // 执行
    auto result = engine->run("分析 HunterClaw 项目的代码结构", {}, callbacks);
}

// ============================================================================
// 示例 4: 文件日志记录
// ============================================================================

void example_file_logging() {
    std::cout << "\n=== 示例 4: 文件日志记录 ===\n";
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    auto engine = react::ReactEngine::create(tool_mgr, llm_client);
    
    // 打开日志文件
    std::ofstream log_file("agent_execution.log", std::ios::app);
    
    auto timestamp = []() -> std::string {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    };
    
    react::StreamCallbacks callbacks;
    
    // 记录推理
    callbacks.on_reasoning_stream = [&log_file, &timestamp](const std::string& reasoning) {
        log_file << "[" << timestamp() << "] [REASONING] " << reasoning << "\n";
    };
    
    // 记录输出
    callbacks.on_assistant_delta = [&log_file](const std::string& text) {
        log_file << text;
    };
    
    // 记录块回复
    callbacks.on_block_reply = [&log_file, &timestamp](const std::string& block) {
        log_file << "\n[" << timestamp() << "] [BLOCK_REPLY] "
                 << block.size() << " chars\n" << block << "\n";
    };
    
    // 记录工具调用
    callbacks.on_tool_start = [&log_file, &timestamp](const std::string& name, const core::JsonValue& args) {
        log_file << "[" << timestamp() << "] [TOOL_START] " << name << "\n";
    };
    
    callbacks.on_tool_end = [&log_file, &timestamp](const std::string& name, const tools::ToolResult& result) {
        log_file << "[" << timestamp() << "] [TOOL_END] " << name 
                 << " - " << (result.ok ? "SUCCESS" : "FAILED") << "\n";
    };
    
    callbacks.on_error = [&log_file, &timestamp](const std::string& error) {
        log_file << "[" << timestamp() << "] [ERROR] " << error << "\n";
    };
    
    // 执行
    auto result = engine->run("生成项目的 README.md 文件", {}, callbacks);
    
    log_file.close();
    std::cout << "日志已写入 agent_execution.log\n";
}

// ============================================================================
// 示例 5: 块回复配置调优
// ============================================================================

void example_block_reply_tuning() {
    std::cout << "\n=== 示例 5: 块回复配置调优 ===\n";
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    
    // 🎯 核心: 块回复配置
    react::ReactConfig config;
    
    // 启用块回复
    config.block_reply.enabled = true;
    
    // 设置阈值
    config.block_reply.min_chars = 50;    // 最小 50 字符才触发
    config.block_reply.max_chars = 300;   // 最大 300 字符就触发
    
    // 分块模式: 在文本结束时分块
    config.block_reply.break_mode = react::ReactConfig::BlockReply::BreakMode::TextEnd;
    
    // 分块偏好: 在段落边界分块
    config.block_reply.break_preference = react::ReactConfig::BlockReply::BreakPreference::Paragraph;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    react::StreamCallbacks callbacks;
    
    callbacks.on_block_reply = [](const std::string& block) {
        std::cout << "\n╔════════════════════════════════════════╗\n"
                  << "║           块回复 (" << block.size() << " 字符)           ║\n"
                  << "╚════════════════════════════════════════╝\n"
                  << block << "\n"
                  << "═══════════════════════════════════════════\n";
    };
    
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    
    // 执行
    auto result = engine->run("写一个简短的 C++ 教程", {}, callbacks);
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "╔══════════════════════════════════════════════════╗\n"
              << "║  HunterClaw 流式输出回调使用示例               ║\n"
              << "╚══════════════════════════════════════════════════╝\n";
    
    try {
        // 示例 1: 基础流式输出
        example_basic_streaming();
        
        // 示例 2: WebSocket 推送 (需要 session_id)
        // example_websocket_streaming("session-12345");
        
        // 示例 3: 进度跟踪
        example_progress_tracking();
        
        // 示例 4: 文件日志
        example_file_logging();
        
        // 示例 5: 块回复配置
        example_block_reply_tuning();
        
        std::cout << "\n✅ 所有示例执行完成！\n";
        
    } catch (const std::exception& e) {
        std::cerr << "❌ 错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
