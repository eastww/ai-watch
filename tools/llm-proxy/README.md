# Local LLM Image-Bridge Proxy

本地中转代理：把 VS Code `OAI Compatible Provider for Copilot` 发出的**含图片请求**，先在**本地显卡**上用 Ollama VLM 把图片解析成文字描述，再以**纯文本**转发给上游 API（如 `api.sfkey.cn`），从根本上解决"模型不支持图片输入"（400001）的报错。

```
VS Code 扩展 (johnny-zhao.oai-compatible-copilot)
        │  baseUrl = http://127.0.0.1:8090/v1
        ▼
本地代理 (FastAPI :8090) ──含图片──► 本地 Ollama qwen2.5vl（3080）→ 图片描述文本
        │
        ▼ 纯文本请求
上游 API (api.sfkey.cn) ◄────── 原 API Key
```

## 一、安装与启动

### 1. 安装 Ollama + 视觉模型
1. 官网下载安装 Ollama：<https://ollama.com/download>（或 `irm https://ollama.com/install.ps1 | iex`）
2. 拉取视觉模型（RTX 3080 10G 推荐 7B）：
   ```powershell
   ollama pull qwen2.5vl:7b
   ```
3. 验证：`ollama list` 能看到模型可用的完整路径是 `$env:LOCALAPPDATA\Programs\Ollama\ollama.exe`（刚装完 PATH 可能未刷新）。

### 2. 配置代理
编辑 `config.yaml`：
- `upstream.base_url` / `api_key` 填你现有的（`https://api.sfkey.cn/v1` + key）
- 也可以不写 key，改用环境变量 `UPSTREAM_API_KEY`（更安全）

### 3. 安装 Python 依赖 + 启动
```powershell
cd D:\Projects\ai-watch\tools\llm-proxy
C:/Python314/python.exe -m pip install -r requirements.txt
C:/Python314/python.exe server.py
```
启动成功会看到 `Uvicorn running on http://127.0.0.1:8090`。

## 二、配置 VS Code 扩展

扩展名：`johnny-zhao.oai-compatible-copilot`（OAI Compatible Provider for Copilot）。配置键是 **`oaicopilot.*`**（不是 oaiCompatible.*）。

### 方式 A：可视化配置界面（推荐）
1. `Ctrl+Shift+P` → 运行 `OAICopilot: Open Configuration UI`（或点右下角状态栏的 `OAICopilot`）。
2. **添加供应商（Add Provider）**：Base URL 填 `http://127.0.0.1:8090/v1`，API Key 随便填（如 `local`，代理不校验），API 模式选 `openai`，保存。
3. **添加模型（Add Model）**：模型 ID 填 `deepseek-v4-pro`，并把该模型关联到上一步的本地 provider，保存。
4. 打开 Copilot Chat，点模型选择器 → Manage Models → OAI Compatible → 选刚加的模型。

> 注意：使用配置界面后，全局 `oaicopilot.baseUrl` 会失效，以供应商/模型级配置为准。

### 方式 B：直接改 settings.json
```json
"oaicopilot.baseUrl": "http://127.0.0.1:8090/v1",
"oaicopilot.models": [
    {
        "id": "deepseek-v4-pro",
        "owned_by": "local-proxy",
        "context_length": 32768,
        "max_tokens": 4096
    }
]
```
API Key：在 Copilot Chat 里点模型选择器 → Manage Models → OAI Compatible → 输入任意 key（如 `local`）即可，存在本地。

### ⚠️ 关键：必须指向本地代理
- `oaicopilot.baseUrl` 一定要是 `http://127.0.0.1:8090/v1`。
- 如果还是 `https://api.sfkey.cn/v1`，图片请求会直接打到上游，报 400001。
- 模型配置里的 `"vision": true` 只是扩展宣称支持，实际能不能处理图片由代理决定；只要走代理，图片就会本地解析后转文本。

## 三、自测

不经过扩展，直接用 PowerShell/Python 测试（发一条**带图片**的消息）：
```python
# test_image.py 已提供，会生成一张测试图并走完整链路
C:/Python314/python.exe -X utf8 test_image.py
```
或手动：
```powershell
$img = "data:image/png;base64," + [Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\path\to\截图.png"))
$body = @{ model = "deepseek-v4-pro"; stream = $false; messages = @(@{ role = "user"; content = @(
    @{ type = "text"; text = "这个报错是什么问题？" },
    @{ type = "image_url"; image_url = @{ url = $img } }
) } ) } | ConvertTo-Json -Depth 10
Invoke-RestMethod -Uri "http://127.0.0.1:8090/v1/chat/completions" -Method Post -ContentType "application/json" -Body $body
```

## 四、常见问题

| 现象 | 处理 |
|---|---|
| 日志 `Ollama VLM 失败` | 确认 Ollama 在跑（`ollama list` 不报错），模型名正确 |
| 上游 401/403 | 检查 `api_key` 是否有该模型权限（当前 key 只有 deepseek-v4-pro） |
| 返回 `[图片]（本地解析失败）` | 看代理终端日志，通常是 Ollama 未启动或模型未拉取 |
| 想要更快解析纯文字截图 | 装 `rapidocr_onnxruntime` 并调整 `vision.py` 优先 OCR |

## 五、说明
- 只监听 `127.0.0.1`，不会暴露到局域网。
- 图片不会上传到上游 API，只在本地 3080 上跑 VLM 解析。
- 模型名映射见 `config.yaml` 的 `model_map`。