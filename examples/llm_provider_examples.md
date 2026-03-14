# HunterClaw LLM Provider 使用示例

## 1. OpenAI Provider

```cpp
#include "hunterclaw/llm/provider.h"
#include <iostream>

using namespace hunterclaw::llm;

int main() {
    // 创建 OpenAI Provider
    auto provider_result = ProviderFactory::create_openai(
        "sk-xxxxx",           // API Key
        "gpt-4",              // 模型
        "https://api.openai.com/v1"  // 可选: 自定义端点
    );
    
    if (!provider_result.is_ok()) {
        std::cerr << "Failed to create provider: " 
                  << provider_result.error().message() << std::endl;
        return 1;
    }
    
    auto provider = provider_result.value();
    
    // 生成文本
    GenerateOptions options;
    options.messages = {
        Message::system("You are a helpful assistant."),
        Message::user("What is the capital of France?")
    };
    options.max_tokens = 100;
    options.temperature = 0.7;
    
    auto response = provider->generate(options);
    
    if (response.is_ok()) {
        std::cout << "Response: " << response.value().content << std::endl;
        std::cout << "Tokens: " << response.value().input_tokens 
                  << " in, " << response.value().output_tokens << " out" << std::endl;
    } else {
        std::cerr << "Error: " << response.error().message() << std::endl;
    }
    
    return 0;
}
```

## 2. Tool Calling (OpenAI)

```cpp
// 定义工具
ToolDefinition read_file_tool;
read_file_tool.name = "read_file";
read_file_tool.description = "Read the contents of a file";
read_file_tool.input_schema = core::JsonValue::parse(R"({
    "type": "object",
    "properties": {
        "path": {
            "type": "string",
            "description": "Path to the file"
        }
    },
    "required": ["path"]
})");

GenerateOptions options;
options.messages = {
    Message::user("Read the file /etc/hosts")
};
options.tools = {read_file_tool};
options.tool_choice = "auto";

auto response = provider->generate(options);

if (response.is_ok() && !response.value().tool_calls.empty()) {
    for (const auto& call : response.value().tool_calls) {
        std::cout << "Tool call: " << call.name << std::endl;
        std::cout << "Arguments: " << call.input.to_json_string() << std::endl;
        
        // 执行工具并返回结果
        std::string result = execute_tool(call.name, call.input);
        
        // 将工具结果发送回 LLM
        options.messages.push_back(Message::tool_result(result, call.id, call.name));
    }
    
    // 继续对话
    auto final_response = provider->generate(options);
    std::cout << "Final: " << final_response.value().content << std::endl;
}
```

## 3. Anthropic Provider (Claude)

```cpp
auto provider = ProviderFactory::create_anthropic(
    "sk-ant-xxxxx",
    "claude-3-5-sonnet-20241022"
).value();

GenerateOptions options;
options.messages = {
    Message::system("You are Claude, an AI assistant."),
    Message::user("Explain quantum computing in simple terms.")
};
options.max_tokens = 500;

auto response = provider->generate(options);
std::cout << response.value().content << std::endl;

// 检查 Thinking blocks (Claude 3.5 Sonnet)
if (response.value().content.find("[Thinking]") != std::string::npos) {
    std::cout << "Model used extended thinking!" << std::endl;
}

// 检查 Prompt Caching
if (response.value().cache_read_tokens > 0) {
    std::cout << "Cache hit: " << response.value().cache_read_tokens 
              << " tokens read from cache" << std::endl;
}
```

## 4. Ollama Provider (本地推理)

```cpp
auto provider = ProviderFactory::create_ollama(
    "llama3.2",                  // 模型
    "http://127.0.0.1:11434"     // Ollama 服务地址
).value();

// 列出本地可用模型
auto ollama_provider = std::dynamic_pointer_cast<OllamaProvider>(provider);
auto models = ollama_provider->list_models();

if (models.is_ok()) {
    std::cout << "Available models:" << std::endl;
    for (const auto& model : models.value()) {
        std::cout << "  - " << model << std::endl;
    }
}

// 生成文本
GenerateOptions options;
options.messages = {Message::user("Tell me a joke")};
auto response = provider->generate(options);
std::cout << response.value().content << std::endl;
```

## 5. Gemini Provider (超长上下文)

```cpp
auto provider = ProviderFactory::create_gemini(
    "AIza-xxxxx",
    "gemini-2.0-flash-exp"  // 1M tokens 上下文
).value();

// Gemini 支持超长文档
std::string long_document = read_file("very_long_paper.txt");  // 100K tokens

GenerateOptions options;
options.messages = {
    Message::user("Analyze this document:\n\n" + long_document)
};
options.max_tokens = 4096;

auto response = provider->generate(options);
std::cout << "Analysis: " << response.value().content << std::endl;
std::cout << "Input tokens: " << response.value().input_tokens << std::endl;
```

## 6. 自动 Provider 发现

```cpp
// 从环境变量自动发现 Provider
// 需要设置环境变量:
//   - OPENAI_API_KEY=sk-xxxxx
//   - ANTHROPIC_API_KEY=sk-ant-xxxxx
//   - GOOGLE_API_KEY=AIza-xxxxx
//   - OLLAMA_BASE_URL=http://localhost:11434

auto discovered = ProviderFactory::discover_providers();

std::cout << "Discovered " << discovered.size() << " providers:" << std::endl;

for (const auto& config : discovered) {
    std::cout << "  - " << config.provider_id 
              << " (" << config.model << ")" << std::endl;
    
    auto provider = ProviderFactory::create(config).value();
    
    GenerateOptions options;
    options.messages = {Message::user("Hello!")};
    
    auto response = provider->generate(options);
    if (response.is_ok()) {
        std::cout << "    Response: " << response.value().content << std::endl;
    }
}
```

## 7. 流式响应 (计划中)

```cpp
GenerateOptions options;
options.messages = {Message::user("Write a short story")};

// 流式生成
provider->generate_stream(options, [](const StreamChunk& chunk) {
    if (!chunk.is_final) {
        std::cout << chunk.delta << std::flush;
    } else {
        std::cout << "\n[Done]" << std::endl;
    }
});
```

## 8. 自定义 Provider

```cpp
// 注册自定义 Provider
ProviderFactory::register_provider("my-custom-provider", 
    [](const ProviderConfig& config) -> core::Result<Provider::Ptr> {
        return std::make_shared<MyCustomProvider>(config);
    }
);

// 使用自定义 Provider
ProviderConfig config;
config.provider_id = "my-custom-provider";
config.model = "my-model-v1";
config.api_key = "xxxxx";

auto provider = ProviderFactory::create(config).value();
```

## 9. 多 Provider 并行调用

```cpp
#include <future>

std::vector<Provider::Ptr> providers = {
    ProviderFactory::create_openai("sk-xxx", "gpt-4").value(),
    ProviderFactory::create_anthropic("sk-ant-xxx", "claude-3-5-sonnet-20241022").value(),
    ProviderFactory::create_gemini("AIza-xxx", "gemini-2.0-flash-exp").value()
};

GenerateOptions options;
options.messages = {Message::user("What is AI?")};

std::vector<std::future<core::Result<GenerateResponse>>> futures;

for (auto& provider : providers) {
    futures.push_back(std::async(std::launch::async, [&provider, &options]() {
        return provider->generate(options);
    }));
}

// 收集结果
for (size_t i = 0; i < futures.size(); ++i) {
    auto response = futures[i].get();
    if (response.is_ok()) {
        std::cout << "Provider " << i << ": " 
                  << response.value().content << std::endl;
    }
}
```

## 10. Provider 能力查询

```cpp
auto provider = ProviderFactory::create_openai("sk-xxx", "gpt-4").value();

std::cout << "Provider: " << provider->provider_id() << std::endl;
std::cout << "Supports tools: " << provider->supports_tools() << std::endl;
std::cout << "Supports streaming: " << provider->supports_streaming() << std::endl;

// 估算 token
std::string text = "Hello, world!";
int tokens = provider->estimate_tokens(text);
std::cout << "Estimated tokens: " << tokens << std::endl;
```

## 编译

```bash
# CMakeLists.txt
add_executable(llm_example examples/llm_example.cc)
target_link_libraries(llm_example 
    hunterclaw_llm
    curl
    jsoncpp
)

# 编译
mkdir build && cd build
cmake ..
make llm_example

# 运行
export OPENAI_API_KEY=sk-xxxxx
export ANTHROPIC_API_KEY=sk-ant-xxxxx
./llm_example
```

## 环境变量配置

```bash
# OpenAI
export OPENAI_API_KEY=sk-xxxxx

# Anthropic
export ANTHROPIC_API_KEY=sk-ant-xxxxx

# Gemini
export GOOGLE_API_KEY=AIza-xxxxx

# Ollama
export OLLAMA_BASE_URL=http://127.0.0.1:11434

# Azure OpenAI
export AZURE_OPENAI_API_KEY=xxxxx
export AZURE_OPENAI_ENDPOINT=https://xxx.openai.azure.com/
```
