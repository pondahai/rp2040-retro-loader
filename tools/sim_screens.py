#!/usr/bin/env python3
"""
sim_screens.py - 在電腦上模擬載入器的畫面,輸出 PNG 截圖

    python tools/sim_screens.py ../rp2040-handheld-bundle/covers -o shots

照 loader/lcd.c 與 loader/coverflow.c 的繪圖邏輯重畫一次(背景牆、封面帶、
選取框、8x8 文字),背景磁磚與字型直接讀 bgtile.h / font8x8.h。
不是實機截圖: LCD 色偏、SPI 更新的撕裂看不到;燒錄進度數字是假的。
改了那兩個 .c 的畫法,這裡要跟著改。
"""
import argparse
import re, sys, os
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("covers", help="放 .RAW 封面的資料夾")
ap.add_argument("-o", "--out", default="shots")
args = ap.parse_args()
REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
COVERS = args.covers; OUT = args.out
os.makedirs(OUT, exist_ok=True)
W, H = 320, 240
C_BLACK, C_WHITE, C_GREY, C_GREEN, C_RED = 0, 0xFFFF, 0x8410, 0x07E0, 0xF800
C_BGINK, C_BGPAPER = 0xC73E, 0xFFFF

def carr(path, name):
    s = open(path, encoding="utf-8").read()
    body = s[s.index(name):]; body = body[body.index("=") + 1: body.index("};")]
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]

tile = carr(os.path.join(REPO, "loader/bgtile.h"), "bgtile[")
font = carr(os.path.join(REPO, "loader/font8x8.h"), "font8x8[")
TW, TH, STRIDE = 64, 32, 8
fb = [[0] * W for _ in range(H)]

def bg_line(y):
    off = TW // 2 if (y // TH) & 1 else 0
    row = tile[(y % TH) * STRIDE:(y % TH + 1) * STRIDE]
    return [C_BGINK if (row[((x + off) % TW) >> 3] >> (((x + off) % TW) & 7)) & 1 else C_BGPAPER for x in range(W)]

def clear_bg():
    for y in range(H): fb[y] = bg_line(y)
def clear(c):
    for y in range(H): fb[y] = [c] * W
def fill(x, y, w, h, c):
    for j in range(y, y + h):
        for i in range(x, x + w):
            if 0 <= i < W and 0 <= j < H: fb[j][i] = c
def frame(x, y, w, h, c):
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c)
def putc(col, row, ch, fg, bg):
    o = ord(ch);  o = o if 0x20 <= o <= 0x7E else ord('?')
    g = font[(o - 0x20) * 8:(o - 0x20) * 8 + 8]
    for y in range(8):
        for x in range(8): fb[row * 8 + y][col * 8 + x] = fg if g[y] >> x & 1 else bg
def puts_line(row, s, fg, bg):
    for c in range(40): putc(c, row, s[c] if c < len(s) else ' ', fg, bg)

def load_raw(p):
    d = open(p, "rb").read()
    return [(d[i] << 8) | d[i + 1] for i in range(0, len(d), 2)]

names = sorted(f for f in os.listdir(COVERS) if f.upper().endswith(".RAW"))
covers = [load_raw(os.path.join(COVERS, f)) for f in names]
TWN = 96; PITCH = 120; CX = (W - TWN) // 2; SY = 72; SPAN = 2

def render(centre, shift):
    items = [(CX + (k - centre) * PITCH + shift, covers[k]) for k in range(centre - SPAN, centre + SPAN + 1)
             if 0 <= k < len(covers)]
    for y in range(TWN):
        line = bg_line(SY + y)
        for x0, px in items:
            for j in range(TWN):
                if 0 <= x0 + j < W: line[x0 + j] = px[y * TWN + j]
        fb[SY + y] = line
    frame(CX - 3, SY - 3, TWN + 6, TWN + 6, C_BLACK)

def save(name):
    img = Image.new("RGB", (W, H)); p = img.load()
    for y in range(H):
        for x in range(W):
            v = fb[y][x]; p[x, y] = ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
    img.resize((W * 2, H * 2), Image.NEAREST).save(os.path.join(OUT, name))
    print(name)

clear_bg(); render(0, 0); save("01_menu_first.png")
clear_bg(); render(1, 0); save("02_menu_second.png")
clear_bg(); render(1, PITCH * 4 * 4 // 64); save("03_slide_midframe.png")   # frame i=4 of 8
clear(C_WHITE)
puts_line(4, " Loading:", C_GREY, C_WHITE); puts_line(5, names[1].replace(".RAW", ".UF2"), C_BLACK, C_WHITE)
puts_line(7, " Do not power off.", C_RED, C_WHITE); puts_line(28, " WRITING FLASH   42%  (840/2000)", C_BLACK, C_WHITE)
save("04_flashing.png")
