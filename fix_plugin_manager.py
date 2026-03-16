#!/usr/bin/env python3
"""
修复 plugin_manager.cc 中的 Result API 调用
"""

import re

def fix_plugin_manager():
    with open('src/memory/plugin_manager.cc', 'r') as f:
        content = f.read()
    
    # 1. 修复 discover_plugins() 的返回值（返回 Result<vector<string>>）
    content = re.sub(
        r'return core::Result<bool>::Ok\(plugin_ids\);',
        r'return core::Result<std::vector<std::string>>::Ok(plugin_ids);',
        content
    )
    
    # 2. 修复 load_plugin() 的返回值（返回 Result<shared_ptr<MemoryPlugin>>）
    content = re.sub(
        r'if \(!manifest_result\) \{',
        r'if (manifest_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<bool>::Err\(manifest_result\.error\(\)\);',
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err(manifest_result.error);',
        content
    )
    content = re.sub(
        r'if \(!validate_result\) \{',
        r'if (validate_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<bool>::Err\(validate_result\.error\(\)\);',
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err(validate_result.error);',
        content, count=1
    )
    content = re.sub(
        r'return core::Result<bool>::Ok\(plugins_\[manifest\.id\]\);',
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Ok(plugins_[manifest.id]);',
        content, count=1
    )
    
    # 修复 load_plugin 函数中的其他 Err 返回
    lines = content.split('\n')
    fixed_lines = []
    in_load_plugin = False
    in_load_plugin_by_id = False
    in_reload = False
    
    for i, line in enumerate(lines):
        # 检测函数开始
        if 'MemoryPluginManager::load_plugin(' in line:
            in_load_plugin = True
            in_load_plugin_by_id = False
            in_reload = False
        elif 'MemoryPluginManager::load_plugin_by_id(' in line:
            in_load_plugin_by_id = True
            in_load_plugin = False
            in_reload = False
        elif 'MemoryPluginManager::reload_plugin(' in line:
            in_reload = True
            in_load_plugin = False
            in_load_plugin_by_id = False
        elif 'MemoryPluginManager::unload_plugin(' in line or \
             'MemoryPluginManager::discover_and_load_all(' in line or \
             'MemoryPluginManager::unload_all(' in line or \
             'MemoryPluginManager::parse_manifest(' in line or \
             'MemoryPluginManager::validate_manifest(' in line:
            in_load_plugin = False
            in_load_plugin_by_id = False
            in_reload = False
        
        # 在 load_plugin 中替换 Result<bool>::Err 为 Result<shared_ptr<MemoryPlugin>>::Err
        if in_load_plugin and 'return core::Result<bool>::Err(' in line:
            line = line.replace('core::Result<bool>::Err(', 'core::Result<std::shared_ptr<MemoryPlugin>>::Err(')
        
        # 在 load_plugin 中替换 Result<bool>::Ok 为 Result<shared_ptr<MemoryPlugin>>::Ok
        if in_load_plugin and 'return core::Result<bool>::Ok(' in line:
            line = line.replace('core::Result<bool>::Ok(', 'core::Result<std::shared_ptr<MemoryPlugin>>::Ok(')
        
        # 在 load_plugin_by_id 中也要替换
        if in_load_plugin_by_id and 'return core::Result<bool>::Err(' in line:
            line = line.replace('core::Result<bool>::Err(', 'core::Result<std::shared_ptr<MemoryPlugin>>::Err(')
        
        # 在 reload_plugin 中替换
        if in_reload and 'return core::Result<bool>::Err(' in line:
            line = line.replace('return core::Result<bool>::Err(', 'return core::Result<bool>::Err(')  # 保持不变，它返回 bool
        
        fixed_lines.append(line)
    
    content = '\n'.join(fixed_lines)
    
    # 3. 修复 discover_and_load_all() 的判断（Result<vector<string>>）
    content = re.sub(
        r'if \(!discover_result\) \{',
        r'if (discover_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<bool>::Err\(discover_result\.error\(\)\);',
        r'return core::Result<int>::Err(discover_result.error);',
        content
    )
    content = re.sub(
        r'auto plugin_ids = discover_result\.value\(\);',
        r'auto plugin_ids = discover_result.value;',
        content
    )
    
    # 4. 修复 discover_and_load_all() 的返回（返回 Result<int>）
    content = re.sub(
        r'return core::Result<bool>::Ok\(loaded_count\);',
        r'return core::Result<int>::Ok(loaded_count);',
        content
    )
    
    # 5. 修复 unload_plugin() 中的判断（Result<bool>）
    content = re.sub(
        r'if \(!shutdown_result\) \{',
        r'if (shutdown_result.is_err()) {',
        content
    )
    content = re.sub(
        r'log\(\)->warn\("MemoryPluginManager: plugin \{\} shutdown failed: \{\}",\s+plugin_id, shutdown_result\.error\(\)\);',
        r'log()->warn("MemoryPluginManager: plugin {} shutdown failed: {}",\n            plugin_id, shutdown_result.error);',
        content
    )
    
    # 6. 修复 unload_all() 中的判断
    content = re.sub(
        r'if \(!result\) \{',
        r'if (result.is_err()) {',
        content
    )
    content = re.sub(
        r'log\(\)->warn\("MemoryPluginManager: failed to unload plugin \{\}: \{\}", id, result\.error\(\)\);',
        r'log()->warn("MemoryPluginManager: failed to unload plugin {}: {}", id, result.error);',
        content
    )
    
    # 7. 修复 load_plugin_by_id() 中的判断
    content = re.sub(
        r'if \(!manifest_result\) continue;',
        r'if (manifest_result.is_err()) continue;',
        content
    )
    content = re.sub(
        r'manifest_result\.value\(\)\.id',
        r'manifest_result.value.id',
        content
    )
    
    # 8. 修复 reload_plugin() 中的判断
    content = re.sub(
        r'if \(!unload_result\) \{',
        r'if (unload_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<bool>::Err\("Failed to unload plugin: " \+ unload_result\.error\(\)\);',
        r'return core::Result<bool>::Err("Failed to unload plugin: " + unload_result.error);',
        content
    )
    content = re.sub(
        r'if \(!load_result\) \{',
        r'if (load_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<bool>::Err\("Failed to reload plugin: " \+ load_result\.error\(\)\);',
        r'return core::Result<bool>::Err("Failed to reload plugin: " + load_result.error);',
        content
    )
    
    # 9. 修复 provider_registry 中的判断
    content = re.sub(
        r'if \(!register_result\) \{',
        r'if (register_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err\("Failed to register provider: " \+ register_result\.error\(\)\);',
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to register provider: " + register_result.error);',
        content
    )
    content = re.sub(
        r'if \(!init_result\) \{',
        r'if (init_result.is_err()) {',
        content
    )
    content = re.sub(
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err\("Failed to initialize plugin: " \+ init_result\.error\(\)\);',
        r'return core::Result<std::shared_ptr<MemoryPlugin>>::Err("Failed to initialize plugin: " + init_result.error);',
        content
    )
    
    # 10. 修复 parse_manifest() 的返回（返回 Result<PluginManifest>）
    lines = content.split('\n')
    fixed_lines = []
    in_parse_manifest = False
    in_validate_manifest = False
    
    for line in lines:
        if 'MemoryPluginManager::parse_manifest(' in line:
            in_parse_manifest = True
            in_validate_manifest = False
        elif 'MemoryPluginManager::validate_manifest(' in line:
            in_validate_manifest = True
            in_parse_manifest = False
        elif line.strip().startswith('core::Result<') or line.strip().startswith('//') or \
             ('MemoryPluginManager::' in line and '(' in line):
            if 'parse_manifest' not in line and 'validate_manifest' not in line:
                in_parse_manifest = False
                in_validate_manifest = False
        
        if in_parse_manifest:
            if 'return core::Result<bool>::Err(' in line:
                line = line.replace('core::Result<bool>::Err(', 'core::Result<PluginManifest>::Err(')
            elif 'return core::Result<bool>::Ok(' in line:
                line = line.replace('core::Result<bool>::Ok(', 'core::Result<PluginManifest>::Ok(')
        
        # validate_manifest 返回 Result<bool>，保持不变
        
        fixed_lines.append(line)
    
    content = '\n'.join(fixed_lines)
    
    # 写回文件
    with open('src/memory/plugin_manager.cc', 'w') as f:
        f.write(content)
    
    print("✅ plugin_manager.cc 修复完成")

if __name__ == '__main__':
    fix_plugin_manager()
