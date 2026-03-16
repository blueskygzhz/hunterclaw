#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace hunterclaw {
namespace sessions {

static std::shared_ptr<spdlog::logger> log() {
    static auto logger = spdlog::get("hunterclaw");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("hunterclaw");
    }
    return logger;
}

// ============================================================================
// 路径解析
// ============================================================================

std::string resolve_transcript_path(
    const std::string& session_id,
    const std::string& agent_id,
    const std::string& sessions_dir
) {
    // 对齐 OpenClaw:
    // sessions_dir/agent-{agent_id}/session-{session_id}.jsonl
    
    std::string agent_folder = "agent-" + agent_id;
    std::string filename = "session-" + session_id + ".jsonl";
    
    fs::path path = fs::path(sessions_dir) / agent_folder / filename;
    return path.string();
}

// ============================================================================
// Transcript 文件初始化
// ============================================================================

EnsureTranscriptResult ensure_transcript_file(
    const std::string& transcript_path,
    const std::string& session_id
) {
    if (fs::exists(transcript_path)) {
        return { true, "" };
    }
    
    try {
        // 创建父目录
        fs::path path(transcript_path);
        fs::create_directories(path.parent_path());
        
        // 创建 header (对齐 OpenClaw)
        json header;
        header["type"] = "session";
        header["version"] = 5;  // CURRENT_SESSION_VERSION
        header["id"] = session_id;
        header["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
        header["cwd"] = fs::current_path().string();
        
        // 写入第一行
        std::ofstream ofs(transcript_path, std::ios::out);
        if (!ofs.is_open()) {
            return { false, "Failed to open file for writing" };
        }
        
        ofs << header.dump() << "\n";
        ofs.close();
        
        // 设置权限 0600 (只有所有者可读写)
        fs::permissions(transcript_path, 
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
        
        log()->debug("Created transcript file: {}", transcript_path);
        return { true, "" };
        
    } catch (const std::exception& e) {
        return { false, e.what() };
    }
}

// ============================================================================
// 幂等性检查
// ============================================================================

bool transcript_has_idempotency_key(
    const std::string& transcript_path,
    const std::string& idempotency_key
) {
    try {
        std::ifstream ifs(transcript_path);
        if (!ifs.is_open()) {
            return false;
        }
        
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.empty()) {
                continue;
            }
            
            try {
                json parsed = json::parse(line);
                if (parsed.contains("message")) {
                    auto msg = parsed["message"];
                    if (msg.contains("idempotencyKey") && 
                        msg["idempotencyKey"].get<std::string>() == idempotency_key) {
                        return true;
                    }
                }
            } catch (...) {
                continue;
            }
        }
        
        return false;
        
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Transcript 写入
// ============================================================================

AppendTranscriptResult append_assistant_message_to_transcript(
    const std::string& transcript_path,
    const AppendTranscriptOptions& options
) {
    try {
        // 1. 检查文件是否存在
        if (!fs::exists(transcript_path)) {
            if (!options.create_if_missing) {
                return { false, "", "Transcript file not found" };
            }
            
            // 从 transcript_path 提取 session_id (假设格式: session-{id}.jsonl)
            fs::path path(transcript_path);
            std::string filename = path.filename().string();
            std::string session_id = filename.substr(8, filename.size() - 14);  // "session-" + id + ".jsonl"
            
            auto ensure_result = ensure_transcript_file(transcript_path, session_id);
            if (!ensure_result.ok) {
                return { false, "", ensure_result.error };
            }
        }
        
        // 2. 幂等性检查
        if (!options.idempotency_key.empty() && 
            transcript_has_idempotency_key(transcript_path, options.idempotency_key)) {
            log()->debug("Message already exists (idempotency): {}", options.idempotency_key);
            return { true, "", "" };
        }
        
        // 3. 构建消息 (对齐 OpenClaw chat-transcript-inject.ts)
        json message;
        message["role"] = "assistant";
        
        // Content
        std::string text = options.message;
        if (!options.label.empty()) {
            text = "[" + options.label + "]\n\n" + text;
        }
        
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = text;
        message["content"] = json::array({ content_item });
        
        // Timestamp
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        message["timestamp"] = now_ms;
        
        // Usage (全为 0)
        json usage;
        usage["input"] = 0;
        usage["output"] = 0;
        usage["cacheRead"] = 0;
        usage["cacheWrite"] = 0;
        usage["totalTokens"] = 0;
        
        json cost;
        cost["input"] = 0;
        cost["output"] = 0;
        cost["cacheRead"] = 0;
        cost["cacheWrite"] = 0;
        cost["total"] = 0;
        usage["cost"] = cost;
        
        message["usage"] = usage;
        message["stopReason"] = "stop";
        
        // Provider metadata (标记为 gateway-injected)
        message["api"] = "openai-responses";
        message["provider"] = "hunterclaw";
        message["model"] = "gateway-injected";
        
        // Idempotency key (如果提供)
        if (!options.idempotency_key.empty()) {
            message["idempotencyKey"] = options.idempotency_key;
        }
        
        // Abort metadata (如果是 abort)
        if (options.aborted) {
            json abort_meta;
            abort_meta["aborted"] = true;
            abort_meta["origin"] = options.abort_origin;
            abort_meta["runId"] = options.abort_run_id;
            message["hunterclawAbort"] = abort_meta;
        }
        
        // 4. 生成 message_id
        std::string message_id = core::utils::generate_uuid();
        
        // 5. 构建 JSONL 行 (对齐 Pi SessionManager 格式)
        json line;
        line["type"] = "message";
        line["id"] = message_id;
        line["message"] = message;
        // TODO: 实现 parentId 链 (需要读取最后一条消息的 id)
        
        // 6. 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }
        
        ofs << line.dump() << "\n";
        ofs.close();
        
        log()->debug("Appended assistant message to transcript: {} (id: {})",
            transcript_path, message_id);
        
        return { true, message_id, "" };
        
    } catch (const std::exception& e) {
        return { false, "", e.what() };
    }
}

AppendTranscriptResult append_user_message_to_transcript(
    const std::string& transcript_path,
    const std::string& message,
    const std::string& run_id
) {
    try {
        // 1. 检查文件是否存在
        if (!fs::exists(transcript_path)) {
            return { false, "", "Transcript file not found" };
        }
        
        // 2. 构建用户消息
        json msg;
        msg["role"] = "user";
        
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = message;
        msg["content"] = json::array({ content_item });
        
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        msg["timestamp"] = now_ms;
        
        // 3. 生成 message_id
        std::string message_id = core::utils::generate_uuid();
        
        // 4. 构建 JSONL 行
        json line;
        line["type"] = "message";
        line["id"] = message_id;
        line["message"] = msg;
        if (!run_id.empty()) {
            line["runId"] = run_id;
        }
        
        // 5. 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }
        
        ofs << line.dump() << "\n";
        ofs.close();
        
        log()->debug("Appended user message to transcript: {} (id: {})",
            transcript_path, message_id);
        
        return { true, message_id, "" };
        
    } catch (const std::exception& e) {
        return { false, "", e.what() };
    }
}

// ============================================================================
// Transcript 读取
// ============================================================================

std::vector<llm::LLMMessage> read_transcript_messages(
    const std::string& transcript_path,
    int limit
) {
    std::vector<llm::LLMMessage> messages;
    
    try {
        if (!fs::exists(transcript_path)) {
            log()->warn("Transcript file not found: {}", transcript_path);
            return messages;
        }
        
        std::ifstream ifs(transcript_path);
        if (!ifs.is_open()) {
            log()->error("Failed to open transcript file: {}", transcript_path);
            return messages;
        }
        
        // 读取所有行 (跳过 header)
        std::vector<std::string> lines;
        std::string line;
        bool first = true;
        
        while (std::getline(ifs, line)) {
            if (line.empty()) {
                continue;
            }
            
            // 跳过 header 行 (type: "session")
            if (first) {
                first = false;
                try {
                    json parsed = json::parse(line);
                    if (parsed.value("type", "") == "session") {
                        continue;
                    }
                } catch (...) {
                    // 解析失败,继续
                }
            }
            
            lines.push_back(line);
        }
        
        ifs.close();
        
        // 从后往前取 limit 条
        int start = std::max(0, static_cast<int>(lines.size()) - limit);
        
        for (int i = start; i < static_cast<int>(lines.size()); ++i) {
            try {
                json parsed = json::parse(lines[i]);
                
                if (parsed.value("type", "") != "message") {
                    continue;
                }
                
                if (!parsed.contains("message")) {
                    continue;
                }
                
                json msg_json = parsed["message"];
                
                // 转换为 LLMMessage
                llm::LLMMessage msg;
                
                std::string role = msg_json.value("role", "");
                if (role == "user") {
                    msg.role = llm::LLMMessageRole::User;
                } else if (role == "assistant") {
                    msg.role = llm::LLMMessageRole::Assistant;
                } else if (role == "system") {
                    msg.role = llm::LLMMessageRole::System;
                } else if (role == "tool") {
                    msg.role = llm::LLMMessageRole::Tool;
                } else {
                    continue;  // 未知角色,跳过
                }
                
                // Content
                if (msg_json.contains("content") && msg_json["content"].is_array()) {
                    std::string content_text;
                    for (const auto& item : msg_json["content"]) {
                        if (item.value("type", "") == "text") {
                            content_text += item.value("text", "");
                        }
                    }
                    msg.content = content_text;
                }
                
                messages.push_back(msg);
                
            } catch (const std::exception& e) {
                log()->warn("Failed to parse transcript line: {}", e.what());
                continue;
            }
        }
        
        log()->debug("Read {} messages from transcript: {}", messages.size(), transcript_path);
        
    } catch (const std::exception& e) {
        log()->error("Failed to read transcript: {}", e.what());
    }
    
    return messages;
}

}  // namespace sessions
}  // namespace hunterclaw
