#include <string.h>
#include "pico/stdlib.h"
#include "board.h"
#include "lcd.h"
#include "thumb.h"
#include "coverflow.h"

#define PITCH      120                          /* 相鄰兩張封面的中心距 */
#define CENTRE_X   ((LCD_W - THUMB_W) / 2)      /* 中央那張的左緣 */
#define STRIP_Y    72                           /* 封面帶的上緣 */
#define SPAN       2                            /* 中央左右各畫幾張 */
#define FRAMES     8                            /* 一次滑動的幀數 */

/*
 * 封面快取。滑動時每一幀都要用到同一張圖的不同片段,每幀重讀 SD 卡是不可能的
 * (18KB 一張),所以整張留在 RAM 裡。RP2040 有 264KB 而載入器幾乎沒用,
 * 5 張 92KB 綽綽有餘 —— 這就是左右滑動做得起來的原因。
 *
 * 為什麼是 5 張而不是 3 張: 動畫途中畫面上最多會同時出現 to-2 .. to+1 這 4 張
 * (滑動起始那一幀還看得到舊的兩側),取 5 才能讓 idx % NSLOT 在任何連續視窗內
 * 都不撞號。撞號的後果是每一幀都去重讀 SD 卡,動畫直接變成幻燈片。
 */
#define NSLOT 5
/*
 * 對齊到 4 是效能而不是正確性: 合成每條掃描線要 memcpy 幾十 KB,而 M0+ 沒有
 * 非對齊存取,位址是奇數的話整個複製會退化成逐 byte 搬。THUMB_BYTES 是 4 的
 * 倍數,所以每一格都跟著對齊。
 */
static uint8_t __attribute__((aligned(4))) slot_px[NSLOT][THUMB_BYTES];
static int     slot_for[NSLOT];     /* 這一格放的是第幾筆,-1 = 空 */
static bool    slot_ok[NSLOT];      /* 圖讀成功了沒(失敗就畫佔位色塊) */

void cf_reset(void)
{
    for (int i = 0; i < NSLOT; i++) slot_for[i] = -1;
}

static const uint8_t *slot_get(const fl_entry_t *entries, int idx, bool *ok)
{
    int s = idx % NSLOT;
    if (slot_for[s] != idx) {
        slot_ok[s]  = thumb_load(&entries[idx], slot_px[s]);
        slot_for[s] = idx;
    }
    *ok = slot_ok[s];
    return slot_px[s];
}

/*
 * 畫一幀。shift 是整排封面的水平位移(0 = 靜止),動畫就是拿同一個函式
 * 餵不同的 shift。
 *
 * 只重畫封面帶那條 320x96 的橫幅: 61KB,在 32MHz 的 SPI 上約 15ms,
 * 換算 60fps 有餘。整個畫面是 150KB(38ms),差別就是流暢與頓挫。
 */
static void render(const fl_entry_t *entries, int count, int centre, int shift)
{
    const uint8_t *px[2 * SPAN + 1];
    bool           ok[2 * SPAN + 1];
    int            x0[2 * SPAN + 1];
    int            n = 0;

    for (int k = centre - SPAN; k <= centre + SPAN; k++) {
        if (k < 0 || k >= count) continue;
        int x = CENTRE_X + (k - centre) * PITCH + shift;
        if (x >= LCD_W || x + THUMB_W <= 0) continue;   /* 整張在畫面外 */
        px[n] = slot_get(entries, k, &ok[n]);
        x0[n] = x;
        n++;
    }

    uint8_t line[LCD_W * 2];
    lcd_blit_begin(0, STRIP_Y, LCD_W, THUMB_H);

    for (int y = 0; y < THUMB_H; y++) {
        /* 0xFF 填滿剛好就是白色(RGB565 的 0xFFFF),省一個迴圈 */
        memset(line, 0xFF, sizeof(line));

        for (int i = 0; i < n; i++) {
            int src_x = 0, dst_x = x0[i], w = THUMB_W;

            /* 貼齊畫面邊緣: 兩側的封面本來就是故意露一半的 */
            if (dst_x < 0) { src_x = -dst_x; w -= src_x; dst_x = 0; }
            if (dst_x + w > LCD_W) w = LCD_W - dst_x;
            if (w <= 0) continue;

            if (ok[i]) {
                memcpy(&line[dst_x * 2], &px[i][(y * THUMB_W + src_x) * 2], (size_t)w * 2);
            } else {
                /* 沒有圖的照樣要能選,畫成灰色色塊佔住位置 */
                for (int j = 0; j < w; j++) {
                    line[(dst_x + j) * 2]     = C_GREY >> 8;
                    line[(dst_x + j) * 2 + 1] = C_GREY & 0xff;
                }
            }
        }
        lcd_blit(line, sizeof(line));
    }

    /*
     * 選取框固定在畫面正中央不動 —— 動的是封面。沒有文字可以標示「選中的是
     * 哪一個」,位置就是唯一的線索,所以這個框不能省。
     */
    lcd_frame(CENTRE_X - 3, STRIP_Y - 3, THUMB_W + 6, THUMB_H + 6, C_BLACK);
}

void cf_draw(const fl_entry_t *entries, int count, int cursor)
{
    if (count) render(entries, count, cursor, 0);
}

void cf_slide(const fl_entry_t *entries, int count, int from, int to)
{
    if (!count || from == to) { cf_draw(entries, count, to); return; }

    /*
     * 動畫開始前先把會用到的圖全部讀進來。讀 SD 卡要幾十毫秒,夾在幀與幀
     * 之間就會變成一次明顯的卡頓 —— 寧可在按下的那一刻多等一下。
     */
    for (int k = to - SPAN; k <= to + SPAN; k++) {
        if (k < 0 || k >= count) continue;
        bool ok;
        (void)slot_get(entries, k, &ok);
    }

    int start = (to > from) ? PITCH : -PITCH;

    /*
     * 減速收尾(ease-out): 位移按 (1-t)^2 收斂。等速滑動看起來像投影片,
     * 尾巴慢下來才像「滑過去停住」。整數算就好,不必動到浮點。
     */
    for (int i = 1; i <= FRAMES; i++) {
        int left = FRAMES - i;
        render(entries, count, to, start * left * left / (FRAMES * FRAMES));
    }
}
