#ifndef FLASHER_H
#define FLASHER_H

#include <stdbool.h>
#include <stdint.h>
#include "fatlite.h"

typedef enum {
    UF2_OK = 0,
    UF2_ERR_READ,          /* SD 讀取失敗 */
    UF2_ERR_MAGIC,         /* 不是 UF2 檔 */
    UF2_ERR_FAMILY,        /* 不是給 RP2040 的 */
    UF2_ERR_RANGE,         /* 目標位址超出 flash */
    UF2_ERR_NO_APP_BLOCK,  /* 整個檔案都在保留區裡，沒有本體可以寫 */
} uf2_result_t;

/* 進度回呼: done/total 以 UF2 block 計。可為 NULL。 */
typedef void (*uf2_progress_fn)(uint32_t done, uint32_t total);

uf2_result_t uf2_flash_file(const fl_entry_t *e, uf2_progress_fn on_progress);

const char *uf2_result_str(uf2_result_t r);

#endif /* FLASHER_H */
