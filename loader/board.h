/*
 * board.h - 掌機的腳位配置
 *
 * 數字取自 rp2040-ili9341-infones 的 software/infones/CMakeLists.txt 與
 * main.cpp,兩邊必須一致 —— 載入器點得亮螢幕、專題點不亮(或反過來)
 * 幾乎都是這裡對不上。
 */
#ifndef BOARD_H
#define BOARD_H

/* ---- LCD: ILI9341 on spi0 ---- */
#define LCD_SPI          spi0
#define LCD_PIN_DC       20
#define LCD_PIN_CS       17
#define LCD_PIN_CLK      18
#define LCD_PIN_MOSI     19
#define LCD_PIN_RST      21
#define LCD_PIN_BL       22
#define LCD_SPI_HZ       (32 * 1000 * 1000)

#define LCD_W            320
#define LCD_H            240

/* ---- SD card on spi1 ---- */
#define SD_SPI           spi1
#define SD_PIN_CS        13
#define SD_PIN_SCK       10
#define SD_PIN_MOSI      11
#define SD_PIN_MISO      12
/* infones 用 100kHz 初始化(不是規格上限的 400kHz),沿用已知可用的值 */
#define SD_SPI_SLOW_HZ   (100 * 1000)
#define SD_SPI_FAST_HZ   (25 * 1000 * 1000)

/* ---- 按鍵: 全部 active-low, 內部上拉 ---- */
#define BTN_PIN_UP       9
#define BTN_PIN_DOWN     5
#define BTN_PIN_LEFT     8
#define BTN_PIN_RIGHT    6
#define BTN_PIN_SELECT   28
#define BTN_PIN_START    4
#define BTN_PIN_A        2
#define BTN_PIN_B        3

#endif /* BOARD_H */
