/*
 * main.c - 掌機開機選單
 *
 * 冷開機一律進這裡。列出 SD 卡根目錄的 .uf2,光棒選一個,寫進 flash 的
 * APP 區,然後交棒過去。不記住上次選了什麼 —— 每次都重燒。
 *
 * 整支程式活在 flash 最前面的 16KB 裡(見 common/boot_map.h),而且這 16KB
 * 是跟跳板共用的同一塊地。超過就會蓋到專題本體,所以 CMake 有一道
 * build 期的大小檢查擋著。
 */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot_map.h"
#include "launch.h"
#include "board.h"
#include "lcd.h"
#include "sdspi.h"
#include "fatlite.h"
#include "flasher.h"

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

/* 截斷到 max 個字元,不足則補空白到 pad 寬度 */
static void sb_str_field(sb_t *b, const char *s, int max, int pad)
{
    int n = 0;
    while (*s && n < max) { sb_ch(b, *s++); n++; }
    while (n < pad) { sb_ch(b, ' '); n++; }
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
#define LIST_TOP    4          /* 清單從第幾列開始畫 */
#define LIST_ROWS   20

static fl_entry_t entries[MAX_ENTRIES];
static int entry_count;
static int cursor;
static int scroll;

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
 * 回傳這次「剛按下」的按鍵。長按會以固定間隔重複,不然在 20 幾個檔案裡
 * 一格一格按到目標很痛苦。
 */
static uint32_t buttons_poll(void)
{
    static uint32_t prev;
    static uint32_t hold_since;
    static uint32_t next_repeat;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t cur = buttons_read();
    uint32_t edge = cur & ~prev;

    if (edge) {
        hold_since = now;
        next_repeat = now + 400;      /* 第一次重複前先等久一點 */
    } else if (cur && cur == prev && now >= next_repeat) {
        edge = cur;
        next_repeat = now + 90;
    } else if (!cur) {
        hold_since = 0;
    }
    (void)hold_since;

    prev = cur;
    return edge;
}

/* ---------------------------------------------------------------- 畫面 */

static void draw_header(void)
{
    lcd_puts_line(0, " RP2040 RETRO HANDHELD - LOADER", C_BLACK, C_GREEN);
    lcd_puts_line(2, " SD:/*.uf2", C_GREY, C_BLACK);
}

static void draw_footer(const char *msg, uint16_t fg)
{
    lcd_puts_line(LCD_ROWS - 2, msg, fg, C_BLACK);
    lcd_puts_line(LCD_ROWS - 1, " UP/DOWN select   A/START run   B rescan",
                  C_GREY, C_BLACK);
}

static void draw_list(void)
{
    /* 讓光棒永遠留在可見範圍內 */
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + LIST_ROWS) scroll = cursor - LIST_ROWS + 1;

    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = scroll + i;
        char line[LCD_COLS + 1];

        if (idx < entry_count) {
            /* 尺寸放右邊,長檔名被截掉時至少還看得出是哪一支 */
            sb_t b; sb_init(&b, line, sizeof(line));
            sb_ch(&b, ' ');
            sb_str_field(&b, entries[idx].name, 28, 28);
            sb_ch(&b, ' ');
            sb_uint(&b, (unsigned)(entries[idx].size / 1024), 5);
            sb_ch(&b, 'K');
        } else {
            line[0] = 0;
        }

        bool sel = (idx == cursor) && (idx < entry_count);
        lcd_puts_line(LIST_TOP + i, line,
                      sel ? C_BLACK : C_WHITE,
                      sel ? C_YELLOW : C_BLACK);
    }
}

static void on_progress(uint32_t done, uint32_t total)
{
    /* 每 32 個 block 更新一次就好 —— 畫面是 blocking SPI,畫太勤會拖慢燒錄 */
    if (done % 32 && done != total) return;

    char line[LCD_COLS + 1];
    unsigned pct = total ? (unsigned)(done * 100u / total) : 0;
    sb_t b; sb_init(&b, line, sizeof(line));
    sb_str(&b, " WRITING FLASH  ");
    sb_uint(&b, pct, 3);
    sb_str(&b, "%  (");
    sb_uint(&b, (unsigned)done, 0);
    sb_ch(&b, '/');
    sb_uint(&b, (unsigned)total, 0);
    sb_ch(&b, ')');
    lcd_puts_line(LCD_ROWS - 2, line, C_YELLOW, C_BLACK);
}

/* ---------------------------------------------------------------- 流程 */

static void fatal(const char *msg)
{
    lcd_clear(C_BLACK);
    lcd_puts_line(2, " LOADER STOPPED", C_BLACK, C_RED);
    lcd_puts_line(4, msg, C_WHITE, C_BLACK);
    lcd_puts_line(6, " Fix it and power-cycle, or hold BOOTSEL", C_GREY, C_BLACK);
    lcd_puts_line(7, " to reflash over USB.", C_GREY, C_BLACK);
    while (1) tight_loop_contents();
}

static bool scan_card(void)
{
    entry_count = 0;
    cursor = 0;
    scroll = 0;

    if (!sd_init())  return false;
    if (!fl_mount()) return false;

    entry_count = fl_list_uf2(entries, MAX_ENTRIES);
    return true;
}

static void run_selected(void)
{
    const fl_entry_t *e = &entries[cursor];

    lcd_clear(C_BLACK);
    draw_header();
    lcd_puts_line(4, " Loading:", C_GREY, C_BLACK);
    lcd_puts_line(5, e->name, C_WHITE, C_BLACK);
    lcd_puts_line(7, " Do not power off.", C_RED, C_BLACK);

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
        lcd_puts_line(LCD_ROWS - 1, " Flash is now incomplete. Pick another.",
                      C_WHITE, C_BLACK);
        sleep_ms(3000);
        return;
    }

    if (!app_present()) {
        fatal(" Wrote OK but no valid vector table at APP_BASE.");
    }

    lcd_puts_line(LCD_ROWS - 2, " Starting...", C_GREEN, C_BLACK);
    sleep_ms(200);

    launch_app();
}

int main(void)
{
    buttons_init();
    lcd_init();
    lcd_clear(C_BLACK);
    draw_header();

    if (!scan_card()) {
        fatal(" No SD card, or not FAT16/FAT32.");
    }

    if (entry_count == 0) {
        fatal(" No .uf2 files in the SD card root.");
    }

    draw_list();
    draw_footer(" Ready.", C_GREEN);

    while (1) {
        uint32_t p = buttons_poll();

        if (p & (1u << B_UP)) {
            if (cursor > 0) { cursor--; draw_list(); }
        } else if (p & (1u << B_DOWN)) {
            if (cursor < entry_count - 1) { cursor++; draw_list(); }
        } else if (p & ((1u << B_A) | (1u << B_START))) {
            run_selected();
            /* 只有燒錄失敗才會回到這裡 */
            lcd_clear(C_BLACK);
            draw_header();
            draw_list();
            draw_footer(" Previous attempt failed.", C_RED);
        } else if (p & (1u << B_B)) {
            lcd_clear(C_BLACK);
            draw_header();
            if (!scan_card() || entry_count == 0) {
                fatal(" No SD card or no .uf2 found on rescan.");
            }
            draw_list();
            draw_footer(" Rescanned.", C_GREEN);
        }

        sleep_ms(16);
    }
}
