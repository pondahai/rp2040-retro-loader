/*
 * trampoline.c - 讓「偏移過的專題 UF2」能被單獨燒錄使用
 *
 * 每個專題本體都 link 在 APP_BASE(0x10004000),前面 16KB 是空的。
 * 如果使用者是用 USB BOOTSEL 把 UF2 直接拖進 Pico、而不是透過載入器,
 * 那 flash 最前面就沒有東西可以開機。這支跳板就是填進那 16KB 的東西:
 * 提供 boot2、被 ROM 叫起來、然後立刻把控制權交給 APP_BASE。
 *
 * 換句話說,同一份 UF2 兩種用法:
 *   USB 拖進去   -> 跳板落在 0x10000000,開機 -> 跳 APP_BASE
 *   載入器從 SD 載 -> 載入器丟掉跳板那幾個 block,自己跳 APP_BASE
 *
 * 這裡刻意不做任何事(不點螢幕、不讀按鍵)。跳板要盡量短,因為它跟載入器
 * 共用同一塊 16KB 的地,而且它每次開機都會被執行。
 */
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "launch.h"

int main(void)
{
    if (!app_present()) {
        /*
         * 前面 16KB 有跳板、後面卻沒有本體 —— 通常是 UF2 合併時漏了本體,
         * 或是被載入器寫壞了。與其跳進垃圾位址變成一台沒反應的機器,
         * 不如掛回 BOOTSEL,使用者至少看得到 USB 磁碟可以重燒。
         */
        reset_usb_boot(0, 0);
    }

    launch_app();
}
