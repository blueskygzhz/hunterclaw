#include "hunterclaw/gateway/session_device_id.h"
#include "hunterclaw/core/utils.h"
#include <httplib.h>         // ✅ 完整包含 httplib (需要 Request 完整定义)
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

// ============================================================================
// 辅助函数
// ============================================================================

// Base64 解码 (简化实现)
static std::string base64_decode(const std::string& encoded) {
    const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    std::string decoded;
    std::vector<int> T(256, -1);
    
    for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;
    
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return decoded;
}

// SHA256 计算
static std::string sha256_hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)input.c_str(), input.size(), hash);
    
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    return oss.str();
}

// ============================================================================
// 公开 API 实现
// ============================================================================

std::string extract_device_id_from_request(const httplib::Request& req) {
    auto logger = spdlog::get("console");
    
    // ========== 策略 1: HTTP Header ==========
    
    // X-Device-ID (HunterClaw 标准)
    if (req.has_header("X-Device-ID")) {
        std::string device_id = req.get_header_value("X-Device-ID");
        if (logger) {
            logger->debug("[DeviceID] Extracted from X-Device-ID: {}", device_id);
        }
        return device_id;
    }
    
    // X-OpenClaw-Device-ID (OpenClaw 兼容)
    if (req.has_header("X-OpenClaw-Device-ID")) {
        std::string device_id = req.get_header_value("X-OpenClaw-Device-ID");
        if (logger) {
            logger->debug("[DeviceID] Extracted from X-OpenClaw-Device-ID: {}", device_id);
        }
        return device_id;
    }
    
    // ========== 策略 2: Authorization Token ==========
    
    if (req.has_header("Authorization")) {
        std::string auth_header = req.get_header_value("Authorization");
        
        // 检查 Bearer token
        const std::string bearer_prefix = "Bearer ";
        if (auth_header.substr(0, bearer_prefix.size()) == bearer_prefix) {
            std::string token = auth_header.substr(bearer_prefix.size());
            
            auto device_id_opt = parse_device_id_from_token(token);
            if (device_id_opt) {
                if (logger) {
                    logger->debug("[DeviceID] Extracted from Bearer token: {}", *device_id_opt);
                }
                return *device_id_opt;
            }
        }
    }
    
    // ========== 策略 3: Cookie ==========
    
    if (req.has_header("Cookie")) {
        std::string cookie_header = req.get_header_value("Cookie");
        auto cookies = parse_cookies(cookie_header);
        
        // openclaw_device_id
        if (cookies.count("openclaw_device_id")) {
            std::string device_id = cookies["openclaw_device_id"];
            if (logger) {
                logger->debug("[DeviceID] Extracted from Cookie: {}", device_id);
            }
            return device_id;
        }
        
        // device_id (备用)
        if (cookies.count("device_id")) {
            std::string device_id = cookies["device_id"];
            if (logger) {
                logger->debug("[DeviceID] Extracted from Cookie (fallback): {}", device_id);
            }
            return device_id;
        }
    }
    
    // ========== 策略 4: 计算稳定 Device ID (Fallback) ==========
    
    std::string ip = req.remote_addr;
    std::string user_agent = req.has_header("User-Agent") 
        ? req.get_header_value("User-Agent") 
        : "";
    
    std::string computed_id = compute_stable_device_id(ip, user_agent);
    
    if (logger) {
        logger->debug("[DeviceID] Computed fallback from IP+UA: {}", computed_id);
    }
    
    return computed_id;
}

std::string compute_stable_device_id(
    const std::string& ip,
    const std::string& user_agent
) {
    // 构造输入: IP + "|" + UA
    std::string input = ip + "|" + user_agent;
    
    // SHA256 哈希
    std::string full_hash = sha256_hex(input);
    
    // 取前 16 字符 (64位十六进制 = 32 字节)
    std::string device_id = "computed-" + full_hash.substr(0, 16);
    
    return device_id;
}

std::optional<std::string> parse_device_id_from_token(const std::string& token) {
    auto logger = spdlog::get("console");
    
    try {
        // 尝试 Base64 解码
        std::string decoded = base64_decode(token);
        
        // 尝试解析 JSON
        json j = json::parse(decoded);
        
        // 检查 device_id 字段
        if (j.contains("device_id")) {
            return j["device_id"].get<std::string>();
        }
        
        // 备用: deviceId (camelCase)
        if (j.contains("deviceId")) {
            return j["deviceId"].get<std::string>();
        }
        
    } catch (const std::exception& e) {
        if (logger) {
            logger->debug("[DeviceID] Failed to parse token: {}", e.what());
        }
    }
    
    return std::nullopt;
}

std::unordered_map<std::string, std::string> parse_cookies(
    const std::string& cookie_header
) {
    std::unordered_map<std::string, std::string> cookies;
    
    std::istringstream iss(cookie_header);
    std::string cookie_pair;
    
    while (std::getline(iss, cookie_pair, ';')) {
        // Trim leading/trailing spaces
        cookie_pair.erase(0, cookie_pair.find_first_not_of(" \t"));
        cookie_pair.erase(cookie_pair.find_last_not_of(" \t") + 1);
        
        // Split by '='
        size_t eq_pos = cookie_pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = cookie_pair.substr(0, eq_pos);
            std::string value = cookie_pair.substr(eq_pos + 1);
            
            // Trim key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            cookies[key] = value;
        }
    }
    
    return cookies;
}

std::string build_device_session_key(
    const std::string& agent_id,
    const std::string& device_id,
    const std::optional<std::string>& channel_id
) {
    // 构造 Session Key: agent:{agentId}:device:{deviceId}[:channel:{channelId}]
    std::string session_key = "agent:" + agent_id + ":device:" + device_id;
    
    if (channel_id && !channel_id->empty()) {
        session_key += ":channel:" + *channel_id;
    }
    
    return session_key;
}

bool is_valid_device_id(const std::string& device_id) {
    // 检查长度 (8-64 字符)
    if (device_id.empty() || device_id.size() < 8 || device_id.size() > 64) {
        return false;
    }
    
    // 检查字符: 只允许字母数字和连字符
    for (char c : device_id) {
        if (!std::isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }
    
    return true;
}

} // namespace gateway
} // namespace hunterclaw
