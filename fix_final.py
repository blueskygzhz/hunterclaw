#!/usr/bin/env python3
import re

# Read the file
with open('src/memory/provider_registry.cc', 'r') as f:
    lines = f.readlines()

# Manual fixes
new_content = []
for i, line in enumerate(lines):
    # Line 23: if (provider.is_err()) -> if (!provider)
    if 'if (provider.is_err())' in line:
        line = '    if (!provider) {\n'
    
    # Line 80: wrong Result type
    elif 'return core::Result<bool>::Err("Provider \'" + provider_id + "\' not found");' in line and i > 70 and i < 85:
        line = '        return core::Result<std::shared_ptr<MemoryProvider>>::Err("Provider \'" + provider_id + "\' not found");\n'
    
    # Line 83: return core::Ok(it->second);
    elif 'return core::Ok(it->second);' in line and i > 70 and i < 90:
        line = '    return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);\n'
    
    # Line ~90: return core::Ok(it->second); (get_default_provider)
    elif 'return core::Ok(it->second);' in line and i > 90 and i < 110:
        line = '    return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);\n'
    
    # Line 131: auto stats_result = provider->get_stats();
    # if (stats_result.is_err()) should be if (stats_result.is_ok())
    elif 'if (stats_result.is_err())' in line and 'get_stats' in lines[i-2]:
        line = '        if (stats_result.is_ok()) {\n'
    
    # Line 162: return core::Err("No factory...
    elif 'return core::Err("No factory registered' in line:
        line = '        return core::Result<std::string>::Err("No factory registered for provider type \'" + provider_type + "\'");\n'
    
    # Line 167: return core::Err("Failed to create provider:
    elif 'return core::Err("Failed to create provider: "' in line:
        line = '        return core::Result<std::string>::Err("Failed to create provider: " + result.error);\n'
    
    # Line 294: !impl_->manager (for remove_memory)
    elif 'if (impl_->manager.is_err())' in line and i > 290 and i < 300:
        line = '    if (!impl_->manager) {\n'
    
    # Line 295: return core::Result<...>::Err for remove_memory
    elif 'return core::Result<std::vector<Memory>>::Err("Memory manager not initialized");' in line and i > 290 and i < 300:
        line = '        return core::Result<bool>::Err("Memory manager not initialized");\n'
    
    # Line 303: return core::Ok(success);
    elif 'return core::Ok(success);' in line and i > 295 and i < 310:
        line = '        return core::Result<bool>::Ok(success);\n'
    
    # Line 313: if (remove_result.is_err())
    elif 'if (remove_result.is_err())' in line and i > 305 and i < 320:
        line = '    if (remove_result.is_err()) {\n'
    
    # Line 318: if (add_result.is_err())
    elif 'if (add_result.is_err())' in line and i > 315 and i < 325:
        line = '    if (add_result.is_err()) {\n'
    
    # Line 321: return core::Err("Failed to update
    elif 'return core::Err("Failed to update memory:' in line:
        line = '        return core::Result<bool>::Err("Failed to update memory: " + add_result.error);\n'
    
    # Line 327: if (impl_->manager.is_err()) for clear_memories
    elif 'if (impl_->manager.is_err())' in line and i > 325 and i < 335:
        line = '    if (!impl_->manager) {\n'
    
    # Line 329: wrong Result type for clear_memories
    elif 'return core::Result<std::vector<Memory>>::Err("Memory manager not initialized");' in line and i > 325 and i < 335:
        line = '        return core::Result<bool>::Err("Memory manager not initialized");\n'
    
    # Line 333: return core::Err("Clear not implemented
    elif 'return core::Err("Clear not implemented' in line:
        line = '        return core::Result<bool>::Err("Clear not implemented yet");\n'
    
    # Line 336: return core::Err(std::string("Failed to clear
    elif 'return core::Err(std::string("Failed to clear' in line:
        line = '        return core::Result<bool>::Err(std::string("Failed to clear memories: ") + e.what());\n'
    
    # Line ~248: if (impl_->manager.is_err()) for add_memory
    elif 'if (impl_->manager.is_err())' in line and i > 245 and i < 255:
        line = '    if (!impl_->manager) {\n'
    
    # Line ~250: for add_memory return type fix
    elif 'return core::Result<std::vector<Memory>>::Err("Memory manager not initialized");' in line and i > 245 and i < 255:
        line = '        return core::Result<std::string>::Err("Memory manager not initialized");\n'
    
    # Line ~255: return core::Err(std::string("Failed to add memory:
    elif 'return core::Err(std::string("Failed to add memory:' in line:
        line = '        return core::Result<std::string>::Err(std::string("Failed to add memory: ") + e.what());\n'
    
    # Line ~264: if (impl_->manager.is_err()) for search_memories
    elif 'if (impl_->manager.is_err())' in line and i > 260 and i < 270:
        line = '    if (!impl_->manager) {\n'
    
    # Line ~290: return core::Err(std::string("Failed to search memories:
    elif 'return core::Err(std::string("Failed to search memories:' in line:
        line = '        return core::Result<std::vector<Memory>>::Err(std::string("Failed to search memories: ") + e.what());\n'
    
    # Line ~305: return core::Err(std::string("Failed to remove memory:
    elif 'return core::Err(std::string("Failed to remove memory:' in line:
        line = '        return core::Result<bool>::Err(std::string("Failed to remove memory: ") + e.what());\n'
    
    new_content.append(line)

# Write back
with open('src/memory/provider_registry.cc', 'w') as f:
    f.writelines(new_content)

print("Fixed all issues in provider_registry.cc")
