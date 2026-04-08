// 🎯 严格对齐 OpenClaw: src/agents/sandbox/config-hash.ts (57 lines)
// 将沙箱配置序列化为确定性 SHA-256 hash，用于检测配置变更触发容器重建

#include <string>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

namespace hunterclaw {
namespace sandbox {

using json = nlohmann::json;

// 🎯 对齐 OpenClaw: hashTextSha256
static std::string hash_text_sha256(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    }
    return oss.str();
}

// 🎯 对齐 OpenClaw: normalizeForHash (递归规范化)
static json normalize_for_hash(const json& value) {
    if (value.is_null()) return nullptr;
    if (value.is_array()) {
        json arr = json::array();
        for (const auto& item : value) {
            auto normalized = normalize_for_hash(item);
            if (!normalized.is_null()) arr.push_back(normalized);
        }
        return arr;
    }
    if (value.is_object()) {
        // Sort keys deterministically
        std::vector<std::string> keys;
        for (auto& [k, v] : value.items()) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        json obj = json::object();
        for (const auto& k : keys) {
            auto normalized = normalize_for_hash(value[k]);
            if (!normalized.is_null()) obj[k] = normalized;
        }
        return obj;
    }
    return value;
}

// 🎯 对齐 OpenClaw: computeSandboxConfigHash
std::string compute_sandbox_config_hash(const json& input) {
    auto payload = normalize_for_hash(input);
    return hash_text_sha256(payload.dump());
}

// 🎯 对齐 OpenClaw: computeSandboxBrowserConfigHash
std::string compute_sandbox_browser_config_hash(const json& input) {
    auto payload = normalize_for_hash(input);
    return hash_text_sha256(payload.dump());
}

}  // namespace sandbox
}  // namespace hunterclaw
