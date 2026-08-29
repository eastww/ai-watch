# -*- coding: utf-8 -*-
"""本地端到端自测：发一条带图片的消息走完整链路"""
import base64
import io

import httpx
from PIL import Image, ImageDraw

# 1. 生成一张小测试图（白底黑字 "Error: Model not support image"）
img = Image.new("RGB", (420, 100), "white")
d = ImageDraw.Draw(img)
d.text((20, 30), "Error: Model not support image", fill="black")

buf = io.BytesIO()
img.save(buf, format="PNG")
b64 = base64.b64encode(buf.getvalue()).decode()

# 2. 构造 OpenAI 兼容消息（含图片）
payload = {
    "model": "deepseek-v4-pro",
    "stream": False,
    "messages": [
        {
            "role": "user",
            "content": [
                {"type": "text", "text": "这张图里写了什么？"},
                {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b64}"}},
            ],
        }
    ],
}

r = httpx.post("http://127.0.0.1:8090/v1/chat/completions", json=payload, timeout=300)
print("HTTP status:", r.status_code)
try:
    data = r.json()
    if "choices" in data:
        print("回复内容:", data["choices"][0]["message"]["content"][:300])
    else:
        print("返回:", data)
except Exception as e:  # noqa: BLE001
    print("解析失败:", e, r.text[:500])