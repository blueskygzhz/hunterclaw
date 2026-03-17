#ifndef HUNTERCLAW_SECURITY_TIMING_SAFE_H
#define HUNTERCLAW_SECURITY_TIMING_SAFE_H

#include <string>
#include <cstddef>

namespace hunterclaw {
namespace security {

/**
 * @brief 常量时间字符串比较 - 防止时序攻击
 * 
 * 对齐 OpenClaw 的 timingSafeEqual() 实现
 * 
 * @param a 第一个字符串
 * @param b 第二个字符串
 * @return true 如果字符串相等
 * @return false 如果字符串不相等或长度不同
 * 
 * @note 即使字符串不相等,执行时间也保持一致,防止时序攻击
 * @note 参考: OpenClaw crypto.timingSafeEqual()
 */
inline bool timing_safe_equal(const std::string& a, const std::string& b) {
    // 🎯 OpenClaw 对齐: 长度不同立即返回 false
    if (a.size() != b.size()) {
        return false;
    }

    // 🎯 常量时间比较: 无论何时检测到差异,都继续完整遍历
    unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }

    // 🎯 只有 result 为 0 时才相等 (所有位都匹配)
    return result == 0;
}

/**
 * @brief 常量时间缓冲区比较 - 防止时序攻击
 * 
 * @param a 第一个缓冲区
 * @param b 第二个缓冲区
 * @param len 要比较的字节数
 * @return true 如果缓冲区内容相等
 * @return false 如果缓冲区内容不相等
 */
inline bool timing_safe_equal(const void* a, const void* b, size_t len) {
    const unsigned char* pa = static_cast<const unsigned char*>(a);
    const unsigned char* pb = static_cast<const unsigned char*>(b);

    unsigned char result = 0;
    for (size_t i = 0; i < len; ++i) {
        result |= pa[i] ^ pb[i];
    }

    return result == 0;
}

} // namespace security
} // namespace hunterclaw

#endif // HUNTERCLAW_SECURITY_TIMING_SAFE_H
