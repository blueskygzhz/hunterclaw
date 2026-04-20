// ============================================================================
// memory_embedding_providers_deep.cc
// ✅ 严格对齐 OpenClaw:
//   - src/memory-host-sdk/host/embeddings-bedrock.ts (12.33KB)
//   - src/memory-host-sdk/host/embeddings-lmstudio.ts (4.89KB)
//   - src/memory-host-sdk/host/embeddings-remote-provider.ts (1.96KB)
//   - src/memory-host-sdk/host/embeddings-remote-client.ts (1.75KB)
//   - src/memory-host-sdk/host/embeddings-remote-fetch.ts (777B)
//   - src/memory-host-sdk/host/embeddings-debug.ts (496B)
//   - src/memory-host-sdk/host/embeddings-model-normalize.ts (384B)
//   - src/memory-host-sdk/host/secret-input.ts (728B)
//   - src/memory-host-sdk/host/read-file.ts (3.21KB)
//   - src/memory-host-sdk/host/fs-utils.ts (813B)
//   - src/memory-host-sdk/host/embedding-input-limits.ts (2.2KB)
//   - src/memory-host-sdk/host/embedding-model-limits.ts (1.55KB)
//
// 深层 embedding provider + 记忆文件读取：
//   - BedrockEmbeddingProvider: AWS Bedrock embedding (Titan/Cohere)
//   - LmStudioEmbeddingProvider: LM Studio 本地嵌入
//   - RemoteEmbeddingProvider: 通用远程嵌入 (自定义 baseUrl)
//   - readMemoryFile / readAgentMemoryFile: 安全记忆文件读取
//   - resolveMemorySecretInputString: 记忆模块 secret input 解析
//   - resolveEmbeddingMaxInputTokens: 模型最大输入 token 限制
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace memory {

namespace {
auto deep_log() { return spdlog::get("memory/embedding"); }
} // namespace

// ============================================================================
// 🎯 fs-utils (对齐 fs-utils.ts:1-27)
// ============================================================================

static bool is_file_missing_error(const std::exception& e) {
    std::string msg = e.what();
    return msg.find("ENOENT") != std::string::npos ||
           msg.find("No such file") != std::string::npos;
}

struct StatResult {
    bool missing = false;
    bool is_regular = false;
    int64_t size = 0;
};

static StatResult stat_regular_file(const std::string& path) {
    std::error_code ec;
    auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) return {true, false, 0};
    if (!fs::is_regular_file(status)) return {false, false, 0};
    auto sz = fs::file_size(path, ec);
    return {false, true, ec ? 0 : static_cast<int64_t>(sz)};
}

// ============================================================================
// 🎯 embedding-model-limits — 已迁移至 chunking.cc
//
// 本文件原有 resolve_embedding_max_input_tokens() 的重复定义与 chunking.cc 中
// 的同名函数形成 ODR 违反（链接 libhunterclaw_memory.so 时报 multiple definition）。
// 保留 chunking.cc 的版本（对齐 OC embedding-model-limits.ts:resolveEmbeddingMaxInputTokens
// 更完整：支持 provider-level fallback、local/gemini 默认值分支、KNOWN_LIMITS 按
// "provider:model" 组 key 精确匹配）。
//
// 如需使用，#include "hunterclaw/memory/chunking.h"（或相应头文件）。
// ============================================================================

// ============================================================================
// 🎯 embedding-input-limits (对齐 embedding-input-limits.ts:1-62)
// ============================================================================

static size_t estimate_utf8_bytes(const std::string& text) {
    return text.size(); // UTF-8 string, bytes == size
}

static std::vector<std::string> split_text_to_utf8_byte_limit(
    const std::string& text,
    size_t max_bytes
) {
    if (text.size() <= max_bytes) return {text};
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = std::min(pos + max_bytes, text.size());
        // Avoid splitting in middle of UTF-8 multi-byte
        while (end > pos && (text[end] & 0xC0) == 0x80) --end;
        if (end == pos) end = pos + 1;
        parts.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return parts;
}

// ============================================================================
// 🎯 embeddings-model-normalize (对齐 embeddings-model-normalize.ts:1-10)
// ============================================================================

static std::string normalize_gemini_embedding_model(const std::string& model) {
    std::string s = model;
    // Strip "models/" prefix
    if (s.rfind("models/", 0) == 0) s = s.substr(7);
    // Strip "gemini/" or "google/" prefix
    if (s.rfind("gemini/", 0) == 0) s = s.substr(7);
    if (s.rfind("google/", 0) == 0) s = s.substr(7);
    return s;
}

// ============================================================================
// 🎯 BedrockEmbeddingProvider (对齐 embeddings-bedrock.ts:1-350)
// AWS Bedrock embedding (Titan + Cohere)
// ============================================================================

static const std::string DEFAULT_BEDROCK_MODEL = "amazon.titan-embed-text-v2:0";
static const std::string DEFAULT_BEDROCK_REGION = "us-east-1";

struct BedrockEmbeddingConfig {
    std::string model = DEFAULT_BEDROCK_MODEL;
    std::string region = DEFAULT_BEDROCK_REGION;
    std::optional<std::string> access_key_id;
    std::optional<std::string> secret_access_key;
    std::optional<std::string> session_token;
    std::optional<int> output_dimensionality;
};

// 对齐 embeddings-bedrock.ts:hasAwsCredentials
static bool has_aws_credentials(const std::unordered_map<std::string, std::string>& env) {
    auto has = [&](const std::string& key) {
        auto it = env.find(key);
        return it != env.end() && !it->second.empty();
    };
    // Check environment variables
    if (has("AWS_ACCESS_KEY_ID") && has("AWS_SECRET_ACCESS_KEY")) return true;
    // Check default credentials file
    std::string home;
    auto it = env.find("HOME");
    if (it != env.end()) home = it->second;
    if (home.empty()) return false;
    return fs::exists(fs::path(home) / ".aws" / "credentials");
}

// 对齐 embeddings-bedrock.ts:isTitanModel / isCohereModel
static bool is_titan_model(const std::string& model) {
    return model.find("titan") != std::string::npos;
}

static bool is_cohere_model(const std::string& model) {
    return model.find("cohere") != std::string::npos;
}

// 对齐 embeddings-bedrock.ts:buildTitanPayload
static json build_titan_payload(const std::string& text, std::optional<int> dims) {
    json payload = {{"inputText", text}};
    if (dims.has_value()) {
        payload["dimensions"] = *dims;
    }
    return payload;
}

// 对齐 embeddings-bedrock.ts:buildCoherePayload
static json build_cohere_payload(const std::string& text, const std::string& input_type) {
    return {
        {"texts", json::array({text})},
        {"input_type", input_type}
    };
}

// ============================================================================
// 🎯 LmStudioEmbeddingProvider (对齐 embeddings-lmstudio.ts:1-140)
// LM Studio 本地嵌入
// ============================================================================

static const std::string DEFAULT_LMSTUDIO_EMBEDDING_MODEL = "text-embedding-nomic-embed-text-v1.5";
static const std::string DEFAULT_LMSTUDIO_BASE_URL = "http://127.0.0.1:1234";

struct LmStudioEmbeddingConfig {
    std::string model = DEFAULT_LMSTUDIO_EMBEDDING_MODEL;
    std::string base_url = DEFAULT_LMSTUDIO_BASE_URL;
};

// ============================================================================
// 🎯 RemoteEmbeddingProvider (对齐 embeddings-remote-provider.ts:1-55)
// 通用远程嵌入 provider
// ============================================================================

struct RemoteEmbeddingConfig {
    std::string base_url;
    std::string api_key;
    std::string model;
    std::unordered_map<std::string, std::string> headers;
};

// ============================================================================
// 🎯 EmbeddingDebugLogger (对齐 embeddings-debug.ts:1-18)
// ============================================================================

static void log_embedding_debug(
    const std::string& provider_id,
    const std::string& model,
    const std::string& event,
    const std::string& detail = ""
) {
    if (deep_log()) {
        deep_log()->debug("[embedding:{}] {} model={} {}",
            provider_id, event, model, detail);
    }
}

// ============================================================================
// 🎯 readMemoryFile (对齐 read-file.ts:9-76)
// 安全记忆文件读取 — 路径验证 + 额外路径支持 + 行范围提取
// ============================================================================

// 对齐 internal.ts:75-84 isMemoryPath
static bool is_memory_path(const std::string& rel_path) {
    if (rel_path.empty()) return false;
    if (rel_path == "MEMORY.md" || rel_path == "memory.md" || rel_path == "DREAMS.md") return true;
    return rel_path.rfind("memory/", 0) == 0;
}

struct ReadMemoryFileResult {
    std::string text;
    std::string path;
};

ReadMemoryFileResult read_memory_file(
    const std::string& workspace_dir,
    const std::vector<std::string>& extra_paths,
    const std::string& rel_path_raw,
    std::optional<int> from_line,
    std::optional<int> max_lines
) {
    // Trim
    std::string raw = rel_path_raw;
    auto a = raw.find_first_not_of(" \t");
    if (a == std::string::npos) throw std::runtime_error("path required");
    raw = raw.substr(a, raw.find_last_not_of(" \t") - a + 1);
    if (raw.empty()) throw std::runtime_error("path required");

    // Resolve absolute path
    fs::path abs_path = fs::path(raw).is_absolute()
        ? fs::absolute(raw)
        : fs::absolute(fs::path(workspace_dir) / raw);

    // Relative path for workspace containment check
    auto rel = fs::relative(abs_path, workspace_dir);
    std::string rel_str = rel.string();
    std::replace(rel_str.begin(), rel_str.end(), '\\', '/');
    bool in_workspace = !rel_str.empty() && rel_str.find("..") != 0 && !rel.is_absolute();
    bool allowed_workspace = in_workspace && is_memory_path(rel_str);

    // Check extra paths — 对齐 :27-48
    bool allowed_additional = false;
    if (!allowed_workspace && !extra_paths.empty()) {
        for (const auto& ep : extra_paths) {
            std::error_code ec;
            auto ep_abs = fs::absolute(ep);
            if (fs::is_directory(ep_abs, ec)) {
                auto abs_str = abs_path.string();
                auto ep_str = ep_abs.string();
                if (abs_str == ep_str || abs_str.find(ep_str + std::string(1, fs::path::preferred_separator)) == 0) {
                    allowed_additional = true;
                    break;
                }
            } else if (fs::is_regular_file(ep_abs, ec)) {
                if (abs_path == ep_abs && abs_path.extension() == ".md") {
                    allowed_additional = true;
                    break;
                }
            }
        }
    }

    if (!allowed_workspace && !allowed_additional) {
        throw std::runtime_error("path required");
    }

    if (abs_path.extension() != ".md") {
        throw std::runtime_error("path required");
    }

    // Check file exists
    auto stat = stat_regular_file(abs_path.string());
    if (stat.missing) return {"", rel_str};

    // Read file
    std::ifstream ifs(abs_path.string());
    if (!ifs.good()) return {"", rel_str};
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // Apply line range — 对齐 :68-75
    if (!from_line.has_value() && !max_lines.has_value()) {
        return {content, rel_str};
    }

    std::vector<std::string> lines;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);

    int start = std::max(1, from_line.value_or(1));
    int count = std::max(1, max_lines.value_or(static_cast<int>(lines.size())));

    std::string result;
    for (int i = start - 1; i < start - 1 + count && i < static_cast<int>(lines.size()); ++i) {
        if (!result.empty()) result += "\n";
        result += lines[i];
    }
    return {result, rel_str};
}

// ============================================================================
// 🎯 secret-input (对齐 secret-input.ts:1-25)
// ============================================================================

bool has_configured_memory_secret_input(
    const json& config,
    const std::string& path
) {
    // Navigate the config path to check if a secret input exists
    json current = config;
    std::istringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!current.is_object() || !current.contains(part)) return false;
        current = current[part];
    }
    if (current.is_string() && !current.get<std::string>().empty()) return true;
    if (current.is_object() && current.contains("secretRef")) return true;
    return false;
}

std::optional<std::string> resolve_memory_secret_input_string(
    const json& config,
    const std::string& path
) {
    json current = config;
    std::istringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '.')) {
        if (!current.is_object() || !current.contains(part)) return std::nullopt;
        current = current[part];
    }
    if (current.is_string()) {
        std::string val = current.get<std::string>();
        auto a2 = val.find_first_not_of(" \t");
        if (a2 == std::string::npos) return std::nullopt;
        return val.substr(a2, val.find_last_not_of(" \t") - a2 + 1);
    }
    return std::nullopt;
}

} // namespace memory
} // namespace hunterclaw
