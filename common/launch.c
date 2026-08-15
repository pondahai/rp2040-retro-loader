/*
 * launch.c - 交棒給 APP_BASE 的專題本體
 *
 * 載入器與跳板共用同一份。正常開機時這四件事是硬體 + boot2 幫忙做的，
 * 手動跳轉就得自己補做:
 *
 *   1. 關掉所有中斷來源 —— 否則跳過去之後,我們留下的 handler 位址還在
 *      NVIC 裡,一個 timer 中斷就會跳回一個已經不存在的函式。
 *   2. 清 XIP cache —— 換了一整塊 flash 內容之後,cache 裡可能還留著舊指令。
 *   3. 設 VTOR —— 告訴 CPU 向量表搬到 APP_BASE 了。
 *   4. 設 MSP 再跳 —— 用專題自己的堆疊頂端,不是我們的。
 *
 * 不 reset 周邊是刻意的: pico-sdk 的 runtime_init() 在專題那頭開機時
 * 已經 reset 掉除了 QSPI/PLL/USB 之外的所有 block。我們只要保證在那之前
 * 沒有東西會插進來就好。前提是載入器不能留下進行中的 DMA —— 所以載入器
 * 全程用 blocking SPI,沒有動 DMA。
 */
#include <stdint.h>
#include <stdbool.h>
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/nvic.h"
#include "boot_map.h"
#include "launch.h"

bool app_present(void)
{
    const uint32_t *vt = (const uint32_t *)APP_BASE;
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];

    /*
     * 空白 flash 讀出來是 0xFFFFFFFF,直接跳過去只會得到一台沒有反應的機器
     * (沒有看門狗可以救)。粗略驗一下向量表的頭兩格像不像真的:
     * 堆疊頂端要落在 SRAM,進入點要落在 APP 區且是 Thumb(bit0 = 1)。
     */
    if (sp < 0x20000000u || sp > 0x20042000u) return false;
    if ((pc & 1u) == 0) return false;
    if (pc < APP_BASE || pc >= FLASH_XIP_BASE + FLASH_TOTAL_SIZE) return false;
    return true;
}

void __attribute__((noreturn)) launch_app(void)
{
    const uint32_t *vt = (const uint32_t *)APP_BASE;
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];

    __asm volatile ("cpsid i" ::: "memory");

    /* M0+ 只有一組 32-bit 的 ICER/ICPR */
    nvic_hw->icer = 0xffffffffu;
    nvic_hw->icpr = 0xffffffffu;

    /* SysTick 不歸 NVIC 管,要另外關 */
    systick_hw->csr = 0;
    systick_hw->rvr = 0;
    systick_hw->cvr = 0;

    /* 清 XIP cache。讀回 flush 暫存器會擋到清完為止。 */
    xip_ctrl_hw->flush = 1;
    (void)xip_ctrl_hw->flush;

    scb_hw->vtor = (uint32_t)vt;

    /*
     * 換堆疊跟跳轉必須綁在一起,中間不能讓編譯器插入任何會碰 stack 的東西,
     * 所以寫成一段 asm。跳過去之後就不再回來了。
     */
    __asm volatile (
        "msr msp, %0\n"
        "bx  %1\n"
        :
        : "r" (sp), "r" (pc)
        : "memory"
    );

    __builtin_unreachable();
}
