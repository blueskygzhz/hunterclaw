#include "hunterclaw/gateway/http/openai_compat.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <ctime>
#include <sstream>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {
namespace http {

// Helper for logging
namespace {
    template<typename... Args>
    inline void log_debug(const char* fmt, Args&&... args) {
        auto logger = spdlog::get("console");
        if (logger) logger->debug(fmt, std::forward<Args>(args)...);
    }
    
    // Convert string role to LLMMessageRole
    inline llm::LLMMessageRole string_to_role(const std::string& role) {
        if (role == "system") return llm::LLMMessageRole::System;
        if (role == "assistant") return llm::LLMMessageRole::Assistant;
        if (role == "tool") return llm::LLMMessageRole::Tool;
        return llm::LLMMessageRole::User;  // default
    }
}

// ============================================================================
// ChatMessage Implementation
// ============================================================================

json ChatMessage::to_json() const {
    json j = {
        {"role", role},
        {"content", content}
    };
    if (name.has_value()) {
        j["name"] = name.value();
    }
    return j;
}

ChatMessage ChatMessage::from_json(const json& j) {
    ChatMessage msg;
    msg.role = j.value("role", "user");
    msg.content = j.value("content", "");
    if (j.contains("name")) {
        msg.name = j["name"].get<std::string>();
    }
    return msg;
}

// ============================================================================
// FunctionDefinition Implementation
// ============================================================================

json FunctionDefinition::to_json() const {
    return {
        {"name", name},
        {"description", description},
        {"parameters", parameters}
    };
}

FunctionDefinition FunctionDefinition::from_json(const json& j) {
    FunctionDefinition func;
    func.name = j.value("name", "");
    func.description = j.value("description", "");
    func.parameters = j.value("parameters", json::object());
    return func;
}

// ============================================================================
// ChatCompletionRequest Implementation
// ============================================================================

ChatCompletionRequest ChatCompletionRequest::from_json(const json& j) {
    ChatCompletionRequest req;
    
    req.model = j.value("model", "gpt-3.5-turbo");
    
    // Parse messages
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const auto& msg_json : j["messages"]) {
            req.messages.push_back(ChatMessage::from_json(msg_json));
        }
    }
    
    // Optional parameters
    if (j.contains("temperature")) req.temperature = j["temperature"].get<double>();
    if (j.contains("top_p")) req.top_p = j["top_p"].get<double>();
    if (j.contains("max_tokens")) req.max_tokens = j["max_tokens"].get<int>();
    if (j.contains("stream")) req.stream = j["stream"].get<bool>();
    if (j.contains("presence_penalty")) req.presence_penalty = j["presence_penalty"].get<double>();
    if (j.contains("frequency_penalty")) req.frequency_penalty = j["frequency_penalty"].get<double>();
    if (j.contains("user")) req.user = j["user"].get<std::string>();
    if (j.contains("n")) req.n = j["n"].get<int>();
    
    // Stop sequences
    if (j.contains("stop")) {
        if (j["stop"].is_string()) {
            req.stop = std::vector<std::string>{j["stop"].get<std::string>()};
        } else if (j["stop"].is_array()) {
            req.stop = j["stop"].get<std::vector<std::string>>();
        }
    }
    
    // Functions
    if (j.contains("functions") && j["functions"].is_array()) {
        std::vector<FunctionDefinition> functions;
        for (const auto& func_json : j["functions"]) {
            functions.push_back(FunctionDefinition::from_json(func_json));
        }
        req.functions = functions;
    }
    
    if (j.contains("function_call")) {
        req.function_call = j["function_call"];
    }
    
    return req;
}

// ============================================================================
// ChatCompletionResponse Implementation
// ============================================================================

json ChatCompletionResponse::Choice::to_json() const {
    json j = {
        {"index", index},
        {"message", message.to_json()}
    };
    if (finish_reason.has_value()) {
        j["finish_reason"] = finish_reason.value();
    }
    return j;
}

json ChatCompletionResponse::Usage::to_json() const {
    return {
        {"prompt_tokens", prompt_tokens},
        {"completion_tokens", completion_tokens},
        {"total_tokens", total_tokens}
    };
}

json ChatCompletionResponse::to_json() const {
    json choices_json = json::array();
    for (const auto& choice : choices) {
        choices_json.push_back(choice.to_json());
    }
    
    return {
        {"id", id},
        {"object", object},
        {"created", created},
        {"model", model},
        {"choices", choices_json},
        {"usage", usage.to_json()}
    };
}

// ============================================================================
// ChatCompletionChunk Implementation
// ============================================================================

json ChatCompletionChunk::ChoiceDelta::Delta::to_json() const {
    json j = json::object();
    if (role.has_value()) j["role"] = role.value();
    if (content.has_value()) j["content"] = content.value();
    if (function_call.has_value()) j["function_call"] = function_call.value();
    return j;
}

json ChatCompletionChunk::ChoiceDelta::to_json() const {
    json j = {
        {"index", index},
        {"delta", delta.to_json()}
    };
    if (finish_reason.has_value()) {
        j["finish_reason"] = finish_reason.value();
    } else {
        j["finish_reason"] = nullptr;
    }
    return j;
}

json ChatCompletionChunk::to_json() const {
    json choices_json = json::array();
    for (const auto& choice : choices) {
        choices_json.push_back(choice.to_json());
    }
    
    return {
        {"id", id},
        {"object", object},
        {"created", created},
        {"model", model},
        {"choices", choices_json}
    };
}

std::string ChatCompletionChunk::to_sse() const {
    return "data: " + to_json().dump() + "\n\n";
}

// ============================================================================
// ModelInfo Implementation
// ============================================================================

json ModelInfo::to_json() const {
    return {
        {"id", id},
        {"object", object},
        {"created", created},
        {"owned_by", owned_by}
    };
}

// ============================================================================
// OpenAICompat Implementation
// ============================================================================

OpenAICompat::OpenAICompat(std::shared_ptr<llm::LLMClient> llm_client)
    : llm_client_(llm_client)
{
    // Setup default model mappings
    // Map OpenAI model names to HunterClaw LLM provider/model pairs
    model_mapping_ = {
        {"gpt-4", {"openai", "gpt-4"}},
        {"gpt-4-turbo", {"openai", "gpt-4-turbo-preview"}},
        {"gpt-3.5-turbo", {"openai", "gpt-3.5-turbo"}},
        {"gpt-3.5-turbo-16k", {"openai", "gpt-3.5-turbo-16k"}},
        
        // Fallback: treat unknown models as hunterclaw default
        {"hunterclaw", {"default", "default"}},
        {"default", {"default", "default"}}
    };
}

core::Result<ChatCompletionResponse> OpenAICompat::create_chat_completion(
    const ChatCompletionRequest& request
) {
    try {
        // Convert messages
        auto llm_messages = convert_messages(request.messages);
        
        // Call LLM with simplified interface
        auto llm_result = llm_client_->generate_from_messages(llm_messages);
        
        if (llm_result.is_err()) {
            return core::Result<ChatCompletionResponse>::Err(llm_result.error);
        }
        
        // Convert response
        auto response = convert_response(llm_result.value, request);
        
        return core::Result<ChatCompletionResponse>::Ok(response);
        
    } catch (const std::exception& e) {
        return core::Result<ChatCompletionResponse>::Err(
            std::string("OpenAI compat error: ") + e.what()
        );
    }
}

core::Result<bool> OpenAICompat::create_chat_completion_stream(
    const ChatCompletionRequest& request,
    StreamCallback on_chunk,
    ErrorCallback on_error
) {
    try {
        // Convert messages
        auto llm_messages = convert_messages(request.messages);
        
        std::string completion_id = generate_completion_id();
        int64_t created = std::time(nullptr);
        
        // Stream from LLM using simple callback
        auto llm_result = llm_client_->generate_stream(
            llm_messages.empty() ? "" : llm_messages.back().content,
            [&](const std::string& chunk_text) {
                // Convert to OpenAI chunk format
                ChatCompletionChunk openai_chunk;
                openai_chunk.id = completion_id;
                openai_chunk.object = "chat.completion.chunk";
                openai_chunk.created = created;
                openai_chunk.model = request.model;
                
                ChatCompletionChunk::ChoiceDelta choice;
                choice.index = 0;
                choice.delta.content = chunk_text;
                
                openai_chunk.choices.push_back(choice);
                
                // Call user callback
                on_chunk(openai_chunk);
            }
        );
        
        if (llm_result.is_err()) {
            on_error(llm_result.error);
            return core::Result<bool>::Err(llm_result.error);
        }
        
        return core::Result<bool>::Ok(true);
        
    } catch (const std::exception& e) {
        std::string error = std::string("OpenAI streaming error: ") + e.what();
        on_error(error);
        return core::Result<bool>::Err(error);
    }
}

std::vector<ModelInfo> OpenAICompat::list_models() const {
    std::vector<ModelInfo> models;
    
    int64_t now = std::time(nullptr);
    
    // List all supported models
    for (const auto& [model_name, provider_info] : model_mapping_) {
        ModelInfo info;
        info.id = model_name;
        info.object = "model";
        info.created = now - 86400;  // Created 1 day ago (fake timestamp)
        info.owned_by = "hunterclaw";
        models.push_back(info);
    }
    
    return models;
}

// ========== Private Helper Methods ==========

std::vector<llm::LLMMessage> OpenAICompat::convert_messages(
    const std::vector<ChatMessage>& openai_messages
) {
    std::vector<llm::LLMMessage> llm_messages;
    
    for (const auto& openai_msg : openai_messages) {
        llm::LLMMessage llm_msg;
        llm_msg.role = string_to_role(openai_msg.role);
        llm_msg.content = openai_msg.content;
        
        if (openai_msg.name.has_value()) {
            llm_msg.name = openai_msg.name.value();
        }
        
        llm_messages.push_back(llm_msg);
    }
    
    return llm_messages;
}

ChatCompletionResponse OpenAICompat::convert_response(
    const std::string& llm_response,
    const ChatCompletionRequest& original_request
) {
    ChatCompletionResponse response;
    
    response.id = generate_completion_id();
    response.object = "chat.completion";
    response.created = std::time(nullptr);
    response.model = original_request.model;
    
    // Simple conversion: llm_response is just a string now
    ChatCompletionResponse::Choice choice;
    choice.index = 0;
    choice.message.role = "assistant";
    choice.message.content = llm_response;
    choice.finish_reason = "stop";
    
    response.choices.push_back(choice);
    
    // Simple token estimation
    int prompt_tokens = 0;
    for (const auto& msg : original_request.messages) {
        prompt_tokens += estimate_tokens(msg.content);
    }
    int completion_tokens = estimate_tokens(llm_response);
    
    response.usage.prompt_tokens = prompt_tokens;
    response.usage.completion_tokens = completion_tokens;
    response.usage.total_tokens = prompt_tokens + completion_tokens;
    
    return response;
}

std::string OpenAICompat::generate_completion_id() const {
    // Generate ID like "chatcmpl-abc123xyz"
    std::string uuid = core::utils::generate_uuid();
    return "chatcmpl-" + uuid.substr(0, 8);
}

int OpenAICompat::estimate_tokens(const std::string& text) const {
    // Rough estimate: ~4 characters per token for English
    return static_cast<int>(text.length() / 4);
}

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<OpenAICompat> create_openai_compat(
    std::shared_ptr<llm::LLMClient> llm_client
) {
    return std::make_shared<OpenAICompat>(llm_client);
}

} // namespace http
} // namespace gateway
} // namespace hunterclaw
