#ifndef HUNTERCLAW_UTILS_SYSTEM_H
#define HUNTERCLAW_UTILS_SYSTEM_H

#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace utils {

using json = nlohmann::json;

// ============================================================================
// 1. Delivery Context - Message delivery routing information
// ============================================================================

struct DeliveryContext {
    std::optional<std::string> channel;
    std::optional<std::string> to;
    std::optional<std::string> account_id;
    std::optional<std::string> thread_id;  // Can be string or number
    
    json to_json() const;
    static std::optional<DeliveryContext> from_json(const json& j);
};

struct DeliveryContextSessionSource {
    std::optional<std::string> channel;
    std::optional<std::string> last_channel;
    std::optional<std::string> last_to;
    std::optional<std::string> last_account_id;
    std::optional<std::string> last_thread_id;
    std::optional<DeliveryContext> delivery_context;
};

class DeliveryContextUtils {
public:
    static std::optional<DeliveryContext> normalize(const DeliveryContext& context);
    static DeliveryContext merge(const std::optional<DeliveryContext>& base,
                                  const std::optional<DeliveryContext>& override);
    static DeliveryContextSessionSource normalize_session_delivery_fields(
        const DeliveryContextSessionSource& source);
    static std::optional<DeliveryContext> from_session(
        const DeliveryContextSessionSource& entry);
};

// ============================================================================
// 2. API Key Masking - Secure API key display
// ============================================================================

class ApiKeyMasker {
public:
    // Mask API key for safe display
    static std::string mask_api_key(const std::string& value);
    
    // Mask multiple keys in text
    static std::string mask_keys_in_text(const std::string& text,
                                         const std::vector<std::string>& keys);
    
    // Detect and mask common API key patterns
    static std::string auto_mask_text(const std::string& text);
};

// ============================================================================
// 3. Safe JSON - JSON stringify with error handling
// ============================================================================

class SafeJSON {
public:
    // Safe JSON stringify (handles BigInt, Functions, Errors, Uint8Array)
    static std::optional<std::string> stringify(const json& value);
    
    // Safe JSON parse
    static std::optional<json> parse(const std::string& str);
    
    // Stringify with custom replacer
    using Replacer = std::function<json(const std::string& key, const json& value)>;
    static std::optional<std::string> stringify_with_replacer(const json& value,
                                                               Replacer replacer);
    
    // Safe deep clone
    static std::optional<json> deep_clone(const json& value);
};

// ============================================================================
// 4. Directive Tags - Parse inline directive tags [[tag]]
// ============================================================================

struct InlineDirectiveParseResult {
    std::string text;
    bool audio_as_voice = false;
    std::optional<std::string> reply_to_id;
    std::optional<std::string> reply_to_explicit_id;
    bool reply_to_current = false;
    bool has_audio_tag = false;
    bool has_reply_tag = false;
};

struct InlineDirectiveParseOptions {
    std::optional<std::string> current_message_id;
    bool strip_audio_tag = true;
    bool strip_reply_tags = true;
};

struct StripInlineDirectiveTagsResult {
    std::string text;
    bool changed = false;
};

class DirectiveTags {
public:
    // Parse inline directives from text
    static InlineDirectiveParseResult parse_inline_directives(
        const std::string& text,
        const InlineDirectiveParseOptions& options = {});
    
    // Strip directive tags for display
    static StripInlineDirectiveTagsResult strip_inline_directive_tags_for_display(
        const std::string& text);
    
    // Strip directive tags from message
    static json strip_inline_directive_tags_from_message_for_display(const json& message);
    
    // Check if text contains directives
    static bool contains_directives(const std::string& text);
};

// ============================================================================
// 5. Provider Utils - Provider-specific capabilities
// ============================================================================

class ProviderUtils {
public:
    // Check if provider requires reasoning tags (<think>, <final>)
    static bool is_reasoning_tag_provider(const std::string& provider);
    
    // Check if provider supports streaming
    static bool supports_streaming(const std::string& provider);
    
    // Check if provider supports function calling
    static bool supports_function_calling(const std::string& provider);
    
    // Check if provider supports vision
    static bool supports_vision(const std::string& provider);
    
    // Get provider capabilities
    struct ProviderCapabilities {
        bool streaming = false;
        bool function_calling = false;
        bool vision = false;
        bool reasoning_tags = false;
        std::optional<int> max_tokens;
        std::optional<int> context_window;
    };
    static ProviderCapabilities get_capabilities(const std::string& provider);
};

// ============================================================================
// 6. Message Channel - Channel normalization
// ============================================================================

class MessageChannelUtils {
public:
    // Normalize message channel name
    static std::optional<std::string> normalize_message_channel(const std::string& channel);
    
    // Get channel type
    enum class ChannelType {
        Discord,
        Slack,
        Telegram,
        WhatsApp,
        Signal,
        iMessage,
        Web,
        Unknown
    };
    static ChannelType get_channel_type(const std::string& channel);
    
    // Validate channel name
    static bool is_valid_channel(const std::string& channel);
};

// ============================================================================
// 7. Account ID - Account ID normalization
// ============================================================================

class AccountIdUtils {
public:
    // Normalize account ID
    static std::optional<std::string> normalize_account_id(const std::string& account_id);
    
    // Validate account ID format
    static bool is_valid_account_id(const std::string& account_id);
    
    // Extract account ID from context
    static std::optional<std::string> extract_from_context(const json& context);
};

// ============================================================================
// 8. Boolean Utils - Boolean parsing
// ============================================================================

class BooleanUtils {
public:
    // Parse boolean from string
    static std::optional<bool> parse_boolean(const std::string& value);
    
    // Parse boolean with default
    static bool parse_boolean_or(const std::string& value, bool default_val);
    
    // Is truthy value
    static bool is_truthy(const std::string& value);
    
    // Is falsy value
    static bool is_falsy(const std::string& value);
};

// ============================================================================
// 9. Chunk Items - Batch processing utilities
// ============================================================================

class ChunkUtils {
public:
    // Chunk items into batches
    template<typename T>
    static std::vector<std::vector<T>> chunk_items(const std::vector<T>& items, size_t chunk_size);
    
    // Chunk items by size (bytes)
    static std::vector<std::vector<std::string>> chunk_by_size(
        const std::vector<std::string>& items,
        size_t max_bytes);
    
    // Chunk items by predicate
    template<typename T>
    static std::vector<std::vector<T>> chunk_by_predicate(
        const std::vector<T>& items,
        std::function<bool(const std::vector<T>&, const T&)> predicate);
};

// ============================================================================
// 10. Fetch Timeout - HTTP timeout utilities
// ============================================================================

class FetchTimeoutUtils {
public:
    // Create fetch with timeout
    struct FetchOptions {
        int timeout_ms = 30000;
        std::unordered_map<std::string, std::string> headers;
        std::optional<std::string> method;
        std::optional<std::string> body;
    };
    
    struct FetchResponse {
        int status_code = 0;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
        bool ok = false;
    };
    
    static std::optional<FetchResponse> fetch_with_timeout(
        const std::string& url,
        const FetchOptions& options = {30000, {}, std::nullopt, std::nullopt});
};

// ============================================================================
// 11. Normalize Secret Input - Secret normalization
// ============================================================================

class NormalizeSecretInput {
public:
    // Normalize secret string
    static std::string normalize_secret_string(const std::string& value);
    
    // Normalize secret value (can be string or object)
    static json normalize_secret_value(const json& value);
    
    // Validate secret format
    static bool is_valid_secret(const json& value);
};

// ============================================================================
// 12. Queue Helpers - Queue management utilities
// ============================================================================

class QueueHelpers {
public:
    // Run with concurrency limit
    template<typename T, typename Func>
    static std::vector<T> run_with_concurrency(
        const std::vector<std::function<T()>>& tasks,
        size_t concurrency);
    
    // Process queue items with rate limiting
    template<typename T>
    static void process_with_rate_limit(
        const std::vector<T>& items,
        std::function<void(const T&)> processor,
        int items_per_second);
};

// ============================================================================
// 13. Reaction Level - Reaction level parsing
// ============================================================================

enum class ReactionLevel {
    None,
    Low,
    Medium,
    High,
    Critical
};

class ReactionLevelUtils {
public:
    // Parse reaction level from string
    static std::optional<ReactionLevel> parse_reaction_level(const std::string& value);
    
    // Convert reaction level to string
    static std::string to_string(ReactionLevel level);
    
    // Compare reaction levels
    static bool is_higher_or_equal(ReactionLevel a, ReactionLevel b);
};

// ============================================================================
// 14. Shell Argv - Shell argument parsing
// ============================================================================

class ShellArgvParser {
public:
    // Parse shell command line into argv
    static std::vector<std::string> parse_shell_argv(const std::string& command);
    
    // Quote shell argument
    static std::string quote_shell_arg(const std::string& arg);
    
    // Join argv into command string
    static std::string join_shell_argv(const std::vector<std::string>& argv);
};

// ============================================================================
// 15. Transcript Tools - Transcript utilities
// ============================================================================

class TranscriptTools {
public:
    // Format transcript for display
    static std::string format_transcript(const std::vector<json>& messages);
    
    // Extract text from transcript
    static std::string extract_text(const std::vector<json>& messages);
    
    // Filter transcript by role
    static std::vector<json> filter_by_role(const std::vector<json>& messages,
                                            const std::string& role);
    
    // Truncate transcript to max tokens
    static std::vector<json> truncate_to_max_tokens(const std::vector<json>& messages,
                                                     int max_tokens);
};

// ============================================================================
// 16. Usage Format - Token usage formatting
// ============================================================================

class UsageFormat {
public:
    struct TokenUsage {
        int input_tokens = 0;
        int output_tokens = 0;
        int cache_creation_tokens = 0;
        int cache_read_tokens = 0;
    };
    
    // Format token usage for display
    static std::string format_usage(const TokenUsage& usage);
    
    // Calculate total tokens
    static int total_tokens(const TokenUsage& usage);
    
    // Calculate cost estimate (USD)
    static double estimate_cost(const TokenUsage& usage,
                               const std::string& model);
};

// ============================================================================
// 17. With Timeout - Generic timeout utilities
// ============================================================================

class TimeoutUtils {
public:
    // Run function with timeout
    template<typename T>
    static std::optional<T> with_timeout(std::function<T()> func, int timeout_ms);
    
    // Run async function with timeout
    template<typename T>
    static std::optional<T> with_timeout_async(std::function<T()> func, int timeout_ms);
    
    // Sleep with cancellation
    static void sleep_ms(int ms);
};

// ============================================================================
// Utils System Manager
// ============================================================================

class UtilsSystem {
public:
    UtilsSystem();
    ~UtilsSystem();
    
    // Initialize utils system
    bool initialize();
    void shutdown();
    
    // Get subsystems
    DeliveryContextUtils& delivery_context() { return delivery_context_; }
    ApiKeyMasker& api_key_masker() { return api_key_masker_; }
    SafeJSON& safe_json() { return safe_json_; }
    DirectiveTags& directive_tags() { return directive_tags_; }
    ProviderUtils& provider_utils() { return provider_utils_; }
    MessageChannelUtils& message_channel() { return message_channel_; }
    AccountIdUtils& account_id() { return account_id_; }
    BooleanUtils& boolean_utils() { return boolean_utils_; }
    ChunkUtils& chunk_utils() { return chunk_utils_; }
    FetchTimeoutUtils& fetch_timeout() { return fetch_timeout_; }
    NormalizeSecretInput& normalize_secret() { return normalize_secret_; }
    QueueHelpers& queue_helpers() { return queue_helpers_; }
    ReactionLevelUtils& reaction_level() { return reaction_level_; }
    ShellArgvParser& shell_argv() { return shell_argv_; }
    TranscriptTools& transcript_tools() { return transcript_tools_; }
    UsageFormat& usage_format() { return usage_format_; }
    TimeoutUtils& timeout_utils() { return timeout_utils_; }

private:
    DeliveryContextUtils delivery_context_;
    ApiKeyMasker api_key_masker_;
    SafeJSON safe_json_;
    DirectiveTags directive_tags_;
    ProviderUtils provider_utils_;
    MessageChannelUtils message_channel_;
    AccountIdUtils account_id_;
    BooleanUtils boolean_utils_;
    ChunkUtils chunk_utils_;
    FetchTimeoutUtils fetch_timeout_;
    NormalizeSecretInput normalize_secret_;
    QueueHelpers queue_helpers_;
    ReactionLevelUtils reaction_level_;
    ShellArgvParser shell_argv_;
    TranscriptTools transcript_tools_;
    UsageFormat usage_format_;
    TimeoutUtils timeout_utils_;
    
    bool initialized_ = false;
};

} // namespace utils
} // namespace hunterclaw

#endif // HUNTERCLAW_UTILS_SYSTEM_H
