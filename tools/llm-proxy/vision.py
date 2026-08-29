# -*- coding: utf-8 -*-
"""
本地图片解析后端：
1. 首选 Ollama VLM（如 qwen2.5vl），用本地显卡跑，图片 -> 文字描述
2. 可选 RapidOCR 兜底（纯文本图片更快更准，CPU 即可）
"""
import asyncio
import base64
import logging
from typing import Optional

import httpx

log = logging.getLogger("llm-proxy.vision")

OLLAMA_URL = "http://127.0.0.1:11434"

SYSTEM_PROMPT = (
    "你是一个图片内容分析助手。请用简洁的中文描述这张图片的主要内容："
    "如果是界面/报错截图，请逐字提取其中所有文字和关键信息（尤其是错误信息、代码、配置项）；"
    "如果是图表/图片，描述其结构和要点。输出只包含分析结果，不要加多余解释。"
)


async def ollama_describe(image_base64: str, model: str, timeout: float = 180.0) -> str:
    """调用 Ollama /api/generate 让 VLM 描述图片。image_base64 可能是裸 base64、data-url 或 http url。"""
    payload = {"model": model, "prompt": SYSTEM_PROMPT, "stream": False}
    stripped: str = image_base64

    if image_base64.startswith("data:"):
        _, _, stripped = image_base64.partition(",")
    elif image_base64.startswith("http://") or image_base64.startswith("https://"):
        # 远程图片：先下载成 base64
        async with httpx.AsyncClient(timeout=60) as dc:
            resp = await dc.get(image_base64)
            resp.raise_for_status()
            stripped = base64.b64encode(resp.content).decode()

    payload["images"] = [stripped]

    async with httpx.AsyncClient(timeout=timeout) as client:
        resp = await client.post(f"{OLLAMA_URL}/api/generate", json=payload)
        resp.raise_for_status()
        data = resp.json()
        if data.get("error"):
            raise RuntimeError(data["error"])
        text = data.get("response", "").strip()
        if not text:
            raise RuntimeError("Ollama 返回空结果")
        return text


async def rapidocr_describe(image_bytes: bytes) -> str:
    """纯 OCR：提取图片中的全部文字（RapidOCR onnxruntime CPU 版，可选依赖）"""
    try:
        from rapidocr_onnxruntime import RapidOCR
    except ImportError:
        raise RuntimeError("未安装 rapidocr_onnxruntime，请 pip install rapidocr_onnxruntime") from None

    engine = RapidOCR()
    result = await asyncio.get_event_loop().run_in_executor(
        None, lambda: engine(image_bytes)
    )
    lines = result if isinstance(result, tuple) else (result or (None, None))
    text_lines = []
    if lines and lines[0]:
        for item in lines[0]:
            text_lines.append(item[1])
    text = "\n".join(text_lines).strip()
    if not text:
        return "(图片中未识别到文字)"
    return text


async def describe_image(image: str, model: str) -> str:
    """统一入口：image 为 data-url(base64) 或 http url。默认 VLM，可 fallback OCR。"""
    try:
        return await ollama_describe(image, model)
    except Exception as e:  # noqa: BLE001
        log.warning("Ollama VLM 失败(%s)，退回 OCR", e)
        try:
            if image.startswith("data:"):
                _, _, b64 = image.partition(",")
                img_bytes = base64.b64decode(b64)
            elif image.startswith("http"):
                async with httpx.AsyncClient(timeout=60) as dc:
                    resp = await dc.get(image)
                    resp.raise_for_status()
                    img_bytes = resp.content
            else:
                img_bytes = base64.b64decode(image)
            return await rapidocr_describe(img_bytes)
        except Exception as e2:  # noqa: BLE001
            log.error("OCR 兜底也失败: %s", e2)
            raise RuntimeError(f"本地图片解析失败: {e2}") from e2