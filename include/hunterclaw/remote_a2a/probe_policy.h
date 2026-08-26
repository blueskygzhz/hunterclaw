#pragma once

// ============================================================================
// remote_a2a/probe_policy.h
// 退避探测策略。纯值类型，无状态、无 IO。
//
// ⚠ 这不是「轮询」，三点区别：
//   ① 只在 SSE 静默超过 idle_timeout 后才启动（正常路径完全不探测）
//   ② 指数退避，不是固定频率
//   ③ 有次数上限，max_no_progress 次无推进即判 Zombie，不无限循环
// ============================================================================

#include <algorithm>
#include <cstdint>

namespace hunterclaw {
namespace remote_a2a {

struct ProbePolicy {
    /// 首次探测间隔
    int64_t base_interval_ms = 5000;
    /// 退避上限
    int64_t max_interval_ms = 60000;
    /// 连续无推进次数上限 → 判 Zombie。
    /// ⚠ 至少 2 —— 设 1 会因单次抖动误判。
    int max_no_progress = 6;

    /// 指数退避：5s, 10s, 20s, 40s, 60s, 60s...
    /// ⚠ 必须钳制移位次数，否则 n 较大时 `<<` 未定义行为/溢出为负。
    int64_t interval_for(int n) const {
        if (n < 0) n = 0;
        if (n >= 40) return max_interval_ms;  // 提前短路，避免移位溢出
        int64_t v = base_interval_ms << n;
        if (v <= 0 || v > max_interval_ms) return max_interval_ms;
        return v;
    }

    /// 累计到判 Zombie 的总探测耗时（用于配置校验与文档）
    int64_t total_probe_budget_ms() const {
        int64_t sum = 0;
        for (int i = 0; i < max_no_progress; ++i) sum += interval_for(i);
        return sum;
    }
};

}  // namespace remote_a2a
}  // namespace hunterclaw
