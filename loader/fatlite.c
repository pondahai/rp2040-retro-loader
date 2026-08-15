#include <string.h>
#include "sdspi.h"
#include "fatlite.h"

#define SECTOR_SIZE 512

static uint32_t fat_start;      /* 第一份 FAT 的 LBA */
static uint32_t data_start;     /* 資料區第一個磁區的 LBA */
static uint32_t root_start;     /* FAT16: 根目錄區 LBA；FAT32: 未用 */
static uint32_t root_entries;   /* FAT16: 根目錄項目數；FAT32: 0 */
static uint32_t root_cluster;   /* FAT32: 根目錄起始 cluster */
static uint8_t  sec_per_clus;
static bool     is_fat32;

static uint8_t  secbuf[SECTOR_SIZE];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t clus_to_lba(uint32_t clus)
{
    return data_start + (clus - 2) * sec_per_clus;
}

/* 取得 cluster 的下一個 cluster。回傳 >= EOC 代表鏈結束。 */
static uint32_t next_cluster(uint32_t clus)
{
    uint8_t buf[SECTOR_SIZE];
    uint32_t off = is_fat32 ? clus * 4u : clus * 2u;
    uint32_t sec = fat_start + off / SECTOR_SIZE;
    uint32_t idx = off % SECTOR_SIZE;

    if (!sd_read_blocks(sec, buf, 1)) return 0x0FFFFFFFu;

    if (is_fat32) return rd32(&buf[idx]) & 0x0FFFFFFFu;
    /* FAT16 的 EOC 是 0xFFF8-0xFFFF，統一抬到 FAT32 的範圍方便比較 */
    uint32_t v = rd16(&buf[idx]);
    return (v >= 0xFFF8u) ? 0x0FFFFFFFu : v;
}

static bool eoc(uint32_t clus)
{
    return clus < 2 || clus >= 0x0FFFFFF8u;
}

bool fl_mount(void)
{
    uint8_t buf[SECTOR_SIZE];
    uint32_t vbr_lba = 0;

    if (!sd_read_blocks(0, buf, 1)) return false;

    /*
     * LBA0 可能是 MBR(有分割表)也可能直接就是 VBR(整張卡一個檔案系統)。
     * 用「每磁區 byte 數」這格是不是 512 來判斷 —— MBR 那個位置不會是 512。
     */
    if (rd16(&buf[11]) != SECTOR_SIZE) {
        if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
        vbr_lba = rd32(&buf[0x1BE + 8]);        /* 第一個分割區的起始 LBA */
        if (!vbr_lba) return false;
        if (!sd_read_blocks(vbr_lba, buf, 1)) return false;
        if (rd16(&buf[11]) != SECTOR_SIZE) return false;
    }

    sec_per_clus = buf[13];
    if (!sec_per_clus) return false;

    uint32_t reserved   = rd16(&buf[14]);
    uint8_t  num_fats   = buf[16];
    root_entries        = rd16(&buf[17]);
    uint32_t total_sec  = rd16(&buf[19]);
    if (!total_sec) total_sec = rd32(&buf[32]);
    uint32_t fat_size   = rd16(&buf[22]);
    if (!fat_size) fat_size = rd32(&buf[36]);
    if (!fat_size || !num_fats) return false;

    fat_start  = vbr_lba + reserved;
    root_start = fat_start + (uint32_t)num_fats * fat_size;

    /* FAT16 的根目錄是固定大小的一塊區域，排在 FAT 後面、資料區前面 */
    uint32_t root_sectors = (root_entries * 32u + SECTOR_SIZE - 1) / SECTOR_SIZE;
    data_start = root_start + root_sectors;

    uint32_t data_sectors = total_sec - (data_start - vbr_lba);
    uint32_t clusters     = data_sectors / sec_per_clus;

    /* 官方判斷法就是看 cluster 數，不能看 FAT 大小或檔案系統字串 */
    is_fat32 = (clusters >= 65525u);
    root_cluster = is_fat32 ? rd32(&buf[44]) : 0;

    return true;
}

/* 把 8.3 目錄項目的名字轉成 "NAME.EXT"。 */
static void name_83(const uint8_t *d, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && d[i] != ' '; i++) out[n++] = (char)d[i];
    if (d[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && d[i] != ' '; i++) out[n++] = (char)d[i];
    }
    out[n] = 0;
}

/* LFN 項目裡 13 個 UTF-16 字元散在三段固定位移 */
static const uint8_t lfn_off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};

/* 自己寫比拉 <strings.h> 的 strcasecmp 進來省事，也省掉幾十 bytes */
static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (!ca) return 0;
    }
}

static bool ends_with_uf2(const char *s)
{
    size_t n = strlen(s);
    if (n < 4) return false;
    const char *e = s + n - 4;
    return e[0] == '.' &&
           (e[1] == 'u' || e[1] == 'U') &&
           (e[2] == 'f' || e[2] == 'F') &&
           e[3] == '2';
}

/*
 * 走一遍根目錄。兩種用法共用這一份走訪邏輯,因為 FAT16/FAT32 的目錄結構差異
 * (固定區 vs cluster 鏈)與 LFN 的拼裝都不是想重寫第二次的東西。
 *
 *   want == NULL: 收集所有 .uf2,依檔名排序,回傳筆數(最多 max)
 *   want != NULL: 找檔名剛好等於 want 的那一個,找到回傳 1,沒有回傳 0
 */
static int walk_root(fl_entry_t *out, int max, const char *want)
{
    int count = 0;
    char lfn[FL_NAME_MAX + 1];
    bool lfn_valid = false;

    uint32_t clus = root_cluster;
    uint32_t sec  = is_fat32 ? clus_to_lba(clus) : root_start;
    uint32_t sec_in_clus = 0;
    /* FAT16 的根目錄不是 cluster 鏈，磁區數是算出來的固定值 */
    uint32_t fat16_left = (root_entries * 32u + SECTOR_SIZE - 1) / SECTOR_SIZE;

    while (count < max) {
        if (is_fat32) {
            if (eoc(clus)) break;
        } else {
            if (fat16_left == 0) break;
        }

        if (!sd_read_blocks(sec, secbuf, 1)) break;

        for (int i = 0; i < SECTOR_SIZE; i += 32) {
            uint8_t *d = &secbuf[i];
            if (d[0] == 0x00) return count;       /* 目錄到此為止 */
            if (d[0] == 0xE5) { lfn_valid = false; continue; }

            uint8_t attr = d[11];

            if (attr == 0x0F) {
                /* LFN 片段。序號在 d[0] 低 5 bits，1 起算。 */
                uint8_t seq = d[0] & 0x1F;
                if (seq == 0 || seq > (FL_NAME_MAX / 13)) { lfn_valid = false; continue; }
                if (d[0] & 0x40) { memset(lfn, 0, sizeof(lfn)); lfn_valid = true; }
                if (!lfn_valid) continue;
                int base = (seq - 1) * 13;
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = rd16(&d[lfn_off[k]]);
                    if (ch == 0xFFFF) break;
                    if (base + k >= FL_NAME_MAX) break;
                    /* 非 ASCII 一律換成 '?'：字型本來就只有 ASCII */
                    lfn[base + k] = (ch == 0) ? 0 : (ch < 0x80 ? (char)ch : '?');
                }
                continue;
            }

            if (attr & (0x08 | 0x10)) { lfn_valid = false; continue; }  /* 磁碟標籤/目錄 */

            fl_entry_t e;
            if (lfn_valid && lfn[0]) {
                lfn[FL_NAME_MAX] = 0;
                strcpy(e.name, lfn);
            } else {
                name_83(d, e.name);
            }
            lfn_valid = false;

            if (want) {
                if (name_cmp(e.name, want) != 0) continue;
            } else {
                if (!ends_with_uf2(e.name)) continue;
            }

            e.cluster = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
            e.size    = rd32(&d[28]);
            if (e.cluster < 2 || e.size == 0) continue;

            if (want) { out[0] = e; return 1; }

            /* 邊讀邊插入排序，選單就不必再排一次 */
            int p = count;
            while (p > 0 && name_cmp(out[p - 1].name, e.name) > 0) {
                out[p] = out[p - 1];
                p--;
            }
            out[p] = e;
            if (++count >= max) return count;
        }

        sec++;
        if (is_fat32) {
            if (++sec_in_clus >= sec_per_clus) {
                clus = next_cluster(clus);
                sec_in_clus = 0;
                if (!eoc(clus)) sec = clus_to_lba(clus);
            }
        } else {
            fat16_left--;
        }
    }

    return count;
}

int fl_list_uf2(fl_entry_t *out, int max)
{
    return walk_root(out, max, NULL);
}

bool fl_find(const char *name, fl_entry_t *out)
{
    return walk_root(out, 1, name) == 1;
}

void fl_open(const fl_entry_t *e, fl_file_t *f)
{
    f->cluster     = e->cluster;
    f->sec_in_clus = 0;
    f->remaining   = e->size;
}

bool fl_read_sector(fl_file_t *f, uint8_t *buf)
{
    if (f->remaining == 0 || eoc(f->cluster)) return false;

    if (!sd_read_blocks(clus_to_lba(f->cluster) + f->sec_in_clus, buf, 1)) return false;

    f->remaining = (f->remaining > SECTOR_SIZE) ? f->remaining - SECTOR_SIZE : 0;

    if (++f->sec_in_clus >= sec_per_clus) {
        f->cluster     = next_cluster(f->cluster);
        f->sec_in_clus = 0;
    }
    return true;
}
