#!/usr/bin/env python3
import re

# Fix provider.h
with open('include/hunterclaw/memory/provider.h', 'r') as f:
    content = f.read()

# Fix operator!
content = re.sub(r'if \(!result\)', 'if (result.is_err())', content)

# Fix .error() and .value()
content = re.sub(r'result\.error\(\)', 'result.error', content)
content = re.sub(r'result\.value\(\)', 'result.value', content)

# Fix core::Err and core::Ok in provider.h
content = re.sub(r'return core::Err\(result\.error\);', 
                 'return core::Result<std::vector<std::string>>::Err(result.error);', content)
content = re.sub(r'ids\.push_back\(result\.value\);', 
                 'ids.push_back(result.value);', content)
content = re.sub(r'return core::Ok\(ids\);', 
                 'return core::Result<std::vector<std::string>>::Ok(ids);', content)
content = re.sub(r'return core::Err\("Not implemented"\);', 
                 'return core::Result<Memory>::Err("Not implemented");', content)
content = re.sub(r'return core::Err\("Session-scoped clear not implemented"\);',
                 'return core::Result<bool>::Err("Session-scoped clear not implemented");', content)

with open('include/hunterclaw/memory/provider.h', 'w') as f:
    f.write(content)

print("Fixed provider.h")

# Now fix remaining issues in provider_registry.cc
with open('src/memory/provider_registry.cc', 'r') as f:
    content = f.read()

# Fix remaining core::Ok that were not caught
content = re.sub(r'return core::Ok\(provider_id\);', 
                 'return core::Result<std::string>::Ok(provider_id);', content)
content = re.sub(r'return core::Ok\(std::static_pointer_cast<MemoryProvider>\(session_provider\)\);',
                 'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(session_provider));', content)
content = re.sub(r'return core::Ok\(id\);', 
                 'return core::Result<std::string>::Ok(id);', content)
content = re.sub(r'return core::Ok\(memories\);', 
                 'return core::Result<std::vector<Memory>>::Ok(memories);', content)
content = re.sub(r'return core::Ok\(success\);', 
                 'return core::Result<bool>::Ok(success);', content)
content = re.sub(r'return core::Ok\(true\);', 
                 'return core::Result<bool>::Ok(true);', content)
content = re.sub(r'return core::Ok\(stats\);', 
                 'return core::Result<MemoryStats>::Ok(stats);', content)
content = re.sub(r'return core::Ok\(std::static_pointer_cast<MemoryProvider>\(provider\)\);',
                 'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(provider));', content)

# Fix remaining core::Err
content = re.sub(r'return core::Err\(provider_result\.error\);', 
                 'return core::Result<std::shared_ptr<MemoryProvider>>::Err(provider_result.error);', content)

# Fix wrong Result type - search_memories should return Result<vector<Memory>>
content = re.sub(r'return core::Result<std::string>::Err\("Memory manager not initialized"\);',
                 'return core::Result<std::vector<Memory>>::Err("Memory manager not initialized");', 
                 content)

with open('src/memory/provider_registry.cc', 'w') as f:
    f.write(content)

print("Fixed provider_registry.cc again")
