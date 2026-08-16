/*
 * flasher.c - 把 SD 卡上的 UF2 寫進 flash 的 APP 區
 *
 * 整份設計的關鍵就一行: 丟掉 target_addr < APP_BASE 的 block。
 *
 * 那些 block 是跳板 —— 專題的 UF2 之所以自帶跳板,是為了讓它可以被 USB
 * 直接拖進 Pico 單獨使用。但我們是從 SD 卡載入的,前面那 16KB 現在住著
 * 載入器自己。照寫下去等於把自己蓋掉,下次開機就沒有選單了。
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "boot_map.h"
#include "fatlite.h"
#include "uf2.h"
#include "flasher.h"

#define SECTOR_SIZE 4096u
#define NO_SECTOR   0xFFFFFFFFu

uint32_t uf2_sectors_written;

static uint8_t  sector_buf[SECTOR_SIZE];
static uint32_t sector_off = NO_SECTOR;   /* 目前緩衝中的 sector, 以 flash offset 表示 */

static void sector_flush(void)
{
    if (sector_off == NO_SECTOR) return;

    /*
     * flash 裡已經是同一份就別重寫。重選同一個專題是最常見的操作,而每個
     * sector 的抹寫壽命只有十萬次量級 —— 省下來的不只是時間,是板子的命。
     * 中途斷電留下的半個 image 也會因此只補寫真正壞掉的那幾塊。
     */
    if (memcmp((const uint8_t *)(FLASH_XIP_BASE + sector_off),
               sector_buf, SECTOR_SIZE) == 0) {
        sector_off = NO_SECTOR;
        return;
    }

    /*
     * flash_range_erase/program 本身是在 RAM 執行的,但期間 XIP 會被關掉,
     * 所以此時任何從 flash 取指的中斷處理常式都會讓機器當掉。
     * 載入器沒有開 DMA、沒有啟動 core1,把中斷關掉就沒有東西會插進來。
     */
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(sector_off, SECTOR_SIZE);
    flash_range_program(sector_off, sector_buf, SECTOR_SIZE);
    restore_interrupts(save);

    uf2_sectors_written++;

    sector_off = NO_SECTOR;
}

static void sector_write(uint32_t flash_off, const uint8_t *data, uint32_t len)
{
    uint32_t base = flash_off & ~(SECTOR_SIZE - 1);

    if (base != sector_off) {
        sector_flush();
        /*
         * 沒被 UF2 蓋到的部分填 0xFF (= 抹除後的狀態)。UF2 的 block 通常
         * 是連續的,但格式並不保證,所以不能假設整個 sector 都會被填滿。
         */
        memset(sector_buf, 0xFF, SECTOR_SIZE);
        sector_off = base;
    }

    memcpy(&sector_buf[flash_off - base], data, len);
}

uf2_result_t uf2_flash_file(const fl_entry_t *e, uf2_progress_fn on_progress)
{
    static uf2_block_t blk;   /* 512 bytes, 不放 stack */
    fl_file_t f;
    uint32_t done = 0;
    uint32_t total = e->size / 512u;
    bool wrote_any = false;

    fl_open(e, &f);
    sector_off = NO_SECTOR;
    uf2_sectors_written = 0;

    while (fl_read_sector(&f, (uint8_t *)&blk)) {
        done++;

        if (blk.magic_start0 != UF2_MAGIC_START0 ||
            blk.magic_start1 != UF2_MAGIC_START1 ||
            blk.magic_end    != UF2_MAGIC_END) {
            return UF2_ERR_MAGIC;
        }

        /* 家族碼不符的話寫下去只會得到一台跑不起來的機器 */
        if ((blk.flags & UF2_FLAG_FAMILY_ID) && blk.file_size != RP2040_FAMILY_ID) {
            return UF2_ERR_FAMILY;
        }

        /* 這個旗標代表「這塊不是要寫進 flash 的」,例如純資料的 UF2 */
        if (blk.flags & UF2_FLAG_NOT_MAIN_FLASH) continue;
        if (blk.payload_size == 0 || blk.payload_size > 476) continue;

        /* ★ 丟掉跳板。整個載入器方案就靠這一句活下來。 */
        if (blk.target_addr < APP_BASE) continue;

        if (blk.target_addr + blk.payload_size > FLASH_XIP_BASE + FLASH_TOTAL_SIZE) {
            return UF2_ERR_RANGE;
        }

        sector_write(blk.target_addr - FLASH_XIP_BASE, blk.data, blk.payload_size);
        wrote_any = true;

        if (on_progress) on_progress(done, total);
    }

    sector_flush();

    if (!wrote_any) return UF2_ERR_NO_APP_BLOCK;
    return UF2_OK;
}

const char *uf2_result_str(uf2_result_t r)
{
    switch (r) {
    case UF2_OK:               return "OK";
    case UF2_ERR_READ:         return "SD read failed";
    case UF2_ERR_MAGIC:        return "not a UF2 file";
    case UF2_ERR_FAMILY:       return "not an RP2040 UF2";
    case UF2_ERR_RANGE:        return "target address out of flash";
    case UF2_ERR_NO_APP_BLOCK: return "no app blocks (offset build?)";
    default:                   return "unknown error";
    }
}
