# LLM Proxy 故障排除指南

## 概述

本文档提供了LLM Proxy无法连接deepseek的常见问题排查步骤和解决方案。

## 常见问题排查流程

### 1. 代理服务器状态检查

**检查代理是否正常运行：**
```powershell
# 启动代理（如果未运行）
cd D:\Projects\ai-watch\tools\llm-proxy
python server.py

# 检查是否启动成功
# 应该看到类似输出：Uvicorn running on http://127.0.0.1:8090
```

**如果启动失败，请检查：**
- Python依赖是否安装完整：`pip install -r requirements.txt`
- 端口8090是否被其他程序占用
- Python版本是否兼容（建议Python 3.8+）

### 2. 配置文件验证

**检查 config.yaml：**
```yaml
upstream:
  base_url: "https://api.sfkey.cn/v1"  # 确认此URL正确且可访问
  api_key: "sk-SYghRM9Jqyn9Sv5YWybrMMs2dRbnfmLIRQ7uOfQtpxDRHPXa"  # 确认API Key有效

local:
  model: "qwen2.5vl:7b"  # 确认模型名正确
  timeout: 180

vision:
  enabled: true  # 确认图片解析功能开启

model_map:
  "*": "deepseek-v4-pro"  # 确认模型映射正确
```

**使用环境变量替代敏感信息（更安全）：**
```powershell
# 设置环境变量
set UPSTREAM_BASE_URL=https://api.sfkey.cn/v1
set UPSTREAM_API_KEY=your_actual_api_key
```

### 3. 网络连接测试

**测试上游API连接：**
```powershell
# 测试基础连接
curl -I https://api.sfkey.cn/v1/models

# 如果失败，检查：
# - 网络连接是否正常
# - 域名是否可解析
# - 防火墙设置
```

**测试代理连接：**
```powershell
# 测试代理服务器
curl -I http://127.0.0.1:8090/v1/models
```

### 4. Ollama服务状态检查

**确认Ollama运行：**
```powershell
# 检查Ollama版本
ollama --version

# 列出已安装模型
ollama list

# 如果未安装qwen2.5vl:7b，请拉取
ollama pull qwen2.5vl:7b
```

**检查OLLAMA_URL配置：**
```python
# 在vision.py中确认
OLLAMA_URL = "http://127.0.0.1:11434"  # 默认值，确保Ollama监听此端口
```

### 5. VS Code配置验证

**检查扩展配置：**
1. 确认 `oaicopilot.baseUrl` 设置为 `http://127.0.0.1:8090/v1`
2. 确认模型配置正确关联到本地provider
3. 确认API Key在Copilot Chat中正确设置（可使用任意值，如"local"）

**配置示例：**
```json
{
  "oaicopilot.baseUrl": "http://127.0.0.1:8090/v1",
  "oaicopilot.models": [
    {
      "id": "deepseek-v4-pro",
      "owned_by": "local-proxy",
      "context_length": 32768,
      "max_tokens": 4096
    }
  ]
}
```

### 6. 日志分析

**查看代理服务器日志：**
- 启动时关注错误信息
- 特别注意以下错误：
  - "upstream 401/403" - API Key无效或权限不足
  - "Ollama VLM 失败" - Ollama未运行或模型问题
  - "connection refused" - 端口被占用或服务未启动

**启用详细日志：**
在 `server.py` 中可以调整日志级别：
```python
logging.basicConfig(level=logging.DEBUG, format="%(asctime)s [%(levelname)s] %(message)s")
```

### 7. 端到端测试

**使用测试脚本验证：**
```powershell
cd D:\Projects\ai-watch\tools\llm-proxy
python test_image.py
```

**预期输出：**
- HTTP status: 200
- 回复内容: 包含图片描述的文本

如果测试失败，请检查：
- 图片解析是否正常
- 代理转发是否成功
- 上游API响应是否正常

## 常见问题解决方案

### 问题1：代理启动失败

**症状：** `server.py` 启动时报错

**解决方案：**
```powershell
# 检查Python版本
python --version

# 安装缺失依赖
pip install -r requirements.txt

# 检查端口占用
netstat -ano | findstr :8090

# 如果端口被占用，修改server.py中的端口
# 在uvicorn.run(app, host="127.0.0.1", port=8091, log_level="info")
```

### 问题2：无法连接上游API

**症状：** 代理日志显示401/403错误

**解决方案：**
```powershell
# 验证API Key
# 访问 https://api.sfkey.cn/v1/models 检查是否需要认证

# 确认API Key有deepseek-v4-pro权限
# 联系API提供商确认Key有效性

# 尝试使用环境变量
set UPSTREAM_API_KEY=your_correct_api_key
```

### 问题3：Ollama未运行或模型问题

**症状：** 日志显示"Ollama VLM 失败"

**解决方案：**
```powershell
# 启动Ollama服务
ollama serve

# 拉取模型（如果未安装）
ollama pull qwen2.5vl:7b

# 检查模型列表
ollama list

# 验证模型运行
ollama run qwen2.5vl:7b "你好"
```

### 问题4：VS Code无法连接代理

**症状：** VS Code显示连接错误

**解决方案：**
```powershell
# 确认代理正在运行
# 检查VS Code网络代理设置
# 确认防火墙允许8090端口

# 测试连接
curl http://127.0.0.1:8090/v1/models
```

### 问题5：图片解析失败

**症状：** 返回"[图片]（本地解析失败）"

**解决方案：**
```powershell
# 检查Ollama服务状态
ollama list

# 检查vision.py中的模型配置
# 尝试使用RapidOCR兜底（安装rapidocr_onnxruntime）
pip install rapidocr_onnxruntime

# 调整超时时间
# 在config.yaml中增加timeout值
local:
  model: "qwen2.5vl:7b"
  timeout: 300  # 增加超时时间
```

## 高级排查

### 检查Python环境
```powershell
# 确认Python环境正确
python -c "import fastapi, uvicorn, httpx, yaml, PIL; print('所有依赖可用')"
```

### 检查网络代理
```powershell
# 如果使用代理，确认代理设置正确
# 在config.yaml中可以配置HTTP代理
# upstream:
#   base_url: "https://api.sfkey.cn/v1"
#   proxy: "http://proxy.example.com:8080"
```

### 调试模式
```powershell
# 启动代理时启用调试
python server.py --debug
```

## 联系支持

如果以上步骤都无法解决问题，请提供以下信息：
1. 代理服务器的完整日志输出
2. VS Code中的错误信息
3. Ollama的运行状态和输出
4. 网络连接测试结果

## 更新日志

- 2026-08-29: 初始版本
- 版本：1.0.0

---

**注意：** 本文档适用于LLM Proxy v1.0，配置可能因版本更新而有所变化。请参考最新的README.md获取最新信息。