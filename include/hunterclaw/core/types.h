#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <optional>
#include <variant>
#include <functional>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace core {

// Forward declarations
template<typename T> struct Result;

// ============================================================================
// Basic Types
// ============================================================================

using Timestamp = std::chrono::system_clock::time_point;
using Duration = std::chrono::milliseconds;
using ByteArray = std::vector<uint8_t>;

// Simple JSON-like value type (for basic use cases)
class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    
    JsonValue() : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), bool_val_(b) {}
    JsonValue(int64_t n) : type_(Type::Number), num_val_(static_cast<double>(n)) {}
    JsonValue(double n) : type_(Type::Number), num_val_(n) {}
    JsonValue(const std::string& s) : type_(Type::String), str_val_(s) {}
    JsonValue(const char* s) : type_(Type::String), str_val_(s) {}
    
    // Constructor from nlohmann::json
    JsonValue(const nlohmann::json& j) : type_(Type::Null) {
        if (j.is_null()) {
            type_ = Type::Null;
        } else if (j.is_boolean()) {
            type_ = Type::Bool;
            bool_val_ = j.get<bool>();
        } else if (j.is_number()) {
            type_ = Type::Number;
            num_val_ = j.get<double>();
        } else if (j.is_string()) {
            type_ = Type::String;
            str_val_ = j.get<std::string>();
        } else if (j.is_array()) {
            type_ = Type::Array;
            for (const auto& item : j) {
                arr_val_.push_back(std::make_shared<JsonValue>(item));
            }
        } else if (j.is_object()) {
            type_ = Type::Object;
            for (auto it = j.begin(); it != j.end(); ++it) {
                obj_val_[it.key()] = std::make_shared<JsonValue>(it.value());
            }
        }
    }
    
    static JsonValue null() { return JsonValue(); }
    static JsonValue array() { 
        JsonValue v; 
        v.type_ = Type::Array;
        return v;
    }
    static JsonValue object() { 
        JsonValue v; 
        v.type_ = Type::Object;
        return v;
    }
    static JsonValue object(std::initializer_list<std::pair<std::string, JsonValue>> items) {
        JsonValue v;
        v.type_ = Type::Object;
        for (const auto& item : items) {
            v.obj_val_[item.first] = std::make_shared<JsonValue>(item.second);
        }
        return v;
    }
    
    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }
    
    bool as_bool() const { return bool_val_; }
    double as_number() const { return num_val_; }
    std::string as_string() const { return str_val_; }
    
    JsonValue& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
        }
        if (!obj_val_[key]) {
            obj_val_[key] = std::make_shared<JsonValue>();
        }
        return *obj_val_[key];
    }
    
    const JsonValue& operator[](const std::string& key) const {
        if (type_ != Type::Object) {
            throw std::runtime_error("JsonValue is not an object");
        }
        auto it = obj_val_.find(key);
        if (it == obj_val_.end()) {
            throw std::runtime_error("Key not found: " + key);
        }
        return *it->second;
    }
    
    // Alias for contains (for compatibility)
    bool has_key(const std::string& key) const {
        return contains(key);
    }
    
    // Alternative name for contains
    bool has(const std::string& key) const {
        return contains(key);
    }
    
    // Get array size
    size_t size() const {
        if (type_ == Type::Array) {
            return arr_val_.size();
        } else if (type_ == Type::Object) {
            return obj_val_.size();
        }
        return 0;
    }
    
    // Array operator[]
    JsonValue& operator[](size_t index) {
        if (type_ != Type::Array) {
            throw std::runtime_error("JsonValue is not an array");
        }
        if (index >= arr_val_.size()) {
            throw std::runtime_error("Array index out of bounds");
        }
        return *arr_val_[index];
    }
    
    const JsonValue& operator[](size_t index) const {
        if (type_ != Type::Array) {
            throw std::runtime_error("JsonValue is not an array");
        }
        if (index >= arr_val_.size()) {
            throw std::runtime_error("Array index out of bounds");
        }
        return *arr_val_[index];
    }
    
    // Get value with default fallback
    template<typename T>
    T get(const std::string& key, const T& default_value) const {
        if (type_ != Type::Object) return default_value;
        auto it = obj_val_.find(key);
        if (it == obj_val_.end()) return default_value;
        
        if constexpr (std::is_same_v<T, std::string>) {
            return it->second->is_string() ? it->second->as_string() : default_value;
        } else if constexpr (std::is_same_v<T, bool>) {
            return it->second->is_bool() ? it->second->as_bool() : default_value;
        } else if constexpr (std::is_same_v<T, double>) {
            return it->second->is_number() ? it->second->as_number() : default_value;
        } else if constexpr (std::is_same_v<T, int>) {
            return it->second->is_number() ? static_cast<int>(it->second->as_number()) : default_value;
        }
        return default_value;
    }
    
    void push_back(const JsonValue& val) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
        }
        arr_val_.push_back(std::make_shared<JsonValue>(val));
    }
    
    // Convert to nlohmann::json
    nlohmann::json to_json() const {
        switch (type_) {
            case Type::Null:
                return nullptr;
            case Type::Bool:
                return bool_val_;
            case Type::Number:
                return num_val_;
            case Type::String:
                return str_val_;
            case Type::Array: {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& item : arr_val_) {
                    arr.push_back(item->to_json());
                }
                return arr;
            }
            case Type::Object: {
                nlohmann::json obj = nlohmann::json::object();
                for (const auto& [key, value] : obj_val_) {
                    obj[key] = value->to_json();
                }
                return obj;
            }
        }
        return nullptr;
    }
    
    // Convert to JSON string
    std::string to_string() const {
        return to_json().dump();
    }
    
    // Parse JSON from string (implemented after Result definition)
    static Result<JsonValue> parse(const std::string& json_str);
    
    // Check if object contains key
    bool contains(const std::string& key) const {
        if (type_ != Type::Object) return false;
        return obj_val_.find(key) != obj_val_.end();
    }
    
    // Get value with type checking
    template<typename T>
    T get() const {
        if constexpr (std::is_same_v<T, bool>) {
            return as_bool();
        } else if constexpr (std::is_same_v<T, double>) {
            return as_number();
        } else if constexpr (std::is_same_v<T, int>) {
            return static_cast<int>(as_number());
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return static_cast<int64_t>(as_number());
        } else if constexpr (std::is_same_v<T, std::string>) {
            return as_string();
        }
        throw std::runtime_error("Unsupported type for JsonValue::get()");
    }
    
    // Iterator support for objects
    auto begin() const { return obj_val_.begin(); }
    auto end() const { return obj_val_.end(); }
    auto begin() { return obj_val_.begin(); }
    auto end() { return obj_val_.end(); }
    
    // Get const reference to object value
    const JsonValue& at(const std::string& key) const {
        if (type_ != Type::Object) {
            throw std::runtime_error("JsonValue is not an object");
        }
        auto it = obj_val_.find(key);
        if (it == obj_val_.end()) {
            throw std::runtime_error("Key not found: " + key);
        }
        return *it->second;
    }

private:
    Type type_ = Type::Null;
    bool bool_val_ = false;
    double num_val_ = 0.0;
    std::string str_val_;
    std::vector<std::shared_ptr<JsonValue>> arr_val_;
    std::map<std::string, std::shared_ptr<JsonValue>> obj_val_;
};

// ============================================================================
// Result Types (Rust-style error handling)
// ============================================================================

template<typename T>
struct Result {
    bool ok;
    T value;
    std::string error;
    
    static Result<T> Ok(T val) {
        return Result<T>{true, std::move(val), ""};
    }
    
    static Result<T> Err(std::string err) {
        return Result<T>{false, T{}, std::move(err)};
    }
    
    bool is_ok() const { return ok; }
    bool is_err() const { return !ok; }
    
    T unwrap() const {
        if (!ok) {
            throw std::runtime_error("Result::unwrap() called on Err: " + error);
        }
        return value;
    }
    
    T unwrap_or(T default_val) const {
        return ok ? value : default_val;
    }
};

// Specialization for Result<void>
template<>
struct Result<void> {
    bool ok;
    std::string error;
    
    static Result<void> Ok() {
        return Result<void>{true, ""};
    }
    
    static Result<void> Err(std::string err) {
        return Result<void>{false, std::move(err)};
    }
    
    bool is_ok() const { return ok; }
    bool is_err() const { return !ok; }
    
    void unwrap() const {
        if (!ok) {
            throw std::runtime_error("Result::unwrap() called on Err: " + error);
        }
    }
};

// JsonValue::parse implementation (must be after Result definition)
inline Result<JsonValue> JsonValue::parse(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        return Result<JsonValue>::Ok(JsonValue(j));
    } catch (const std::exception& e) {
        return Result<JsonValue>::Err(std::string("JSON parse error: ") + e.what());
    }
}

// ============================================================================
// Configuration Types
// ============================================================================

// OpenClaw-compatible Gateway Configuration
struct GatewayConfig {
    std::string host = "127.0.0.1";  // Default to localhost (OpenClaw default)
    uint16_t port = 18789;            // OpenClaw default port
    bool tls_enabled = false;
    std::string tls_cert_path;
    std::string tls_key_path;
    
    // 🎯 100% 对齐 OpenClaw: Gateway Authentication
    struct Auth {
        bool enabled = false;                  // OpenClaw: gateway.auth.enabled
        std::string mode = "none";             // OpenClaw: gateway.auth.mode ("none", "bearer", "device", "challenge", "trusted-proxy")
        std::string bearer_token;              // OpenClaw: gateway.auth.bearerToken
        std::string password;                  // OpenClaw: gateway.auth.password (legacy)
        std::vector<std::string> scopes;       // OpenClaw: default scopes for authenticated clients
        bool allow_tailscale = false;          // OpenClaw: gateway.auth.allowTailscale (future)
        
        // 🎯 新增: Trusted Proxy 配置 (对齐 OpenClaw)
        struct TrustedProxy {
            std::string user_header = "x-forwarded-user";  // OpenClaw: gateway.auth.trustedProxy.userHeader
            std::vector<std::string> required_headers;      // OpenClaw: gateway.auth.trustedProxy.requiredHeaders
            std::vector<std::string> allow_users;           // OpenClaw: gateway.auth.trustedProxy.allowUsers
        } trusted_proxy;
        
        // 🎯 新增: 速率限制配置 (对齐 OpenClaw)
        struct RateLimit {
            int max_attempts = 10;              // OpenClaw: gateway.auth.rateLimit.maxAttempts
            int64_t window_ms = 60000;          // OpenClaw: gateway.auth.rateLimit.windowMs (1 min)
            int64_t lockout_ms = 300000;        // OpenClaw: gateway.auth.rateLimit.lockoutMs (5 min)
            bool exempt_loopback = true;        // OpenClaw: gateway.auth.rateLimit.exemptLoopback
        } rate_limit;
    } auth;
    
    // 🎯 新增: 受信任代理 IP 列表 (对齐 OpenClaw)
    std::vector<std::string> trusted_proxies;  // OpenClaw: gateway.trustedProxies
    
    size_t max_connections = 10000;
    size_t max_payload_bytes = 25 * 1024 * 1024;  // 25MB
    Duration heartbeat_interval{30000};  // 30s
    
    // OpenClaw specific
    bool verbose = false;                 // Verbose logging
    bool allow_remote = false;            // Allow non-localhost connections
};

struct PluginConfig {
    std::string id;
    std::string name;
    std::string version;
    bool enabled = true;
    std::map<std::string, std::string> env;
    std::map<std::string, JsonValue> config;
};

struct SkillConfig {
    struct Limits {
        size_t max_candidates_per_root = 300;
        size_t max_skills_loaded_per_source = 200;
        size_t max_skills_in_prompt = 150;
        size_t max_skills_prompt_chars = 30000;
        size_t max_skill_file_bytes = 256000;
    } limits;
    
    struct Load {
        std::vector<std::string> extra_dirs;
        bool watch = true;  // 默认启用热重载 (100% 对齐 OpenClaw)
        int watch_debounce_ms = 250;  // 防抖延迟 (毫秒)
    } load;
    
    struct Install {
        bool prefer_brew = true;
        std::string node_manager = "pnpm";  // npm, pnpm, yarn, bun
    } install;
    
    std::vector<std::string> allow_bundled;
    std::map<std::string, PluginConfig> entries;
};

// Simplified agent config for Config struct (to avoid circular dependency)
struct AgentConfigData {
    std::string id;
    bool is_default = false;
    std::string name;
    std::string workspace;
    std::string agent_dir;
    std::vector<std::string> skills;
    JsonValue model;  // Model configuration as JsonValue
    JsonValue extra;  // Other configurations as JsonValue
};

struct Config {
    GatewayConfig gateway;
    SkillConfig skills;
    std::map<std::string, PluginConfig> plugins;
    std::vector<AgentConfigData> agents;  // Agent configurations
    JsonValue agents_json;  // JSON representation of agents config for flexible access
    
    std::string workspace_dir;
    std::string config_dir;  // ~/.hunterclaw
};

// ============================================================================
// Session Types
// ============================================================================

struct SessionKey {
    std::string channel;      // telegram, discord, slack
    std::string account_id;   // default, or specific account
    std::string target_id;    // user/channel/group ID
    
    // Device authentication fields
    std::string device_id;    // Unique device identifier
    std::string device_token; // Device authentication token
    
    std::string to_string() const {
        return channel + ":" + account_id + ":" + target_id;
    }
    
    static SessionKey from_string(const std::string& str);
};

struct SessionInfo {
    SessionKey key;
    std::string session_id;  // UUID
    Timestamp created_at;
    Timestamp last_active_at;
    std::map<std::string, std::string> metadata;
};

// ============================================================================
// Message Types
// ============================================================================

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct Message {
    std::string id;
    MessageRole role;
    std::string content;
    Timestamp timestamp;
    std::map<std::string, JsonValue> metadata;
};

// ============================================================================
// Protocol Types (WebSocket frames)
// ============================================================================

enum class FrameType {
    Request,    // req
    Response,   // res
    Event       // event
};

struct RequestFrame {
    FrameType type = FrameType::Request;
    std::string id;      // UUID
    std::string method;  // RPC method name
    JsonValue params;
};

struct ResponseFrame {
    FrameType type = FrameType::Response;
    std::string id;      // Request ID
    bool ok;
    JsonValue payload;
    std::string error;
};

struct EventFrame {
    FrameType type = FrameType::Event;
    std::string event;   // Event name
    uint64_t seq = 0;    // Sequence number
    JsonValue payload;
};

// ============================================================================
// Plugin Types
// ============================================================================

enum class PluginKind {
    Native,    // .so/.dll
    Script,    // Python/Lua
    Wasm       // WebAssembly
};

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    PluginKind kind = PluginKind::Native;
    
    std::vector<std::string> channels;   // Provided channels
    std::vector<std::string> providers;  // LLM providers
    std::vector<std::string> skills;     // Skill paths
    
    std::map<std::string, JsonValue> config_schema;
    std::vector<std::string> dependencies;
};

// ============================================================================
// Callback Types
// ============================================================================

using MessageCallback = std::function<void(const Message&)>;
using ErrorCallback = std::function<void(const std::string&)>;
using EventCallback = std::function<void(const std::string& event, const JsonValue& data)>;

// ============================================================================
// Config Functions
// ============================================================================

/// Load configuration from file
Result<Config> load_config(const std::string& config_path);

/// Save configuration to file
Result<bool> save_config(const std::string& config_path, const Config& config);

/// Create default configuration
Config create_default_config();

} // namespace core
} // namespace hunterclaw
