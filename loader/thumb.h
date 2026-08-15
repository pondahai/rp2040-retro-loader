/*
 * thumb.h - 把 SD 卡上的封面圖讀進 RAM
 *
 * 圖不能放在載入器裡: 一張 96x96 RGB565 就是 18KB,而整個保留區只有 16KB。
 * 所以封面一律是 SD 卡上的獨立檔案,選到誰才讀誰。
 *
 * 為什麼是讀進 RAM 而不是直接串到螢幕: 滑動動畫每一幀都要用到同一張圖的
 * 不同片段,每幀重讀 SD 卡是不可能的。RP2040 有 264KB RAM 而載入器幾乎沒用,
 * 快取幾張綽綽有餘 —— 這正是左右滑動做得起來的原因(見 coverflow.c)。
 *
 * 檔案格式刻意選最笨的一種: 沒有標頭,就是 96*96 個 RGB565 big-endian 像素,
 * 剛好 18432 bytes = 36 個磁區。要解碼器就等於要程式碼,而我們沒有空間。
 *
 * 產生方式:
 *     python tools/make_thumb.py doom.png --fit -o DOOM.RAW
 */
#ifndef LOADER_THUMB_H
#define LOADER_THUMB_H

#include "fatlite.h"

#define THUMB_W     96
#define THUMB_H     96
#define THUMB_BYTES (THUMB_W * THUMB_H * 2)

/*
 * 讀 uf2 對應的封面: FOO.UF2 -> 找 FOO.RAW,整張填進 dst(要有 THUMB_BYTES)。
 * 找不到、尺寸不對、讀到一半失敗都回傳 false,此時 dst 的內容不可信任 ——
 * 由呼叫端決定要畫什麼佔位圖。
 */
bool thumb_load(const fl_entry_t *uf2, uint8_t *dst);

#endif /* LOADER_THUMB_H */
