#ifndef HUNTERCLAW_MEMORY_QMD_PROCESS_H_
#define HUNTERCLAW_MEMORY_QMD_PROCESS_H_

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// CLI Process Management
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 对齐: openclaw-analysis/src/memory/qmd-process.ts

/**
 * CLI spawn invocation configuration
 */
struct CliSpawnInvocation {
    std::string command;               // Command to execute
    std::vector<std::string> argv;     // Command arguments
    bool shell = false;                // Use shell
    bool windows_hide = false;         // Hide window on Windows
};

/**
 * Resolve spawn invocation for cross-platform execution
 * 
 * @param command Command name (e.g., "qmd")
 * @param args Command arguments
 * @param env Environment variables
 * @param package_name Package name for resolution
 */
CliSpawnInvocation resolve_cli_spawn_invocation(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::map<std::string, std::string>& env,
    const std::string& package_name
);

/**
 * CLI command execution result
 */
struct CliCommandResult {
    std::string stdout_text;
    std::string stderr_text;
    int exit_code = 0;
};

/**
 * Run CLI command with timeout and output limits
 * 
 * @param command_summary Summary for error messages
 * @param invocation Spawn configuration
 * @param env Environment variables
 * @param cwd Working directory
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @param max_output_chars Maximum output characters
 * @param discard_stdout Whether to discard stdout
 * @return Command result
 * @throws std::runtime_error on failure or timeout
 */
CliCommandResult run_cli_command(
    const std::string& command_summary,
    const CliSpawnInvocation& invocation,
    const std::map<std::string, std::string>& env,
    const std::string& cwd,
    int timeout_ms = 0,
    size_t max_output_chars = 200000,
    bool discard_stdout = false
);

/**
 * Append output with character cap
 * Returns truncated flag if limit exceeded
 */
struct OutputWithCap {
    std::string text;
    bool truncated;
};

OutputWithCap append_output_with_cap(
    const std::string& current,
    const std::string& chunk,
    size_t max_chars
);

}  // namespace memory
}  // namespace hunterclaw

#endif  // HUNTERCLAW_MEMORY_QMD_PROCESS_H_
