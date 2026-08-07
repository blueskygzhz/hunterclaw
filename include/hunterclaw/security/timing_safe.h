#ifndef HUNTERCLAW_SECURITY_TIMING_SAFE_H
#define HUNTERCLAW_SECURITY_TIMING_SAFE_H

#include <string>
#include <cstddef>
#include <algorithm>

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

/**
 * @brief 常量时间比较两个可选 UTF-8 secret, 且不通过耗时泄漏长度
 *
 * 🎯 严格对齐 OpenClaw: src/security/secret-equal.ts safeEqualSecret()
 *
 * 与 timing_safe_equal 的区别: 后者在长度不等时立即返回, 会通过耗时旁路
 * 泄漏"长度是否相等"。本函数先把两个 secret 按 max 长度零填充再做常量时间
 * 比较, 使耗时与内容/长度无关; 最后再用长度相等作为额外条件 (padding 后
 * 内容相等但原始长度不同的极端情形仍判不等)。
 */
inline bool safe_equal_secret(const std::string& provided,
                              const std::string& expected) {
    const size_t byte_length = std::max(provided.size(), expected.size());
    if (byte_length == 0) {
        return true;  // 两者皆空 → 相等 (对齐 OpenClaw)
    }

    // 按 max 长度零填充, 常量时间比较全长
    unsigned char result = 0;
    for (size_t i = 0; i < byte_length; ++i) {
        unsigned char pa = i < provided.size()
            ? static_cast<unsigned char>(provided[i]) : 0;
        unsigned char pb = i < expected.size()
            ? static_cast<unsigned char>(expected[i]) : 0;
        result |= pa ^ pb;
    }

    return result == 0 && provided.size() == expected.size();
}

} // namespace security
} // namespace hunterclaw

#endif // HUNTERCLAW_SECURITY_TIMING_SAFE_H
