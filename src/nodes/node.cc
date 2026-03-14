#include "hunterclaw/nodes/node.h"
#include <algorithm>

namespace hunterclaw {
namespace nodes {

bool Node::supports_command(const std::string& command) const {
    return std::find(commands.begin(), commands.end(), command) != commands.end();
}

bool Node::has_permission(const std::string& command) const {
    auto it = permissions.find(command);
    if (it == permissions.end()) {
        // If no explicit permission set, default to allowed if command is supported
        return supports_command(command);
    }
    return it->second;
}

} // namespace nodes
} // namespace hunterclaw
