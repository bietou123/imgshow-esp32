#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkimg.py  —  任意图片 -> 128x128 RGB565 (little-endian) 裸数据 .565

用法:
    python tools/mkimg.py photo.jpg                 # 生成 photo.565
    python tools/mkimg.py *.jpg -o out/             # 批量
    python tools/mkimg.py a.png --fit contain       # 留黑边而不是裁切

依赖: pip install pillow

生成的 .565 固定 32768 字节，直接通过网页上传，或者
用 mklittlefs 打包整个 data 目录再烧进 LittleFS 区。

不想装 Python 库的话，ffmpeg 一行也能做(裁切模式):
  ffmpeg -i in.jpg -vf "scale=128:128:force_original_aspect_ratio=increase,crop=128:128" \
         -pix_fmt rgb565le -f rawvideo out.565
"""
import argparse, os, sys, glob

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 pillow:  pip install pillow")

W = H = 128


def to565(img, fit):
    img = img.convert("RGB")
    if fit == "contain":
        img.thumbnail((W, H), Image.LANCZOS)
        canvas = Image.new("RGB", (W, H), (0, 0, 0))
        canvas.paste(img, ((W - img.width) // 2, (H - img.height) // 2))
        img = canvas
    else:  # cover: 先放大到铺满，再中心裁切
        s = max(W / img.width, H / img.height)
        img = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)
        left = (img.width - W) // 2
        top = (img.height - H) // 2
        img = img.crop((left, top, left + W, top + H))

    out = bytearray(W * H * 2)
    px = img.load()
    i = 0
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = v & 0xFF          # 低字节在前 = little-endian
            out[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--outdir", default=None)
    ap.add_argument("--fit", choices=["cover", "contain"], default="cover")
    a = ap.parse_args()

    paths = []
    for f in a.files:
        paths.extend(glob.glob(f) or [f])
    if a.outdir:
        os.makedirs(a.outdir, exist_ok=True)

    for p in paths:
        try:
            data = to565(Image.open(p), a.fit)
        except Exception as e:
            print("skip %s: %s" % (p, e))
            continue
        base = os.path.splitext(os.path.basename(p))[0][:12]
        dst = os.path.join(a.outdir or os.path.dirname(p) or ".", base + ".565")
        with open(dst, "wb") as fh:
            fh.write(data)
        print("%s -> %s (%d bytes)" % (p, dst, len(data)))


if __name__ == "__main__":
    main()
