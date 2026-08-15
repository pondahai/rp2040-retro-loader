#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "board.h"
#include "lcd.h"
#include "font8x8.h"

/* ILI9341 / DCS 指令,名稱沿用 infones main.cpp 的寫法方便對照 */
#define DCS_SOFT_RESET          0x01
#define DCS_EXIT_SLEEP_MODE     0x11
#define DCS_EXIT_INVERT_MODE    0x20
#define DCS_SET_DISPLAY_ON      0x29
#define DCS_SET_COLUMN_ADDRESS  0x2A
#define DCS_SET_PAGE_ADDRESS    0x2B
#define DCS_WRITE_MEMORY_START  0x2C
#define DCS_SET_ADDRESS_MODE    0x36
#define DCS_SET_PIXEL_FORMAT    0x3A

#define PIXEL_FORMAT_16BIT      0x55
#define ADDRESS_MODE_BGR        0x08
#define ADDRESS_MODE_SWAP_XY    0x20

/* 跟 infones 用同一組,不然載入器跟遊戲的畫面方向會不一樣 */
#define ADDRESS_MODE (ADDRESS_MODE_BGR | ADDRESS_MODE_SWAP_XY)

static void wr_cmd(uint8_t c)
{
    gpio_put(LCD_PIN_DC, 0);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_PIN_CS, 1);
}

static void wr_data(const uint8_t *d, size_t n)
{
    if (!n) return;
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI, d, n);
    gpio_put(LCD_PIN_CS, 1);
}

static void set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t d[4];

    d[0] = x1 >> 8; d[1] = x1 & 0xff; d[2] = x2 >> 8; d[3] = x2 & 0xff;
    wr_cmd(DCS_SET_COLUMN_ADDRESS);
    wr_data(d, 4);

    d[0] = y1 >> 8; d[1] = y1 & 0xff; d[2] = y2 >> 8; d[3] = y2 & 0xff;
    wr_cmd(DCS_SET_PAGE_ADDRESS);
    wr_data(d, 4);

    wr_cmd(DCS_WRITE_MEMORY_START);
}

void lcd_init(void)
{
    gpio_init(LCD_PIN_DC);  gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_init(LCD_PIN_CS);  gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);

    gpio_set_function(LCD_PIN_CLK,  GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);
    spi_init(LCD_SPI, LCD_SPI_HZ);

    gpio_init(LCD_PIN_RST); gpio_set_dir(LCD_PIN_RST, GPIO_OUT);
    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(50);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(50);

    wr_cmd(DCS_SOFT_RESET);
    sleep_ms(150);

    uint8_t v;
    v = ADDRESS_MODE;      wr_cmd(DCS_SET_ADDRESS_MODE);  wr_data(&v, 1);
    v = PIXEL_FORMAT_16BIT; wr_cmd(DCS_SET_PIXEL_FORMAT); wr_data(&v, 1);

    wr_cmd(DCS_EXIT_INVERT_MODE);
    wr_cmd(DCS_EXIT_SLEEP_MODE);
    sleep_ms(150);
    wr_cmd(DCS_SET_DISPLAY_ON);
    sleep_ms(50);

    gpio_init(LCD_PIN_BL); gpio_set_dir(LCD_PIN_BL, GPIO_OUT);
    gpio_put(LCD_PIN_BL, 1);
}

void lcd_clear(uint16_t bg)
{
    /* 一次送一整列 320 像素,比一次一個像素快得多,又只吃 640 bytes stack */
    uint8_t line[LCD_W * 2];
    for (int i = 0; i < LCD_W; i++) {
        line[i * 2]     = bg >> 8;
        line[i * 2 + 1] = bg & 0xff;
    }
    set_window(0, 0, LCD_W - 1, LCD_H - 1);
    for (int y = 0; y < LCD_H; y++) {
        wr_data(line, sizeof(line));
    }
}

void lcd_putc(int col, int row, char ch, uint16_t fg, uint16_t bg)
{
    if (col < 0 || col >= LCD_COLS || row < 0 || row >= LCD_ROWS) return;
    if ((unsigned char)ch < FONT_FIRST_CH || (unsigned char)ch > FONT_LAST_CH) ch = '?';

    const uint8_t *g = &font8x8[((unsigned char)ch - FONT_FIRST_CH) * 8];
    uint8_t cell[8 * 8 * 2];
    uint8_t *p = cell;

    for (int y = 0; y < 8; y++) {
        uint8_t bits = g[y];
        for (int x = 0; x < 8; x++) {
            /* font8x8 的 bit0 是最左邊的點 */
            uint16_t c = (bits & (1u << x)) ? fg : bg;
            *p++ = c >> 8;
            *p++ = c & 0xff;
        }
    }

    int px = col * 8, py = row * 8;
    set_window(px, py, px + 7, py + 7);
    wr_data(cell, sizeof(cell));
}

void lcd_puts(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s && col < LCD_COLS) {
        lcd_putc(col++, row, *s++, fg, bg);
    }
}

void lcd_puts_line(int row, const char *s, uint16_t fg, uint16_t bg)
{
    for (int col = 0; col < LCD_COLS; col++) {
        char ch = *s ? *s++ : ' ';
        lcd_putc(col, row, ch, fg, bg);
    }
}
