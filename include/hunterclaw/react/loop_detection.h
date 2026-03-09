#pragma once

#include "hunterclaw/core/json.h"
#include "hunterclaw/tools/tool.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

namespace hunterclaw {
namespace react {

// 对齐 OpenClaw 的工具循环检测
// 参考: openclaw-analysis/src/agents/tool-loop-detection.ts

// 检测器类型
enum class LoopDetectorKind {
    GenericRepeat,         // 通用重复检测
    KnownPollNoProgress,   // 无进展轮询检测
    GlobalCircuitBreaker,  // 全局断路器
    PingPong              // 乒乓循环检测 (A→B→A→B)
};

// 检测结果
struct LoopDetectionResult {
    bool stuck = false;                      // 是否陷入循环
    
    enum class Level {
        None,
        Warning,    // 警告级别
        Critical    // 严重级别 (应停止)
    } level = Level::None;
    
    LoopDetectorKind detector = LoopDetectorKind::GenericRepeat;
    int count = 0;                           // 重复次数
    std::string message;                     // 错误消息
    std::string paired_tool_name;            // 配对工具名 (for ping-pong)
    std::string warning_key;                 // 警告键
};

// 工具调用记录
struct ToolCallRecord {
    std::string tool_name;
    std::string params_hash;                 // 参数的哈希值
    int64_t timestamp_ms = 0;
    std::string tool_call_id;
    
    // 执行结果
    bool executed = false;
    bool success = false;
    std::string result_hash;                 // 结果的哈希值
    std::string error;
};

// 循环检测配置
struct LoopDetectionConfig {
    bool enabled = true;
    
    // 滑动窗口大小
    int history_size = 30;
    
    // 阈值
    int warning_threshold = 10;              // 警告: 10次相同调用
    int critical_threshold = 20;             // 严重: 20次无进展
    int global_circuit_breaker_threshold = 30;  // 熔断: 30次强制停止
    
    // 启用的检测器
    struct Detectors {
        bool generic_repeat = true;
        bool known_poll_no_progress = true;
        bool ping_pong = true;
    } detectors;
};

// 循环检测器状态
class LoopDetectionState {
public:
    static std::shared_ptr<LoopDetectionState> create(
        const LoopDetectionConfig& config = LoopDetectionConfig{}
    );
    
    // 检测工具调用是否陷入循环
    LoopDetectionResult detect_tool_call_loop(
        const std::string& tool_name,
        const core::JsonValue& params
    );
    
    // 记录工具调用
    void record_tool_call(
        const std::string& tool_name,
        const core::JsonValue& params,
        const std::string& tool_call_id
    );
    
    // 记录工具调用结果
    void record_tool_call_outcome(
        const std::string& tool_name,
        const tools::ToolResult& result,
        const std::string& error = ""
    );
    
    // 重置状态
    void reset();
    
    // 获取配置
    const LoopDetectionConfig& config() const { return config_; }
    
    // 统计信息
    int total_tool_calls() const { return history_.size(); }
    std::vector<std::string> recent_tool_names(int count = 10) const;
    
private:
    LoopDetectionState(const LoopDetectionConfig& config);
    
    // 检测器实现
    LoopDetectionResult detect_generic_repeat(
        const std::string& tool_name,
        const std::string& params_hash
    );
    
    LoopDetectionResult detect_known_poll_no_progress(
        const std::string& tool_name,
        const std::string& params_hash
    );
    
    LoopDetectionResult detect_global_circuit_breaker();
    
    LoopDetectionResult detect_ping_pong(
        const std::string& tool_name
    );
    
    // 计算哈希
    std::string compute_params_hash(const core::JsonValue& params);
    std::string compute_result_hash(const tools::ToolResult& result);
    
    LoopDetectionConfig config_;
    std::vector<ToolCallRecord> history_;    // 滑动窗口历史
    
    // 用于快速查找
    std::map<std::string, int> tool_name_counts_;           // 工具名计数
    std::map<std::string, int> tool_params_counts_;         // 工具名+参数计数
    std::map<std::string, int> no_progress_counts_;         // 无进展计数 (相同参数+相同结果)
};

// 辅助函数
namespace loop_detection {

// 创建工具循环摘要信息
std::string create_loop_summary(
    const LoopDetectionResult& result,
    const std::vector<ToolCallRecord>& recent_calls
);

// 检查是否为已知的轮询工具 (如 read_file, list_dir 等)
bool is_known_polling_tool(const std::string& tool_name);

} // namespace loop_detection

} // namespace react
} // namespace hunterclaw
