#pragma once
#include <string>

namespace hunterclaw {
namespace base64 {

std::string encode(const std::string& input);
std::string decode(const std::string& input);

}} // namespace hunterclaw::base64
