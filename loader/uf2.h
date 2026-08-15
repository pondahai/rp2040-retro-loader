/*
 * uf2.h - UF2 檔案格式 (Microsoft, 512 bytes 一個 block)
 * https://github.com/microsoft/uf2
 */
#ifndef UF2_H
#define UF2_H

#include <stdint.h>

#define UF2_MAGIC_START0 0x0A324655u   /* "UF2\n" */
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u

#define UF2_FLAG_NOT_MAIN_FLASH   0x00000001u
#define UF2_FLAG_FAMILY_ID        0x00002000u

#define RP2040_FAMILY_ID 0xE48BFF56u

typedef struct {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size;      /* 有 UF2_FLAG_FAMILY_ID 時這格是 family id */
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;

_Static_assert(sizeof(uf2_block_t) == 512, "UF2 block must be 512 bytes");

#endif /* UF2_H */
