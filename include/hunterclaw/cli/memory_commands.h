#pragma once

#include "hunterclaw/cli/command.h"

namespace hunterclaw {
namespace cli {

// ============================================================================
// Memory CLI 命令 — 严格对齐 OpenClaw extensions/memory-core/src/cli.ts
//
// OpenClaw memory CLI 只有以下子命令:
//   - memory search <query>     → 搜索记忆
//   - memory get <path>         → 读取记忆文件
//   - memory status             → 显示记忆系统状态
//   - memory promote            → 手动触发短期记忆晋升 (dreaming)
//
// 以下命令在 OpenClaw 中不存在，已删除:
//   - memory:list   (文件列表 → OpenClaw 通过 agents.files.list 实现)
//   - memory:stats  (统计 → OpenClaw 通过 doctor.memory.status 实现)
//   - memory:compact (压缩 → OpenClaw 由自动定时器或 sync 触发)
//   - memory:reindex (重索引 → OpenClaw 由 sync({ force: true }) 自动触发)
//   - memory:config  (配置 → OpenClaw 通过 config.get gateway 方法)
// ============================================================================

// 🎯 对齐 OpenClaw: memory search <query>
class MemorySearchCommand : public Command {
public:
    std::string name() const override { return "memory:search"; }
    std::string description() const override { 
        return "Search memory files semantically";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

// 🎯 对齐 OpenClaw: memory get <path>
class MemoryGetCommand : public Command {
public:
    std::string name() const override { return "memory:get"; }
    std::string description() const override { 
        return "Get memory file content";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

// 🎯 对齐 OpenClaw: memory status
class MemoryStatusCommand : public Command {
public:
    std::string name() const override { return "memory:status"; }
    std::string description() const override { 
        return "Show memory system status (provider, files, chunks, vector, fts)";
    }
    
    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

// 🎯 对齐 OpenClaw: memory promote
class MemoryPromoteCommand : public Command {
public:
    std::string name() const override { return "memory:promote"; }
    std::string description() const override {
        return "Rank short-term recalls and optionally append top entries to MEMORY.md";
    }

    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

// 🎯 对齐 OpenClaw: memory rem-harness
class MemoryRemHarnessCommand : public Command {
public:
    std::string name() const override { return "memory:rem-harness"; }
    std::string description() const override {
        return "Preview REM reflections, candidate truths, and deep promotions without writing";
    }

    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

// 🎯 对齐 OpenClaw: memory rem-backfill
class MemoryRemBackfillCommand : public Command {
public:
    std::string name() const override { return "memory:rem-backfill"; }
    std::string description() const override {
        return "Write grounded historical REM summaries into DREAMS.md for UI review";
    }

    std::vector<CommandOption> get_options() const override;
    int execute(const CommandArgs& cmd_args) override;
};

} // namespace cli
} // namespace hunterclaw
