#include "hunterclaw/memory/session_files.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <iomanip>
#include <openssl/sha.h>

// 严格对齐 OpenClaw src/memory/session-files.ts

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

namespace {

// Hash text using SHA-256 (对齐 openclaw internal.ts:hashText)
std::string hash_text(const std::string& text) {
    // Use SHA-256 via OpenSSL
    unsigned char digest[32];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
}

} // anonymous namespace

// ============================================================================
// normalize_session_text (对齐 session-files.ts:39-44)
// ============================================================================
std::string normalize_session_text(const std::string& value) {
    if (value.empty()) {
        return "";
    }
    
    // Replace multiple whitespace with single space
    std::string result = value;
    
    // Replace \n+ with space
    result = std::regex_replace(result, std::regex("\\s*\\n+\\s*"), " ");
    
    // Replace multiple spaces with single space
    result = std::regex_replace(result, std::regex("\\s+"), " ");
    
    // Trim
    size_t start = result.find_first_not_of(" \t\r\n");
    size_t end = result.find_last_not_of(" \t\r\n");
    
    if (start == std::string::npos) {
        return "";
    }
    
    return result.substr(start, end - start + 1);
}

// ============================================================================
// redact_sensitive_text (简化版)
// ============================================================================
std::string redact_sensitive_text(const std::string& text) {
    // Simplified version - just return original text
    // TODO: Implement proper redaction logic
    return text;
}

// ============================================================================
// extract_session_text (对齐 session-files.ts:46-72)
// ============================================================================
std::string extract_session_text(const json& content) {
    // Case 1: string content (对齐 session-files.ts:47-50)
    if (content.is_string()) {
        std::string normalized = normalize_session_text(content.get<std::string>());
        return normalized.empty() ? "" : normalized;
    }
    
    // Case 2: array content (对齐 session-files.ts:51-72)
    if (!content.is_array()) {
        return "";
    }
    
    std::vector<std::string> parts;
    
    for (const auto& block : content) {
        if (!block.is_object()) {
            continue;
        }
        
        // Check type === "text"
        if (!block.contains("type") || !block["type"].is_string() ||
            block["type"].get<std::string>() != "text") {
            continue;
        }
        
        // Check text field
        if (!block.contains("text") || !block["text"].is_string()) {
            continue;
        }
        
        std::string normalized = normalize_session_text(block["text"].get<std::string>());
        if (!normalized.empty()) {
            parts.push_back(normalized);
        }
    }
    
    if (parts.empty()) {
        return "";
    }
    
    // Join with space
    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) oss << " ";
        oss << parts[i];
    }
    
    return oss.str();
}

// ============================================================================
// list_session_files_for_agent (对齐 session-files.ts:21-33)
// ============================================================================
core::Result<std::vector<std::string>> list_session_files_for_agent(
    const std::string& agent_id
) {
    // 对齐 openclaw: resolveSessionTranscriptsDirForAgent(agentId)
    // openclaw 路径: ~/.openclaw/sessions/{agentId}/transcripts
    // HunterClaw 路径: ~/.hunterclaw/sessions/{agentId}
    std::string dir;
    const char* home = std::getenv("HOME");
    if (home) {
        dir = std::string(home) + "/.hunterclaw/sessions/" + agent_id;
    } else {
        dir = ".hunterclaw/sessions/" + agent_id;
    }

    std::vector<std::string> result;

    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            return core::Result<std::vector<std::string>>::Ok(result);
        }

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::string filename = entry.path().filename().string();
            if (filename.size() > 6 && filename.substr(filename.size() - 6) == ".jsonl") {
                result.push_back(entry.path().string());
            }
        }

        return core::Result<std::vector<std::string>>::Ok(result);

    } catch (const std::exception& e) {
        // 对齐 openclaw: catch {} return []
        return core::Result<std::vector<std::string>>::Ok(result);
    }
}

// ============================================================================
// session_path_for_file (对齐 session-files.ts:35-37)
// ============================================================================
std::string session_path_for_file(const std::string& abs_path) {
    fs::path p(abs_path);
    std::string basename = p.filename().string();
    
    // Return "sessions/{basename}"
    std::string result = "sessions/" + basename;
    
    // Replace backslash with forward slash
    std::replace(result.begin(), result.end(), '\\', '/');
    
    return result;
}

// ============================================================================
// build_session_entry (对齐 session-files.ts:74-131)
// ============================================================================
core::Result<SessionFileEntry> build_session_entry(const std::string& abs_path) {
    try {
        // Get file stats (对齐 session-files.ts:76)
        if (!fs::exists(abs_path)) {
            return core::Result<SessionFileEntry>::Err("File not found: " + abs_path);
        }
        
        auto ftime = fs::last_write_time(abs_path);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        int64_t mtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            sctp.time_since_epoch()
        ).count();
        
        size_t size = fs::file_size(abs_path);
        
        // Read file (对齐 session-files.ts:77)
        std::ifstream file(abs_path);
        if (!file.is_open()) {
            return core::Result<SessionFileEntry>::Err("Cannot open file: " + abs_path);
        }
        
        std::string raw((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
        file.close();
        
        // Parse JSONL (对齐 session-files.ts:78-116)
        std::istringstream iss(raw);
        std::string line;
        std::vector<std::string> collected;
        std::vector<int> line_map;
        
        int jsonl_idx = 0;
        while (std::getline(iss, line)) {
            jsonl_idx++;
            
            // Skip empty lines
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }
            
            // Parse JSON line (对齐 session-files.ts:86-91)
            json record;
            try {
                record = json::parse(line);
            } catch (...) {
                continue;
            }
            
            // Check type === "message" (对齐 session-files.ts:92-98)
            if (!record.is_object() || !record.contains("type") ||
                record["type"].get<std::string>() != "message") {
                continue;
            }
            
            // Extract message (对齐 session-files.ts:99-107)
            if (!record.contains("message") || !record["message"].is_object()) {
                continue;
            }
            
            const auto& message = record["message"];
            if (!message.contains("role") || !message["role"].is_string()) {
                continue;
            }
            
            std::string role = message["role"].get<std::string>();
            if (role != "user" && role != "assistant") {
                continue;
            }
            
            // Extract text (对齐 session-files.ts:108-111)
            if (!message.contains("content")) {
                continue;
            }
            
            std::string text = extract_session_text(message["content"]);
            if (text.empty()) {
                continue;
            }
            
            // Redact and format (对齐 session-files.ts:112-115)
            std::string safe = redact_sensitive_text(text);
            std::string label = (role == "user") ? "User" : "Assistant";
            
            collected.push_back(label + ": " + safe);
            line_map.push_back(jsonl_idx);
        }
        
        // Build content (对齐 session-files.ts:117)
        std::ostringstream content_oss;
        for (size_t i = 0; i < collected.size(); ++i) {
            if (i > 0) content_oss << "\n";
            content_oss << collected[i];
        }
        std::string content = content_oss.str();
        
        // Build hash (对齐 session-files.ts:118-124)
        std::ostringstream hash_oss;
        hash_oss << content << "\n";
        for (size_t i = 0; i < line_map.size(); ++i) {
            if (i > 0) hash_oss << ",";
            hash_oss << line_map[i];
        }
        std::string hash_input = hash_oss.str();
        std::string hash_value = hash_text(hash_input);
        
        // Build entry (对齐 session-files.ts:118-126)
        SessionFileEntry entry;
        entry.path = session_path_for_file(abs_path);
        entry.abs_path = abs_path;
        entry.mtime_ms = mtime_ms;
        entry.size = size;
        entry.hash = hash_value;
        entry.content = content;
        entry.line_map = line_map;
        
        return core::Result<SessionFileEntry>::Ok(entry);
        
    } catch (const std::exception& e) {
        // 对齐 openclaw: log.debug + return null
        // 返回 Err 而非 null，调用方按 openclaw 语义处理（跳过该文件）
        return core::Result<SessionFileEntry>::Err(
            std::string("Failed reading session file: ") + e.what()
        );
    }
}

} // namespace memory
} // namespace hunterclaw
