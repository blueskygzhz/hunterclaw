# OpenClaw 配置文件完整指南 📋

## 🎯 配置文件位置

### 默认位置
```bash
~/.openclaw/openclaw.json
```

### 环境变量覆盖

| 环境变量 | 说明 | 示例 |
|----------|------|------|
| `OPENCLAW_CONFIG_PATH` | 指定配置文件路径 | `export OPENCLAW_CONFIG_PATH=/path/to/config.json` |
| `OPENCLAW_STATE_DIR` | 指定状态目录（配置在此目录下） | `export OPENCLAW_STATE_DIR=~/.my-openclaw` |
| `OPENCLAW_HOME` | 覆盖 HOME 目录 | `export OPENCLAW_HOME=/custom/home` |

### 历史兼容路径
OpenClaw 会按以下顺序查找配置文件（优先使用第一个存在的文件）：

```bash
# 新版本路径
~/.openclaw/openclaw.json

# 历史遗留路径（兼容）
~/.openclaw/clawdbot.json
~/.openclaw/moldbot.json
~/.openclaw/moltbot.json
~/.clawdbot/clawdbot.json
~/.moldbot/moldbot.json
~/.moltbot/moltbot.json
```

---

## 📂 快速查看配置文件

### 方法 1: 使用 cat/less
```bash
# 查看完整配置
cat ~/.openclaw/openclaw.json

# 分页查看（推荐）
less ~/.openclaw/openclaw.json

# 或者使用 bat（语法高亮）
bat ~/.openclaw/openclaw.json
```

### 方法 2: 使用 jq（格式化查看）
```bash
# 格式化输出
jq '.' ~/.openclaw/openclaw.json

# 查看特定配置项
jq '.agentDefaults.blockStreamingBreak' ~/.openclaw/openclaw.json

# 查看所有流式配置
jq '.agentDefaults | {blockStreamingDefault, blockStreamingBreak, blockStreamingChunk, blockStreamingCoalesce}' ~/.openclaw/openclaw.json
```

### 方法 3: 使用编辑器
```bash
# VSCode
code ~/.openclaw/openclaw.json

# Vim
vim ~/.openclaw/openclaw.json

# Nano
nano ~/.openclaw/openclaw.json
```

### 方法 4: 检查配置文件是否存在
```bash
# 查找所有可能的配置文件
find ~/.openclaw ~/.clawdbot ~/.moldbot ~/.moltbot -name "*.json" 2>/dev/null

# 检查默认路径
if [ -f ~/.openclaw/openclaw.json ]; then
    echo "✅ 配置文件存在: ~/.openclaw/openclaw.json"
    ls -lh ~/.openclaw/openclaw.json
else
    echo "❌ 配置文件不存在"
fi
```

---

## 🔍 配置文件关键部分

### 1. Agent 默认配置
```json
{
  "agentDefaults": {
    "model": "claude-3-5-sonnet-20241022",
    "temperature": 0.7,
    "maxTokens": 4096,
    
    // 流式输出配置 ⭐
    "blockStreamingDefault": "on",
    "blockStreamingBreak": "text_end",
    "blockStreamingChunk": {
      "minChars": 100,
      "maxChars": 500,
      "breakPreference": "paragraph"
    },
    "blockStreamingCoalesce": {
      "minChars": 100,
      "maxChars": 1000,
      "idleMs": 250
    }
  }
}
```

### 2. 模型提供商配置
```json
{
  "providers": {
    "anthropic": {
      "apiKey": "${ANTHROPIC_API_KEY}",
      "baseUrl": "https://api.anthropic.com"
    },
    "openai": {
      "apiKey": "${OPENAI_API_KEY}",
      "baseUrl": "https://api.openai.com/v1"
    }
  }
}
```

### 3. 工具配置
```json
{
  "tools": {
    "enabled": ["filesystem", "shell", "web_search"],
    "filesystem": {
      "allowedPaths": ["/workspace", "~/projects"]
    },
    "shell": {
      "timeout": 30000
    }
  }
}
```

### 4. Gateway 配置
```json
{
  "gateway": {
    "enabled": true,
    "port": 18789,
    "bind": "127.0.0.1",
    "auth": {
      "type": "token",
      "token": "${OPENCLAW_GATEWAY_TOKEN}"
    }
  }
}
```

---

## 📊 实用配置查询脚本

### 创建配置查看工具
```bash
#!/bin/bash
# save as: openclaw-config-viewer.sh

CONFIG_PATH="${OPENCLAW_CONFIG_PATH:-$HOME/.openclaw/openclaw.json}"

if [ ! -f "$CONFIG_PATH" ]; then
    echo "❌ 配置文件不存在: $CONFIG_PATH"
    exit 1
fi

echo "📋 OpenClaw 配置文件: $CONFIG_PATH"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# 基本信息
echo "📦 文件信息:"
ls -lh "$CONFIG_PATH"
echo ""

# 流式配置
echo "🌊 流式输出配置:"
jq -r '.agentDefaults | {
  enabled: .blockStreamingDefault,
  breakMode: .blockStreamingBreak,
  chunk: .blockStreamingChunk,
  coalesce: .blockStreamingCoalesce
}' "$CONFIG_PATH"
echo ""

# 模型配置
echo "🤖 默认模型:"
jq -r '.agentDefaults.model // "未配置"' "$CONFIG_PATH"
echo ""

# Gateway 配置
echo "🌐 Gateway 状态:"
jq -r '.gateway | {
  enabled: .enabled,
  port: .port,
  bind: .bind
}' "$CONFIG_PATH"
echo ""

# 环境变量
echo "🔧 相关环境变量:"
env | grep -E "^(OPENCLAW|ANTHROPIC|OPENAI)" | sort
```

### 使用方式
```bash
# 赋予执行权限
chmod +x openclaw-config-viewer.sh

# 运行
./openclaw-config-viewer.sh
```

---

## 🛠️ 配置文件编辑

### 安全编辑流程
```bash
# 1. 备份原配置
cp ~/.openclaw/openclaw.json ~/.openclaw/openclaw.json.backup

# 2. 编辑配置
vim ~/.openclaw/openclaw.json

# 3. 验证 JSON 格式
jq '.' ~/.openclaw/openclaw.json > /dev/null && echo "✅ JSON 格式正确" || echo "❌ JSON 格式错误"

# 4. 如果格式错误，恢复备份
# mv ~/.openclaw/openclaw.json.backup ~/.openclaw/openclaw.json
```

### 使用 jq 修改配置
```bash
# 修改 blockStreamingBreak
jq '.agentDefaults.blockStreamingBreak = "message_end"' ~/.openclaw/openclaw.json > /tmp/config.json
mv /tmp/config.json ~/.openclaw/openclaw.json

# 启用块流式输出
jq '.agentDefaults.blockStreamingDefault = "on"' ~/.openclaw/openclaw.json > /tmp/config.json
mv /tmp/config.json ~/.openclaw/openclaw.json

# 修改最小字符数
jq '.agentDefaults.blockStreamingChunk.minChars = 200' ~/.openclaw/openclaw.json > /tmp/config.json
mv /tmp/config.json ~/.openclaw/openclaw.json
```

---

## 🔄 配置文件加载逻辑

### OpenClaw 配置加载流程

```
1. 解析环境变量
   ├─ OPENCLAW_CONFIG_PATH → 直接使用
   └─ OPENCLAW_STATE_DIR → 拼接 /openclaw.json

2. 如果没有环境变量覆盖:
   ├─ 检查 ~/.openclaw/openclaw.json
   ├─ 检查 ~/.openclaw/clawdbot.json（历史遗留）
   ├─ 检查 ~/.clawdbot/clawdbot.json（历史遗留）
   └─ 使用第一个存在的文件

3. 加载配置内容:
   ├─ 读取 JSON5 格式
   ├─ 解析环境变量引用（${VAR}）
   ├─ 应用 includes（合并其他配置文件）
   ├─ 应用默认值
   ├─ 验证配置项
   └─ 返回完整配置对象
```

### 相关源码位置
```
src/config/
├── io.ts                    # 配置文件读写
├── paths.ts                 # 路径解析逻辑 ⭐
├── defaults.ts              # 默认值应用
├── validation.ts            # 配置验证
├── types.ts                 # TypeScript 类型定义
└── types.agent-defaults.ts  # Agent 默认配置类型
```

---

## 📝 配置示例模板

### 最小配置
```json
{
  "agentDefaults": {
    "model": "claude-3-5-sonnet-20241022"
  },
  "providers": {
    "anthropic": {
      "apiKey": "${ANTHROPIC_API_KEY}"
    }
  }
}
```

### 完整流式配置
```json
{
  "agentDefaults": {
    "model": "claude-3-5-sonnet-20241022",
    "temperature": 0.7,
    "maxTokens": 4096,
    
    // 流式输出配置
    "blockStreamingDefault": "on",
    "blockStreamingBreak": "text_end",
    
    // 软分块配置
    "blockStreamingChunk": {
      "minChars": 100,
      "maxChars": 500,
      "breakPreference": "paragraph"
    },
    
    // 合并配置
    "blockStreamingCoalesce": {
      "minChars": 100,
      "maxChars": 1000,
      "idleMs": 250
    }
  },
  
  "providers": {
    "anthropic": {
      "apiKey": "${ANTHROPIC_API_KEY}",
      "baseUrl": "https://api.anthropic.com"
    }
  },
  
  "gateway": {
    "enabled": true,
    "port": 18789,
    "bind": "127.0.0.1"
  }
}
```

---

## ⚠️ 常见问题

### Q1: 配置文件不存在怎么办？
```bash
# 创建默认配置
mkdir -p ~/.openclaw
cat > ~/.openclaw/openclaw.json <<EOF
{
  "agentDefaults": {
    "model": "claude-3-5-sonnet-20241022"
  },
  "providers": {
    "anthropic": {
      "apiKey": "\${ANTHROPIC_API_KEY}"
    }
  }
}
EOF
```

### Q2: 如何验证配置格式？
```bash
# 使用 jq 验证
jq empty ~/.openclaw/openclaw.json && echo "✅ 格式正确" || echo "❌ 格式错误"

# 或使用 Python
python3 -m json.tool ~/.openclaw/openclaw.json > /dev/null && echo "✅ 格式正确" || echo "❌ 格式错误"
```

### Q3: 环境变量没有被替换？
```bash
# OpenClaw 使用 ${VAR} 语法引用环境变量
# 确保环境变量已设置
export ANTHROPIC_API_KEY="your-key-here"

# 配置文件中使用
"apiKey": "${ANTHROPIC_API_KEY}"
```

### Q4: 如何查看当前生效的配置？
```bash
# 启动 OpenClaw 时会打印配置路径
openclaw start --verbose

# 或查看日志
tail -f ~/.openclaw/logs/openclaw.log | grep -i config
```

---

## 🚀 快速命令参考

```bash
# 查看配置文件
cat ~/.openclaw/openclaw.json

# 格式化查看
jq '.' ~/.openclaw/openclaw.json

# 查看流式配置
jq '.agentDefaults | {blockStreamingDefault, blockStreamingBreak}' ~/.openclaw/openclaw.json

# 编辑配置
vim ~/.openclaw/openclaw.json

# 验证格式
jq empty ~/.openclaw/openclaw.json

# 备份配置
cp ~/.openclaw/openclaw.json ~/.openclaw/openclaw.json.backup

# 查找所有配置文件
find ~ -name "openclaw.json" -o -name "clawdbot.json" 2>/dev/null

# 检查配置文件权限
ls -la ~/.openclaw/openclaw.json
```

---

## 📚 相关文档

- [OPENCLAW_BLOCK_REPLY_CONFIG.md](./OPENCLAW_BLOCK_REPLY_CONFIG.md) - Block Reply 配置详解
- [STREAMING_CALLBACKS_GUIDE.md](./STREAMING_CALLBACKS_GUIDE.md) - HunterClaw 流式回调指南
- [STREAMING_CALLBACKS_QUICKREF.md](./STREAMING_CALLBACKS_QUICKREF.md) - 快速参考

---

**最后更新**: 2025-03-13  
**维护者**: HunterClaw Team
