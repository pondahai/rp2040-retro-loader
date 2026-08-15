/*
 * boot_map.h - flash 佈局的單一事實來源
 *
 * 載入器、跳板、以及每個被載入的專題(infones/doom/apple2...)都引用這一份，
 * 任何一邊改了數字而另一邊沒改，就會在啟動時安靜地跳進垃圾位址。
 *
 *   0x10000000  +----------------------------+
 *               | boot2 (256B)               |
 *               | 載入器  或  跳板            |  BOOT_REGION_SIZE = 16KB
 *               |   兩者擇一，看是怎麼燒進去的  |
 *   0x10004000  +----------------------------+  APP_BASE
 *               | 專題本體 (無自己的 boot2)    |
 *               |   最前面就是向量表           |
 *               +----------------------------+
 */
#ifndef BOOT_MAP_H
#define BOOT_MAP_H

#define FLASH_XIP_BASE      0x10000000u

/* 保留給載入器/跳板的區塊。放大這個數字要同步重編所有專題。 */
#define BOOT_REGION_SIZE    (16u * 1024u)

/* 專題本體的起點。這裡是向量表第一個 byte，不是 boot2。 */
#define APP_BASE            (FLASH_XIP_BASE + BOOT_REGION_SIZE)   /* 0x10004000 */

/* 給 flash_range_erase/program 用的 offset(不含 XIP_BASE) */
#define APP_FLASH_OFFSET    BOOT_REGION_SIZE                      /* 0x4000 */

/* 假設 2MB flash。改板子要一起改。 */
#define FLASH_TOTAL_SIZE    (2u * 1024u * 1024u)
#define APP_MAX_SIZE        (FLASH_TOTAL_SIZE - BOOT_REGION_SIZE)

#endif /* BOOT_MAP_H */
