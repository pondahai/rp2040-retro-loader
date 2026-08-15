/*
 * sdspi.h - 極簡 SD 卡驅動 (SPI 模式, 只讀)
 *
 * 為什麼不直接抄 infones 的 drivers/sdcard: 那份綁著 FatFs 的 diskio 介面,
 * 而 FatFs 本身 (ff.c 7000 行) 塞不進 16KB。這裡只留載入器真正會用到的:
 * 初始化 + 讀 512-byte 區塊。寫入完全不實作 —— 載入器不該有能力弄壞 SD 卡。
 *
 * 不過初始化序列是照 infones 那份抄的 —— 它在這塊硬體上已經證明可用,
 * 而 SD 卡的初始化對時脈、上拉、順序都很敏感,自己想比較容易踩雷。
 */
#ifndef SDSPI_H
#define SDSPI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 分階段回報,而不是一個 bool。載入器沒有 serial 可以看,
 * 「卡在哪一步」只能靠這個帶回螢幕上。
 */
typedef enum {
    SD_OK = 0,
    SD_ERR_CMD0,     /* 卡片完全沒回應 —— 沒插卡、接線、或供電 */
    SD_ERR_CMD8,     /* 有回應但電壓範圍談不攏 */
    SD_ERR_ACMD41,   /* 卡片一直沒完成初始化 */
    SD_ERR_CMD58,    /* 讀不到 OCR */
} sd_result_t;

sd_result_t sd_init(void);
const char *sd_result_str(sd_result_t r);

/* 從 LBA 開始讀 count 個 512-byte 區塊到 buf。 */
bool sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count);

#endif /* SDSPI_H */
