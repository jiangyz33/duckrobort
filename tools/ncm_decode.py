"""网易云 .ncm 解密（对齐 ncmdump 参考实现）

流程：
  魔数 CTENFDAM → 跳 2 字节 → 4 字节 key 长度 → key 数据（逐字节 ^0x64）
  → AES-128-ECB 解密（密钥 hzHRAmso5kInbaxW）→ 去 PKCS7 填充 → 去掉前 17 字节前缀
  → 4 字节 meta 长度 → meta（逐字节 ^0x63）→ 去 22 字节前缀 → base64 → AES → JSON
  → 4 字节 CRC + 跳 5 字节 → 4 字节封面长度 + 封面 → 音频

音频不是标准 RC4 流密码，而是「查表逐字节异或」，且每 0x8000 字节重置一次 j。

用法: python ncm_decode.py <输入.ncm> <输出目录>
"""

import base64
import json
import os
import sys

from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad

MAGIC = b"CTENFDAM"
CORE_KEY = bytes(
    [0x68, 0x7A, 0x48, 0x52, 0x41, 0x6D, 0x73, 0x6F,
     0x35, 0x6B, 0x49, 0x6E, 0x62, 0x61, 0x78, 0x57]
)
META_KEY = b"#14ljk_!\\]&0U<'("


def _build_key_box(key):
    box = bytearray(range(256))
    c = 0
    last_byte = 0
    offset = 0
    for i in range(256):
        swap = box[i]
        c = (swap + last_byte + key[offset]) & 0xFF
        offset += 1
        if offset >= len(key):
            offset = 0
        box[i] = box[c]
        box[c] = swap
        last_byte = c
    return box


def _decrypt_audio(data, box):
    out = bytearray(data)
    n = len(out)
    pos = 0
    while pos < n:
        chunk_len = min(0x8000, n - pos)
        for i in range(1, chunk_len + 1):
            j = i & 0xFF
            out[pos + i - 1] ^= box[(box[j] + box[(box[j] + j) & 0xFF]) & 0xFF]
        pos += chunk_len
    return bytes(out)


def decode(path, out_dir):
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            raise ValueError("不是 ncm 文件（魔数不匹配）")
        f.seek(2, 1)

        key_len = int.from_bytes(f.read(4), "little")
        key_data = bytes(b ^ 0x64 for b in f.read(key_len))
        key = unpad(AES.new(CORE_KEY, AES.MODE_ECB).decrypt(key_data), 16)[17:]
        box = _build_key_box(key)

        meta_len = int.from_bytes(f.read(4), "little")
        meta = {}
        if meta_len:
            raw_meta = bytes(b ^ 0x63 for b in f.read(meta_len))
            try:
                plain = unpad(AES.new(META_KEY, AES.MODE_ECB).decrypt(
                    base64.b64decode(raw_meta[22:])), 16)
                text = plain.decode("utf-8")
                if text.startswith("music:"):
                    text = text[len("music:"):]
                meta = json.loads(text)
            except Exception as exc:  # noqa: BLE001
                print(f"  [warn] 元数据解析失败: {exc}")

        f.seek(4, 1)   # CRC32
        f.seek(5, 1)   # 保留
        image_len = int.from_bytes(f.read(4), "little")
        f.seek(image_len, 1)
        audio = _decrypt_audio(f.read(), box)

    fmt = (meta.get("format") or "").lower()
    if audio[:4] == b"fLaC":
        fmt = "flac"
    elif audio[:3] == b"ID3":
        fmt = "mp3"
    elif audio[0] == 0xFF and (audio[1] & 0xE0) == 0xE0:
        fmt = "mp3"
    if fmt not in ("flac", "mp3"):
        raise ValueError(f"解密后无法识别音频格式（前 4 字节 {audio[:4]!r}）")

    name = meta.get("musicName") or os.path.splitext(os.path.basename(path))[0]
    try:
        artist = meta["artist"][0][0]
    except Exception:  # noqa: BLE001
        artist = ""
    if artist and artist not in name:
        name = f"{artist} - {name}"

    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, f"{name}.{fmt}")
    with open(dst, "wb") as f:
        f.write(audio)

    print(f"  标题: {meta.get('musicName', '?')}")
    print(f"  歌手: {artist}")
    print(f"  专辑: {meta.get('album', '')}")
    print(f"  格式: {fmt}   {len(audio) / 1024 / 1024:.2f} MB")
    print(f"  输出: {dst}")
    return dst


if __name__ == "__main__":
    decode(sys.argv[1], sys.argv[2])
