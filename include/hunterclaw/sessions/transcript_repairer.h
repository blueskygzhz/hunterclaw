#pragma once

#include "hunterclaw/core/result.h"
#include "hunterclaw/llm/llm_client.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace sessions {
namespace repair {

/**
 * @brief 修复操作类型
 */
enum class RepairActionType {
    Insert,   // 插入合成 tool result
    Move,     // 移动 tool result 到正确位置
    Delete    // 删除孤立/重复的 tool result
};

/**
 * @brief 修复操作
 */
struct RepairAction {
    RepairActionType type;
    int index;                                 // 操作位置
    std::optional<int> target_index;           // 移动目标位置
    std::optional<llm::LLMMessage> message;    // 插入的消息
    std::string reason;                        // 修复原因
};

/**
 * @brief 修复报告
 */
struct RepairReport {
    std::vector<llm::LLMMessage> messages;     // 修复后的消息
    
    // 统计信息
    int added_synthetic_results = 0;           // 添加的合成结果
    int dropped_duplicate_results = 0;         // 删除的重复结果
    int dropped_orphan_results = 0;            // 删除的孤立结果
    int dropped_invalid_tool_calls = 0;        // 删除的无效 tool calls
    bool messages_moved = false;               // 是否有消息移动
    
    std::vector<RepairAction> actions;         // 执行的修复操作
};

/**
 * @brief Transcript 修复器
 * 
 * 对齐 OpenClaw 的 session-transcript-repair.ts
 */
class TranscriptRepairer {
public:
    /**
     * @brief 修复 Tool Call/Result 配对
     * 
     * 对齐 OpenClaw: repairToolUseResultPairing()
     * 
     * 功能：
     * 1. 移动 toolResult 紧跟在对应的 assistant toolCall 之后
     * 2. 插入合成的 error toolResult（缺失的情况）
     * 3. 删除重复的 toolResult（同一 id）
     * 4. 删除孤立的 toolResult（没有匹配的 toolCall）
     */
    static RepairReport repair_tool_use_pairing(
        const std::vector<llm::LLMMessage>& messages,
        bool allow_synthetic_results = true
    );
    
    /**
     * @brief 修复 Tool Call 输入
     * 
     * 对齐 OpenClaw: repairToolCallInputs()
     * 
     * 功能：
     * 1. 删除无效的 tool call（缺少 input/id/name）
     * 2. 删除不在白名单的 tool calls
     * 3. 脱敏 sessions_spawn 的 attachments
     */
    static RepairReport repair_tool_call_inputs(
        const std::vector<llm::LLMMessage>& messages,
        const std::vector<std::string>& tool_whitelist
    );
    
    /**
     * @brief 应用 Google Turn Ordering
     * 
     * 对齐 OpenClaw: applyGoogleTurnOrdering()
     * 
     * Google 要求: user → assistant → user → assistant...
     * 移除连续的 assistant 消息（只保留最后一条）
     */
    static std::vector<llm::LLMMessage> apply_google_turn_ordering(
        const std::vector<llm::LLMMessage>& messages
    );
    
    /**
     * @brief 验证 Anthropic 消息格式
     * 
     * 对齐 OpenClaw: validateAnthropicTurns()
     * 
     * Anthropic 要求:
     * 1. 第一条消息必须是 user
     * 2. user/assistant 交替
     * 3. toolResult 紧跟在 assistant toolCall 之后
     */
    static std::vector<llm::LLMMessage> validate_anthropic_turns(
        const std::vector<llm::LLMMessage>& messages
    );
    
    /**
     * @brief 验证 Gemini 消息格式
     * 
     * 对齐 OpenClaw: validateGeminiTurns()
     */
    static std::vector<llm::LLMMessage> validate_gemini_turns(
        const std::vector<llm::LLMMessage>& messages
    );
    
    /**
     * @brief 创建合成的 tool result（错误）
     */
    static llm::LLMMessage create_synthetic_tool_result(
        const std::string& tool_call_id,
        const std::string& tool_name,
        const std::string& error_message = "Error: tool result missing"
    );
    
    /**
     * @brief 检查 tool call 是否有效
     */
    static bool is_valid_tool_call(
        const core::JsonValue& tool_call,
        const std::vector<std::string>& tool_whitelist
    );
    
private:
    /**
     * @brief 应用修复操作
     */
    static std::vector<llm::LLMMessage> apply_repairs(
        const std::vector<llm::LLMMessage>& messages,
        const std::vector<RepairAction>& actions
    );
    
    /**
     * @brief 构建 Tool Call 索引 (id -> message index)
     */
    static std::map<std::string, int> build_tool_call_index(
        const std::vector<llm::LLMMessage>& messages
    );
    
    /**
     * @brief 检查是否有重复的 tool result
     */
    static std::map<std::string, std::vector<int>> find_duplicate_tool_results(
        const std::vector<llm::LLMMessage>& messages
    );
};

} // namespace repair
} // namespace sessions
} // namespace hunterclaw
