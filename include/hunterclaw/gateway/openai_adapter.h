#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/llm/llm_client.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hunterclaw {
namespace gateway {

// ============================================================================
// OpenAI API Request/Response Types (100% compatible)
// ============================================================================

/// Chat Message
struct ChatMessage {
    std::string role;       // system, user, assistant, function
    std::string content;
    std::optional<std::string> name;
    
    nlohmann::json to_json() const;
    static ChatMessage from_json(const nlohmann::json& j);
};

/// Function Call Definition
struct FunctionDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameters;
    
    nlohmann::json to_json() const;
    static FunctionDefinition from_json(const nlohmann::json& j);
};

/// Chat Completion Request
struct ChatCompletionRequest {
    std::string model;                    // e.g., "gpt-4", "gpt-3.5-turbo"
    std::vector<ChatMessage> messages;
    
    // Optional parameters
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> max_tokens;
    std::optional<bool> stream;
    std::optional<std::vector<std::string>> stop;
    std::optional<double> presence_penalty;
    std::optional<double> frequency_penalty;
    std::optional<std::string> user;
    std::optional<int> n;                 // Number of completions
    
    // Function calling
    std::optional<std::vector<FunctionDefinition>> functions;
    std::optional<nlohmann::json> function_call;  // "auto", "none", or {"name": "..."}
    
    static ChatCompletionRequest from_json(const nlohmann::json& j);
};

/// Chat Completion Response (non-streaming)
struct ChatCompletionResponse {
    std::string id;                       // e.g., "chatcmpl-abc123"
    std::string object;                   // "chat.completion"
    int64_t created;                      // Unix timestamp
    std::string model;
    
    struct Choice {
        int index;
        ChatMessage message;
        std::optional<std::string> finish_reason;  // "stop", "length", "function_call"
        
        nlohmann::json to_json() const;
    };
    
    std::vector<Choice> choices;
    
    struct Usage {
        int prompt_tokens;
        int completion_tokens;
        int total_tokens;
        
        nlohmann::json to_json() const;
    };
    
    Usage usage;
    
    nlohmann::json to_json() const;
};

/// Chat Completion Chunk (streaming)
struct ChatCompletionChunk {
    std::string id;
    std::string object;                   // "chat.completion.chunk"
    int64_t created;
    std::string model;
    
    struct ChoiceDelta {
        int index;
        
        struct Delta {
            std::optional<std::string> role;
            std::optional<std::string> content;
            std::optional<nlohmann::json> function_call;
            
            nlohmann::json to_json() const;
        };
        
        Delta delta;
        std::optional<std::string> finish_reason;
        
        nlohmann::json to_json() const;
    };
    
    std::vector<ChoiceDelta> choices;
    
    nlohmann::json to_json() const;
    std::string to_sse() const;  // Convert to SSE format: "data: {json}\n\n"
};

/// Model Info
struct ModelInfo {
    std::string id;
    std::string object;                   // "model"
    int64_t created;
    std::string owned_by;
    
    nlohmann::json to_json() const;
};

// ============================================================================
// OpenAI API Adapter
// ============================================================================

class OpenAIAdapter {
public:
    using StreamCallback = std::function<void(const ChatCompletionChunk&)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    explicit OpenAIAdapter(std::shared_ptr<llm::LLMClient> llm_client);
    ~OpenAIAdapter() = default;
    
    /// Handle non-streaming chat completion
    core::Result<ChatCompletionResponse> create_chat_completion(
        const ChatCompletionRequest& request
    );
    
    /// Handle streaming chat completion
    core::Result<bool> create_chat_completion_stream(
        const ChatCompletionRequest& request,
        StreamCallback on_chunk,
        ErrorCallback on_error
    );
    
    /// List available models
    std::vector<ModelInfo> list_models() const;
    
private:
    /// Convert OpenAI messages to HunterClaw LLM format
    std::vector<llm::LLMMessage> convert_messages(
        const std::vector<ChatMessage>& openai_messages
    );
    
    /// Convert HunterClaw LLM response to OpenAI format
    ChatCompletionResponse convert_response(
        const std::string& llm_response,
        const ChatCompletionRequest& original_request
    );
    
    /// Generate completion ID
    std::string generate_completion_id() const;
    
    /// Estimate token count (simple heuristic)
    int estimate_tokens(const std::string& text) const;
    
    std::shared_ptr<llm::LLMClient> llm_client_;
    
    // Model mapping: OpenAI model name -> HunterClaw LLM provider/model
    std::map<std::string, std::pair<std::string, std::string>> model_mapping_;
};

// ============================================================================
// Factory Function
// ============================================================================

std::shared_ptr<OpenAIAdapter> create_openai_adapter(
    std::shared_ptr<llm::LLMClient> llm_client
);

} // namespace gateway
} // namespace hunterclaw
