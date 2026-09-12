#!/usr/bin/env python3
"""
raw2png.py - 把載入器的 .RAW 封面轉回 PNG,用來檢查風格

    python tools/raw2png.py DOOM.RAW -o doom.png
    python tools/raw2png.py *.RAW --sheet covers.png --scale 3

make_thumb.py 的反向。新封面做好之後,跟既有那幾張並排放大看一次 ——
單獨看都覺得沒問題,排在一起才會發現自己那張特別暗、特別空、或色數
明顯少一個數量級。風格規範見
rp2040-retro-handheld/docs/ICON-STYLE.md。

.RAW 沒有標頭,就是 96*96 個 big-endian RGB565 像素,固定 18432 bytes。
"""

import argparse
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow: pip install Pillow")

W = H = 96
EXPECT = W * H * 2


def load(path):
    data = open(path, "rb").read()
    if len(data) != EXPECT:
        raise SystemExit("%s: 應該是 %d bytes,實際 %d —— 這不是載入器吃的封面"
                         % (path, EXPECT, len(data)))
    img = Image.new("RGB", (W, H))
    px = img.load()
    for i in range(W * H):
        v = struct.unpack_from(">H", data, i * 2)[0]
        px[i % W, i // W] = (((v >> 11) & 0x1F) * 255 // 31,
                             ((v >> 5) & 0x3F) * 255 // 63,
                             (v & 0x1F) * 255 // 31)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="+", help="一個以上的 .RAW")
    ap.add_argument("-o", "--output", help="單張輸出的 PNG")
    ap.add_argument("--sheet", help="把全部並排成一張對照圖")
    ap.add_argument("--scale", type=int, default=3, help="放大倍率(預設 3)")
    a = ap.parse_args()

    imgs = [(p, load(p)) for p in a.source]

    for p, im in imgs:
        colors = len(im.getcolors(W * H) or [])
        note = "  <- 色數偏少,可能忘了畫明暗" if colors < 100 else ""
        print("%-34s %5d 色%s" % (p, colors, note))

    s = a.scale
    if a.sheet:
        sheet = Image.new("RGB", (W * s * len(imgs), H * s), (255, 255, 255))
        for i, (_p, im) in enumerate(imgs):
            sheet.paste(im.resize((W * s, H * s), Image.NEAREST), (i * W * s, 0))
        sheet.save(a.sheet)
        print("->", a.sheet)
    elif a.output:
        im = imgs[0][1]
        im.resize((W * s, H * s), Image.NEAREST).save(a.output)
        print("->", a.output)
    else:
        ap.error("要嘛給 -o,要嘛給 --sheet")


if __name__ == "__main__":
    main()
