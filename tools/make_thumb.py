#!/usr/bin/env python3
"""
make_thumb.py - 把一張圖轉成載入器吃的 96x96 RGB565 縮圖

    python tools/make_thumb.py doom.png -o DOOM.RAW

輸出沒有標頭,就是 96*96 個 big-endian RGB565 像素,固定 18432 bytes。
載入器只用檔案長度來擋「拖錯檔案」(沒有標頭可以驗),所以長度必須剛好。

檔名要跟 UF2 一致: DOOM_TINY.UF2 配 DOOM_TINY.RAW,兩個都放 SD 卡根目錄。

為什麼不是 PNG/JPEG: 解碼器就等於程式碼,而載入器全部只有 16KB。
為什麼是 big-endian: ILI9341 線上格式就是高位元組先送,這樣載入器可以把
從 SD 讀到的磁區原封不動丟給 SPI,中間不必逐像素轉換。
"""

import argparse
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("需要 Pillow: pip install Pillow")

W = H = 96
EXPECT = W * H * 2


def _close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a[:3], b))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="來源圖檔 (png/jpg/...)")
    ap.add_argument("-o", "--output", required=True, help="輸出的 .RAW")
    ap.add_argument("--fit", action="store_true",
                    help="等比例縮放後置中裁切(預設是直接拉伸成正方形)")
    ap.add_argument("--bg", default="000000",
                    help="透明區要合成的底色,RRGGBB 十六進位(預設 000000 黑)")
    ap.add_argument("--drop-bg", action="store_true",
                    help="把四角連通的單色背景(通常是去背照的白底)換成 --bg 的顏色")
    ap.add_argument("--drop-tol", type=int, default=40,
                    help="--drop-bg 的容許誤差,越大吃掉越多(預設 40)")
    args = ap.parse_args()

    try:
        bg = tuple(int(args.bg.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError:
        sys.exit(f"--bg 要是 RRGGBB 格式,收到的是 {args.bg!r}")

    im = Image.open(args.source)

    # RGB565 沒有 alpha,載入器也不可能做透明(那需要 framebuffer,而它只有 16KB)。
    # 所以透明區必須在這裡就決定要變成什麼顏色 —— 直接 convert("RGB") 的話
    # PIL 會把透明區塗成黑色,通常不是想要的結果。
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        flat = Image.new("RGB", im.size, bg)
        flat.paste(im, mask=im.split()[-1])
        im = flat
    else:
        im = im.convert("RGB")

    if args.drop_bg:
        # 從四角 flood fill,而不是全域取代所有接近白的像素 —— 那會把主體內部
        # 的白(螢幕反光、按鍵標字)一起挖成黑洞。只換掉跟邊緣連通的那一片。
        #
        # 在縮放前做: 全解析度下背景與主體的邊界最乾淨,之後縮小時 LANCZOS
        # 會自然把邊緣混進底色,不會留下一圈白邊。
        for corner in [(0, 0), (im.width - 1, 0),
                       (0, im.height - 1), (im.width - 1, im.height - 1)]:
            if _close(im.getpixel(corner), bg, args.drop_tol):
                continue        # 這一角本來就是底色了,不必動
            ImageDraw.floodfill(im, corner, bg, thresh=args.drop_tol)

    if args.fit:
        # 等比例縮到短邊剛好,再從中間裁一塊正方形
        scale = max(W / im.width, H / im.height)
        im = im.resize((max(1, round(im.width * scale)),
                        max(1, round(im.height * scale))), Image.LANCZOS)
        left = (im.width - W) // 2
        top = (im.height - H) // 2
        im = im.crop((left, top, left + W, top + H))
    else:
        im = im.resize((W, H), Image.LANCZOS)

    out = bytearray()
    for r, g, b in im.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += v.to_bytes(2, "big")

    if len(out) != EXPECT:
        sys.exit(f"內部錯誤: 產出 {len(out)} bytes,應該是 {EXPECT}")

    with open(args.output, "wb") as f:
        f.write(out)

    print(f"{args.output}: {W}x{H} RGB565, {len(out)} bytes")


if __name__ == "__main__":
    main()
