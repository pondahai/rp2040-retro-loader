#!/usr/bin/env python3
"""
merge_uf2.py - 把跳板與一個以上的專題片段合併成一份可單獨燒錄的 UF2

專題本體 link 在 0x10004000,前面 16KB 是空的,所以它自己那份 UF2
沒辦法用 USB 直接拖進 Pico —— flash 最前面沒有東西可以開機。
把跳板接在前面就補上了這一段:

    python tools/merge_uf2.py build/trampoline.uf2 infones.uf2 -o infones_standalone.uf2

第二個以後的參數可以有很多個,依 flash 位址由低到高排好。這是給
「一份韌體不夠、還帶著資料」的專題用的 —— DOOM 就是: 韌體 257KB 之外
還有 1.7MB 的地圖資料要燒到 TINY_WAD_ADDR:

    python tools/bin2uf2.py doom1.whx -a 0x10046000 -o doom_wad.uf2
    python tools/merge_uf2.py build/trampoline.uf2 doom.uf2 doom_wad.uf2 -o DOOM.uf2

這麼做的理由見 bin2uf2.py 的說明: 資料跟韌體綁在同一份檔案裡,每次選這個
專題都會一起重寫,就不怕被其他專題的映像蓋掉。

各段之間的空隙會用 0xFF 的 block 補滿,讓整份 UF2 的位址連續 ——
實測發現位址不連續的 UF2 拖進 RPI-RP2 磁碟時只會寫進第一段(見 pad_gap)。

合併出來的檔案兩種用法都吃得下:
  * USB BOOTSEL 拖進去 -> 跳板落在 0x10000000,開機後跳 0x10004000
  * 放進 SD 卡給載入器 -> 載入器丟掉 0x10004000 以下的 block,其餘照寫

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


def pad_gap(prev, nxt):
    """
    把前一段結尾到下一段起點之間的空隙用 0xFF 的 block 補滿,
    讓整份 UF2 的位址連續。

    為什麼要補: 實測發現 Windows 把「位址不連續」的 UF2 拖進 RPI-RP2
    磁碟時,只有第一段會寫進去,後面完全不見,而且沒有任何錯誤訊息
    (連續的同尺寸 UF2 則正常)。確切機制未查明,但斷點是唯一的變因。

    跳板存在的理由就是讓合併版能被 USB 直接燒錄,所以這個情境不能壞。
    代價是檔案多幾十塊(約 20KB),相對於 1MB 可以忽略。
    """
    if not prev or not nxt:
        return []

    last = prev[-1]
    gap_start = target_addr(last) + payload_size(last)
    gap_end = target_addr(nxt[0])

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
    ap.add_argument("parts", nargs="+",
                    help="專題本體的 uf2 (link 在 0x10004000),"
                         "後面可以再接資料段的 uf2,依位址由低到高")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--no-pad", action="store_true",
                    help="不要用 0xFF 補各段之間的空隙(預設會補,見 pad_gap)")
    args = ap.parse_args()

    tramp = read_blocks(args.trampoline)
    parts = [read_blocks(p) for p in args.parts]

    # 放錯位置的話,合併出來的東西會安靜地壞掉,所以在這裡擋
    bad = [hex(target_addr(b)) for b in tramp if target_addr(b) >= APP_BASE]
    if bad:
        sys.exit(f"跳板不該有 >= {APP_BASE:#x} 的 block: {bad[:4]}")

    for name, blocks in zip(args.parts, parts):
        bad = [hex(target_addr(b)) for b in blocks if target_addr(b) < APP_BASE]
        if bad:
            sys.exit(
                f"{name} 有 < {APP_BASE:#x} 的 block: {bad[:4]}\n"
                "它八成還是用預設的 linker script 編的。"
                "請改用 app/memmap_app.ld 重編。"
            )

    # 各段必須由低到高且互不重疊。重疊的後果是後面那段把前面覆蓋掉一部分,
    # 而且燒完才會發現 —— 症狀是「開得起來但資料是壞的」,最難查的那種。
    prev_name, prev_end = args.trampoline, None
    for name, blocks in zip(args.parts, parts):
        start = target_addr(blocks[0])
        if prev_end is not None and start < prev_end:
            sys.exit(f"{name} 從 {start:#x} 開始,與前一段 {prev_name} "
                     f"(結尾 {prev_end:#x}) 重疊")
        prev_name = name
        prev_end = target_addr(blocks[-1]) + payload_size(blocks[-1])

    merged = list(tramp)
    for blocks in parts:
        if not args.no_pad:
            merged += pad_gap(merged, blocks)
        merged += blocks
    total = len(merged)

    # block_no / num_blocks 是整份檔案的流水號,接起來之後要重編
    for i, b in enumerate(merged):
        struct.pack_into("<II", b, 20, i, total)

    with open(args.output, "wb") as f:
        for b in merged:
            f.write(b)

    desc = f"跳板 {len(tramp)}"
    for name, blocks in zip(args.parts, parts):
        desc += f" + {name} {len(blocks)}"
    filler = total - len(tramp) - sum(len(b) for b in parts)
    if filler:
        desc += f" + 填充 {filler}"
    print(f"{args.output}: {total} blocks ({desc}), {total * BLOCK_SIZE} bytes")


if __name__ == "__main__":
    main()
