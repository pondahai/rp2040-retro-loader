/*
 * sdspi.h - 極簡 SD 卡驅動 (SPI 模式, 只讀)
 *
 * 為什麼不直接抄 infones 的 drivers/sdcard: 那份綁著 FatFs 的 diskio 介面,
 * 而 FatFs 本身 (ff.c 7000 行) 塞不進 16KB。這裡只留載入器真正會用到的:
 * 初始化 + 讀 512-byte 區塊。寫入完全不實作 —— 載入器不該有能力弄壞 SD 卡。
 */
#ifndef SDSPI_H
#define SDSPI_H

#include <stdbool.h>
#include <stdint.h>

/* 回傳 false 表示沒插卡、或卡不回應。 */
bool sd_init(void);

/* 從 LBA 開始讀 count 個 512-byte 區塊到 buf。 */
bool sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count);

#endif /* SDSPI_H */
