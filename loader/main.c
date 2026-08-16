/*
 * main.c - 掌機開機選單
 *
 * 冷開機一律進這裡。列出 SD 卡根目錄的 .uf2,用封面圖左右選一個,寫進 flash
 * 的 APP 區,然後交棒過去。不記住上次選了什麼 —— 每次都重燒。
 *
 * 選單本身沒有文字,只有封面(見 coverflow.c)。但錯誤與燒錄進度是文字 ——
 * 那些沒有圖可以代替,而載入器沒有 serial 可看,螢幕是唯一的回饋管道,
 * 沉默的失敗最難查。
 *
 * 整支程式活在 flash 最前面的 16KB 裡(見 common/boot_map.h),而且這 16KB
 * 是跟跳板共用的同一塊地。超過就會蓋到專題本體,所以 CMake 有一道
 * build 期的大小檢查擋著。
 */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/timer.h"
#include "boot_map.h"
#include "launch.h"
#include "board.h"
#include "lcd.h"
#include "sdspi.h"
#include "fatlite.h"
#include "flasher.h"
#include "thumb.h"
#include "coverflow.h"

/* ----------------------------------------------------------- 字串工具 */
/*
 * 不用 snprintf: newlib 的格式化程式碼即使是 nano 版也要 1.5-2KB,
 * 而載入器連同跳板一起只有 16KB 可用。這裡只需要接字串跟印十進位整數。
 */
typedef struct { char *p; char *end; } sb_t;

static void sb_init(sb_t *b, char *buf, size_t n)
{
    b->p = buf;
    b->end = buf + n - 1;
    *b->p = 0;
}

static void sb_ch(sb_t *b, char c)
{
    if (b->p < b->end) { *b->p++ = c; *b->p = 0; }
}

static void sb_str(sb_t *b, const char *s)
{
    while (*s) sb_ch(b, *s++);
}

/* 右靠的十進位整數,寬度不足補空白 */
static void sb_uint(sb_t *b, unsigned v, int width)
{
    char tmp[11];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < width; i++) sb_ch(b, ' ');
    while (n) sb_ch(b, tmp[--n]);
}

#define MAX_ENTRIES 24

static fl_entry_t entries[MAX_ENTRIES];
static int entry_count;
static int cursor;

/* ---------------------------------------------------------------- 按鍵 */

static const uint8_t btn_pins[] = {
    BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_LEFT, BTN_PIN_RIGHT,
    BTN_PIN_SELECT, BTN_PIN_START, BTN_PIN_A, BTN_PIN_B,
};

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_SELECT, B_START, B_A, B_B, B_COUNT };

static void buttons_init(void)
{
    for (unsigned i = 0; i < count_of(btn_pins); i++) {
        gpio_init(btn_pins[i]);
        gpio_set_dir(btn_pins[i], GPIO_IN);
        gpio_pull_up(btn_pins[i]);   /* 全部 active-low */
    }
}

static uint32_t buttons_read(void)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count_of(btn_pins); i++) {
        if (!gpio_get(btn_pins[i])) v |= 1u << i;
    }
    return v;
}

/*
 * 時間一律用 timer_hw->timerawl 的 32-bit 微秒值,不走 to_ms_since_boot()。
 * 後者拿 64-bit 微秒去除以 1000,會把 __aeabi_ldiv 整包(約 1KB)拖進 16KB
 * 的預算裡 —— 那是這支程式裡最肥的單一項,而選單需要的只是「過了沒」。
 *
 * 32-bit 微秒約 71.6 分鐘就 wrap,所以比較必須寫成有號差值而不是 a >= b;
 * 只要間隔遠小於 wrap 週期(這裡最長 400ms),減法在 wrap 前後都是對的。
 */
#define REPEAT_DELAY_US 400000u      /* 第一次重複前先等久一點 */
#define REPEAT_RATE_US   90000u

/*
 * 回傳這次「剛按下」的按鍵。長按會以固定間隔重複,不然在 20 幾個檔案裡
 * 一格一格按到目標很痛苦。
 */
static uint32_t buttons_poll(void)
{
    static uint32_t prev;
    static uint32_t next_repeat;

    uint32_t now = timer_hw->timerawl;
    uint32_t cur = buttons_read();
    uint32_t edge = cur & ~prev;

    if (edge) {
        next_repeat = now + REPEAT_DELAY_US;
    } else if (cur && cur == prev && (int32_t)(now - next_repeat) >= 0) {
        edge = cur;
        next_repeat = now + REPEAT_RATE_US;
    }

    prev = cur;
    return edge;
}

/* ---------------------------------------------------------------- 畫面 */

/*
 * 文字畫面(掃卡、燒錄、錯誤)一律白底黑字,跟選單的白底一致。
 * 這些是選單之外的狀態,不必省字 —— 它們是唯一能說明「為什麼不動了」的東西。
 */
#define STATUS_ROW 4

static void status(const char *msg, uint16_t fg)
{
    lcd_puts_line(STATUS_ROW, msg, fg, C_WHITE);
}

static void on_progress(uint32_t done, uint32_t total)
{
    /* 每 32 個 block 更新一次就好 —— 畫面是 blocking SPI,畫太勤會拖慢燒錄 */
    if (done % 32 && done != total) return;

    char line[LCD_COLS + 1];
    unsigned pct = total ? (unsigned)(done * 100u / total) : 0;
    sb_t b; sb_init(&b, line, sizeof(line));
    /*
     * flash 裡本來就是這一份的話一個 sector 都不會被抹寫,這時喊 WRITING
     * 是騙人的 —— 那種情況下拔電不會壞事。一旦真的寫下去就翻成 WRITING
     * 並且不會再翻回來,因為從那一刻起中途斷電就會留下半個 image。
     * 兩個字串等長,才不會在原地留下上一輪的殘字。
     */
    sb_str(&b, uf2_sectors_written ? " WRITING FLASH  " : " VERIFYING      ");
    sb_uint(&b, pct, 3);
    sb_str(&b, "%  (");
    sb_uint(&b, (unsigned)done, 0);
    sb_ch(&b, '/');
    sb_uint(&b, (unsigned)total, 0);
    sb_ch(&b, ')');
    lcd_puts_line(LCD_ROWS - 2, line, C_BLACK, C_WHITE);
}

/* ---------------------------------------------------------------- 流程 */

/* 兩處掛掉的原因一樣,共用同一份字串 —— 詳細差異看 STATUS_ROW 那行 */
static const char msg_no_uf2[] = " No SD card, or no .uf2 in root.";

/*
 * 刻意不清畫面 —— 上面那行階段訊息(STATUS_ROW)要留著,它說明了走到哪一步
 * 才失敗。載入器沒有 serial 可以看(stdio USB 為了省空間關掉了),螢幕是
 * 唯一的回饋管道。
 */
static void fatal(const char *msg)
{
    lcd_puts_line(LCD_ROWS - 6, " LOADER STOPPED", C_WHITE, C_RED);
    lcd_puts_line(LCD_ROWS - 4, msg, C_BLACK, C_WHITE);
    lcd_puts_line(LCD_ROWS - 2, " Fix and power-cycle, or BOOTSEL to reflash.",
                  C_GREY, C_WHITE);
    while (1) tight_loop_contents();
}

static bool scan_card(void)
{
    entry_count = 0;
    cursor = 0;
    cf_reset();          /* 換一批檔案了,舊的封面快取全部作廢 */

    /*
     * 每一步都先把自己印出來再做。卡住的時候,螢幕上停在哪一行就是
     * 卡在哪一步 —— 這比事後猜有用得多。
     */
    status(" Init SD card...", C_GREY);
    sd_result_t sr = sd_init();
    if (sr != SD_OK) {
        char line[LCD_COLS + 1];
        sb_t b; sb_init(&b, line, sizeof(line));
        sb_str(&b, " SD init FAILED: ");
        sb_str(&b, sd_result_str(sr));
        status(line, C_RED);
        return false;
    }

    status(" Mounting FAT...", C_GREY);
    if (!fl_mount()) {
        status(" FAT mount FAILED", C_RED);
        return false;
    }

    status(" Scanning *.uf2...", C_GREY);
    entry_count = fl_list_uf2(entries, MAX_ENTRIES);

    if (entry_count == 0) {
        status(" Mounted OK, no .uf2 found", C_RED);
    } else {
        status("", C_GREY);
    }
    return true;
}

static void run_selected(void)
{
    const fl_entry_t *e = &entries[cursor];

    lcd_clear(C_WHITE);
    lcd_puts_line(4, " Loading:", C_GREY, C_WHITE);
    lcd_puts_line(5, e->name, C_BLACK, C_WHITE);
    lcd_puts_line(7, " Do not power off.", C_RED, C_WHITE);

    uf2_result_t r = uf2_flash_file(e, on_progress);

    if (r != UF2_OK) {
        char line[LCD_COLS + 1];
        sb_t b; sb_init(&b, line, sizeof(line));
        sb_str(&b, " FAILED: ");
        sb_str(&b, uf2_result_str(r));
        /*
         * 這裡不能回選單就算了 —— flash 已經被寫了一部分,APP 區的內容
         * 現在是半個 image。讓使用者知道要重選一個燒完整。
         */
        lcd_puts_line(LCD_ROWS - 2, line, C_WHITE, C_RED);
        lcd_puts_line(LCD_ROWS - 1, " Flash incomplete. Pick another.",
                      C_BLACK, C_WHITE);
        sleep_ms(3000);
        return;
    }

    if (!app_present()) {
        fatal(" Wrote OK, but bad vector table at APP_BASE.");
    }

    lcd_puts_line(LCD_ROWS - 2, " Starting...", C_GREEN, C_WHITE);
    sleep_ms(200);

    launch_app();
}

/* 回到選單: 整個畫面重來一次,把上一輪的文字清乾淨 */
static void show_menu(void)
{
    lcd_clear_bg();
    cf_draw(entries, entry_count, cursor);
}

static void move_cursor(int delta)
{
    int to = cursor + delta;
    if (to < 0 || to >= entry_count) return;
    cf_slide(entries, entry_count, cursor, to);
    cursor = to;
}

int main(void)
{
    buttons_init();

    /*
     * 逃生口: 開機時按住 SELECT 就直接進 BOOTSEL。
     *
     * 載入器沒有 USB(stdio 為了省空間關掉了),所以 picotool 沒有 reset
     * interface 可以用 —— 一旦燒進去,唯一能重燒的方法就是實體 BOOTSEL
     * 按鈕。有些機殼按不到那顆按鈕,那就等於磚了。
     *
     * 放在最前面是刻意的: LCD、SD 都還沒初始化,就算它們壞掉也救得回來。
     */
    if (!gpio_get(BTN_PIN_SELECT)) {
        reset_usb_boot(0, 0);
    }

    /*
     * 軟重置直接穿透給專題,不顯示選單、不重燒。
     *
     * 為什麼需要這個: 有些專題用「重置晶片」來切換自己的狀態。infones 就是
     * 這樣 —— 它的選單選好遊戲後把 ROM 燒進 flash,然後 watchdog_enable()
     * 重置,開機時再用 watchdog_caused_reboot() 判斷這次要直接進遊戲。
     * (menu.cpp 的註解說重置是為了拿到乾淨的 core1/DMA 狀態,聲音才正常。)
     *
     * 載入器搬進 0x10000000 之後,那次重置就不再回到 infones,而是掉進
     * 載入器的選單 —— 使用者得再選一次、再等重燒 1MB。是載入器改變了
     * 「重置」的意義,所以由載入器負責把它修回來,而不是要求每個專題改寫。
     *
     * watchdog_hw->reason 是唯讀的,軟體清不掉,而載入器是用「跳」的、
     * 自己從不重置晶片 —— 所以這個值會原封不動地傳給專題,它讀到的跟
     * 沒有載入器時一模一樣。
     *
     * 冷開機(含按實體 RESET 拉 RUN 腳)時 reason 為 0,照常顯示選單 ——
     * 這正是原始需求「冷開機一律先進載入器選單」。
     */
    if (watchdog_hw->reason && app_present() && gpio_get(BTN_PIN_B)) {
        launch_app();
    }

    lcd_init();
    lcd_clear_bg();

    if (!scan_card() || entry_count == 0) {
        fatal(msg_no_uf2);
    }

    show_menu();

    while (1) {
        uint32_t p = buttons_poll();

        /*
         * 封面是橫向排列,所以左右才是主要的方向鍵;上下一併收下,
         * 因為手指習慣不是設計能規定的。
         */
        if (p & ((1u << B_LEFT) | (1u << B_UP))) {
            move_cursor(-1);
        } else if (p & ((1u << B_RIGHT) | (1u << B_DOWN))) {
            move_cursor(1);
        } else if (p & ((1u << B_A) | (1u << B_START))) {
            run_selected();
            /* 只有燒錄失敗才會回到這裡 */
            show_menu();
        } else if (p & (1u << B_B)) {
            lcd_clear_bg();
            if (!scan_card() || entry_count == 0) {
                fatal(msg_no_uf2);
            }
            show_menu();
        }

        sleep_ms(16);
    }
}
