#include <string.h>
#include "fatlite.h"
#include "thumb.h"

#define SECTOR_SIZE 512

/* FOO.UF2 -> FOO.RAW。回傳 false 代表名字太長塞不下。 */
static bool raw_name(const char *uf2, char *out, size_t n)
{
    size_t len = strlen(uf2);

    /* 呼叫端保證是 .uf2(選單只收這種),所以直接砍掉最後 4 個字元 */
    if (len < 4) return false;
    len -= 4;

    if (len + 5 > n) return false;      /* ".RAW" + '\0' */
    memcpy(out, uf2, len);
    strcpy(out + len, ".RAW");
    return true;
}

bool thumb_load(const fl_entry_t *uf2, uint8_t *dst)
{
    char name[FL_NAME_MAX + 1];
    fl_entry_t raw;

    if (!raw_name(uf2->name, name, sizeof(name))) return false;
    if (!fl_find(name, &raw)) return false;

    /*
     * 尺寸不對就不讀。少一個 byte 都不行 —— 沒有標頭可以驗,檔案長度是唯一
     * 能拿來擋「拖錯檔案」的東西,而餵錯長度的資料只會在螢幕上變成雪花。
     */
    if (raw.size != THUMB_BYTES) return false;

    fl_file_t f;
    fl_open(&raw, &f);

    /*
     * 直接讀進 dst。磁區大小整除 THUMB_BYTES(18432 / 512 = 36),所以不必
     * 處理最後一塊讀太多的情況 —— 這是當初把尺寸訂成 96x96 的原因之一。
     */
    for (uint32_t off = 0; off < THUMB_BYTES; off += SECTOR_SIZE) {
        if (!fl_read_sector(&f, dst + off)) return false;
    }
    return true;
}
