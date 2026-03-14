#include "hunterclaw/llm/provider.h"
#include "hunterclaw/core/logger.h"
#include <curl/curl.h>
#include <sstream>

namespace hunterclaw {
namespace llm {

extern core::Result<HttpResponse> http_post(
    const std::string& url,
    const std::string& json_body,
    const std::map<std::string, std::string>& headers
);

// ============================================================================
// Ollama Provider 实现
// ============================================================================

class OllamaProvider : public Provider {
public:
    OllamaProvider(const ProviderConfig& config) : config_(config) {
        HUNTERCLAW_LOG_INFO("OllamaProvider initialized: model={}, base_url={}", 
            config.model, config.base_url);
    }
    
    core::Result<GenerateResponse> generate(const GenerateOptions& options) override {
        auto request_json = build_request_json(options);
        
        std::map<std::string, std::string> headers = {
            {"Content-Type", "application/json"}
        };
        
        std::string url = config_.base_url + "/api/chat";
        
        auto http_result = http_post(url, request_json, headers);
        if (!http_result.is_ok()) {
            return core::Error(http_result.error().message());
        }
        
        return parse_response(http_result.value());
    }
    
    core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) override {
        return core::Error("Streaming not yet implemented for Ollama");
    }
    
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.length() / 4);
    }
    
    const ProviderConfig& config() const override { return config_; }
    std::string provider_id() const override { return "ollama"; }
    ProviderAPI api_type() const override { return ProviderAPI::Ollama; }
    bool supports_tools() const override { return true; }
    bool supports_streaming() const override { return true; }
    
    // Ollama 特有功能: 列出本地模型
    core::Result<std::vector<std::string>> list_models() {
        std::string url = config_.base_url + "/api/tags";
        
        CURL* curl = curl_easy_init();
        if (!curl) {
            return core::Error("Failed to initialize CURL");
        }
        
        std::string response_body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        
        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            return core::Error(std::string("CURL error: ") + curl_easy_strerror(res));
        }
        
        // Parse response
        Json::CharReaderBuilder reader_builder;
        Json::Value root;
        std::string errs;
        
        std::istringstream stream(response_body);
        if (!Json::parseFromStream(reader_builder, stream, &root, &errs)) {
            return core::Error("Failed to parse JSON: " + errs);
        }
        
        std::vector<std::string> models;
        const auto& models_json = root["models"];
        if (models_json.isArray()) {
            for (const auto& model : models_json) {
                models.push_back(model["name"].asString());
            }
        }
        
        return models;
    }
    
private:
    std::string build_request_json(const GenerateOptions& options) {
        Json::Value root;
        root["model"] = config_.model;
        root["stream"] = false;
        
        // Messages
        Json::Value messages_json(Json::arrayValue);
        for (const auto& msg : options.messages) {
            Json::Value msg_json;
            
            switch (msg.role) {
                case MessageRole::System:
                    msg_json["role"] = "system";
                    break;
                case MessageRole::User:
                    msg_json["role"] = "user";
                    break;
                case MessageRole::Assistant:
                    msg_json["role"] = "assistant";
                    break;
                case MessageRole::Tool:
                    msg_json["role"] = "tool";
                    break;
            }
            
            msg_json["content"] = msg.content;
            messages_json.append(msg_json);
        }
        root["messages"] = messages_json;
        
        // Options
        Json::Value opts;
        if (options.temperature.has_value()) {
            opts["temperature"] = options.temperature.value();
        }
        if (options.top_p.has_value()) {
            opts["top_p"] = options.top_p.value();
        }
        if (options.top_k.has_value()) {
            opts["top_k"] = options.top_k.value();
        }
        
        if (!opts.empty()) {
            root["options"] = opts;
        }
        
        // Tools (Ollama native format)
        if (!options.tools.empty()) {
            Json::Value tools_json(Json::arrayValue);
            for (const auto& tool : options.tools) {
                Json::Value tool_json;
                tool_json["type"] = "function";
                tool_json["function"]["name"] = tool.name;
                tool_json["function"]["description"] = tool.description;
                tool_json["function"]["parameters"] = tool.input_schema.to_json();
                tools_json.append(tool_json);
            }
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
            return core::Error("Failed to parse JSON: " + errs);
        }
        
        GenerateResponse response;
        
        // Extract message
        const auto& message = root["message"];
        if (!message.isNull()) {
            response.content = message["content"].asString();
            
            // Tool calls
            const auto& tool_calls = message["tool_calls"];
            if (tool_calls.isArray()) {
                for (const auto& call : tool_calls) {
                    ToolCall tool_call;
                    tool_call.name = call["function"]["name"].asString();
                    
                    std::string args_str = call["function"]["arguments"].asString();
                    tool_call.input = core::JsonValue::parse(args_str);
                    
                    response.tool_calls.push_back(tool_call);
                }
            }
        }
        
        return response;
    }
    
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* str = static_cast<std::string*>(userdata);
        str->append(ptr, size * nmemb);
        return size * nmemb;
    }
    
    ProviderConfig config_;
};

core::Result<Provider::Ptr> create_ollama_provider(const ProviderConfig& config) {
    return std::make_shared<OllamaProvider>(config);
}

} // namespace llm
} // namespace hunterclaw
