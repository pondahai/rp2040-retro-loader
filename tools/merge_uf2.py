#!/usr/bin/env python3
"""
merge_uf2.py - 把跳板與專題本體合併成一份可單獨燒錄的 UF2

專題本體 link 在 0x10004000,前面 16KB 是空的,所以它自己那份 UF2
沒辦法用 USB 直接拖進 Pico —— flash 最前面沒有東西可以開機。
把跳板接在前面就補上了這一段:

    python tools/merge_uf2.py build/trampoline.uf2 infones.uf2 -o infones_standalone.uf2

合併出來的檔案兩種用法都吃得下:
  * USB BOOTSEL 拖進去 -> 跳板落在 0x10000000,開機後跳 0x10004000
  * 放進 SD 卡給載入器 -> 載入器丟掉 0x10004000 以下的 block,只寫本體

也就是說 SD 卡上放合併版就好,不必為兩種用法各準備一個檔案。

UF2 格式: https://github.com/microsoft/uf2
"""

import argparse
import struct
import sys

BLOCK_SIZE = 512
MAGIC0 = 0x0A324655
MAGIC1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30

APP_BASE = 0x10004000   # 必須跟 common/boot_map.h 一致


def read_blocks(path):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) % BLOCK_SIZE:
        sys.exit(f"{path}: 長度 {len(data)} 不是 512 的倍數,不像 UF2")

    blocks = []
    for i in range(0, len(data), BLOCK_SIZE):
        b = bytearray(data[i:i + BLOCK_SIZE])
        m0, m1 = struct.unpack_from("<II", b, 0)
        (mend,) = struct.unpack_from("<I", b, 508)
        if m0 != MAGIC0 or m1 != MAGIC1 or mend != MAGIC_END:
            sys.exit(f"{path}: 第 {i // BLOCK_SIZE} 塊的 magic 不對")
        blocks.append(b)
    return blocks


def target_addr(b):
    return struct.unpack_from("<I", b, 12)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trampoline", help="build/trampoline.uf2")
    ap.add_argument("app", help="專題本體的 uf2 (link 在 0x10004000)")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    tramp = read_blocks(args.trampoline)
    app = read_blocks(args.app)

    # 兩邊都放錯位置的話,合併出來的東西會安靜地壞掉,所以在這裡擋
    bad = [hex(target_addr(b)) for b in tramp if target_addr(b) >= APP_BASE]
    if bad:
        sys.exit(f"跳板不該有 >= {APP_BASE:#x} 的 block: {bad[:4]}")

    bad = [hex(target_addr(b)) for b in app if target_addr(b) < APP_BASE]
    if bad:
        sys.exit(
            f"專題本體有 < {APP_BASE:#x} 的 block: {bad[:4]}\n"
            "它八成還是用預設的 linker script 編的。"
            "請改用 app/memmap_app.ld 重編。"
        )

    merged = tramp + app
    total = len(merged)

    # block_no / num_blocks 是整份檔案的流水號,接起來之後要重編
    for i, b in enumerate(merged):
        struct.pack_into("<II", b, 20, i, total)

    with open(args.output, "wb") as f:
        for b in merged:
            f.write(b)

    print(f"{args.output}: {total} blocks "
          f"(跳板 {len(tramp)} + 本體 {len(app)}), {total * BLOCK_SIZE} bytes")


if __name__ == "__main__":
    main()
