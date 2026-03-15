#!/usr/bin/env python3
import re

# Read the file
with open('src/memory/provider_registry.cc', 'r') as f:
    content = f.read()

# Fix Error< patterns
# Pattern 1: return Error<"..." 
content = re.sub(r'return Error<"([^"]+)"\)', r'return core::Result<bool>::Err("\1")', content)
content = re.sub(r'return Error<std::string\("([^"]+)"\) \+ ([^)]+)\)', r'return core::Result<std::string>::Err(std::string("\1") + \2)', content)

# Pattern 2: return Error<provider_result.error()
content = re.sub(r'return Error<([^)]+\.error\(\))\)', r'return core::Result<std::shared_ptr<MemoryProvider>>::Err(\1)', content)

# Fix core::Ok patterns  
content = re.sub(r'return core::Ok\(true\);', r'return core::Result<bool>::Ok(true);', content)
content = re.sub(r'return core::Ok\(success\);', r'return core::Result<bool>::Ok(success);', content)
content = re.sub(r'return core::Ok\(it->second\);', r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);', content)
content = re.sub(r'return core::Ok\(provider_id\);', r'return core::Result<std::string>::Ok(provider_id);', content)
content = re.sub(r'return core::Ok\(id\);', r'return core::Result<std::string>::Ok(id);', content)
content = re.sub(r'return core::Ok\(memories\);', r'return core::Result<std::vector<Memory>>::Ok(memories);', content)
content = re.sub(r'return core::Ok\(stats\);', r'return core::Result<MemoryStats>::Ok(stats);', content)
content = re.sub(r'return core::Ok\(std::static_pointer_cast<MemoryProvider>\(provider\)\);', 
                 r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(provider));', content)
content = re.sub(r'return core::Ok\(std::static_pointer_cast<MemoryProvider>\(session_provider\)\);',
                 r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(session_provider));', content)

# More specific Error patterns
content = re.sub(r'return Error<"Failed to create provider: " \+ result\.error\(\)\);',
                 r'return core::Result<std::string>::Err("Failed to create provider: " + result.error);', content)
content = re.sub(r'return Error<"Failed to update memory: " \+ add_result\.error\(\)\);',
                 r'return core::Result<bool>::Err("Failed to update memory: " + add_result.error);', content)

# Generic patterns for remaining Error and core::Ok
content = re.sub(r'return Error<"([^"]+)"\);', r'return core::Result<bool>::Err("\1");', content)
content = re.sub(r'return Error<std::string\("([^"]+)"\) \+ e\.what\(\)\);',
                 lambda m: f'return core::Result<std::string>::Err(std::string("{m.group(1)}") + e.what());', content)

# Write back
with open('src/memory/provider_registry.cc', 'w') as f:
    f.write(content)

print("Fixed Result API syntax")
