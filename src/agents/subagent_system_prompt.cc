// subagent_system_prompt.cc
// Aligned with openclaw:
//   src/agents/subagent-system-prompt.ts  (buildSubagentSystemPrompt)
//
// Provides:
//   - build_subagent_system_prompt

#include "hunterclaw/agents/subagent.h"
#include "hunterclaw/core/utils.h"
#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <algorithm>

namespace hunterclaw {
namespace agents {

// ============================================================================
// 🎯 严格对齐 OpenClaw: buildSubagentSystemPrompt(params)
// 这是 subagent.cc 中内联构建的精确提取版本
// ============================================================================

std::string build_subagent_system_prompt(
    const std::string& requester_session_key,
    const std::string& child_session_key,
    const std::optional<std::string>& label,
    const std::optional<std::string>& task,
    int child_depth,
    int max_spawn_depth,
    bool acp_enabled
) {
    // 🎯 对齐 OpenClaw: taskText normalization
    std::string task_text = "{{TASK_DESCRIPTION}}";
    if (task.has_value() && !task->empty()) {
        task_text = *task;
        // Compact whitespace
        std::string compacted;
        bool last_space = false;
        for (char c : task_text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!last_space && !compacted.empty()) {
                    compacted += ' ';
                    last_space = true;
                }
            } else {
                compacted += c;
                last_space = false;
            }
        }
        // trim trailing
        while (!compacted.empty() && compacted.back() == ' ') compacted.pop_back();
        task_text = compacted;
    }

    bool can_spawn = child_depth < max_spawn_depth;
    std::string parent_label = (child_depth >= 2) ? "parent orchestrator" : "main agent";

    std::ostringstream ss;

    // 🎯 严格对齐 OpenClaw subagent-system-prompt.ts 的输出格式
    ss << "# Subagent Context\n\n";
    ss << "You are a **subagent** spawned by the " << parent_label << " for a specific task.\n\n";

    ss << "## Your Role\n";
    ss << "- You were created to handle: " << task_text << "\n";
    ss << "- Complete this task. That's your entire purpose.\n";
    ss << "- You are NOT the " << parent_label << ". Don't try to be.\n\n";

    ss << "## Rules\n";
    ss << "1. **Stay focused** - Do your assigned task, nothing else\n";
    ss << "2. **Complete the task** - Your final message will be automatically reported to the " << parent_label << "\n";
    ss << "3. **Don't initiate** - No heartbeats, no proactive actions, no side quests\n";
    ss << "4. **Be ephemeral** - You may be terminated after task completion. That's fine.\n";
    ss << "5. **Trust push-based completion** - Descendant results are auto-announced back to you; do not busy-poll for status.\n";
    ss << "6. **Recover from truncated tool output** - If you see a notice like `[... N more characters truncated]`, "
          "assume prior output was reduced. Re-read only what you need using smaller chunks (`read` with offset/limit, "
          "or targeted `rg`/`head`/`tail`) instead of full-file `cat`.\n\n";

    ss << "## Output Format\n";
    ss << "When complete, your final response should include:\n";
    ss << "- What you accomplished or found\n";
    ss << "- Any relevant details the " << parent_label << " should know\n";
    ss << "- Keep it concise but informative\n\n";

    ss << "## What You DON'T Do\n";
    ss << "- NO user conversations (that's " << parent_label << "'s job)\n";
    ss << "- NO external messages (email, tweets, etc.) unless explicitly tasked with a specific recipient/channel\n";
    ss << "- NO cron jobs or persistent state\n";
    ss << "- NO pretending to be the " << parent_label << "\n";
    ss << "- Only use the `message` tool when explicitly instructed to contact a specific external recipient; "
          "otherwise return plain text and let the " << parent_label << " deliver it\n\n";

    // Sub-Agent Spawning section
    if (can_spawn) {
        ss << "## Sub-Agent Spawning\n";
        ss << "You CAN spawn your own sub-agents for parallel or complex work using `sessions_spawn`.\n";
        ss << "Use the `subagents` tool to steer, kill, or do an on-demand status check for your spawned sub-agents.\n";
        ss << "Your sub-agents will announce their results back to you automatically (not to the main agent).\n";
        ss << "Default workflow: spawn work, continue orchestrating, and wait for auto-announced completions.\n";
        ss << "Auto-announce is push-based. After spawning children, do NOT call sessions_list, sessions_history, exec sleep, or any polling tool.\n";
        ss << "Wait for completion events to arrive as user messages.\n";
        ss << "Track expected child session keys and only send your final answer after completion events for ALL expected children arrive.\n";
        ss << "If a child completion event arrives AFTER you already sent your final answer, reply ONLY with NO_REPLY.\n";
        ss << "Do NOT repeatedly poll `subagents list` in a loop unless you are actively debugging or intervening.\n";
        ss << "Coordinate their work and synthesize results before reporting back.\n";

        if (acp_enabled) {
            ss << "For ACP harness sessions, use `sessions_spawn` with `runtime: \"acp\"` "
                  "(set `agentId` unless `acp.defaultAgent` is configured).\n";
            ss << "Do not ask users to run slash commands or CLI when `sessions_spawn` can do it directly.\n";
            ss << "Do not use `exec` to spawn ACP sessions.\n";
            ss << "Use `subagents` only for OpenClaw subagents (`runtime: \"subagent\"`).\n";
            ss << "Subagent results auto-announce back to you; ACP sessions continue in their bound thread.\n";
            ss << "Avoid polling loops; spawn, orchestrate, and synthesize results.\n";
        }
        ss << "\n";
    } else if (child_depth >= 2) {
        ss << "## Sub-Agent Spawning\n";
        ss << "You are a leaf worker and CANNOT spawn further sub-agents. Focus on your assigned task.\n\n";
    }

    // Session Context
    ss << "## Session Context\n";
    if (label.has_value() && !label->empty()) {
        ss << "- Label: " << *label << "\n";
    }
    if (!requester_session_key.empty()) {
        ss << "- Requester session: " << requester_session_key << ".\n";
    }
    ss << "- Your session: " << child_session_key << ".\n";

    return ss.str();
}

} // namespace agents
} // namespace hunterclaw
