#include "hunterclaw/gateway/auth_manager.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// Helper function to get logger
static inline std::shared_ptr<spdlog::logger> get_logger() {
    static auto logger = spdlog::get("auth_manager");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("auth_manager");
    }
    return logger;
}

// ============================================================================
// AuthManager::Impl
// ============================================================================

struct AuthManager::Impl {
    std::shared_ptr<core::Config> config;
    
    // 设备数据库 (device_id → DeviceInfo)
    std::map<std::string, DeviceInfo> devices;
    mutable std::mutex devices_mutex;
    
    // 挑战缓存 (nonce → DeviceChallenge)
    std::map<std::string, DeviceChallenge> challenges;
    mutable std::mutex challenges_mutex;
    
    // 数据文件路径
    std::string devices_file = "./data/auth/devices.json";
    
    explicit Impl(std::shared_ptr<core::Config> cfg) : config(cfg) {
        // 确保数据目录存在
        std::filesystem::create_directories("./data/auth");
        
        // 自动加载设备数据
        load_devices();
    }
    
    // 加载设备数据
    core::Result<bool> load_devices() {
        std::lock_guard<std::mutex> lock(devices_mutex);
        
        try {
            if (!std::filesystem::exists(devices_file)) {
                get_logger()->info("[AuthManager] No devices file found, starting fresh");
                return core::Result<bool>::Ok(true);
            }
            
            std::ifstream file(devices_file);
            if (!file.is_open()) {
                return core::Result<bool>::Err("Failed to open devices file");
            }
            
            json j;
            file >> j;
            file.close();
            
            // 解析设备列表
            if (!j.is_array()) {
                return core::Result<bool>::Err("Invalid devices file format");
            }
            
            for (const auto& device_json : j) {
                DeviceInfo device;
                device.device_id = device_json.value("device_id", "");
                device.device_token = device_json.value("device_token", "");
                device.device_name = device_json.value("device_name", "");
                device.platform = device_json.value("platform", "");
                device.revoked = device_json.value("revoked", false);
                device.public_key = device_json.value("public_key", "");
                
                // 解析 scopes
                if (device_json.contains("scopes") && device_json["scopes"].is_array()) {
                    for (const auto& scope : device_json["scopes"]) {
                        device.scopes.push_back(scope.get<std::string>());
                    }
                }
                
                // 解析时间戳
                if (device_json.contains("created_at")) {
                    int64_t ts = device_json["created_at"];
                    device.created_at = core::Timestamp(std::chrono::nanoseconds(ts));
                }
                if (device_json.contains("expires_at")) {
                    int64_t ts = device_json["expires_at"];
                    device.expires_at = core::Timestamp(std::chrono::nanoseconds(ts));
                }
                
                // 解析元数据
                if (device_json.contains("metadata") && device_json["metadata"].is_object()) {
                    for (auto& [key, value] : device_json["metadata"].items()) {
                        if (value.is_string()) {
                            device.metadata[key] = value.get<std::string>();
                        } else {
                            device.metadata[key] = value.dump();
                        }
                    }
                }
                
                devices[device.device_id] = device;
            }
            
            get_logger()->info("[AuthManager] Loaded {} devices", devices.size());
            return core::Result<bool>::Ok(true);
            
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(
                std::string("Failed to load devices: ") + e.what()
            );
        }
    }
    
    // 保存设备数据
    core::Result<bool> save_devices() {
        std::lock_guard<std::mutex> lock(devices_mutex);
        
        try {
            json j = json::array();
            
            for (const auto& [_, device] : devices) {
                json device_json;
                device_json["device_id"] = device.device_id;
                device_json["device_token"] = device.device_token;
                device_json["device_name"] = device.device_name;
                device_json["platform"] = device.platform;
                device_json["revoked"] = device.revoked;
                device_json["public_key"] = device.public_key;
                device_json["scopes"] = device.scopes;
                device_json["created_at"] = device.created_at.time_since_epoch().count();
                device_json["expires_at"] = device.expires_at.time_since_epoch().count();
                device_json["metadata"] = device.metadata;
                
                j.push_back(device_json);
            }
            
            std::ofstream file(devices_file);
            if (!file.is_open()) {
                return core::Result<bool>::Err("Failed to open devices file for writing");
            }
            
            file << j.dump(2);
            file.close();
            
            get_logger()->info("[AuthManager] Saved {} devices", devices.size());
            return core::Result<bool>::Ok(true);
            
        } catch (const std::exception& e) {
            return core::Result<bool>::Err(
                std::string("Failed to save devices: ") + e.what()
            );
        }
    }
};

// ============================================================================
// AuthManager Implementation
// ============================================================================

AuthManager::AuthManager(std::shared_ptr<core::Config> config)
    : impl_(std::make_unique<Impl>(config)) {}

AuthManager::~AuthManager() = default;

// ========== 连接鉴权 ==========

AuthResult AuthManager::authenticate_connect(
    const json& params,
    const std::string& nonce
) {
    AuthResult result;
    
    // 获取鉴权配置
    auto& auth_config = impl_->config->gateway.auth;
    
    get_logger()->debug("[AuthManager] Authenticating connection (mode: {})", 
        auth_config.mode);
    
    // 1. 无鉴权模式
    if (!auth_config.enabled || auth_config.mode == "none") {
        get_logger()->debug("[AuthManager] Auth disabled, granting default scopes");
        result.ok = true;
        result.device_id = params.value("deviceId", "anonymous");
        result.scopes = {
            scopes::AGENT_RUN,
            scopes::SESSION_READ,
            scopes::SESSION_WRITE
        };
        return result;
    }
    
    // 2. Bearer Token 模式
    if (auth_config.mode == "bearer") {
        std::string token = params.value("token", "");
        
        if (!validate_bearer_token(token)) {
            get_logger()->warn("[AuthManager] Bearer token validation failed");
            result.ok = false;
            result.reason = auth_errors::NOT_AUTHENTICATED;
            return result;
        }
        
        get_logger()->debug("[AuthManager] Bearer token validated");
        result.ok = true;
        result.device_id = params.value("deviceId", "authenticated");
        result.scopes = {
            scopes::AGENT_RUN,
            scopes::SESSION_READ,
            scopes::SESSION_WRITE,
            scopes::SKILL_INSTALL,
            scopes::PLUGIN_INSTALL,
            scopes::ADMIN_FULL
        };
        return result;
    }
    
    // 3. 设备认证模式
    if (auth_config.mode == "device") {
        std::string device_id = params.value("deviceId", "");
        std::string device_token = params.value("deviceToken", "");
        
        if (device_id.empty() || device_token.empty()) {
            get_logger()->warn("[AuthManager] Missing deviceId or deviceToken");
            result.ok = false;
            result.reason = auth_errors::NOT_AUTHENTICATED;
            return result;
        }
        
        if (!verify_device(device_id, device_token)) {
            get_logger()->warn("[AuthManager] Device verification failed: {}", device_id);
            result.ok = false;
            result.reason = auth_errors::DEVICE_TOKEN_MISMATCH;
            return result;
        }
        
        // 获取设备权限
        auto device_opt = get_device(device_id);
        if (!device_opt.has_value()) {
            result.ok = false;
            result.reason = auth_errors::NOT_AUTHENTICATED;
            return result;
        }
        
        get_logger()->debug("[AuthManager] Device authenticated: {}", device_id);
        result.ok = true;
        result.device_id = device_id;
        result.scopes = device_opt->scopes;
        return result;
    }
    
    // 4. 挑战-响应模式
    if (auth_config.mode == "challenge") {
        std::string device_id = params.value("deviceId", "");
        std::string nonce_param = params.value("nonce", nonce);
        std::string signature = params.value("signature", "");
        
        if (device_id.empty() || nonce_param.empty() || signature.empty()) {
            get_logger()->warn("[AuthManager] Missing challenge parameters");
            result.ok = false;
            result.reason = auth_errors::NOT_AUTHENTICATED;
            return result;
        }
        
        if (!verify_device_signature(device_id, nonce_param, signature)) {
            get_logger()->warn("[AuthManager] Signature verification failed: {}", device_id);
            result.ok = false;
            result.reason = auth_errors::INVALID_SIGNATURE;
            return result;
        }
        
        // 获取设备权限
        auto device_opt = get_device(device_id);
        if (!device_opt.has_value()) {
            result.ok = false;
            result.reason = auth_errors::NOT_AUTHENTICATED;
            return result;
        }
        
        get_logger()->debug("[AuthManager] Challenge-response authenticated: {}", device_id);
        result.ok = true;
        result.device_id = device_id;
        result.scopes = device_opt->scopes;
        return result;
    }
    
    // 未知鉴权模式
    get_logger()->error("[AuthManager] Invalid auth mode: {}", auth_config.mode);
    result.ok = false;
    result.reason = auth_errors::INVALID_AUTH_MODE;
    return result;
}

// ========== Token 验证 ==========

bool AuthManager::validate_bearer_token(const std::string& token) const {
    if (!impl_->config->gateway.auth.enabled) {
        return true;
    }
    
    if (impl_->config->gateway.auth.mode != "bearer") {
        return true;
    }
    
    std::string expected = impl_->config->gateway.auth.bearer_token;
    if (expected.empty()) {
        get_logger()->warn("[AuthManager] Bearer token not configured");
        return false;
    }
    
    // 支持两种格式:
    // 1. "sk-hunterclaw-abc123"
    // 2. "Bearer sk-hunterclaw-abc123"
    return token == expected || token == ("Bearer " + expected);
}

bool AuthManager::validate_password(const std::string& password) const {
    // TODO: 实现密码验证
    // 可以使用 bcrypt 等加密算法
    return !password.empty();
}

// ========== 设备管理 ==========

core::Result<DeviceInfo> AuthManager::register_device(
    const std::string& device_name,
    const std::string& platform,
    const std::vector<std::string>& scopes
) {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    DeviceInfo device;
    device.device_id = "device-" + core::utils::generate_uuid();
    device.device_token = "token-" + core::utils::generate_random_string(32);
    device.device_name = device_name;
    device.platform = platform;
    device.created_at = core::utils::now();
    device.expires_at = core::utils::now() + std::chrono::hours(24 * 365);  // 1 年
    device.revoked = false;
    
    // 设置权限
    if (scopes.empty()) {
        // 默认权限
        device.scopes = {
            scopes::AGENT_RUN,
            scopes::SESSION_READ,
            scopes::SESSION_WRITE,
            scopes::SKILL_LIST,
            scopes::PLUGIN_LIST
        };
    } else {
        device.scopes = scopes;
    }
    
    // 存储
    impl_->devices[device.device_id] = device;
    
    // 保存到磁盘
    impl_->save_devices();
    
    get_logger()->info("[AuthManager] Registered device: {} ({})", 
        device.device_name, device.device_id);
    
    return core::Result<DeviceInfo>::Ok(device);
}

bool AuthManager::verify_device(
    const std::string& device_id,
    const std::string& device_token
) {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    auto it = impl_->devices.find(device_id);
    if (it == impl_->devices.end()) {
        get_logger()->debug("[AuthManager] Device not found: {}", device_id);
        return false;
    }
    
    const auto& device = it->second;
    
    // 检查 Token
    if (device.device_token != device_token) {
        get_logger()->debug("[AuthManager] Device token mismatch: {}", device_id);
        return false;
    }
    
    // 检查是否撤销
    if (device.revoked) {
        get_logger()->debug("[AuthManager] Device revoked: {}", device_id);
        return false;
    }
    
    // 检查是否过期
    if (core::utils::now() > device.expires_at) {
        get_logger()->debug("[AuthManager] Device token expired: {}", device_id);
        return false;
    }
    
    return true;
}

std::optional<DeviceInfo> AuthManager::get_device(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    auto it = impl_->devices.find(device_id);
    if (it == impl_->devices.end()) {
        return std::nullopt;
    }
    
    return it->second;
}

std::vector<DeviceInfo> AuthManager::list_devices() const {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    std::vector<DeviceInfo> result;
    for (const auto& [_, device] : impl_->devices) {
        result.push_back(device);
    }
    return result;
}

bool AuthManager::revoke_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    auto it = impl_->devices.find(device_id);
    if (it == impl_->devices.end()) {
        return false;
    }
    
    it->second.revoked = true;
    
    // 保存到磁盘
    impl_->save_devices();
    
    get_logger()->info("[AuthManager] Revoked device: {}", device_id);
    return true;
}

bool AuthManager::delete_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    auto it = impl_->devices.find(device_id);
    if (it == impl_->devices.end()) {
        return false;
    }
    
    impl_->devices.erase(it);
    
    // 保存到磁盘
    impl_->save_devices();
    
    get_logger()->info("[AuthManager] Deleted device: {}", device_id);
    return true;
}

bool AuthManager::update_device(
    const std::string& device_id,
    const std::map<std::string, std::string>& updates
) {
    std::lock_guard<std::mutex> lock(impl_->devices_mutex);
    
    auto it = impl_->devices.find(device_id);
    if (it == impl_->devices.end()) {
        return false;
    }
    
    auto& device = it->second;
    
    // 更新字段
    for (const auto& [key, value] : updates) {
        if (key == "device_name") {
            device.device_name = value;
        } else if (key == "platform") {
            device.platform = value;
        } else {
            device.metadata[key] = value;
        }
    }
    
    // 保存到磁盘
    impl_->save_devices();
    
    get_logger()->debug("[AuthManager] Updated device: {}", device_id);
    return true;
}

// ========== 挑战-响应鉴权 ==========

DeviceChallenge AuthManager::create_challenge() {
    std::lock_guard<std::mutex> lock(impl_->challenges_mutex);
    
    DeviceChallenge challenge;
    challenge.nonce = core::utils::generate_random_string(32);
    challenge.created_at = core::utils::now();
    challenge.expires_in_ms = 300000;  // 5 分钟
    
    impl_->challenges[challenge.nonce] = challenge;
    
    get_logger()->debug("[AuthManager] Created challenge: {}", challenge.nonce);
    return challenge;
}

bool AuthManager::verify_device_signature(
    const std::string& device_id,
    const std::string& nonce,
    const std::string& signature
) {
    std::lock_guard<std::mutex> lock(impl_->challenges_mutex);
    
    // 1. 检查挑战是否存在
    auto it = impl_->challenges.find(nonce);
    if (it == impl_->challenges.end()) {
        get_logger()->debug("[AuthManager] Challenge not found: {}", nonce);
        return false;
    }
    
    // 2. 检查挑战是否过期
    if (it->second.is_expired()) {
        get_logger()->debug("[AuthManager] Challenge expired: {}", nonce);
        impl_->challenges.erase(it);
        return false;
    }
    
    // 3. 获取设备公钥
    auto device_opt = get_device(device_id);
    if (!device_opt.has_value()) {
        get_logger()->debug("[AuthManager] Device not found: {}", device_id);
        return false;
    }
    
    const auto& device = device_opt.value();
    if (device.public_key.empty()) {
        get_logger()->debug("[AuthManager] Device has no public key: {}", device_id);
        return false;
    }
    
    // 4. 验证签名
    // TODO: 实现签名验证 (使用 OpenSSL/libsodium)
    // bool valid = crypto::verify_signature(nonce, signature, device.public_key);
    bool valid = true;  // 临时: 总是通过
    
    // 5. 清理挑战 (一次性使用)
    impl_->challenges.erase(it);
    
    get_logger()->debug("[AuthManager] Signature verification result: {}", valid);
    return valid;
}

// ========== 权限检查 ==========

bool AuthManager::has_scope(
    const std::vector<std::string>& scopes,
    const std::string& required_scope
) {
    // 1. 检查是否有 admin.full (拥有所有权限)
    if (std::find(scopes.begin(), scopes.end(), scopes::ADMIN_FULL) != scopes.end()) {
        return true;
    }
    
    // 2. 检查是否有精确匹配的权限
    if (std::find(scopes.begin(), scopes.end(), required_scope) != scopes.end()) {
        return true;
    }
    
    // 3. 检查通配符权限 (e.g., "agent.*" 匹配 "agent.run")
    for (const auto& scope : scopes) {
        if (scope.back() == '*') {
            std::string prefix = scope.substr(0, scope.size() - 1);
            if (required_scope.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
    }
    
    return false;
}

// ========== 持久化 ==========

core::Result<bool> AuthManager::save_devices() {
    return impl_->save_devices();
}

core::Result<bool> AuthManager::load_devices() {
    return impl_->load_devices();
}

} // namespace gateway
} // namespace hunterclaw
