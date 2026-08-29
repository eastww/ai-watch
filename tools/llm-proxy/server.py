# -*- coding: utf-8 -*-
"""
本地中转代理：把"含图片的 OpenAI 兼容请求"转成"纯文本请求"再转发给上游 API。
图片部分用本地 Ollama VLM 解析成文字描述，插入消息后发给上游（如 api.sfkey.cn）。

用法:
    python server.py
    # 或 uvicorn server:app --host 127.0.0.1 --port 8090
"""
import json
import logging
import os
from typing import Any, AsyncIterator

import httpx
import yaml
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, StreamingResponse

from vision import describe_image

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("llm-proxy")

# ---------------- 配置加载 ----------------
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.yaml")


def load_config() -> dict[str, Any]:
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    # 允许用环境变量覆盖敏感项
    env = os.environ
    cfg.setdefault("upstream", {})
    cfg["upstream"]["base_url"] = env.get("UPSTREAM_BASE_URL", cfg["upstream"].get("base_url", ""))
    cfg["upstream"]["api_key"] = env.get("UPSTREAM_API_KEY", cfg["upstream"].get("api_key", ""))
    cfg.setdefault("local", {"model": "qwen2.5vl:7b", "timeout": 180})
    cfg.setdefault("vision", {"enabled": True})
    cfg.setdefault("model_map", {})
    return cfg


CONFIG = load_config()
UPSTREAM_BASE = CONFIG["upstream"]["base_url"].rstrip("/")
# 规范化：config 里允许带 /v1 或不带，统一去掉末尾 /v1 后按根地址存
for suffix in ("/v1", "/v1/"):
    if UPSTREAM_BASE.endswith(suffix):
        UPSTREAM_BASE = UPSTREAM_BASE[: -len(suffix)]
        break
UPSTREAM_KEY = CONFIG["upstream"]["api_key"]
LOCAL_MODEL = CONFIG["local"].get("model", "qwen2.5vl:7b")
VISION_ENABLED = CONFIG["vision"].get("enabled", True)
MODEL_MAP = CONFIG.get("model_map", {})

app = FastAPI(title="Local LLM Image-Bridge Proxy")


# ---------------- 请求清洗：图片 -> 文本 ----------------
def extract_text(content: Any) -> list[str]:
    """取出纯文本部分（保留原有文本块）"""
    parts: list[str] = []
    if isinstance(content, str):
        parts.append(content)
    elif isinstance(content, list):
        for item in content:
            if isinstance(item, dict):
                if item.get("type") == "text" and isinstance(item.get("text"), str):
                    parts.append(item["text"])
                elif "text" in item and isinstance(item["text"], str):
                    parts.append(item["text"])
    return parts


def extract_images(content: Any) -> list[str]:
    """取出所有图片(base64 data URL 或 http url)，返回 [raw, ...]"""
    images: list[str] = []
    if isinstance(content, list):
        for item in content:
            if isinstance(item, dict) and item.get("type") == "image_url":
                url = item.get("image_url", "")
                if isinstance(url, dict):
                    url = url.get("url", "")
                if url:
                    images.append(url)
    return images


def strip_data_url(data_url: str) -> str | None:
    """data:image/png;base64,xxx -> base64 内容；http(s) 链接原样返回；其他返回 None"""
    if data_url.startswith("data:"):
        _, _, b64 = data_url.partition(",")
        return b64
    if data_url.startswith("http://") or data_url.startswith("https://"):
        return data_url
    return None


async def build_text_content(content: Any) -> str:
    """把 content 清洗成纯文本字符串（图片用本地 VLM 描述后拼入）"""
    texts = extract_text(content)
    images = extract_images(content)

    merged = "\n".join(t for t in texts if t.strip()).strip()

    if not VISION_ENABLED:
        return merged or "(该消息包含图片，但本地图片解析已关闭)"

    for raw in images:
        payload = strip_data_url(raw)
        if not payload:
            continue
        try:
            log.info("本地 VLM 解析图片中（%s）...", LOCAL_MODEL)
            desc = await describe_image(payload, LOCAL_MODEL)
            desc = desc.strip()
            # 截断过长的描述，控制注入 token 量
            max_chars = CONFIG["vision"].get("max_desc_chars", 800)
            if len(desc) > max_chars:
                desc = desc[:max_chars] + "...(截断)"
            log.info("图片解析完成: %s", (desc[:60] + "...") if len(desc) > 60 else desc)
        except Exception as e:  # noqa: BLE001
            log.warning("本地图片解析失败: %s", e)
            desc = "[图片]（本地解析失败）"
        block = f"\n\n[用户提供的图片，由本地模型解析后的内容：]\n{desc}\n[图片内容结束]"
        merged = f"{merged}{block}" if merged else f"用户提供了一张图片：{block}"

    return merged or "(空消息)"


def dict_clean(body: dict) -> dict:
    """递归删除空值，避免参数冲突"""
    if isinstance(body, dict):
        return {k: dict_clean(v) for k, v in body.items() if v is not None and v != ""}
    if isinstance(body, list):
        return [dict_clean(i) for i in body]
    return body


def map_model(name: str | None) -> str:
    """请求里的模型名 -> 上游实际模型名（支持 '*' 通配）"""
    amap = MODEL_MAP or {}
    if name in amap:
        return amap[name]
    if "*" in amap:
        return amap["*"]
    return name or "deepseek-v4-pro"


# ---------------- 路由 ----------------
@app.get("/v1/models")
async def list_models() -> JSONResponse:
    try:
        async with httpx.AsyncClient(timeout=15) as client:
            r = await client.get(
                f"{UPSTREAM_BASE}/v1/models",
                headers={"Authorization": f"Bearer {UPSTREAM_KEY}"},
            )
            if r.status_code == 200:
                return JSONResponse(r.json())
            log.warning("上游 /models 返回 %s", r.status_code)
    except Exception as e:  # noqa: BLE001
        log.warning("查询上游模型列表失败: %s", e)
    # 兜底：告诉扩展我们接受任意模型名（会被 model_map 映射）
    return JSONResponse({"object": "list", "data": [{"id": k or "*", "object": "model"} for k in MODEL_MAP] or [
        {"id": "*", "object": "model"}
    ]})


@app.post("/v1/chat/completions", response_model=None)
async def chat_completions(request: Request):
    try:
        body = await request.json()
    except Exception:  # noqa: BLE001
        return JSONResponse({"error": {"message": "invalid JSON body"}}, status_code=400)

    # 清洗 messages：图片 -> 文本
    messages = body.get("messages", [])
    new_messages: list[dict] = []
    for msg in messages:
        content = msg.get("content")
        if isinstance(content, list) and any(
            isinstance(i, dict) and i.get("type") == "image_url" for i in content
        ):
            text = await build_text_content(content)
            new_msg = dict(msg)
            new_msg["content"] = text
            new_messages.append(new_msg)
        else:
            new_messages.append(msg)

    upstream_model = map_model(body.get("model"))

    payload = dict_clean({
        "model": upstream_model,
        "messages": new_messages,
        "temperature": body.get("temperature"),
        "max_tokens": body.get("max_tokens"),
        "stream": body.get("stream", True),
        "top_p": body.get("top_p"),
        "stop": body.get("stop"),
        "tools": body.get("tools"),
    })

    headers = {
        "Authorization": f"Bearer {UPSTREAM_KEY}",
        "Content-Type": "application/json",
    }
    url = f"{UPSTREAM_BASE}/v1/chat/completions"

    log.info("转发 -> %s model=%s stream=%s images=%d",
             url, upstream_model, payload.get("stream"), sum(1 for m in messages if isinstance(m.get("content"), list)))

    client = httpx.AsyncClient(timeout=300)

    async def sse_proxy() -> AsyncIterator[bytes]:
        """SSE 流式透传"""
        try:
            async with client.stream("POST", url, json=payload, headers=headers) as resp:
                if resp.status_code >= 400:
                    err_body = (await resp.aread()).decode("utf-8", "replace")
                    log.error("上游错误 %s: %s", resp.status_code, err_body)
                    data = json.dumps({"error": {"message": f"upstream {resp.status_code}: {err_body}"}})
                    yield f"data: {data}\n\n".encode()
                    yield b"data: [DONE]\n\n"
                    return
                async for chunk in resp.aiter_raw():
                    yield chunk
        except Exception as e:  # noqa: BLE001
            log.error("SSE 转发异常: %s", e)
            err = json.dumps({"error": {"message": str(e)}})
            yield f"data: {err}\n\n".encode()
            yield b"data: [DONE]\n\n"
        finally:
            await client.aclose()

    async def non_stream_proxy() -> JSONResponse:
        try:
            resp = await client.post(url, json=payload, headers=headers)
            body = resp.json()
            if resp.status_code >= 400:
                return JSONResponse(body, status_code=resp.status_code)
            return JSONResponse(body)
        except Exception as e:  # noqa: BLE001
            return JSONResponse({"error": {"message": str(e)}}, status_code=502)
        finally:
            await client.aclose()

    if payload.get("stream", True):
        return StreamingResponse(sse_proxy(), media_type="text/event-stream")
    result = await non_stream_proxy()
    return result


@app.get("/health")
async def health() -> dict:
    return {"status": "ok", "vision_enabled": VISION_ENABLED, "local_model": LOCAL_MODEL}


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=8090, log_level="info")