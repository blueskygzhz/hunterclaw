#include "hunterclaw/core/types.h"
#include <sstream>
#include <stdexcept>

namespace hunterclaw {
namespace core {

SessionKey SessionKey::from_string(const std::string& str) {
    std::istringstream iss(str);
    std::string channel, account_id, target_id;
    
    if (!std::getline(iss, channel, ':') ||
        !std::getline(iss, account_id, ':') ||
        !std::getline(iss, target_id)) {
        throw std::invalid_argument("Invalid SessionKey format: " + str);
    }
    
    return SessionKey{channel, account_id, target_id};
}

} // namespace core
} // namespace hunterclaw
