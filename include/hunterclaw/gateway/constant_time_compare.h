#pragma once

#include <string>
#include <cstdint>
#include <cstring>

namespace hunterclaw {
namespace gateway {

/**
 * 🔒 常量时间字符串比较 (防止时序攻击)
 * 
 * ## 为什么需要常量时间比较？
 * 
 * 普通的字符串比较（如 `==` 或 `strcmp`）会在发现第一个不匹配字符时立即返回，
 * 导致比较时间与字符串内容相关：
 * 
 * ```cpp
 * // ❌ 不安全的比较（早期退出）
 * bool unsafe_compare(const string& a, const string& b) {
 *     if (a.size() != b.size()) return false;
 *     for (size_t i = 0; i < a.size(); i++) {
 *         if (a[i] != b[i]) return false;  // ❌ 第一个不匹配就返回！
 *     }
 *     return true;
 * }
 * ```
 * 
 * **时序攻击示例**:
 * 
 * 假设正确的 API Key 是 "sk-correct-key-12345"
 * 
 * | 攻击尝试 | 比较用时 | 攻击者推断 |
 * |---------|---------|-----------|
 * | "ab-xxx" | 1 纳秒 | 第一个字符就错了 |
 * | "sk-xxx" | 3 纳秒 | 前两个字符正确！ |
 * | "sk-cor" | 6 纳秒 | 前六个字符正确！ |
 * | "sk-correct-key-12345" | 全部时间 | ✅ 密钥正确 |
 * 
 * 攻击者通过测量响应时间，逐字符猜测密钥，将暴力破解从 O(n^m) 降低到 O(n*m)！
 * 
 * ## 解决方案：常量时间比较
 * 
 * 无论字符串是否匹配，始终比较完整个字符串，确保执行时间恒定。
 * 
 * OpenClaw 参考:
 * - src/gateway/compare-fixed-time.ts
 * - Node.js crypto.timingSafeEqual()
 * 
 * @param a 字符串 A
 * @param b 字符串 B
 * @return true 如果完全相等，否则 false
 */
inline bool constant_time_compare(const std::string& a, const std::string& b) {
    // 步骤 1: 长度检查（这里可以快速返回，因为长度通常不是秘密）
    // 注意：某些极端安全场景可能需要隐藏长度信息，但大多数情况下不需要
    if (a.size() != b.size()) {
        return false;
    }
    
    // 步骤 2: 常量时间比较
    // 使用异或（XOR）累积差异，而不是早期退出
    uint8_t result = 0;
    
    for (size_t i = 0; i < a.size(); i++) {
        // 关键：使用位运算累积差异，而不是条件跳转
        result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
        //        ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        //        XOR 结果：相同字符 = 0，不同字符 = 非零值
        //        使用 |= 累积所有差异，而不会提前退出循环
    }
    
    // 步骤 3: 返回结果
    // result == 0 表示所有字符都相同
    // result != 0 表示至少有一个字符不同
    return result == 0;
}

/**
 * 常量时间字符串比较（C 字符串版本）
 * 
 * @param a 字符串 A
 * @param b 字符串 B
 * @param len 比较长度
 * @return true 如果前 len 个字符完全相等，否则 false
 */
inline bool constant_time_compare_n(const char* a, const char* b, size_t len) {
    uint8_t result = 0;
    
    for (size_t i = 0; i < len; i++) {
        result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    
    return result == 0;
}

/**
 * 常量时间字节数组比较
 * 
 * @param a 字节数组 A
 * @param b 字节数组 B
 * @param len 比较长度
 * @return true 如果完全相等，否则 false
 * 
 * 对齐标准库实现:
 * - memcmp_s (C11)
 * - crypto.timingSafeEqual (Node.js)
 * - hmac.compare_digest (Python)
 */
inline bool constant_time_memcmp(const void* a, const void* b, size_t len) {
    const uint8_t* pa = static_cast<const uint8_t*>(a);
    const uint8_t* pb = static_cast<const uint8_t*>(b);
    
    uint8_t result = 0;
    
    for (size_t i = 0; i < len; i++) {
        result |= pa[i] ^ pb[i];
    }
    
    return result == 0;
}

/**
 * 示例：验证 API Key
 */
inline bool verify_api_key(const std::string& provided_key, const std::string& expected_key) {
    // ✅ 使用常量时间比较
    return constant_time_compare(provided_key, expected_key);
    
    // ❌ 不要使用普通比较！
    // return provided_key == expected_key;  // 不安全！
}

/**
 * 示例：验证 HMAC 签名
 */
inline bool verify_hmac_signature(
    const std::string& provided_signature,
    const std::string& computed_signature
) {
    // ✅ 使用常量时间比较
    return constant_time_compare(provided_signature, computed_signature);
}

} // namespace gateway
} // namespace hunterclaw
