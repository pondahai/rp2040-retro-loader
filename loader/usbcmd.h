/*
 * usbcmd.h - 選單期間的最小 USB HID 指令通道
 *
 * 只在顯示選單時開。電腦端送一個 8 byte 的 output report(SET_REPORT),
 * byte 0 是指令字元;載入器回一個 8 byte 的 input report(interrupt IN)。
 *
 *   'P' ping    -> 回 "PRTL" + 協定版本
 *   'M' mount   -> 找 SDMOUNT.UF2;找到回 'K' 並啟動,找不到回 'N'
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

/* 回一個 input report:4 個字元 + 協定版本。 */
void usbcmd_reply(uint32_t four_chars);

#endif /* USBCMD_H */
