#!/usr/bin/env python3
"""
gen_app_ld.py - 從 pico-sdk 的 memmap_default.ld 生成 app/memmap_app.ld

用法:
    python tools/gen_app_ld.py ~/.pico-sdk/sdk/2.2.0

為什麼要用生成的、不手寫: memmap_default.ld 在 SDK 版本之間差異不小
(2.x 多了 .tdata/.tbss、.embedded_block,NOLOAD 取代了 COPY,
__etext 的定義方式也換了)。手抄一份很快就會跟 SDK 脫節,而症狀是
「編得過但開機掛掉」這種最難查的那種。

所以這裡的規則是: 原檔一字不改地複製,只動兩處 ——
  1. FLASH region 換成 0x10004000 / 2032k
  2. 拿掉 .boot2 與它的 ASSERT,改丟 /DISCARD/

升級 SDK 之後重跑這支腳本,然後 git diff 看看 SDK 那邊改了什麼。
"""

import argparse
import pathlib
import sys

# 必須跟 common/boot_map.h 一致
APP_ORIGIN = "0x10004000"
APP_LENGTH = "2032k"          # 2048k - 16k

HEADER = f'''/*
 * memmap_app.ld - 給「被載入器載入的專題」用的 linker script
 *
 * 這份是從 pico-sdk 的
 *     src/rp2_common/pico_crt0/rp2040/memmap_default.ld
 * 原封複製後只動兩處生成的(見 tools/gen_app_ld.py)。升級 SDK 時請重跑
 * 那支腳本重新生成,不要手改 —— SDK 之間這個檔案的差異比想像中大。
 *
 * 兩處改動:
 *
 *   1. FLASH 從 pico_flash_region.ld 的 0x10000000/2048k 改成
 *      {APP_ORIGIN}/{APP_LENGTH}。前面 16KB 留給載入器(從 SD 載入時)
 *      或跳板(USB 直接燒錄時)。
 *
 *   2. 拿掉 .boot2 與它的 ASSERT,改成丟進 /DISCARD/。專題自己那份
 *      boot2 永遠不會被執行 —— ROM 只認 flash 最前面那 256 bytes,
 *      而那是載入器/跳板的地盤。留著不但白佔位置,它結尾還寫死
 *      「跳到 0x10000100」,萬一真的被執行會跳到別人家。
 *
 * 結果是 {APP_ORIGIN} 第一個 byte 就是向量表,載入器與跳板都跳這個位址,
 * 沒有 +0x100 之類的特例要記。
 *
 * ⚠ 這裡的數字必須跟 common/boot_map.h 一致。linker script 沒辦法
 *   #include 那個 header,只能靠人工同步。
 *
 * 用法(專題的 CMakeLists.txt):
 *     pico_set_linker_script(<target> <路徑>/memmap_app.ld)
 */

'''

BOOT2_BLOCK = '''    .boot2 : {
        __boot2_start__ = .;
        KEEP (*(.boot2))
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ == 256,
        "ERROR: Pico second stage bootloader must be 256 bytes in size")

'''

DISCARD_BLOCK = '''
    /* pico-sdk 還是會編出 boot2,在這裡明確丟掉,免得 orphan placement
       把它隨便塞到某個地方去。 */
    /DISCARD/ : { *(.boot2) }
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sdk_path", help="pico-sdk 根目錄")
    ap.add_argument("-o", "--output", default="app/memmap_app.ld")
    args = ap.parse_args()

    src = (pathlib.Path(args.sdk_path) /
           "src/rp2_common/pico_crt0/rp2040/memmap_default.ld")
    if not src.is_file():
        sys.exit(f"找不到 {src}")

    s = src.read_text(encoding="utf-8")

    # 1. FLASH region
    inc = '    INCLUDE "pico_flash_region.ld"'
    if inc not in s:
        sys.exit("找不到 pico_flash_region.ld 的 INCLUDE —— SDK 版面可能變了,請人工確認")
    s = s.replace(inc,
                  '    /* 原本是 INCLUDE "pico_flash_region.ld" (0x10000000, 2048k) */\n'
                  f'    FLASH(rx) : ORIGIN = {APP_ORIGIN}, LENGTH = {APP_LENGTH}')

    # 2. 移除 .boot2
    if BOOT2_BLOCK not in s:
        sys.exit(".boot2 區段的樣子跟預期不同 —— SDK 版面可能變了,請人工確認")
    s = s.replace(BOOT2_BLOCK,
                  '    /* .boot2 已移除 —— 見檔頭說明。.text 的第一個東西就是向量表。 */\n\n')

    # 在 SECTIONS 收尾處插入 DISCARD
    tail = "    /* todo assert on extra code */\n}"
    if tail not in s:
        sys.exit("找不到 SECTIONS 的結尾 —— SDK 版面可能變了,請人工確認")
    s = s.replace(tail, "    /* todo assert on extra code */\n" + DISCARD_BLOCK + "}")

    out = pathlib.Path(args.output)
    out.write_text(HEADER + s, encoding="utf-8")
    print(f"{out}: 已從 {src} 生成")


if __name__ == "__main__":
    main()
