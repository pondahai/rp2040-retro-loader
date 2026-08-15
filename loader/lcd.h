/*
 * lcd.h - ILI9341 文字模式 (40 x 30 格, 8x8 字)
 *
 * 只做文字。不做圖形、不做 DMA、不做 framebuffer —— 320x240x16bpp 的
 * framebuffer 是 150KB,而且 DMA 留在半路上會弄髒交棒後的專題。
 * 每個字元直接 blocking 送 128 bytes 出去,慢但夠用,而且省 flash 也省 RAM。
 */
/* 注意: 不能用 LCD_H 當 guard —— board.h 拿它當螢幕高度 */
#ifndef LOADER_LCD_H
#define LOADER_LCD_H

#include <stdint.h>
#include <stddef.h>

#define LCD_COLS 40
#define LCD_ROWS 30

/* RGB565 */
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
#define C_GREEN  0x07E0
#define C_RED    0xF800
#define C_YELLOW 0xFFE0
#define C_BLUE   0x001F

void lcd_init(void);
void lcd_clear(uint16_t bg);

/* col/row 以字元格為單位。超出畫面的部分會被截掉。 */
void lcd_putc(int col, int row, char ch, uint16_t fg, uint16_t bg);
void lcd_puts(int col, int row, const char *s, uint16_t fg, uint16_t bg);

/* 把整列先填滿 bg 再寫字,用來畫光棒與清掉上一次的殘字。 */
void lcd_puts_line(int row, const char *s, uint16_t fg, uint16_t bg);

/*
 * 像素模式。座標是像素不是字元格,唯一的用途是縮圖 —— 仍然沒有 framebuffer,
 * 資料是誰呼叫誰負責一段一段餵進來的(見 lcd.h 開頭的說明)。
 *
 * 用法: lcd_blit_begin() 開一塊矩形,然後連續 lcd_blit() 直到填滿。
 * 像素格式是 RGB565 big-endian(高位元組先送),跟 ILI9341 的線上格式一致。
 */
void lcd_blit_begin(int x, int y, int w, int h);
void lcd_blit(const uint8_t *data, size_t n);
void lcd_fill_rect(int x, int y, int w, int h, uint16_t c);
void lcd_frame(int x, int y, int w, int h, uint16_t c);

#endif /* LOADER_LCD_H */
