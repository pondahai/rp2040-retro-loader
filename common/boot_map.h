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

/*
 * 跨重置的訊息:專題 -> 載入器
 *
 * 載入器對 watchdog 重置的規則是「直接跳回 APP_BASE」(為了 infones,見
 * main.c)。watchdog_hw->reason 唯讀,分不出這次重置是誰、為了什麼。
 * 想讓 watchdog 重置之後**回到選單**的專題(例如 rp2040-retro-sdmount 的
 * 「完成」),在重置前把訊息寫進 watchdog_hw->scratch[RETRO_MSG_SCRATCH]。
 *
 * 用 scratch[0]:pico-sdk 把 scratch[0..3] 保留給使用者,[4..7] 是它和
 * bootrom 自己用的(watchdog_enable / watchdog_reboot)。格式模仿 pico-sdk
 * 的做法 —— 高 16 bit 是魔術數字,不符合就當作沒有訊息,所以不影響任何
 * 沒寫這個值的專題。載入器讀到之後一律清成 0,不會殘留到下一次重置。
 *
 * 專題端用法:
 *     watchdog_hw->scratch[RETRO_MSG_SCRATCH] = RETRO_MSG_SHOW_MENU;
 *     watchdog_reboot(0, 0, 10);
 */
#define RETRO_MSG_SCRATCH   0
#define RETRO_MSG_MAGIC     0x52540000u                 /* 'R' 'T' */
#define RETRO_MSG_MASK      0xFFFF0000u
#define RETRO_MSG_SHOW_MENU (RETRO_MSG_MAGIC | 0x0001u) /* 這次重置顯示選單 */

#endif /* BOOT_MAP_H */
