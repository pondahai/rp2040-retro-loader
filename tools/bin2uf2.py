#!/usr/bin/env python3
"""
bin2uf2.py - 把一段原始資料包成 UF2,燒到指定的 flash 位址

    python tools/bin2uf2.py doom1.whx -a 0x10046000 -o doom_wad.uf2

等價於 `picotool load -t bin doom1.whx -o 0x10046000`,差別是產出一份 UF2 ——
而 UF2 可以放進 SD 卡交給載入器,picotool 不行(那需要一台接著 USB 的電腦)。

## 為什麼需要這個

有些專題不是「一個 uf2 燒完就結束」。DOOM(rp2040-doom)就是: 韌體約 257KB,
地圖資料 doom1.whx 有 1.7MB,兩者分開燒,WAD 的位址寫死在韌體的
TINY_WAD_ADDR 裡。

載入器只寫 UF2 裡的 block,所以「WAD 用 picotool 事先燒一次」的做法有個
惡性後果: WAD 佔的是 flash 高位址,任何映像夠大的其他專題都會把它蓋掉,
下次要玩 DOOM 就得再燒 1.7MB。

把 WAD 也轉成 UF2、跟韌體合併成同一份檔案,問題就消失了 —— 每次選 DOOM
都會把韌體與 WAD 一起重寫,誰蓋掉它都無所謂。

## 載入器那邊不必改

flasher.c 是位址驅動的: target_addr >= APP_BASE 且不超出 flash 尾端就照寫,
它並不在意那是程式還是資料。所以 WAD 的 block 跟韌體的 block 走的是同一條路。

UF2 格式: https://github.com/microsoft/uf2
"""

import argparse
import struct
import sys

BLOCK_SIZE   = 512
PAYLOAD      = 256              # 跟 picotool / pico-sdk 產出的一致
MAGIC0       = 0x0A324655
MAGIC1       = 0x9E5D5157
MAGIC_END    = 0x0AB16F30
FLAG_FAMILY  = 0x00002000
RP2040_FAMILY = 0xE48BFF56

# 必須跟 common/boot_map.h 一致
FLASH_BASE  = 0x10000000
FLASH_SIZE  = 2 * 1024 * 1024
APP_BASE    = 0x10004000


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="原始資料檔 (例如 doom1.whx)")
    ap.add_argument("-a", "--addr", required=True,
                    help="燒錄的目標位址,例如 0x10046000")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--allow-boot-region", action="store_true",
                    help="允許寫進 APP_BASE 以下(預設擋掉,那是載入器的地盤)")
    args = ap.parse_args()

    addr = int(args.addr, 0)

    with open(args.source, "rb") as f:
        data = f.read()
    if not data:
        sys.exit(f"{args.source}: 是空檔案")

    end = addr + len(data)

    # 位址不對的話,燒下去的後果從「資料讀不到」到「開不了機」都有可能,
    # 而且都是事後很難查的症狀。在這裡擋掉。
    if addr % 256:
        sys.exit(f"位址 {addr:#x} 不是 256 的倍數")
    if not args.allow_boot_region and addr < APP_BASE:
        sys.exit(
            f"位址 {addr:#x} 在 APP_BASE({APP_BASE:#x}) 以下 —— 那 16KB 是載入器\n"
            "或跳板的地盤,寫下去下次就沒有選單了。真的要寫請加 --allow-boot-region。"
        )
    if end > FLASH_BASE + FLASH_SIZE:
        over = end - (FLASH_BASE + FLASH_SIZE)
        sys.exit(f"資料結尾 {end:#x} 超出 2MB flash 共 {over} bytes")

    total = (len(data) + PAYLOAD - 1) // PAYLOAD
    out = bytearray()

    for i in range(total):
        chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
        # 最後一塊若不滿 256 要補到滿。RP2040 的 bootrom 以 256-byte page
        # 為單位處理 UF2,payload_size 不是 256 的 block 它直接不收 —— 結果
        # 是資料尾巴沒寫進去,而且因為湊不滿 numBlocks,燒完不會自動重開機
        # (看起來就像「拖進去沒反應」)。picotool/elf2uf2 也都是補滿的。
        # 補 0xFF 而不是 0x00:那是 flash 抹除後的值,不會多耗一次寫入。
        if len(chunk) < PAYLOAD:
            chunk = chunk + b"\xff" * (PAYLOAD - len(chunk))
        b = bytearray(BLOCK_SIZE)
        struct.pack_into("<IIIIIIII", b, 0,
                         MAGIC0, MAGIC1, FLAG_FAMILY,
                         addr + i * PAYLOAD,
                         PAYLOAD,
                         i, total,
                         RP2040_FAMILY)
        b[32:32 + len(chunk)] = chunk
        struct.pack_into("<I", b, 508, MAGIC_END)
        out += b

    with open(args.output, "wb") as f:
        f.write(out)

    print(f"{args.output}: {total} blocks, {len(out)} bytes "
          f"-> flash {addr:#x}..{end:#x} ({len(data)} bytes 資料)")


if __name__ == "__main__":
    main()
