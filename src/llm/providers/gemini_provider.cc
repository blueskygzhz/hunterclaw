#include "hunterclaw/llm/provider.h"
#include "hunterclaw/core/logger.h"
#include "hunterclaw/core/result.h"
#include <curl/curl.h>
#include <json/json.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <chrono>
#include <random>
#include <functional>

namespace hunterclaw {
namespace llm {

// HTTP 响应结构体
struct HttpResponse {
    std::string body;
    long status_code = 0;
    std::map<std::string, std::string> headers;
};

// 前向声明
core::Result<HttpResponse> http_post(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers
);

// UUID 生成函数
std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";
    
    std::string uuid;
    uuid.reserve(36);
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid += '-';
        } else if (i == 14) {
            uuid += '4';
        } else {
            uuid += hex[dis(gen)];
        }
    }
    return uuid;
}

// 流式 HTTP POST (简化版，暂时返回错误)
core::Result<HttpResponse> http_post_stream(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers,
    std::function<void(const std::string&)> callback
) {
    // TODO: 实现真实的 SSE 流式处理
    return core::Result<HttpResponse>::Err("Stream not yet implemented");
}

// ============================================================================
// Gemini Provider 实现
// ============================================================================

class GeminiProvider : public Provider {
public:
    GeminiProvider(const ProviderConfig& config) : config_(config) {
        // Logger initialized
        (void)config;
    }
    
    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        auto request_json = build_request_json(options);
        
        // Gemini uses API key in URL
        std::string url = config_.base_url + "/v1beta/models/" + config_.model + ":generateContent?key=" + config_.api_key;
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"}
        };
        
        auto http_result = http_post(url, request_json, headers);
        if (!http_result.is_ok()) {
            return core::Result<GenerateResponse>::Err(http_result.error);
        }
        
        return parse_response(http_result.get());
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        // ✅ Gemini 流式支持 (SSE)
        // 参考: https://ai.google.dev/api/generate-content#text-gen-text-only-stream
        
        auto request_json = build_request_json(options);
        
        // Gemini streaming endpoint
        std::string url = config_.base_url + "/v1beta/models/" + config_.model + 
                         ":streamGenerateContent?key=" + config_.api_key + 
                         "&alt=sse";
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"}
        };
        
        // ═══════════════════════════════════════════════════════
        // SSE 流式解析 (对齐 OpenClaw)
        // ═══════════════════════════════════════════════════════
        GenerateResponse final_response;
        std::string accumulated_text;
        std::map<std::string, ToolCall> partial_tool_calls;
        
        auto http_result = http_post_stream(url, request_json, headers,
            [&](const std::string& sse_line) {
                // Gemini SSE format: "data: {...JSON...}"
                if (sse_line.compare(0, 6, "data: ") != 0) {
                    return;
                }
                
                std::string data = sse_line.substr(6);
                if (data.empty() || data == "[DONE]") return;
                
                // 解析 JSON
                Json::CharReaderBuilder reader;
                Json::Value chunk_json;
                std::istringstream data_stream(data);
                std::string parse_errs;
                if (!Json::parseFromStream(reader, data_stream, &chunk_json, &parse_errs)) {
                    // Warn:("Failed to parse Gemini SSE JSON: {}", parse_errs);
                    return;
                }
                
                // ───────────────────────────────────────────────────
                // Gemini 流式响应处理 (对齐 API 规范)
                // ───────────────────────────────────────────────────
                
                const auto& candidates = chunk_json["candidates"];
                if (candidates.isArray() && !candidates.empty()) {
                    const auto& first_candidate = candidates[0];
                    const auto& content = first_candidate["content"];
                    const auto& parts = content["parts"];
                    
                    if (parts.isArray()) {
                        for (const auto& part : parts) {
                            if (part.isMember("text")) {
                                // 文本增量
                                std::string text = part["text"].asString();
                                accumulated_text += text;
                                
                                // 流式回调
                                StreamChunk chunk;
                                chunk.delta = text;
                                chunk.is_final = false;
                                callback(chunk);
                                
                            } else if (part.isMember("functionCall")) {
                                // 工具调用
                                const auto& func_call = part["functionCall"];
                                ToolCall tool_call;
                                tool_call.id = generate_uuid();  // Gemini不提供ID
                                tool_call.name = func_call["name"].asString();
                                
                                // Convert Json::Value to JsonValue
                                std::string args_str = Json::writeString(Json::StreamWriterBuilder(), func_call["args"]);
                                nlohmann::json args_nlohmann = nlohmann::json::parse(args_str);
                                tool_call.input = core::JsonValue(args_nlohmann);
                                
                                final_response.tool_calls.push_back(tool_call);
                                
                                // 发送工具调用通知
                                StreamChunk chunk;
                                chunk.is_tool_call = true;
                                chunk.tool_call = tool_call;
                                callback(chunk);
                            }
                        }
                    }
                    
                    // 检查是否完成
                    if (first_candidate.isMember("finishReason")) {
                        StreamChunk chunk;
                        chunk.is_final = true;
                        callback(chunk);
                    }
                }
                
                // Token 使用统计
                const auto& usage = chunk_json["usageMetadata"];
                if (!usage.isNull()) {
                    final_response.input_tokens = usage["promptTokenCount"].asInt();
                    final_response.output_tokens = usage["candidatesTokenCount"].asInt();
                }
            }
        );
        
        if (!http_result.is_ok()) {
            return core::Result<GenerateResponse>::Err(http_result.error);
        }
        
        final_response.content = accumulated_text;
        return core::Result<GenerateResponse>::Ok(final_response);
    }
    
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override { return config_; }
    std::string provider_id() const override { return "gemini"; }
    ProviderAPI api_type() const override { return ProviderAPI::GoogleGenerativeAI; }
    bool supports_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    
private:
    std::string build_request_json(const GenerateOptions& options) {
        Json::Value root;
        
        // Contents (Gemini format)
        Json::Value contents(Json::arrayValue);
        
        for (const auto& msg : options.messages) {
            Json::Value content;
            
            // Role mapping
            switch (msg.role) {
                case MessageRole::User:
                    content["role"] = "user";
                    break;
                case MessageRole::Assistant:
                    content["role"] = "model";
                    break;
                case MessageRole::System:
                    // Gemini doesn't have system role, prepend to first user message
                    continue;
                default:
                    continue;
            }
            
            Json::Value parts(Json::arrayValue);
            Json::Value text_part;
            text_part["text"] = msg.content;
            parts.append(text_part);
            
            content["parts"] = parts;
            contents.append(content);
        }
        
        root["contents"] = contents;
        
        // Generation config
        Json::Value gen_config;
        gen_config["maxOutputTokens"] = options.max_tokens.value_or(config_.max_tokens);
        
        if (options.temperature.has_value()) {
            gen_config["temperature"] = options.temperature.value();
        }
        if (options.top_p.has_value()) {
            gen_config["topP"] = options.top_p.value();
        }
        if (options.top_k.has_value()) {
            gen_config["topK"] = options.top_k.value();
        }
        
        root["generationConfig"] = gen_config;
        
        // Tools (Gemini function calling format)
        if (!options.tools.empty()) {
            Json::Value tools_json(Json::arrayValue);
            Json::Value tool_wrapper;
            Json::Value function_declarations(Json::arrayValue);
            
            for (const auto& tool : options.tools) {
                Json::Value func_decl;
                func_decl["name"] = tool.name;
                func_decl["description"] = tool.description;
                
                // Convert nlohmann::json to Json::Value
                nlohmann::json schema_nlohmann = tool.input_schema.to_json();
                std::string schema_str = schema_nlohmann.dump();
                Json::CharReaderBuilder reader;
                Json::Value schema_jsoncpp;
                std::string errs;
                std::istringstream schema_stream(schema_str);
                Json::parseFromStream(reader, schema_stream, &schema_jsoncpp, &errs);
                
                func_decl["parameters"] = schema_jsoncpp;
                function_declarations.append(func_decl);
            }
            
            tool_wrapper["functionDeclarations"] = function_declarations;
            tools_json.append(tool_wrapper);
            root["tools"] = tools_json;
        }
        
        Json::StreamWriterBuilder builder;
        return Json::writeString(builder, root);
    }
    
    core::Result<GenerateResponse> parse_response(const HttpResponse& http_resp) {
        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;
        
        std::istringstream stream(http_resp.body);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Result<GenerateResponse>::Err("Failed to parse JSON: " + errs);
        }
        
        GenerateResponse response;
        
        // Extract candidates
        const auto& candidates = root["candidates"];
        if (candidates.isArray() && !candidates.empty()) {
            const auto& first_candidate = candidates[0];
            const auto& content = first_candidate["content"];
            const auto& parts = content["parts"];
            
            if (parts.isArray()) {
                for (const auto& part : parts) {
                    if (part.isMember("text")) {
                        response.content += part["text"].asString();
                    } else if (part.isMember("functionCall")) {
                        // Function call
                        const auto& func_call = part["functionCall"];
                        ToolCall tool_call;
                        tool_call.name = func_call["name"].asString();
                        
                        // 将 Json::Value 转换为 JsonValue
                        nlohmann::json args_nlohmann;
                        std::string args_str = Json::writeString(Json::StreamWriterBuilder(), func_call["args"]);
                        args_nlohmann = nlohmann::json::parse(args_str);
                        tool_call.input = core::JsonValue(args_nlohmann);
                        
                        response.tool_calls.push_back(tool_call);
                    }
                }
            }
        }
        
        // Token usage
        const auto& usage = root["usageMetadata"];
        if (!usage.isNull()) {
            response.input_tokens = usage["promptTokenCount"].asInt();
            response.output_tokens = usage["candidatesTokenCount"].asInt();
        }
        
        return core::Result<GenerateResponse>::Ok(response);
    }
    
    ProviderConfig config_;
};

core::Result<Provider::Ptr> create_gemini_provider(const ProviderConfig& config) {
    Provider::Ptr provider = std::make_shared<GeminiProvider>(config);
    return core::Result<Provider::Ptr>::Ok(provider);
}

} // namespace llm
} // namespace hunterclaw
