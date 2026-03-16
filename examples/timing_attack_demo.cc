/**
 * 时序攻击演示程序
 * 
 * 编译运行:
 * ```bash
 * g++ -std=c++17 -O2 timing_attack_demo.cc -o timing_attack_demo
 * ./timing_attack_demo
 * ```
 */

#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cstdint>

using namespace std;
using namespace std::chrono;

// ========================================
// 不安全的比较（早期退出）
// ========================================
bool unsafe_compare(const string& a, const string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] != b[i]) {
            return false;  // ❌ 第一个不匹配就返回！
        }
    }
    
    return true;
}

// ========================================
// 安全的常量时间比较
// ========================================
bool constant_time_compare(const string& a, const string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    
    uint8_t result = 0;
    
    for (size_t i = 0; i < a.size(); i++) {
        result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    
    return result == 0;
}

// ========================================
// 测量比较函数的执行时间
// ========================================
template<typename Func>
double measure_time(Func func, const string& a, const string& b, int iterations = 10000) {
    auto start = high_resolution_clock::now();
    
    for (int i = 0; i < iterations; i++) {
        func(a, b);
    }
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end - start).count();
    
    return static_cast<double>(duration) / iterations;  // 平均时间（纳秒）
}

// ========================================
// 演示 1: 时序攻击可行性
// ========================================
void demo_timing_attack() {
    cout << "=================================================\n";
    cout << "演示 1: 时序攻击可行性\n";
    cout << "=================================================\n\n";
    
    const string correct_key = "sk-prod-a7f3b2c1d9e8";
    
    cout << "正确的密钥: " << correct_key << "\n";
    cout << "密钥长度: " << correct_key.size() << " 字符\n\n";
    
    cout << "使用不安全的比较函数测试不同前缀匹配度:\n";
    cout << "------------------------------------------------------------\n";
    cout << "尝试密钥                     匹配字符  平均时间 (ns)  差异\n";
    cout << "------------------------------------------------------------\n";
    
    vector<string> test_keys = {
        "ab-xxxx-xxxxxxxxxx",  // 0 个字符匹配
        "sk-xxxx-xxxxxxxxxx",  // 2 个字符匹配
        "sk-prod-xxxxxxxxxx",  // 7 个字符匹配
        "sk-prod-a7xxxxxxxx",  // 9 个字符匹配
        "sk-prod-a7f3b2c1d9e8" // 完全匹配
    };
    
    double baseline = 0;
    
    for (size_t idx = 0; idx < test_keys.size(); idx++) {
        const auto& test_key = test_keys[idx];
        
        // 计算匹配字符数
        int matched = 0;
        for (size_t i = 0; i < min(test_key.size(), correct_key.size()); i++) {
            if (test_key[i] == correct_key[i]) {
                matched++;
            } else {
                break;
            }
        }
        
        // 测量时间
        double avg_time = measure_time(unsafe_compare, test_key, correct_key);
        
        if (idx == 0) {
            baseline = avg_time;
        }
        
        double diff = avg_time - baseline;
        
        cout << left << setw(30) << test_key 
             << setw(10) << matched
             << setw(16) << fixed << setprecision(2) << avg_time
             << (diff > 0 ? "+" : "") << fixed << setprecision(2) << diff
             << "\n";
    }
    
    cout << "\n💡 观察: 响应时间随着匹配字符数增加而增加！\n";
    cout << "   攻击者可以利用这一点逐字符猜测密钥。\n\n";
}

// ========================================
// 演示 2: 常量时间比较
// ========================================
void demo_constant_time() {
    cout << "=================================================\n";
    cout << "演示 2: 常量时间比较\n";
    cout << "=================================================\n\n";
    
    const string correct_key = "sk-prod-a7f3b2c1d9e8";
    
    cout << "使用常量时间比较函数测试不同前缀匹配度:\n";
    cout << "------------------------------------------------------------\n";
    cout << "尝试密钥                     匹配字符  平均时间 (ns)  差异\n";
    cout << "------------------------------------------------------------\n";
    
    vector<string> test_keys = {
        "ab-xxxx-xxxxxxxxxx",
        "sk-xxxx-xxxxxxxxxx",
        "sk-prod-xxxxxxxxxx",
        "sk-prod-a7xxxxxxxx",
        "sk-prod-a7f3b2c1d9e8"
    };
    
    double baseline = 0;
    
    for (size_t idx = 0; idx < test_keys.size(); idx++) {
        const auto& test_key = test_keys[idx];
        
        int matched = 0;
        for (size_t i = 0; i < min(test_key.size(), correct_key.size()); i++) {
            if (test_key[i] == correct_key[i]) {
                matched++;
            } else {
                break;
            }
        }
        
        double avg_time = measure_time(constant_time_compare, test_key, correct_key);
        
        if (idx == 0) {
            baseline = avg_time;
        }
        
        double diff = avg_time - baseline;
        
        cout << left << setw(30) << test_key 
             << setw(10) << matched
             << setw(16) << fixed << setprecision(2) << avg_time
             << (diff > 0 ? "+" : "") << fixed << setprecision(2) << diff
             << "\n";
    }
    
    cout << "\n✅ 观察: 响应时间几乎恒定，与匹配字符数无关！\n";
    cout << "   攻击者无法通过时序信息推断密钥内容。\n\n";
}

// ========================================
// 演示 3: 模拟暴力破解
// ========================================
void demo_brute_force() {
    cout << "=================================================\n";
    cout << "演示 3: 时序攻击 vs 暴力破解\n";
    cout << "=================================================\n\n";
    
    const string correct_key = "sk-abc";
    const string charset = "abcdefghijklmnopqrstuvwxyz-";
    
    cout << "目标密钥: " << correct_key << "\n";
    cout << "字符集: " << charset << " (" << charset.size() << " 个字符)\n";
    cout << "密钥长度: " << correct_key.size() << "\n\n";
    
    // 计算理论复杂度
    long long brute_force_attempts = 1;
    for (size_t i = 0; i < correct_key.size(); i++) {
        brute_force_attempts *= charset.size();
    }
    
    long long timing_attack_attempts = correct_key.size() * charset.size();
    
    cout << "📊 攻击复杂度比较:\n";
    cout << "------------------------------------------------------------\n";
    cout << "暴力破解:     " << brute_force_attempts << " 次尝试\n";
    cout << "时序攻击:     " << timing_attack_attempts << " 次尝试\n";
    cout << "复杂度降低:   " << brute_force_attempts / timing_attack_attempts << " 倍!\n\n";
    
    // 模拟时序攻击破解过程
    cout << "🔓 模拟时序攻击破解过程 (使用不安全比较):\n";
    cout << "------------------------------------------------------------\n";
    
    string cracked = "";
    int total_attempts = 0;
    
    for (size_t pos = 0; pos < correct_key.size(); pos++) {
        double max_time = 0;
        char best_char = 'a';
        
        for (char c : charset) {
            string test_key = cracked + c + string(correct_key.size() - pos - 1, 'a');
            
            double avg_time = measure_time(unsafe_compare, test_key, correct_key, 1000);
            total_attempts++;
            
            if (avg_time > max_time) {
                max_time = avg_time;
                best_char = c;
            }
        }
        
        cracked += best_char;
        cout << "位置 " << pos << ": 找到字符 '" << best_char << "' (当前: " << cracked << ")\n";
    }
    
    cout << "\n✅ 成功破解密钥: " << cracked << "\n";
    cout << "   总尝试次数: " << total_attempts << " (理论值: " << timing_attack_attempts << ")\n\n";
}

// ========================================
// 演示 4: XOR 运算原理
// ========================================
void demo_xor_logic() {
    cout << "=================================================\n";
    cout << "演示 4: XOR 运算原理\n";
    cout << "=================================================\n\n";
    
    cout << "常量时间比较使用 XOR 运算累积差异:\n\n";
    
    string a = "secret";
    string b1 = "secret";  // 完全匹配
    string b2 = "secxet";  // 第 4 个字符不同
    
    cout << "比较 1: \"" << a << "\" vs \"" << b1 << "\"\n";
    cout << "------------------------------------------------------------\n";
    cout << "位置  字符A  字符B  十六进制A  十六进制B  XOR结果  累积结果\n";
    cout << "------------------------------------------------------------\n";
    
    uint8_t result1 = 0;
    for (size_t i = 0; i < a.size(); i++) {
        uint8_t xor_result = static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b1[i]);
        result1 |= xor_result;
        
        cout << setw(4) << i
             << setw(8) << a[i]
             << setw(8) << b1[i]
             << "  0x" << hex << setfill('0') << setw(2) << (int)static_cast<uint8_t>(a[i])
             << "      0x" << setw(2) << (int)static_cast<uint8_t>(b1[i])
             << "      0x" << setw(2) << (int)xor_result
             << "      0x" << setw(2) << (int)result1
             << dec << setfill(' ') << "\n";
    }
    
    cout << "\n最终结果: 0x" << hex << setw(2) << setfill('0') << (int)result1 
         << " → " << (result1 == 0 ? "✅ 匹配" : "❌ 不匹配") << dec << "\n\n";
    
    cout << "比较 2: \"" << a << "\" vs \"" << b2 << "\"\n";
    cout << "------------------------------------------------------------\n";
    cout << "位置  字符A  字符B  十六进制A  十六进制B  XOR结果  累积结果\n";
    cout << "------------------------------------------------------------\n";
    
    uint8_t result2 = 0;
    for (size_t i = 0; i < a.size(); i++) {
        uint8_t xor_result = static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b2[i]);
        result2 |= xor_result;
        
        cout << setw(4) << i
             << setw(8) << a[i]
             << setw(8) << b2[i]
             << "  0x" << hex << setfill('0') << setw(2) << (int)static_cast<uint8_t>(a[i])
             << "      0x" << setw(2) << (int)static_cast<uint8_t>(b2[i])
             << "      0x" << setw(2) << (int)xor_result
             << "      0x" << setw(2) << (int)result2
             << dec << setfill(' ') << "\n";
    }
    
    cout << "\n最终结果: 0x" << hex << setw(2) << setfill('0') << (int)result2 
         << " → " << (result2 == 0 ? "✅ 匹配" : "❌ 不匹配") << dec << "\n\n";
    
    cout << "💡 关键点:\n";
    cout << "   1. XOR 运算: 相同字符 → 0x00, 不同字符 → 非零\n";
    cout << "   2. 使用 |= 累积所有差异，不会短路退出\n";
    cout << "   3. 无论何时发现不匹配，循环都会继续完成\n\n";
}

// ========================================
// 主程序
// ========================================
int main() {
    cout << "\n";
    cout << "╔═══════════════════════════════════════════════════════════╗\n";
    cout << "║                                                           ║\n";
    cout << "║       时序攻击防御演示 - HunterClaw Gateway              ║\n";
    cout << "║                                                           ║\n";
    cout << "╚═══════════════════════════════════════════════════════════╝\n";
    cout << "\n";
    
    demo_timing_attack();
    
    cout << "\n按 Enter 继续...\n";
    cin.get();
    
    demo_constant_time();
    
    cout << "\n按 Enter 继续...\n";
    cin.get();
    
    demo_brute_force();
    
    cout << "\n按 Enter 继续...\n";
    cin.get();
    
    demo_xor_logic();
    
    cout << "=================================================\n";
    cout << "演示结束\n";
    cout << "=================================================\n\n";
    
    cout << "🎓 学习要点:\n";
    cout << "   1. ❌ 普通比较 (==, strcmp) 会泄露时序信息\n";
    cout << "   2. ✅ 使用常量时间比较防止时序攻击\n";
    cout << "   3. 🔐 所有密钥/Token 比较必须使用安全函数\n";
    cout << "   4. 🛡️ 结合速率限制和日志监控加强防护\n\n";
    
    return 0;
}
