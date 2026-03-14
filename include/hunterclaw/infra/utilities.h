#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdint>

namespace hunterclaw {
namespace infra {

// ============================================================================
// Backoff & Retry Utilities
// ============================================================================

struct BackoffPolicy {
    int64_t initial_ms;    // Initial delay in milliseconds
    int64_t max_ms;        // Maximum delay in milliseconds
    double factor;         // Backoff factor (e.g., 2.0 for exponential)
    double jitter;         // Jitter factor (0.0 to 1.0)
};

// Compute backoff delay for given attempt
int64_t compute_backoff(const BackoffPolicy& policy, int attempt);

// Sleep with abort signal support
void sleep_with_abort(int64_t ms, std::function<bool()> is_aborted = nullptr);

// ============================================================================
// Timeout Utilities
// ============================================================================

template<typename T>
struct TimeoutResult {
    bool timed_out;
    std::optional<T> value;
    std::string error;
};

// Execute with timeout (callback-based)
template<typename T>
TimeoutResult<T> with_timeout(
    std::function<T()> fn,
    int64_t timeout_ms
);

// ============================================================================
// Safe JSON Utilities
// ============================================================================

struct JsonValue;  // Forward declaration for recursive type

struct JsonValue {
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };
    
    Type type;
    std::string string_value;
    double number_value;
    bool bool_value;
    std::vector<JsonValue> array_value;
    std::unordered_map<std::string, JsonValue> object_value;
    
    JsonValue() : type(Type::Null), number_value(0.0), bool_value(false) {}
};

// Safe JSON stringification (handles bigint, functions, errors, Uint8Array)
std::optional<std::string> safe_json_stringify(const JsonValue& value);

// Safe JSON parsing (never throws)
std::optional<JsonValue> safe_json_parse(const std::string& json);

// ============================================================================
// File Lock Utilities
// ============================================================================

struct FileLockHandle {
    std::string path;
    int fd;
    bool exclusive;
};

struct FileLockOptions {
    bool exclusive = true;      // Exclusive vs shared lock
    int64_t timeout_ms = 5000;  // Lock acquisition timeout
    bool create = false;        // Create file if doesn't exist
};

// Acquire file lock (returns handle or nullopt on failure)
std::optional<FileLockHandle> acquire_file_lock(
    const std::string& path,
    const FileLockOptions& options = {}
);

// Release file lock
void release_file_lock(FileLockHandle& handle);

// Execute with file lock (RAII-style)
template<typename T>
std::optional<T> with_file_lock(
    const std::string& path,
    std::function<T()> fn,
    const FileLockOptions& options = {}
);

// ============================================================================
// JSON File I/O Utilities
// ============================================================================

// Load JSON file (returns nullopt on error)
std::optional<JsonValue> load_json_file(const std::string& path);

// Save JSON file (with atomic write & chmod 600)
bool save_json_file(const std::string& path, const JsonValue& data);

// ============================================================================
// Path Guard Utilities
// ============================================================================

// Normalize Windows path for comparison (handle \\?\, UNC, etc.)
std::string normalize_windows_path_for_comparison(const std::string& input);

// Check if value is a Node.js error with code
bool is_node_error(const std::exception& e);
bool has_node_error_code(const std::exception& e, const std::string& code);

// Check if error is "not found" (ENOENT, ENOTDIR)
bool is_not_found_path_error(const std::exception& e);

// Check if error is symlink open error (ELOOP, EINVAL, ENOTSUP)
bool is_symlink_open_error(const std::exception& e);

// Check if target path is inside root (no ".." escapes)
bool is_path_inside(const std::string& root, const std::string& target);

// ============================================================================
// Safe File System Utilities (fs-safe)
// ============================================================================

enum class SafeOpenErrorCode {
    InvalidPath,
    NotFound,
    OutsideWorkspace,
    Symlink,
    NotFile,
    PathMismatch,
    TooLarge
};

class SafeOpenError : public std::exception {
public:
    SafeOpenErrorCode code;
    std::string message;
    
    SafeOpenError(SafeOpenErrorCode c, const std::string& msg)
        : code(c), message(msg) {}
    
    const char* what() const noexcept override { return message.c_str(); }
};

struct SafeOpenResult {
    int fd;
    std::string real_path;
    int64_t file_size;
    int64_t mtime_ms;
};

struct SafeLocalReadResult {
    std::vector<uint8_t> buffer;
    std::string real_path;
    int64_t file_size;
    int64_t mtime_ms;
};

struct SafeOpenOptions {
    bool reject_hardlinks = false;
    bool reject_symlinks = true;
    int64_t max_bytes = 100 * 1024 * 1024;  // 100MB default
    std::string workspace_root;
};

// Open verified local file (with safety checks)
SafeOpenResult open_verified_local_file(
    const std::string& path,
    const SafeOpenOptions& options = {}
);

// Read local file safely (with size limits & safety checks)
SafeLocalReadResult read_local_file_safe(
    const std::string& path,
    const SafeOpenOptions& options = {}
);

// Write local file safely (atomic write with rename)
bool write_local_file_safe(
    const std::string& path,
    const std::vector<uint8_t>& data,
    const SafeOpenOptions& options = {}
);

// ============================================================================
// Exec Approval Utilities
// ============================================================================

enum class ExecHost {
    Sandbox,
    Gateway,
    Node
};

enum class ExecSecurity {
    Deny,
    Allowlist,
    Full
};

enum class ExecAsk {
    Off,
    OnMiss,
    Always
};

// Normalize exec host value
std::optional<ExecHost> normalize_exec_host(const std::string& value);

// Normalize exec security value
std::optional<ExecSecurity> normalize_exec_security(const std::string& value);

// Normalize exec ask value
std::optional<ExecAsk> normalize_exec_ask(const std::string& value);

struct SystemRunApprovalBinding {
    std::vector<std::string> argv;
    std::optional<std::string> cwd;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<std::string> env_hash;
};

struct SystemRunApprovalFileOperand {
    int argv_index;
    std::string path;
    std::string sha256;
};

struct SystemRunApprovalPlan {
    std::vector<std::string> argv;
    std::optional<std::string> cwd;
    std::string command_text;
    std::optional<std::string> command_preview;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    std::optional<SystemRunApprovalFileOperand> mutable_file_operand;
};

struct ExecApprovalRequestPayload {
    std::string command;
    std::optional<std::string> command_preview;
    std::optional<std::vector<std::string>> command_argv;
    std::optional<std::vector<std::string>> env_keys;
    std::optional<SystemRunApprovalBinding> system_run_binding;
    std::optional<SystemRunApprovalPlan> system_run_plan;
    std::optional<std::string> cwd;
    std::optional<std::string> node_id;
    std::optional<std::string> host;
    std::optional<std::string> security;
    std::optional<std::string> ask;
    std::optional<std::string> agent_id;
    std::optional<std::string> resolved_path;
    std::optional<std::string> session_key;
    std::optional<std::string> turn_source_channel;
    std::optional<std::string> turn_source_to;
    std::optional<std::string> turn_source_account_id;
    std::optional<std::string> turn_source_thread_id;
};

struct ExecApprovalRequest {
    std::string id;
    ExecApprovalRequestPayload request;
    int64_t created_at_ms;
    int64_t expires_at_ms;
};

enum class ExecApprovalDecision {
    Approved,
    Denied,
    Expired,
    Cancelled
};

struct ExecApprovalResolved {
    std::string id;
    ExecApprovalDecision decision;
    std::optional<std::string> resolved_by;
    int64_t ts;
    std::optional<ExecApprovalRequestPayload> request;
};

struct ExecApprovalsDefaults {
    std::optional<ExecSecurity> security;
    std::optional<ExecAsk> ask;
    std::optional<ExecHost> host;
};

// Create exec approval request
ExecApprovalRequest create_exec_approval_request(
    const ExecApprovalRequestPayload& payload,
    int64_t ttl_ms = 60000
);

// Resolve exec approval request
ExecApprovalResolved resolve_exec_approval(
    const ExecApprovalRequest& request,
    ExecApprovalDecision decision,
    const std::string& resolved_by = ""
);

// Check if exec is allowed
bool is_exec_allowed(
    const std::string& command,
    const ExecApprovalsDefaults& defaults,
    const std::vector<std::string>& allowlist = {}
);

// ============================================================================
// Environment & Path Utilities
// ============================================================================

// Expand home prefix (~) in path
std::string expand_home_prefix(const std::string& path, const std::string& home = "");

// Get HOME directory (cross-platform)
std::string get_home_dir();

// Get PATH environment variable as vector
std::vector<std::string> get_path_env();

// Prepend to PATH environment variable
void prepend_to_path_env(const std::string& dir);

// Check if path is absolute (cross-platform)
bool is_absolute_path(const std::string& path);

// Check if path is Windows absolute path
bool is_windows_absolute_path(const std::string& path);

// Join paths (cross-platform)
std::string join_paths(const std::vector<std::string>& parts);

// Normalize path (resolve .., ., etc.)
std::string normalize_path(const std::string& path);

// Get relative path from base to target
std::string relative_path(const std::string& base, const std::string& target);

// ============================================================================
// Rate Limiting Utilities
// ============================================================================

struct FixedWindowRateLimit {
    int max_requests;          // Max requests per window
    int64_t window_ms;         // Window duration in milliseconds
    int64_t window_start_ms;   // Current window start time
    int current_count;         // Current request count in window
};

// Create rate limiter
FixedWindowRateLimit create_rate_limiter(int max_requests, int64_t window_ms);

// Check and consume rate limit
bool check_rate_limit(FixedWindowRateLimit& limiter);

// Reset rate limiter
void reset_rate_limiter(FixedWindowRateLimit& limiter);

// Get remaining capacity
int get_rate_limit_remaining(const FixedWindowRateLimit& limiter);

// ============================================================================
// Archive Utilities
// ============================================================================

struct ArchiveEntry {
    std::string path;
    std::vector<uint8_t> data;
    bool is_directory;
    int64_t mtime_ms;
};

// Create tar.gz archive from files
std::vector<uint8_t> create_tar_gz(const std::vector<ArchiveEntry>& entries);

// Extract tar.gz archive
std::vector<ArchiveEntry> extract_tar_gz(const std::vector<uint8_t>& data);

// Archive directory to tar.gz
std::vector<uint8_t> archive_directory(const std::string& dir_path);

// Extract tar.gz to directory
bool extract_to_directory(const std::vector<uint8_t>& data, const std::string& dest_dir);

// ============================================================================
// Miscellaneous Utilities
// ============================================================================

// Generate random UUID
std::string generate_uuid();

// Get current timestamp in milliseconds
int64_t current_timestamp_ms();

// Get current timestamp in ISO 8601 format
std::string current_timestamp_iso();

// Hash string to SHA256
std::string sha256_hash(const std::string& data);

// Hash bytes to SHA256
std::string sha256_hash_bytes(const std::vector<uint8_t>& data);

// Base64 encode
std::string base64_encode(const std::vector<uint8_t>& data);

// Base64 decode
std::vector<uint8_t> base64_decode(const std::string& encoded);

// Check if string is valid UTF-8
bool is_valid_utf8(const std::string& str);

// Trim whitespace from string
std::string trim(const std::string& str);

// Split string by delimiter
std::vector<std::string> split(const std::string& str, const std::string& delimiter);

// Join strings with delimiter
std::string join(const std::vector<std::string>& parts, const std::string& delimiter);

// Convert string to lowercase
std::string to_lowercase(const std::string& str);

// Convert string to uppercase
std::string to_uppercase(const std::string& str);

// Check if string starts with prefix
bool starts_with(const std::string& str, const std::string& prefix);

// Check if string ends with suffix
bool ends_with(const std::string& str, const std::string& suffix);

// Replace all occurrences in string
std::string replace_all(
    const std::string& str,
    const std::string& from,
    const std::string& to
);

}  // namespace infra
}  // namespace hunterclaw
