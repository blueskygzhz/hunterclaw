#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/llm/provider.h"
#include "hunterclaw/tools/tool.h"
#include <iostream>

using namespace hunterclaw;

// ============================================================================
// 示例 1: 使用 Provider 接口创建 ReactEngine (推荐)
// ============================================================================

void example_react_with_provider() {
    std::cout << "\n=== Example 1: ReactEngine with Provider ===\n";
    
    // 1. 创建 Provider (OpenAI GPT-4)
    auto provider = llm::ProviderFactory::create_openai(
        std::getenv("OPENAI_API_KEY"),
        "gpt-4"
    ).value();
    
    // 2. 创建 ToolManager
    auto tool_mgr = std::make_shared<tools::ToolManager>();
    
    // 注册工具
    auto read_file_tool = std::make_shared<tools::Tool>();
    read_file_tool->name = "read_file";
    read_file_tool->description = "Read a file and return its contents";
    read_file_tool->input_schema = core::JsonValue::parse(R"({
        "type": "object",
        "properties": {
            "path": {"type": "string", "description": "File path"}
        },
        "required": ["path"]
    })");
    read_file_tool->execute = [](const core::JsonValue& args) -> tools::ToolResult {
        tools::ToolResult result;
        std::string path = args["path"].to_string();
        
        // 模拟读取文件
        result.success = true;
        result.content = {
            tools::ToolResultContent::text("File content: Hello, World!")
        };
        result.details = core::JsonValue::object({
            {"path", path},
            {"size", 100}
        });
        
        return result;
    };
    tool_mgr->register_tool(read_file_tool);
    
    // 3. 配置 ReactEngine
    react::ReactConfig config;
    config.max_steps = 10;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
    config.block_reply.enabled = true;
    config.block_reply.break_mode = react::ReactConfig::BlockReply::BreakMode::TextEnd;
    config.parallel_tool_calls = true;
    
    // 4. ✅ 使用 create_with_provider 创建 ReactEngine
    auto engine = react::ReactEngine::create_with_provider(
        tool_mgr,
        provider,
        config
    );
    
    // 5. 设置流式回调
    react::StreamCallbacks callbacks;
    callbacks.session_id = "example-session-123";
    
    callbacks.on_reasoning_stream = [](const std::string& reasoning) {
        std::cout << "[Reasoning] " << reasoning << std::flush;
    };
    
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    
    callbacks.on_block_reply = [](const std::string& block) {
        std::cout << "\n[Block Reply] " << block << "\n";
    };
    
    callbacks.on_tool_start = [](const std::string& tool_name, const core::JsonValue& args) {
        std::cout << "\n[Tool Start] " << tool_name << "\n";
        std::cout << "  Args: " << args.to_json_string() << "\n";
    };
    
    callbacks.on_tool_end = [](const std::string& tool_name, const tools::ToolResult& result) {
        std::cout << "[Tool End] " << tool_name << " - " 
                  << (result.success ? "SUCCESS" : "FAILED") << "\n";
    };
    
    callbacks.on_start = []() {
        std::cout << "[Start] ReactEngine started\n";
    };
    
    callbacks.on_end = []() {
        std::cout << "\n[End] ReactEngine completed\n";
    };
    
    callbacks.on_error = [](const std::string& error) {
        std::cerr << "[Error] " << error << "\n";
    };
    
    // 6. 执行 ReAct 循环
    std::string user_input = "Please read the file 'example.txt' and summarize its content.";
    auto result = engine->run(user_input, {}, callbacks);
    
    // 7. 输出结果
    std::cout << "\n=== Result ===\n";
    std::cout << "Success: " << (result.success ? "YES" : "NO") << "\n";
    std::cout << "Steps: " << result.total_steps << "\n";
    std::cout << "Tool Calls: " << result.tool_calls << "\n";
    std::cout << "Duration: " << result.duration_ms << " ms\n";
    std::cout << "Tokens: " << result.usage.input_tokens << " in, " 
              << result.usage.output_tokens << " out\n";
    std::cout << "\nFinal Response:\n" << result.final_response << "\n";
}

// ============================================================================
// 示例 2: 使用 Anthropic (Claude 3.5 Sonnet) + Thinking
// ============================================================================

void example_react_with_anthropic() {
    std::cout << "\n=== Example 2: ReactEngine with Anthropic (Thinking) ===\n";
    
    // 1. 创建 Anthropic Provider
    auto provider = llm::ProviderFactory::create_anthropic(
        std::getenv("ANTHROPIC_API_KEY"),
        "claude-3-5-sonnet-20241022"
    ).value();
    
    // 2. 工具管理器
    auto tool_mgr = std::make_shared<tools::ToolManager>();
    
    // 注册计算器工具
    auto calc_tool = std::make_shared<tools::Tool>();
    calc_tool->name = "calculator";
    calc_tool->description = "Perform mathematical calculations";
    calc_tool->input_schema = core::JsonValue::parse(R"({
        "type": "object",
        "properties": {
            "expression": {"type": "string", "description": "Math expression"}
        },
        "required": ["expression"]
    })");
    calc_tool->execute = [](const core::JsonValue& args) -> tools::ToolResult {
        tools::ToolResult result;
        std::string expr = args["expression"].to_string();
        
        // 简单计算 (实际应使用数学解析器)
        result.success = true;
        result.content = {
            tools::ToolResultContent::text("Result: 42")
        };
        
        return result;
    };
    tool_mgr->register_tool(calc_tool);
    
    // 3. 配置 (启用 Thinking 流式输出)
    react::ReactConfig config;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;  // 流式推理
    config.block_reply.enabled = true;
    
    auto engine = react::ReactEngine::create_with_provider(tool_mgr, provider, config);
    
    // 4. 回调 (捕获 Thinking)
    react::StreamCallbacks callbacks;
    callbacks.on_reasoning_stream = [](const std::string& reasoning) {
        std::cout << "[Thinking] " << reasoning << std::flush;
    };
    
    callbacks.on_assistant_delta = [](const std::string& text) {
        std::cout << text << std::flush;
    };
    
    // 5. 执行
    std::string user_input = "Calculate the sum of 1 + 2 + 3 + ... + 10";
    auto result = engine->run(user_input, {}, callbacks);
    
    std::cout << "\n\nFinal Response:\n" << result.final_response << "\n";
}

// ============================================================================
// 示例 3: 使用 Ollama 本地推理
// ============================================================================

void example_react_with_ollama() {
    std::cout << "\n=== Example 3: ReactEngine with Ollama (Local) ===\n";
    
    // 1. 创建 Ollama Provider (本地服务)
    auto provider = llm::ProviderFactory::create_ollama(
        "llama3.2",  // 模型名称
        "http://localhost:11434"  // Ollama 服务地址
    ).value();
    
    // 2. 工具管理器
    auto tool_mgr = std::make_shared<tools::ToolManager>();
    
    // 3. 配置
    react::ReactConfig config;
    config.max_steps = 5;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::On;
    
    auto engine = react::ReactEngine::create_with_provider(tool_mgr, provider, config);
    
    // 4. 执行
    std::string user_input = "What is the capital of France?";
    auto result = engine->run(user_input);
    
    std::cout << "Response: " << result.final_response << "\n";
}

// ============================================================================
// 示例 4: 向后兼容 - 使用 LLMClient 接口
// ============================================================================

void example_react_with_llm_client() {
    std::cout << "\n=== Example 4: ReactEngine with LLMClient (Backward Compatible) ===\n";
    
    // 1. 创建 LLMClient (内部使用 Provider)
    llm::LLMConfig config;
    config.provider = "openai";
    config.model = "gpt-4";
    config.api_key = std::getenv("OPENAI_API_KEY");
    config.max_tokens = 4096;
    config.temperature = 0.7;
    
    auto llm_client = llm::LLMClientFactory::create(config);
    
    // 2. 工具管理器
    auto tool_mgr = std::make_shared<tools::ToolManager>();
    
    // 3. ✅ 使用旧的 create() 方法 (向后兼容)
    auto engine = react::ReactEngine::create(
        tool_mgr,
        llm_client,
        react::ReactConfig{}
    );
    
    // 4. 执行
    std::string user_input = "Hello, how are you?";
    auto result = engine->run(user_input);
    
    std::cout << "Response: " << result.final_response << "\n";
}

// ============================================================================
// 示例 5: 从 Provider 创建 LLMClient
// ============================================================================

void example_provider_to_llm_client() {
    std::cout << "\n=== Example 5: Create LLMClient from Provider ===\n";
    
    // 1. 创建 Provider
    auto provider = llm::ProviderFactory::create_gemini(
        std::getenv("GEMINI_API_KEY"),
        "gemini-2.0-flash-exp"
    ).value();
    
    // 2. ✅ 从 Provider 创建 LLMClient (适配器)
    llm::LLMConfig config;
    config.provider = "gemini";
    config.model = "gemini-2.0-flash-exp";
    
    auto llm_client = llm::LLMClientFactory::create_from_provider(provider, config);
    
    // 3. 使用 LLMClient 接口
    auto result = llm_client->generate("Tell me a joke about programming.");
    
    if (result.ok) {
        std::cout << "Response: " << result.value << "\n";
    } else {
        std::cerr << "Error: " << result.error << "\n";
    }
}

// ============================================================================
// 示例 6: 多 Provider 并行调用
// ============================================================================

void example_multi_provider() {
    std::cout << "\n=== Example 6: Multi-Provider Comparison ===\n";
    
    // 创建多个 Provider
    auto openai = llm::ProviderFactory::create_openai(
        std::getenv("OPENAI_API_KEY"), "gpt-4"
    ).value();
    
    auto claude = llm::ProviderFactory::create_anthropic(
        std::getenv("ANTHROPIC_API_KEY"), "claude-3-5-sonnet-20241022"
    ).value();
    
    // 相同的 prompt
    llm::GenerateOptions options;
    options.messages = {llm::Message::user("What is AI?")};
    options.max_tokens = 100;
    
    // 并行调用
    auto openai_result = openai->generate(options);
    auto claude_result = claude->generate(options);
    
    std::cout << "\n[GPT-4 Response]\n" << openai_result.value().content << "\n";
    std::cout << "\n[Claude Response]\n" << claude_result.value().content << "\n";
    
    // Token 统计对比
    std::cout << "\n[Token Usage]\n";
    std::cout << "GPT-4:  " << openai_result.value().input_tokens 
              << " in, " << openai_result.value().output_tokens << " out\n";
    std::cout << "Claude: " << claude_result.value().input_tokens 
              << " in, " << claude_result.value().output_tokens << " out\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================\n";
    std::cout << "  HunterClaw Provider Examples\n";
    std::cout << "========================================\n";
    
    // 运行示例
    try {
        example_react_with_provider();         // 推荐方式
        example_react_with_anthropic();        // Anthropic + Thinking
        example_react_with_ollama();           // 本地推理
        example_react_with_llm_client();       // 向后兼容
        example_provider_to_llm_client();      // Provider → LLMClient
        example_multi_provider();              // 多 Provider 对比
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
