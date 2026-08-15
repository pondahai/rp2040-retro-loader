/*
 * fatlite.h - 極簡 FAT16/FAT32 讀取器 (只讀, 只看根目錄)
 *
 * 存在的唯一理由是 16KB 的預算。FatFs 功能完整但編出來 8-10KB,
 * 而載入器只需要三件事: 掛載、列出根目錄的 .uf2、循序把檔案讀出來。
 *
 * 刻意不支援: 子目錄、寫入、exFAT、多分割區(只認第一個)、
 * 非 512 bytes 的磁區。SD 卡出廠格式都吃得下這些限制。
 */
#ifndef FATLITE_H
#define FATLITE_H

#include <stdbool.h>
#include <stdint.h>

#define FL_NAME_MAX 32

typedef struct {
    char     name[FL_NAME_MAX + 1];
    uint32_t cluster;
    uint32_t size;
} fl_entry_t;

typedef struct {
    uint32_t cluster;       /* 目前所在的 cluster */
    uint32_t sec_in_clus;   /* 在 cluster 裡的第幾個磁區 */
    uint32_t remaining;     /* 還沒讀的 byte 數 */
} fl_file_t;

/* 掛載 SD 卡上的第一個 FAT 分割區。sd_init() 要先成功。 */
bool fl_mount(void);

/*
 * 列出根目錄裡副檔名為 .uf2 的檔案,依檔名排序。
 * 回傳實際找到的數量(最多 max 筆)。
 */
int fl_list_uf2(fl_entry_t *out, int max);

void fl_open(const fl_entry_t *e, fl_file_t *f);

/*
 * 讀下一個 512-byte 磁區到 buf。回傳 false 代表讀完或出錯。
 * 只提供磁區粒度是刻意的 —— UF2 天生就是 512 bytes 一塊,
 * 支援任意長度的讀取只會多出一個用不到的緩衝層。
 */
bool fl_read_sector(fl_file_t *f, uint8_t *buf);

#endif /* FATLITE_H */
