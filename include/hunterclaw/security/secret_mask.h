// ============================================================================
// secret_mask.h - Credential masking for logs/audit output
// 🎯 严格对齐 OpenClaw: src/security/secret-mask.ts maskApiKey()
// ============================================================================
#pragma once

#include <string>

namespace hunterclaw {
namespace security {

// 🎯 对齐 OpenClaw maskApiKey():
// 遮蔽凭据类值, 不在边界处切断多字节 (UTF-8) 序列。
//   - 先strip 控制字符 (U+0000..U+001F, U+007F..U+009F) 并 trim
//   - 空 → "missing"
//   - len<=6  : 首1…尾1
//   - len<=16 : 首2…尾2
//   -否则    : 首8…尾8
// (长度按 Unicode 码点数计, 切片按码点边界, 避免拆断 UTF-8 多字节字符)
std::string mask_api_key(const std::string& value);

}  // namespace security
}  // namespace hunterclaw
