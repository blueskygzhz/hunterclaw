#include "hunterclaw/infra/utilities.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <iomanip>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

namespace hunterclaw {
namespace infra {

// ============================================================================
// Backoff & Retry Utilities
// ============================================================================

int64_t compute_backoff(const BackoffPolicy& policy, int attempt) {
    double base = policy.initial_ms * std::pow(policy.factor, std::max(attempt - 1, 0));
    
    // Add jitter
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);
    double jitter = base * policy.jitter * dis(gen);
    
    return static_cast<int64_t>(std::min(
        static_cast<double>(policy.max_ms),
        std::round(base + jitter)
    ));
}

void sleep_with_abort(int64_t ms, std::function<bool()> is_aborted) {
    if (ms <= 0) return;
    
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::milliseconds(ms);
    
    while (std::chrono::steady_clock::now() < end) {
        if (is_aborted && is_aborted()) {
            throw std::runtime_error("aborted");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ============================================================================
// Safe JSON Utilities
// ============================================================================

std::optional<std::string> safe_json_stringify(const JsonValue& value) {
    try {
        std::ostringstream oss;
        
        switch (value.type) {
            case JsonValue::Type::Null:
                oss << "null";
                break;
                
            case JsonValue::Type::Boolean:
                oss << (value.bool_value ? "true" : "false");
                break;
                
            case JsonValue::Type::Number:
                oss << value.number_value;
                break;
                
            case JsonValue::Type::String:
                oss << "\"";
                for (char c : value.string_value) {
                    switch (c) {
                        case '"': oss << "\\\""; break;
                        case '\\': oss << "\\\\"; break;
                        case '\b': oss << "\\b"; break;
                        case '\f': oss << "\\f"; break;
                        case '\n': oss << "\\n"; break;
                        case '\r': oss << "\\r"; break;
                        case '\t': oss << "\\t"; break;
                        default: oss << c;
                    }
                }
                oss << "\"";
                break;
                
            case JsonValue::Type::Array:
                oss << "[";
                for (size_t i = 0; i < value.array_value.size(); ++i) {
                    if (i > 0) oss << ",";
                    if (!value.array_value[i]) {
                        oss << "null";
                        continue;
                    }
                    auto elem_str = safe_json_stringify(*value.array_value[i]);
                    if (!elem_str) return std::nullopt;
                    oss << *elem_str;
                }
                oss << "]";
                break;
                
            case JsonValue::Type::Object:
                oss << "{";
                bool first = true;
                for (const auto& [key, val] : value.object_value) {
                    if (!first) oss << ",";
                    first = false;
                    oss << "\"" << key << "\":";
                    if (!val) {
                        oss << "null";
                        continue;
                    }
                    auto val_str = safe_json_stringify(*val);
                    if (!val_str) return std::nullopt;
                    oss << *val_str;
                }
                oss << "}";
                break;
        }
        
        return oss.str();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<JsonValue> safe_json_parse(const std::string& json) {
    // Simplified JSON parser (for production, use a real library like nlohmann/json)
    try {
        JsonValue result;
        result.type = JsonValue::Type::Null;
        // TODO: Implement full JSON parsing
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// File Lock Utilities
// ============================================================================

std::optional<FileLockHandle> acquire_file_lock(
    const std::string& path,
    const FileLockOptions& options
) {
    int flags = O_RDWR;
    if (options.create) {
        flags |= O_CREAT;
    }
    
    int fd = open(path.c_str(), flags, 0600);
    if (fd < 0) {
        return std::nullopt;
    }
    
    int lock_op = options.exclusive ? LOCK_EX : LOCK_SH;
    if (options.timeout_ms > 0) {
        lock_op |= LOCK_NB;
    }
    
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (flock(fd, lock_op) == 0) {
            FileLockHandle handle;
            handle.path = path;
            handle.fd = fd;
            handle.exclusive = options.exclusive;
            return handle;
        }
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        
        if (elapsed >= options.timeout_ms) {
            close(fd);
            return std::nullopt;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void release_file_lock(FileLockHandle& handle) {
    if (handle.fd >= 0) {
        flock(handle.fd, LOCK_UN);
        close(handle.fd);
        handle.fd = -1;
    }
}

// ============================================================================
// JSON File I/O Utilities
// ============================================================================

std::optional<JsonValue> load_json_file(const std::string& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::nullopt;
        }
        
        std::string content(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );
        
        return safe_json_parse(content);
    } catch (...) {
        return std::nullopt;
    }
}

bool save_json_file(const std::string& path, const JsonValue& data) {
    try {
        auto json_str = safe_json_stringify(data);
        if (!json_str) return false;
        
        // Create parent directory if needed
        auto last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos) {
            std::string dir = path.substr(0, last_slash);
            mkdir(dir.c_str(), 0700);
        }
        
        // Write to temp file first
        std::string temp_path = path + ".tmp";
        std::ofstream file(temp_path);
        if (!file.is_open()) return false;
        
        file << *json_str << "\n";
        file.close();
        
        // Atomic rename
        if (rename(temp_path.c_str(), path.c_str()) != 0) {
            return false;
        }
        
        // Set permissions
        chmod(path.c_str(), 0600);
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Path Guard Utilities
// ============================================================================

std::string normalize_windows_path_for_comparison(const std::string& input) {
    std::string normalized = input;
    
    // Handle \\?\ prefix
    if (normalized.size() >= 4 && normalized.substr(0, 4) == "\\\\?\\") {
        normalized = normalized.substr(4);
        
        // Handle UNC paths
        if (normalized.size() >= 4 && 
            (normalized.substr(0, 4) == "UNC\\" || normalized.substr(0, 4) == "unc\\")) {
            normalized = "\\\\" + normalized.substr(4);
        }
    }
    
    // Replace forward slashes with backslashes
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    
    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    return normalized;
}

bool is_node_error(const std::exception& e) {
    // Check if exception message contains error code
    std::string msg = e.what();
    return msg.find("ENOENT") != std::string::npos ||
           msg.find("ENOTDIR") != std::string::npos ||
           msg.find("ELOOP") != std::string::npos ||
           msg.find("EINVAL") != std::string::npos ||
           msg.find("ENOTSUP") != std::string::npos;
}

bool has_node_error_code(const std::exception& e, const std::string& code) {
    return std::string(e.what()).find(code) != std::string::npos;
}

bool is_not_found_path_error(const std::exception& e) {
    std::string msg = e.what();
    return msg.find("ENOENT") != std::string::npos ||
           msg.find("ENOTDIR") != std::string::npos;
}

bool is_symlink_open_error(const std::exception& e) {
    std::string msg = e.what();
    return msg.find("ELOOP") != std::string::npos ||
           msg.find("EINVAL") != std::string::npos ||
           msg.find("ENOTSUP") != std::string::npos;
}

bool is_path_inside(const std::string& root, const std::string& target) {
    // Resolve absolute paths
    char root_abs[PATH_MAX];
    char target_abs[PATH_MAX];
    
    if (!realpath(root.c_str(), root_abs) || !realpath(target.c_str(), target_abs)) {
        return false;
    }
    
    std::string root_str(root_abs);
    std::string target_str(target_abs);
    
    // Ensure trailing slash for root
    if (!root_str.empty() && root_str.back() != '/') {
        root_str += '/';
    }
    
    // Check if target starts with root
    return target_str.size() >= root_str.size() &&
           target_str.substr(0, root_str.size()) == root_str;
}

// ============================================================================
// Safe File System Utilities
// ============================================================================

SafeOpenResult open_verified_local_file(
    const std::string& path,
    const SafeOpenOptions& options
) {
    // Check if path is absolute
    if (path.empty() || path[0] != '/') {
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, "Path must be absolute");
    }
    
    // Check workspace boundary
    if (!options.workspace_root.empty() && !is_path_inside(options.workspace_root, path)) {
        throw SafeOpenError(SafeOpenErrorCode::OutsideWorkspace, "Path outside workspace");
    }
    
    // Open file with O_NOFOLLOW to reject symlinks
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    if (options.reject_symlinks) {
        flags |= O_NOFOLLOW;
    }
#endif
    
    int fd = open(path.c_str(), flags);
    if (fd < 0) {
        if (errno == ENOENT) {
            throw SafeOpenError(SafeOpenErrorCode::NotFound, "File not found");
        }
        if (errno == ELOOP) {
            throw SafeOpenError(SafeOpenErrorCode::Symlink, "Symlink rejected");
        }
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, strerror(errno));
    }
    
    // Get file stats
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, "Failed to stat file");
    }
    
    // Check if it's a regular file
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        throw SafeOpenError(SafeOpenErrorCode::NotFile, "Not a regular file");
    }
    
    // Check file size
    if (options.max_bytes > 0 && st.st_size > options.max_bytes) {
        close(fd);
        throw SafeOpenError(SafeOpenErrorCode::TooLarge, "File too large");
    }
    
    // Check hardlinks
    if (options.reject_hardlinks && st.st_nlink > 1) {
        close(fd);
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, "Hardlink rejected");
    }
    
    // Get real path
    char real_path_buf[PATH_MAX];
    if (!realpath(path.c_str(), real_path_buf)) {
        close(fd);
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, "Failed to resolve path");
    }
    
    SafeOpenResult result;
    result.fd = fd;
    result.real_path = real_path_buf;
    result.file_size = st.st_size;
    result.mtime_ms = st.st_mtime * 1000LL;
    
    return result;
}

SafeLocalReadResult read_local_file_safe(
    const std::string& path,
    const SafeOpenOptions& options
) {
    auto open_result = open_verified_local_file(path, options);
    
    // Read file contents
    std::vector<uint8_t> buffer(open_result.file_size);
    ssize_t bytes_read = read(open_result.fd, buffer.data(), buffer.size());
    close(open_result.fd);
    
    if (bytes_read != open_result.file_size) {
        throw SafeOpenError(SafeOpenErrorCode::InvalidPath, "Failed to read file");
    }
    
    SafeLocalReadResult result;
    result.buffer = std::move(buffer);
    result.real_path = std::move(open_result.real_path);
    result.file_size = open_result.file_size;
    result.mtime_ms = open_result.mtime_ms;
    
    return result;
}

bool write_local_file_safe(
    const std::string& path,
    const std::vector<uint8_t>& data,
    const SafeOpenOptions& options
) {
    try {
        // Create temp file
        std::string temp_path = path + ".tmp";
        
        int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            return false;
        }
        
        // Write data
        ssize_t written = write(fd, data.data(), data.size());
        close(fd);
        
        if (written != static_cast<ssize_t>(data.size())) {
            unlink(temp_path.c_str());
            return false;
        }
        
        // Atomic rename
        if (rename(temp_path.c_str(), path.c_str()) != 0) {
            unlink(temp_path.c_str());
            return false;
        }
        
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Exec Approval Utilities
// ============================================================================

std::optional<ExecHost> normalize_exec_host(const std::string& value) {
    std::string normalized = to_lowercase(trim(value));
    if (normalized == "sandbox") return ExecHost::Sandbox;
    if (normalized == "gateway") return ExecHost::Gateway;
    if (normalized == "node") return ExecHost::Node;
    return std::nullopt;
}

std::optional<ExecSecurity> normalize_exec_security(const std::string& value) {
    std::string normalized = to_lowercase(trim(value));
    if (normalized == "deny") return ExecSecurity::Deny;
    if (normalized == "allowlist") return ExecSecurity::Allowlist;
    if (normalized == "full") return ExecSecurity::Full;
    return std::nullopt;
}

std::optional<ExecAsk> normalize_exec_ask(const std::string& value) {
    std::string normalized = to_lowercase(trim(value));
    if (normalized == "off") return ExecAsk::Off;
    if (normalized == "on-miss") return ExecAsk::OnMiss;
    if (normalized == "always") return ExecAsk::Always;
    return std::nullopt;
}

ExecApprovalRequest create_exec_approval_request(
    const ExecApprovalRequestPayload& payload,
    int64_t ttl_ms
) {
    ExecApprovalRequest request;
    request.id = generate_uuid();
    request.request = payload;
    request.created_at_ms = current_timestamp_ms();
    request.expires_at_ms = request.created_at_ms + ttl_ms;
    return request;
}

ExecApprovalResolved resolve_exec_approval(
    const ExecApprovalRequest& request,
    ExecApprovalDecision decision,
    const std::string& resolved_by
) {
    ExecApprovalResolved resolved;
    resolved.id = request.id;
    resolved.decision = decision;
    resolved.resolved_by = resolved_by.empty() ? std::nullopt : std::make_optional(resolved_by);
    resolved.ts = current_timestamp_ms();
    resolved.request = request.request;
    return resolved;
}

bool is_exec_allowed(
    const std::string& command,
    const ExecApprovalsDefaults& defaults,
    const std::vector<std::string>& allowlist
) {
    // Check security policy
    if (defaults.security == ExecSecurity::Deny) {
        return false;
    }
    
    if (defaults.security == ExecSecurity::Full) {
        return true;
    }
    
    // Check allowlist
    for (const auto& pattern : allowlist) {
        if (command.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}

// ============================================================================
// Environment & Path Utilities
// ============================================================================

std::string expand_home_prefix(const std::string& path, const std::string& home) {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    
    std::string home_dir = home.empty() ? get_home_dir() : home;
    
    if (path.size() == 1) {
        return home_dir;
    }
    
    if (path[1] == '/') {
        return home_dir + path.substr(1);
    }
    
    return path;
}

std::string get_home_dir() {
    const char* home = getenv("HOME");
    if (home) return home;
    
    home = getenv("USERPROFILE");
    if (home) return home;
    
    return "/tmp";
}

std::vector<std::string> get_path_env() {
    const char* path = getenv("PATH");
    if (!path) return {};
    
    return split(path, ":");
}

void prepend_to_path_env(const std::string& dir) {
    auto paths = get_path_env();
    paths.insert(paths.begin(), dir);
    std::string new_path = join(paths, ":");
    setenv("PATH", new_path.c_str(), 1);
}

bool is_absolute_path(const std::string& path) {
    if (path.empty()) return false;
    return path[0] == '/' || is_windows_absolute_path(path);
}

bool is_windows_absolute_path(const std::string& path) {
    if (path.size() < 3) return false;
    return std::isalpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

std::string join_paths(const std::vector<std::string>& parts) {
    if (parts.empty()) return "";
    
    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        if (!result.empty() && result.back() != '/') {
            result += '/';
        }
        result += parts[i];
    }
    return result;
}

std::string normalize_path(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        return resolved;
    }
    return path;
}

std::string relative_path(const std::string& base, const std::string& target) {
    // Simplified relative path calculation
    if (target.find(base) == 0) {
        return target.substr(base.size() + 1);
    }
    return target;
}

// ============================================================================
// Rate Limiting Utilities
// ============================================================================

FixedWindowRateLimit create_rate_limiter(int max_requests, int64_t window_ms) {
    FixedWindowRateLimit limiter;
    limiter.max_requests = max_requests;
    limiter.window_ms = window_ms;
    limiter.window_start_ms = current_timestamp_ms();
    limiter.current_count = 0;
    return limiter;
}

bool check_rate_limit(FixedWindowRateLimit& limiter) {
    int64_t now = current_timestamp_ms();
    
    // Check if we need to reset window
    if (now >= limiter.window_start_ms + limiter.window_ms) {
        limiter.window_start_ms = now;
        limiter.current_count = 0;
    }
    
    // Check if we have capacity
    if (limiter.current_count >= limiter.max_requests) {
        return false;
    }
    
    // Consume one request
    limiter.current_count++;
    return true;
}

void reset_rate_limiter(FixedWindowRateLimit& limiter) {
    limiter.window_start_ms = current_timestamp_ms();
    limiter.current_count = 0;
}

int get_rate_limit_remaining(const FixedWindowRateLimit& limiter) {
    return std::max(0, limiter.max_requests - limiter.current_count);
}

// ============================================================================
// Archive Utilities (Simplified)
// ============================================================================

std::vector<uint8_t> create_tar_gz(const std::vector<ArchiveEntry>& entries) {
    // TODO: Implement tar.gz creation using libarchive
    return {};
}

std::vector<ArchiveEntry> extract_tar_gz(const std::vector<uint8_t>& data) {
    // TODO: Implement tar.gz extraction using libarchive
    return {};
}

std::vector<uint8_t> archive_directory(const std::string& dir_path) {
    // TODO: Implement directory archiving
    return {};
}

bool extract_to_directory(const std::vector<uint8_t>& data, const std::string& dest_dir) {
    // TODO: Implement extraction to directory
    return false;
}

// ============================================================================
// Miscellaneous Utilities
// ============================================================================

std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);
    
    std::ostringstream oss;
    oss << std::hex;
    
    for (int i = 0; i < 8; i++) oss << dis(gen);
    oss << "-";
    for (int i = 0; i < 4; i++) oss << dis(gen);
    oss << "-4";
    for (int i = 0; i < 3; i++) oss << dis(gen);
    oss << "-";
    oss << dis2(gen);
    for (int i = 0; i < 3; i++) oss << dis(gen);
    oss << "-";
    for (int i = 0; i < 12; i++) oss << dis(gen);
    
    return oss.str();
}

int64_t current_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::string current_timestamp_iso() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string sha256_hash(const std::string& data) {
    return sha256_hash_bytes(std::vector<uint8_t>(data.begin(), data.end()));
}

std::string sha256_hash_bytes(const std::vector<uint8_t>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    return oss.str();
}

std::string base64_encode(const std::vector<uint8_t>& data) {
    static const char* base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string result;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];
    
    for (size_t idx = 0; idx < data.size(); ++idx) {
        char_array_3[i++] = data[idx];
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (i = 0; i < 4; i++) {
                result += base64_chars[char_array_4[i]];
            }
            i = 0;
        }
    }
    
    if (i) {
        for (j = i; j < 3; j++) {
            char_array_3[j] = '\0';
        }
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        
        for (j = 0; j < i + 1; j++) {
            result += base64_chars[char_array_4[j]];
        }
        
        while (i++ < 3) {
            result += '=';
        }
    }
    
    return result;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::vector<uint8_t> result;
    int i = 0;
    int j = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    
    for (char c : encoded) {
        if (c == '=') break;
        if (base64_chars.find(c) == std::string::npos) continue;
        
        char_array_4[i++] = c;
        if (i == 4) {
            for (i = 0; i < 4; i++) {
                char_array_4[i] = static_cast<unsigned char>(base64_chars.find(char_array_4[i]));
            }
            
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (i = 0; i < 3; i++) {
                result.push_back(char_array_3[i]);
            }
            i = 0;
        }
    }
    
    if (i) {
        for (j = i; j < 4; j++) {
            char_array_4[j] = 0;
        }
        
        for (j = 0; j < 4; j++) {
            char_array_4[j] = static_cast<unsigned char>(base64_chars.find(char_array_4[j]));
        }
        
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        
        for (j = 0; j < i - 1; j++) {
            result.push_back(char_array_3[j]);
        }
    }
    
    return result;
}

bool is_valid_utf8(const std::string& str) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(str.data());
    size_t len = str.size();
    
    for (size_t i = 0; i < len; ) {
        if (bytes[i] <= 0x7F) {
            i++;
        } else if ((bytes[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (bytes[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((bytes[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((bytes[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    
    return true;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    size_t end = str.find(delimiter);
    
    while (end != std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + delimiter.size();
        end = str.find(delimiter, start);
    }
    
    result.push_back(str.substr(start));
    return result;
}

std::string join(const std::vector<std::string>& parts, const std::string& delimiter) {
    if (parts.empty()) return "";
    
    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        result += delimiter + parts[i];
    }
    return result;
}

std::string to_lowercase(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string to_uppercase(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
           str.substr(0, prefix.size()) == prefix;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.substr(str.size() - suffix.size()) == suffix;
}

std::string replace_all(
    const std::string& str,
    const std::string& from,
    const std::string& to
) {
    std::string result = str;
    size_t pos = 0;
    
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.size(), to);
        pos += to.size();
    }
    
    return result;
}

}  // namespace infra
}  // namespace hunterclaw
