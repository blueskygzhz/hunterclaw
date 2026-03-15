#!/usr/bin/env python3
import re

# Read provider_registry.cc
with open('src/memory/provider_registry.cc', 'r') as f:
    lines = f.readlines()

# Add #include <mutex> after #include <stdexcept>
new_lines = []
for i, line in enumerate(lines):
    new_lines.append(line)
    if '#include <stdexcept>' in line:
        new_lines.append('#include <mutex>\n')
        new_lines.append('#include <chrono>\n')

content = ''.join(new_lines)

# Fix core::Err(...) - need to use Result<T>::Err(...)
# Strategy: manually replace based on return type context

replacements = [
    # register_provider - returns Result<bool>
    (r'return core::Err\("Provider is null"\);', 
     'return core::Result<bool>::Err("Provider is null");'),
    (r'return core::Err\("Provider \'" \+ provider_id \+ "\' already registered"\);',
     'return core::Result<bool>::Err("Provider \'" + provider_id + "\' already registered");'),
    (r'return core::Err\("Provider \'" \+ provider_id \+ "\' not found"\);',
     'return core::Result<bool>::Err("Provider \'" + provider_id + "\' not found");'),
    
    # get_provider - returns Result<shared_ptr<MemoryProvider>>
    (r'return core::Err\("Provider \'" \+ provider_id \+ "\' not found"\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("Provider \'" + provider_id + "\' not found");'),
    
    # get_default_provider
    (r'return core::Err\("No default provider set"\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("No default provider set");'),
    (r'return core::Err\("Default provider not found"\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("Default provider not found");'),
    
    # create_and_register_provider - returns Result<string>
    (r'return core::Err\("No factory registered for provider type \'" \+ provider_type \+ "\'"\);',
     'return core::Result<std::string>::Err("No factory registered for provider type \'" + provider_type + "\'");'),
    (r'return core::Err\("Failed to create provider: " \+ result\.error\);',
     'return core::Result<std::string>::Err("Failed to create provider: " + result.error);'),
    
    # create_session_provider
    (r'return core::Err\(provider_result\.error\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err(provider_result.error);'),
    
    # SQLiteMemoryProvider methods
    (r'return core::Err\("Memory manager not initialized"\);',
     'return core::Result<std::string>::Err("Memory manager not initialized");'),
    (r'return core::Err\(std::string\("Failed to add memory: "\) \+ e\.what\(\)\);',
     'return core::Result<std::string>::Err(std::string("Failed to add memory: ") + e.what());'),
    (r'return core::Err\(std::string\("Failed to search memories: "\) \+ e\.what\(\)\);',
     'return core::Result<std::vector<Memory>>::Err(std::string("Failed to search memories: ") + e.what());'),
    (r'return core::Err\(std::string\("Failed to remove memory: "\) \+ e\.what\(\)\);',
     'return core::Result<bool>::Err(std::string("Failed to remove memory: ") + e.what());'),
    (r'return core::Err\("Failed to update memory: " \+ add_result\.error\);',
     'return core::Result<bool>::Err("Failed to update memory: " + add_result.error);'),
    (r'return core::Err\("Clear not implemented yet"\);',
     'return core::Result<bool>::Err("Clear not implemented yet");'),
    (r'return core::Err\(std::string\("Failed to clear memories: "\) \+ e\.what\(\)\);',
     'return core::Result<bool>::Err(std::string("Failed to clear memories: ") + e.what());'),
    (r'return core::Err\("Get memory by ID not implemented yet"\);',
     'return core::Result<Memory>::Err("Get memory by ID not implemented yet");'),
    
    # Factory methods
    (r'return core::Err\(std::string\("Failed to create SQLite provider: "\) \+ e\.what\(\)\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err(std::string("Failed to create SQLite provider: ") + e.what());'),
    (r'return core::Err\("MemOS Cloud provider not implemented yet"\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("MemOS Cloud provider not implemented yet");'),
    (r'return core::Err\("Supabase provider not implemented yet"\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("Supabase provider not implemented yet");'),
    (r'return core::Err\("Unknown provider type: " \+ provider_type\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Err("Unknown provider type: " + provider_type);'),
]

for pattern, replacement in replacements:
    content = re.sub(pattern, replacement, content)

# Fix core::Ok(...)
ok_replacements = [
    (r'return core::Result<bool>::Ok\(true\);', 'return core::Result<bool>::Ok(true);'),
    (r'return core::Result<bool>::Ok\(success\);', 'return core::Result<bool>::Ok(success);'),
    (r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok\(it->second\);', 
     'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(it->second);'),
    (r'return core::Result<std::string>::Ok\(provider_id\);', 
     'return core::Result<std::string>::Ok(provider_id);'),
    (r'return core::Result<std::string>::Ok\(id\);', 
     'return core::Result<std::string>::Ok(id);'),
    (r'return core::Result<std::vector<Memory>>::Ok\(memories\);', 
     'return core::Result<std::vector<Memory>>::Ok(memories);'),
    (r'return core::Result<MemoryStats>::Ok\(stats\);', 
     'return core::Result<MemoryStats>::Ok(stats);'),
    (r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok\(std::static_pointer_cast<MemoryProvider>\(provider\)\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(provider));'),
    (r'return core::Result<std::shared_ptr<MemoryProvider>>::Ok\(std::static_pointer_cast<MemoryProvider>\(session_provider\)\);',
     'return core::Result<std::shared_ptr<MemoryProvider>>::Ok(std::static_pointer_cast<MemoryProvider>(session_provider));'),
]

for pattern, replacement in ok_replacements:
    content = re.sub(pattern, replacement, content)

# Fix .error() calls - should be .error not .error()
content = re.sub(r'\.error\(\)', '.error', content)

# Fix .value() calls - should be .value not .value()
content = re.sub(r'\.value\(\)', '.value', content)

# Fix operator! - should use .is_err()
content = re.sub(r'if \(!([a-z_]+)\) \{', r'if (\1.is_err()) {', content)

# Write back
with open('src/memory/provider_registry.cc', 'w') as f:
    f.write(content)

print("Fixed provider_registry.cc")
