/*
 * usbcmd.h - 選單期間的最小 USB HID 指令通道
 *
 * 只在顯示選單時開。電腦端送一個 32 byte 的 output report(SET_REPORT),
 * byte 0 是指令字元,byte 1.. 是以 0 結尾的參數字串;載入器回一個 8 byte
 * 的 input report(interrupt IN):4 個字元 + 協定版本。
 *
 *   'P' ping    -> 回 "PRTL" + 協定版本
 *   'M' mount   -> 找 SDMOUNT.UF2;找到回 "OK  " 並啟動,找不到回 "NONE"
 *   'L' launch  -> 參數是 SD 卡上的檔名(例如 "DOOM.UF2",不分大小寫,
 *                  可省略 .UF2);找到回 "OK  " 並燒錄執行,找不到回 "NONE"
 *
 * 協定版本 1 的 output report 只有 8 byte、沒有 'L'。
 *
 * 為什麼手寫而不用 TinyUSB: 載入器只剩不到 4KB,TinyUSB 的 device core
 * 加 HID class 就超過了。這裡只做列舉 + 一個 interrupt IN 端點,指令走
 * EP0 的 SET_REPORT,所以連 OUT 端點都不需要。
 *
 * 設計見 rp2040-retro-sdk docs/SD-MOUNT-PLAN.md(S3)。
 */
#ifndef USBCMD_H
#define USBCMD_H

#include <stdint.h>

/* VID:PID —— 開發測試碼,公開散布前要換正式的(同 rp2040-retro-sdmount) */
#define USBCMD_VID 0xCAFE
#define USBCMD_PID 0x4010

/* output report 長度(指令 1 byte + 參數 31 byte) */
#define USBCMD_OUT_LEN 32

void usbcmd_start(void);

/*
 * 完全關掉 USB 控制器並拉掉 D+ 上拉 —— 電腦會看到裝置拔除。
 * 燒錄 flash 之前、交棒之前都必須呼叫:IRQ handler 住在 flash 裡,
 * 燒錄期間 XIP 關閉時被叫到就會當機;交棒後專題也要能用乾淨的 USB
 * 控制器重新列舉。
 */
void usbcmd_stop(void);

/* 取出電腦送來的指令字元,沒有就回 0。 */
uint8_t usbcmd_take(void);

/* 最近一個指令的參數字串(一定以 0 結尾)。在下一個指令進來前有效。 */
const char *usbcmd_arg(void);

/* 回一個 input report:4 個字元 + 協定版本。 */
void usbcmd_reply(uint32_t four_chars);

#endif /* USBCMD_H */
