#!/usr/bin/env python3
"""
make_bgtile.py - 把一張 logo 轉成選單背景牆用的 1bpp 磁磚

    python tools/make_bgtile.py logo.jpg -o loader/bgtile.h

為什麼是 1bpp: 載入器全部只有 16KB,而且已經用掉四分之三。同樣一塊
64x32 的磁磚,RGB565 要 4096 bytes(直接爆掉),1bpp 只要 256 bytes。
反正背景本來就只有「白底」與「淺色 logo」兩種顏色,多的位元沒有用處。

輸出是一個 C 標頭檔,不是二進位檔 —— 磁磚必須跟載入器一起燒進 flash,
不能像封面那樣放 SD 卡(背景在讀卡之前就要畫出來)。

位元順序刻意跟 font8x8 一致: 每列由左到右,bit0 是最左邊的點,
這樣 lcd.c 取點的寫法兩邊可以長得一樣。
"""

import argparse
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow: pip install Pillow")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="來源圖檔 (png/jpg/...)")
    ap.add_argument("-o", "--output", required=True, help="輸出的 .h")
    ap.add_argument("--tile-w", type=int, default=64, help="磁磚寬(預設 64)")
    ap.add_argument("--tile-h", type=int, default=32, help="磁磚高(預設 32)")
    ap.add_argument("--margin", type=int, default=6,
                    help="logo 四周留白,logo 才不會黏成一片(預設 6)")
    ap.add_argument("--threshold", type=int, default=200,
                    help="灰階低於此值算 logo 的點(預設 200)")
    ap.add_argument("--invert", action="store_true",
                    help="來源是深底淺字時加這個")
    args = ap.parse_args()

    tw, th = args.tile_w, args.tile_h
    if tw % 8:
        sys.exit("磁磚寬必須是 8 的倍數(1bpp 一列要湊滿整數個 byte)")
    if tw % 2:
        sys.exit("磁磚寬必須是偶數(交錯排列要位移半塊)")

    im = Image.open(args.source).convert("L")

    # 等比例縮到留白之內。logo 是橫的,通常會卡在寬度上。
    box_w, box_h = tw - 2 * args.margin, th - 2 * args.margin
    if box_w <= 0 or box_h <= 0:
        sys.exit("margin 太大,logo 沒有空間了")
    scale = min(box_w / im.width, box_h / im.height)
    new = (max(1, round(im.width * scale)), max(1, round(im.height * scale)))
    im = im.resize(new, Image.LANCZOS)

    # 白底的磁磚,logo 置中貼上去
    tile = Image.new("L", (tw, th), 255)
    tile.paste(im, ((tw - new[0]) // 2, (th - new[1]) // 2))

    px = tile.load()
    rows = []
    ink = 0
    for y in range(th):
        row = bytearray(tw // 8)
        for x in range(tw):
            on = px[x, y] < args.threshold
            if args.invert:
                on = not on
            if on:
                row[x // 8] |= 1 << (x % 8)
                ink += 1
        rows.append(row)

    if ink == 0:
        sys.exit("轉出來一個點都沒有,調 --threshold 或加 --invert")

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("/*\n"
                " * bgtile.h - 選單背景牆的磁磚 (1bpp)\n"
                " *\n"
                " * 這個檔案是生成的,不要手改:\n"
                f" *     python tools/make_bgtile.py {args.source} -o loader/bgtile.h\n"
                " *\n"
                " * 每一列由左到右,bit0 是最左邊的點(跟 font8x8 同一個約定)。\n"
                " * 1 = logo 的顏色,0 = 底色。畫法見 lcd.c 的 lcd_bg_scanline()。\n"
                " */\n"
                "#ifndef LOADER_BGTILE_H\n"
                "#define LOADER_BGTILE_H\n\n"
                "#include <stdint.h>\n\n")
        f.write(f"#define BGTILE_W {tw}\n")
        f.write(f"#define BGTILE_H {th}\n")
        f.write(f"#define BGTILE_STRIDE {tw // 8}\n\n")
        f.write("static const uint8_t bgtile[BGTILE_H][BGTILE_STRIDE] = {\n")
        for row in rows:
            f.write("    {" + ", ".join(f"0x{b:02X}" for b in row) + "},\n")
        f.write("};\n\n#endif /* LOADER_BGTILE_H */\n")

    print(f"{args.output}: {tw}x{th} 1bpp, {th * (tw // 8)} bytes, {ink} 個點")


if __name__ == "__main__":
    main()
