#include "hunterclaw/sessions/transcript_io.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), PluginHook* types
#include "hunterclaw/core/utils.h"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <set>
#include <unordered_map>

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
        
        // 4. Idempotency key (如果提供)
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
        
        // 5. 生成 message_id
        std::string message_id = core::utils::generate_uuid();

        // 6. 构建 JSONL 行 (before_message_write hook 前)
        json line;
        line["type"] = "message";
        line["id"] = message_id;
        line["message"] = message;
        // ✅ 对齐 OpenClaw: 实现 parentId 链 (读取最后一条消息的 id)
        // 从文件末尾读取最后一行，获取上一条消息的 id
        {
            std::ifstream rf(transcript_path);
            if (rf.is_open()) {
                std::string last_line, cur_line;
                while (std::getline(rf, cur_line)) {
                    if (!cur_line.empty()) last_line = cur_line;
                }
                if (!last_line.empty()) {
                    try {
                        auto last_json = json::parse(last_line);
                        if (last_json.contains("id") && last_json["id"].is_string()) {
                            line["parentId"] = last_json["id"].get<std::string>();
                        }
                    } catch (...) {}
                }
            }
        }

        // =====================================================================
        // Hook: before_message_write
        // Aligned with OpenClaw sessions/transcript.ts — sync hook
        // Allows plugins to block or modify messages before writing to JSONL.
        // =====================================================================
        auto* hook_mgr = tools::get_global_hook_manager();
        std::string line_json = line.dump();
        if (hook_mgr && hook_mgr->has_before_message_write_hooks()) {
            tools::PluginHookBeforeMessageWriteEvent bmw_event;
            bmw_event.message_json = line_json;
            try {
                auto bmw_result = hook_mgr->run_before_message_write(bmw_event);
                if (bmw_result) {
                    if (bmw_result->block) {
                        // Message blocked by plugin — return success without writing
                        log()->debug("Message blocked by before_message_write hook: {}", transcript_path);
                        return { true, message_id, "" };
                    }
                    if (bmw_result->message_json) {
                        // Use modified message JSON
                        line_json = *bmw_result->message_json;
                    }
                }
            } catch (const std::exception& e) {
                log()->warn("before_message_write hook failed: {}", e.what());
            }
        }

        // 7. 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }

        ofs << line_json << "\n";
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

        // =====================================================================
        // Hook: before_message_write
        // Aligned with OpenClaw sessions/transcript.ts — sync hook
        // Allows plugins to block or modify user messages before writing.
        // =====================================================================
        auto* hook_mgr = tools::get_global_hook_manager();
        std::string line_json = line.dump();
        if (hook_mgr && hook_mgr->has_before_message_write_hooks()) {
            tools::PluginHookBeforeMessageWriteEvent bmw_event;
            bmw_event.message_json = line_json;
            try {
                auto bmw_result = hook_mgr->run_before_message_write(bmw_event);
                if (bmw_result) {
                    if (bmw_result->block) {
                        log()->debug("User message blocked by before_message_write hook");
                        return { true, message_id, "" };
                    }
                    if (bmw_result->message_json) {
                        line_json = *bmw_result->message_json;
                    }
                }
            } catch (const std::exception& e) {
                log()->warn("before_message_write hook failed: {}", e.what());
            }
        }

        // 5. 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }

        ofs << line_json << "\n";
        ofs.close();

        log()->debug("Appended user message to transcript: {} (id: {})",
            transcript_path, message_id);

        return { true, message_id, "" };

    } catch (const std::exception& e) {
        return { false, "", e.what() };
    }
}

AppendTranscriptResult append_tool_result_to_transcript(
    const std::string& transcript_path,
    const AppendToolResultOptions& options
) {
    try {
        // 1. 检查文件是否存在
        if (!fs::exists(transcript_path)) {
            return { false, "", "Transcript file not found" };
        }

        // 2. 构建 toolResult 消息 (对齐 OpenClaw toolResult message format)
        json message;
        message["role"] = "tool";
        message["tool_call_id"] = options.tool_call_id;

        // Content: wrap result_json as tool result content
        json content_item;
        content_item["type"] = "tool_result";
        content_item["tool_use_id"] = options.tool_call_id;
        content_item["content"] = options.result_json;
        message["content"] = json::array({ content_item });

        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        message["timestamp"] = now_ms;

        if (!options.tool_name.empty()) {
            message["toolName"] = options.tool_name;
        }
        if (options.is_synthetic) {
            message["isSynthetic"] = true;
        }

        // 3. 生成 message_id
        std::string message_id = core::utils::generate_uuid();

        // 4. 构建 JSONL 行
        json line;
        line["type"] = "message";
        line["id"] = message_id;
        line["message"] = message;
        if (!options.run_id.empty()) {
            line["runId"] = options.run_id;
        }

        // =====================================================================
        // Hook: tool_result_persist
        // Aligned with OpenClaw sessions/transcript.ts — sync hook
        // Allows plugins to modify the toolResult message before writing to JSONL.
        // Called BEFORE writing; handler may return modified message_json.
        // =====================================================================
        auto* hook_mgr = tools::get_global_hook_manager();
        std::string line_json = line.dump();
        if (hook_mgr && hook_mgr->has_tool_result_persist_hooks()) {
            tools::PluginHookToolResultPersistEvent trp_event;
            trp_event.tool_name    = options.tool_name.empty()
                ? std::nullopt : std::make_optional(options.tool_name);
            trp_event.tool_call_id = options.tool_call_id.empty()
                ? std::nullopt : std::make_optional(options.tool_call_id);
            trp_event.message_json = line_json;
            trp_event.is_synthetic = options.is_synthetic;

            tools::PluginHookToolResultPersistContext trp_ctx;
            trp_ctx.tool_name    = trp_event.tool_name;
            trp_ctx.tool_call_id = trp_event.tool_call_id;

            try {
                auto trp_result = hook_mgr->run_tool_result_persist(trp_event, trp_ctx);
                if (trp_result && trp_result->message_json) {
                    // Plugin returned a modified message — use it
                    line_json = *trp_result->message_json;
                }
            } catch (const std::exception& e) {
                log()->warn("tool_result_persist hook failed: {}", e.what());
            }
        }

        // 5. Also run before_message_write (aligned with OpenClaw: toolResult goes
        //    through the same write pipeline as other messages)
        if (hook_mgr && hook_mgr->has_before_message_write_hooks()) {
            tools::PluginHookBeforeMessageWriteEvent bmw_event;
            bmw_event.message_json = line_json;
            try {
                auto bmw_result = hook_mgr->run_before_message_write(bmw_event);
                if (bmw_result) {
                    if (bmw_result->block) {
                        log()->debug("Tool result blocked by before_message_write hook");
                        return { true, message_id, "" };
                    }
                    if (bmw_result->message_json) {
                        line_json = *bmw_result->message_json;
                    }
                }
            } catch (const std::exception& e) {
                log()->warn("before_message_write hook failed (tool result): {}", e.what());
            }
        }

        // 6. 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }

        ofs << line_json << "\n";
        ofs.close();

        log()->debug("Appended tool result to transcript: {} (tool={}, id={})",
            transcript_path, options.tool_name, message_id);

        return { true, message_id, "" };

    } catch (const std::exception& e) {
        return { false, "", e.what() };
    }
}

// ============================================================================
// Transcript 读取 (对齐 OC buildSessionContext + firstKeptEntryId 跳过机制)
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
        
        // ── Pass 1: 读取所有行，找到最新的 compaction 条目 ──
        // 对齐 OC: buildSessionContext() 找到最新 compaction，跳过 firstKeptEntryId 之前的消息
        struct ParsedEntry {
            std::string raw_line;
            std::string type;        // "session" | "message" | "compaction"
            std::string id;
            std::string first_kept_entry_id;  // compaction 条目才有
            std::string summary;              // compaction 条目才有
            int tokens_before = 0;
        };
        
        std::vector<ParsedEntry> entries;
        std::string line;
        
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            
            ParsedEntry entry;
            entry.raw_line = line;
            
            try {
                json parsed = json::parse(line);
                entry.type = parsed.value("type", "");
                entry.id = parsed.value("id", "");
                
                if (entry.type == "compaction") {
                    entry.first_kept_entry_id = parsed.value("firstKeptEntryId", "");
                    entry.summary = parsed.value("summary", "");
                    entry.tokens_before = parsed.value("tokensBefore", 0);
                }
            } catch (...) {
                entry.type = "unknown";
            }
            
            entries.push_back(std::move(entry));
        }
        ifs.close();
        
        // ── Pass 2: 找到最新的 compaction 条目 ──
        int latest_compaction_idx = -1;
        for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
            if (entries[i].type == "compaction") {
                latest_compaction_idx = i;
                break;
            }
        }
        
        // ── Pass 3: 确定跳过哪些消息 ──
        // 对齐 OC: buildSessionContext() 跳过 firstKeptEntryId 之前的 message 条目
        std::string first_kept_id;
        if (latest_compaction_idx >= 0) {
            first_kept_id = entries[latest_compaction_idx].first_kept_entry_id;
        }
        
        // 找到 firstKeptEntryId 在 entries 中的位置
        int first_kept_idx = -1;
        if (!first_kept_id.empty()) {
            for (int i = 0; i < latest_compaction_idx; ++i) {
                if (entries[i].id == first_kept_id) {
                    first_kept_idx = i;
                    break;
                }
            }
        }
        
        // ── Pass 4: 构建消息列表 ──
        std::vector<llm::LLMMessage> all_messages;
        
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const auto& entry = entries[i];
            
            // 跳过 header
            if (entry.type == "session") continue;
            
            // 对齐 OC: compaction 条目 → 注入摘要作为 system 消息
            if (entry.type == "compaction") {
                if (!entry.summary.empty()) {
                    llm::LLMMessage summary_msg;
                    summary_msg.role = llm::LLMMessageRole::System;
                    summary_msg.content = "[Previous conversation summary]\n\n" + entry.summary;
                    all_messages.push_back(summary_msg);
                }
                continue;
            }
            
            // 对齐 OC: 如果有 compaction，跳过 firstKeptEntryId 之前的 message 条目
            if (latest_compaction_idx >= 0 && entry.type == "message" && i < latest_compaction_idx) {
                // 如果在 firstKeptEntryId 之前 → 跳过（已被摘要覆盖）
                if (first_kept_idx >= 0 && i < first_kept_idx) {
                    continue;
                }
                // 如果没找到 firstKeptEntryId (被 truncation 移除了) → firstKeptEntryId 之前的全跳
                if (first_kept_idx < 0 && !first_kept_id.empty()) {
                    continue;
                }
            }
            
            if (entry.type != "message") continue;
            
            try {
                json parsed = json::parse(entry.raw_line);
                if (!parsed.contains("message")) continue;
                
                json msg_json = parsed["message"];
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
                    continue;
                }
                
                if (msg_json.contains("content") && msg_json["content"].is_array()) {
                    std::string content_text;
                    for (const auto& item : msg_json["content"]) {
                        if (item.value("type", "") == "text") {
                            content_text += item.value("text", "");
                        }
                    }
                    msg.content = content_text;
                }
                
                all_messages.push_back(msg);
                
            } catch (const std::exception& e) {
                log()->warn("Failed to parse transcript line: {}", e.what());
            }
        }
        
        // 从后往前取 limit 条 (limit=0 表示全部)
        if (limit > 0 && static_cast<int>(all_messages.size()) > limit) {
            messages.assign(all_messages.end() - limit, all_messages.end());
        } else {
            messages = std::move(all_messages);
        }
        
        log()->debug("Read {} messages from transcript (compaction={}, firstKept={}): {}",
            messages.size(),
            latest_compaction_idx >= 0 ? "yes" : "no",
            first_kept_id.empty() ? "(none)" : first_kept_id,
            transcript_path);
        
    } catch (const std::exception& e) {
        log()->error("Failed to read transcript: {}", e.what());
    }
    
    return messages;
}

// ============================================================================
// Compaction 持久化 (对齐 OC sessionManager.appendCompaction)
// ============================================================================

AppendTranscriptResult append_compaction_to_transcript(
    const std::string& transcript_path,
    const AppendCompactionOptions& options
) {
    try {
        if (!fs::exists(transcript_path)) {
            return { false, "", "Transcript file not found" };
        }
        
        // 生成 compaction entry ID
        std::string compaction_id = core::utils::generate_uuid();
        
        // 读取最后一行获取 parentId
        std::string parent_id;
        {
            std::ifstream rf(transcript_path);
            if (rf.is_open()) {
                std::string last_line, cur_line;
                while (std::getline(rf, cur_line)) {
                    if (!cur_line.empty()) last_line = cur_line;
                }
                if (!last_line.empty()) {
                    try {
                        auto last_json = json::parse(last_line);
                        if (last_json.contains("id") && last_json["id"].is_string()) {
                            parent_id = last_json["id"].get<std::string>();
                        }
                    } catch (...) {}
                }
            }
        }
        
        // 构建 compaction JSONL 行
        // 对齐 OC: { type: "compaction", id, parentId, summary, firstKeptEntryId, tokensBefore, ... }
        json entry;
        entry["type"] = "compaction";
        entry["id"] = compaction_id;
        if (!parent_id.empty()) {
            entry["parentId"] = parent_id;
        } else {
            entry["parentId"] = nullptr;
        }
        entry["summary"] = options.summary;
        entry["firstKeptEntryId"] = options.first_kept_entry_id;
        entry["tokensBefore"] = options.tokens_before;
        
        if (options.tokens_after.has_value()) {
            entry["tokensAfter"] = *options.tokens_after;
        }
        if (options.details_json.has_value()) {
            try {
                entry["details"] = json::parse(*options.details_json);
            } catch (...) {
                entry["details"] = *options.details_json;
            }
        }
        
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        entry["timestamp"] = now_ms;
        
        // 追加到文件
        std::ofstream ofs(transcript_path, std::ios::app);
        if (!ofs.is_open()) {
            return { false, "", "Failed to open transcript file for appending" };
        }
        
        ofs << entry.dump() << "\n";
        ofs.close();
        
        log()->info("[compaction] Appended compaction entry to transcript: {} "
                    "(id={}, firstKeptEntryId={}, tokensBefore={})",
            transcript_path, compaction_id,
            options.first_kept_entry_id, options.tokens_before);
        
        return { true, compaction_id, "" };
        
    } catch (const std::exception& e) {
        return { false, "", e.what() };
    }
}

// ============================================================================
// Session Truncation (对齐 OC session-truncation.ts)
// ============================================================================

TruncationResult truncate_session_after_compaction(
    const std::string& transcript_path,
    bool truncate_after_compaction,
    const std::optional<std::string>& archive_path
) {
    TruncationResult result;
    
    if (!truncate_after_compaction) {
        result.reason = "truncation disabled";
        return result;
    }
    
    try {
        if (!fs::exists(transcript_path)) {
            result.reason = "file not found";
            return result;
        }
        
        // 获取文件大小
        result.bytes_before = static_cast<int64_t>(fs::file_size(transcript_path));
        
        // 读取所有行
        std::vector<std::string> raw_lines;
        {
            std::ifstream ifs(transcript_path);
            std::string line;
            while (std::getline(ifs, line)) {
                if (!line.empty()) raw_lines.push_back(line);
            }
        }
        
        if (raw_lines.empty()) {
            result.reason = "empty file";
            return result;
        }
        
        // 解析所有条目
        struct Entry {
            std::string raw;
            std::string type;
            std::string id;
            std::string parent_id;
            std::string first_kept_entry_id;
            bool is_header = false;
        };
        
        std::vector<Entry> entries;
        for (const auto& raw : raw_lines) {
            Entry e;
            e.raw = raw;
            try {
                json parsed = json::parse(raw);
                e.type = parsed.value("type", "");
                e.id = parsed.value("id", "");
                if (parsed.contains("parentId") && parsed["parentId"].is_string()) {
                    e.parent_id = parsed["parentId"].get<std::string>();
                }
                if (e.type == "compaction") {
                    e.first_kept_entry_id = parsed.value("firstKeptEntryId", "");
                }
                e.is_header = (e.type == "session");
            } catch (...) {
                e.type = "unknown";
            }
            entries.push_back(std::move(e));
        }
        
        // 找到最新的 compaction 条目
        int latest_compaction_idx = -1;
        for (int i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
            if (entries[i].type == "compaction") {
                latest_compaction_idx = i;
                break;
            }
        }
        
        if (latest_compaction_idx < 0) {
            result.reason = "no compaction entry found";
            return result;
        }
        
        if (latest_compaction_idx == 0) {
            result.reason = "compaction already at root";
            return result;
        }
        
        std::string first_kept_id = entries[latest_compaction_idx].first_kept_entry_id;
        
        // 对齐 OC: 收集被摘要的条目 ID (firstKeptEntryId 之前的)
        std::set<std::string> summarized_ids;
        for (int i = 0; i < latest_compaction_idx; ++i) {
            if (!first_kept_id.empty() && entries[i].id == first_kept_id) {
                break;  // 从这里开始是未摘要尾部
            }
            summarized_ids.insert(entries[i].id);
        }
        
        // 对齐 OC: 只移除被摘要的 message 条目，保留非 message 状态
        std::set<std::string> removed_ids;
        for (const auto& e : entries) {
            if (summarized_ids.count(e.id) && e.type == "message") {
                removed_ids.insert(e.id);
            }
        }
        
        if (removed_ids.empty()) {
            result.reason = "no entries to remove";
            return result;
        }
        
        // 对齐 OC: 可选归档
        if (archive_path.has_value() && !archive_path->empty()) {
            try {
                fs::create_directories(fs::path(*archive_path).parent_path());
                fs::copy_file(transcript_path, *archive_path,
                    fs::copy_options::overwrite_existing);
                log()->info("[session-truncation] Archived pre-truncation file to {}",
                    *archive_path);
            } catch (const std::exception& e) {
                log()->warn("[session-truncation] Failed to archive: {}", e.what());
            }
        }
        
        // 对齐 OC: 构建 id→entry map 用于 re-parenting
        std::unordered_map<std::string, std::string> parent_map;  // id → parentId
        for (const auto& e : entries) {
            if (!e.id.empty()) {
                parent_map[e.id] = e.parent_id;
            }
        }
        
        // 对齐 OC: 保留未被移除的条目，re-parent 必要时
        std::vector<std::string> kept_lines;
        for (const auto& e : entries) {
            if (removed_ids.count(e.id)) {
                continue;
            }
            
            // 对齐 OC: 沿 parent chain 找到最近的保留祖先
            std::string new_parent = e.parent_id;
            while (!new_parent.empty() && removed_ids.count(new_parent)) {
                auto it = parent_map.find(new_parent);
                new_parent = (it != parent_map.end()) ? it->second : "";
            }
            
            if (new_parent != e.parent_id && !e.is_header) {
                // Re-parent: 修改 JSON 中的 parentId
                try {
                    json parsed = json::parse(e.raw);
                    if (new_parent.empty()) {
                        parsed["parentId"] = nullptr;
                    } else {
                        parsed["parentId"] = new_parent;
                    }
                    kept_lines.push_back(parsed.dump());
                } catch (...) {
                    kept_lines.push_back(e.raw);  // fallback: 保留原始行
                }
            } else {
                kept_lines.push_back(e.raw);
            }
        }
        
        // 对齐 OC: 原子写入 (tmp + rename)
        std::string content;
        for (const auto& l : kept_lines) {
            content += l + "\n";
        }
        
        std::string tmp_file = transcript_path + ".truncate-tmp";
        {
            std::ofstream ofs(tmp_file);
            ofs << content;
        }
        fs::rename(tmp_file, transcript_path);
        
        result.truncated = true;
        result.entries_removed = static_cast<int>(removed_ids.size());
        result.bytes_after = static_cast<int64_t>(content.size());
        
        double reduction = result.bytes_before > 0
            ? (1.0 - static_cast<double>(result.bytes_after) / result.bytes_before) * 100.0
            : 0.0;
        
        log()->info("[session-truncation] Truncated session file: "
                    "entriesBefore={} entriesAfter={} removed={} "
                    "bytesBefore={} bytesAfter={} reduction={:.1f}%",
            entries.size(), kept_lines.size(), result.entries_removed,
            result.bytes_before, result.bytes_after, reduction);
        
    } catch (const std::exception& e) {
        result.reason = e.what();
        log()->warn("[session-truncation] Failed: {}", e.what());
    }
    
    return result;
}

}  // namespace sessions
}  // namespace hunterclaw
