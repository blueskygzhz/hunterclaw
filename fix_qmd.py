#!/usr/bin/env python3

# Read qmd_manager.cc
with open('src/memory/qmd_manager.cc', 'r') as f:
    lines = f.readlines()

new_lines = []
for i, line in enumerate(lines):
    # Fix QmdCommandOptions temporary object - need to create variable first
    if 'QmdCommandOptions{qmd_config_.update.command_timeout_ms})' in line:
        indent = len(line) - len(line.lstrip())
        var_line = ' ' * indent + 'QmdCommandOptions opts{qmd_config_.update.command_timeout_ms, {}};\n'
        new_lines.append(var_line)
        line = line.replace('QmdCommandOptions{qmd_config_.update.command_timeout_ms}', '&opts')
    
    # Fix search() signature - SearchOptions& -> SearchOptions*
    elif line.strip().startswith('const SearchOptions& opts') and 'search' in lines[i-2]:
        line = line.replace('const SearchOptions& opts', 'const SearchOptions* opts')
    
    # Fix sync() signature - SyncParams& -> SyncParams*
    elif 'void QmdMemoryManager::sync(const SyncParams& params)' in line:
        line = line.replace('const SyncParams& params', 'const SyncParams* params')
    
    # Fix params reference to pointer
    elif 'if (params.progress)' in line:
        line = line.replace('params.progress', 'params->progress')
    elif 'params.progress(progress)' in line:
        line = line.replace('params.progress', 'params->progress')
    
    new_lines.append(line)

# Write back
with open('src/memory/qmd_manager.cc', 'w') as f:
    f.writelines(new_lines)

print("Fixed qmd_manager.cc")
