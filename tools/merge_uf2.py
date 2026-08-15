#!/usr/bin/env python3
"""
merge_uf2.py - 把跳板與專題本體合併成一份可單獨燒錄的 UF2

專題本體 link 在 0x10004000,前面 16KB 是空的,所以它自己那份 UF2
沒辦法用 USB 直接拖進 Pico —— flash 最前面沒有東西可以開機。
把跳板接在前面就補上了這一段:

    python tools/merge_uf2.py build/trampoline.uf2 infones.uf2 -o infones_standalone.uf2

跳板與本體之間的空隙會用 0xFF 的 block 補滿,讓整份 UF2 的位址連續 ——
實測發現位址不連續的 UF2 拖進 RPI-RP2 磁碟時只會寫進第一段(見 pad_gap)。

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


def payload_size(b):
    return struct.unpack_from("<I", b, 16)[0]


def make_filler(template, addr, size=256):
    """
    產生一塊「內容全是 0xFF」的 UF2 block。

    0xFF 就是 flash 抹除後的狀態,所以寫進去等於什麼都沒做 —— 而且這段
    位址本來就是保留給載入器/跳板的,不會蓋到任何東西。

    flags 與 family id 沿用範本(跳板的第一塊),免得填充塊跟其他塊不一致。
    """
    b = bytearray(template)
    struct.pack_into("<I", b, 12, addr)          # target_addr
    struct.pack_into("<I", b, 16, size)          # payload_size
    b[32:32 + 476] = b"\xff" * 476
    return b


def pad_gap(tramp, app):
    """
    把跳板結尾到本體起點之間的空隙用 0xFF 的 block 補滿,
    讓整份 UF2 的位址連續。

    為什麼要補: 實測發現 Windows 把「位址不連續」的 UF2 拖進 RPI-RP2
    磁碟時,只有第一段會寫進去,後面完全不見,而且沒有任何錯誤訊息
    (連續的同尺寸 UF2 則正常)。確切機制未查明,但斷點是唯一的變因。

    跳板存在的理由就是讓合併版能被 USB 直接燒錄,所以這個情境不能壞。
    代價是檔案多幾十塊(約 20KB),相對於 1MB 可以忽略。
    """
    if not tramp or not app:
        return []

    last = tramp[-1]
    gap_start = target_addr(last) + payload_size(last)
    gap_end = target_addr(app[0])

    if gap_start >= gap_end:
        return []

    filler = []
    addr = gap_start
    while addr < gap_end:
        size = min(256, gap_end - addr)
        filler.append(make_filler(last, addr, size))
        addr += size
    return filler


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trampoline", help="build/trampoline.uf2")
    ap.add_argument("app", help="專題本體的 uf2 (link 在 0x10004000)")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--no-pad", action="store_true",
                    help="不要用 0xFF 補跳板與本體之間的空隙(預設會補,見 pad_gap)")
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

    filler = [] if args.no_pad else pad_gap(tramp, app)
    merged = tramp + filler + app
    total = len(merged)

    # block_no / num_blocks 是整份檔案的流水號,接起來之後要重編
    for i, b in enumerate(merged):
        struct.pack_into("<II", b, 20, i, total)

    with open(args.output, "wb") as f:
        for b in merged:
            f.write(b)

    parts = f"跳板 {len(tramp)}"
    if filler:
        parts += f" + 填充 {len(filler)}"
    parts += f" + 本體 {len(app)}"
    print(f"{args.output}: {total} blocks ({parts}), {total * BLOCK_SIZE} bytes")


if __name__ == "__main__":
    main()
