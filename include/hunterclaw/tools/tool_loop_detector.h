// HunterClaw - High-Performance AI Agent Platform (C++)
// Copyright (c) 2025 HunterClaw Team
// 
// Tool 循环检测器 (100% 对齐 OpenClaw)
// 防止 Agent 陷入重复调用工具的循环模式

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <deque>
#include <functional>
#include "hunterclaw/core/types.h"

// Forward declarations
namespace hunterclaw {
namespace tools {
    struct ToolContext;
    struct ToolResult;
    struct BeforeHookOutcome;
}
}

namespace hunterclaw {
namespace tools {

/// Tool 调用记录
struct ToolCallRecord {
    std::string tool_name;
    core::JsonValue params;
    std::chrono::system_clock::time_point timestamp;
    bool success;
    std::string result_hash;  // 结果内容的 hash
};

/// 循环检测结果
struct LoopDetectionResult {
    bool is_looping;           // 是否检测到循环
    std::string pattern;       // 循环模式描述
    int loop_count;            // 循环次数
    std::vector<std::string> loop_sequence;  // 循环序列
    std::string suggestion;    // 建议操作
};

/// Tool 循环检测器配置
struct LoopDetectorConfig {
    // 窗口大小 (检查最近 N 次调用)
    size_t window_size = 20;
    
    // 重复阈值 (相同调用重复 N 次视为循环)
    int exact_repeat_threshold = 3;
    
    // 模式循环阈值 (A->B->C->A 重复 N 次)
    int pattern_repeat_threshold = 2;
    
    // 时间窗口 (秒) - 只检查这个时间范围内的调用
    int time_window_seconds = 300;  // 5 分钟
    
    // 是否考虑参数差异
    bool compare_params = true;
    
    // 是否考虑结果差异
    bool compare_results = false;
};

/// Tool 循环检测器 (100% 对齐 OpenClaw)
class ToolLoopDetector {
public:
    explicit ToolLoopDetector(const LoopDetectorConfig& config = LoopDetectorConfig{});
    
    /// 记录工具调用
    void record_call(
        const std::string& tool_name,
        const core::JsonValue& params,
        bool success,
        const std::string& result = ""
    );
    
    /// 检测循环
    LoopDetectionResult detect_loop();
    
    /// 清除历史记录
    void clear_history();
    
    /// 获取调用历史
    const std::deque<ToolCallRecord>& get_history() const;
    
    /// 设置配置
    void set_config(const LoopDetectorConfig& config);
    
    /// 获取配置
    const LoopDetectorConfig& get_config() const;

private:
    LoopDetectorConfig config_;
    std::deque<ToolCallRecord> call_history_;
    
    // 检测完全相同的重复调用
    LoopDetectionResult detect_exact_repeats();
    
    // 检测模式循环 (A->B->C->A)
    LoopDetectionResult detect_pattern_loops();
    
    // 检测乒乓循环 (A->B->A->B)
    LoopDetectionResult detect_ping_pong_loops();
    
    // 计算参数相似度
    bool params_similar(const core::JsonValue& p1, const core::JsonValue& p2);
    
    // 计算字符串 hash
    std::string compute_hash(const std::string& content);
    
    // 清理过期记录
    void cleanup_old_records();
};

/// 内置循环检测 Hook (可直接用于 ToolHookManager)
namespace builtin_loop_detectors {

/// 创建循环检测 Before Hook
/// 在工具执行前检测是否存在循环模式
std::function<BeforeHookOutcome(const std::string&, const core::JsonValue&, const ToolContext&)>
create_loop_detection_hook(std::shared_ptr<ToolLoopDetector> detector);

/// 创建循环记录 After Hook  
/// 在工具执行后记录调用历史
std::function<void(const std::string&, const core::JsonValue&, const ToolResult&, double, const std::string&)>
create_loop_recording_hook(std::shared_ptr<ToolLoopDetector> detector);

}  // namespace builtin_loop_detectors

}  // namespace tools
}  // namespace hunterclaw
