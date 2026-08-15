#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "board.h"
#include "sdspi.h"

#define CMD0   0    /* GO_IDLE_STATE */
#define CMD8   8    /* SEND_IF_COND */
#define CMD12  12   /* STOP_TRANSMISSION */
#define CMD17  17   /* READ_SINGLE_BLOCK */
#define CMD18  18   /* READ_MULTIPLE_BLOCK */
#define CMD55  55   /* APP_CMD */
#define CMD58  58   /* READ_OCR */
#define ACMD41 41   /* SD_SEND_OP_COND */

static bool sd_hc;   /* true = SDHC/SDXC, 位址以區塊為單位而非 byte */

static inline void cs_low(void)  { gpio_put(SD_PIN_CS, 0); }
static inline void cs_high(void) { gpio_put(SD_PIN_CS, 1); }

static uint8_t xchg(uint8_t v)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &v, &r, 1);
    return r;
}

/* 空轉時鐘。SD 卡靠這個推進自己的狀態機,不送資料也得給它時脈。 */
static void clock_idle(int bytes)
{
    while (bytes--) xchg(0xff);
}

/*
 * 等卡片吐出非 0xff 的回應(R1)。逾時回 0xff。
 *
 * 用時間而不是迴圈次數: 初始化階段跑在 100kHz,一次 xchg 就要 80us,
 * 「2000 次」在慢速下等於 160ms,在高速下卻只有 3ms —— 同一個數字在兩種
 * 時脈下意義完全不同。之前 ACMD41 迴圈「2000 次」被我當成 2 秒,實際上
 * 最壞情況是 2.7 分鐘,螢幕就一直停在 Init SD card... 看起來像當機。
 */
static uint8_t wait_r1(void)
{
    absolute_time_t deadline = make_timeout_time_ms(200);
    do {
        uint8_t r = xchg(0xff);
        if (!(r & 0x80)) return r;
    } while (!time_reached(deadline));
    return 0xff;
}

static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t buf[6];
    buf[0] = 0x40 | cmd;
    buf[1] = arg >> 24;
    buf[2] = arg >> 16;
    buf[3] = arg >> 8;
    buf[4] = arg;
    /* CRC 只在 CMD0/CMD8 需要正確,之後卡片就不檢查了 */
    buf[5] = (cmd == CMD0) ? 0x95 : (cmd == CMD8) ? 0x87 : 0x01;

    spi_write_blocking(SD_SPI, buf, 6);
    if (cmd == CMD12) xchg(0xff);   /* CMD12 前面會多吐一個 stuff byte */
    return wait_r1();
}

static uint8_t send_acmd(uint8_t cmd, uint32_t arg)
{
    send_cmd(CMD55, 0);
    return send_cmd(cmd, arg);
}

sd_result_t sd_init(void)
{
    sd_result_t err = SD_OK;
    sd_hc = false;

    /*
     * 腳位設定順序與上拉都照 infones 的 drivers/sdcard/sdcard.c 抄 ——
     * 那份在這塊硬體上已經證明可用。
     *
     * MISO 的上拉不能省: infones 的註解直接寫著 "pull up of MISO is MUST"。
     * 沒有它,卡片在未驅動匯流排的空檔會讓 MISO 浮空,讀回來的是雜訊,
     * R1 回應就變成隨機值 —— 第一版漏掉這行,結果卡在初始化出不來。
     */
    gpio_init(SD_PIN_SCK);
    gpio_init(SD_PIN_MISO);
    gpio_pull_up(SD_PIN_MISO);
    gpio_init(SD_PIN_MOSI);
    gpio_pull_up(SD_PIN_MOSI);

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    cs_high();

    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);

    /* 初始化階段慢速。infones 用 100kHz,不是規格上限的 400kHz。 */
    spi_init(SD_SPI, SD_SPI_SLOW_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    sleep_ms(10);

    /* 先給 >=74 個時脈,卡片才會進 SPI 模式 */
    cs_low();
    clock_idle(10);
    bool ok = false;

    /* 進 idle state。這一步失敗代表卡片完全沒回應。 */
    for (int i = 0; i < 10; i++) {
        if (send_cmd(CMD0, 0) == 0x01) { ok = true; break; }
    }
    if (!ok) { err = SD_ERR_CMD0; goto fail; }

    /* CMD8 判斷是 v2 卡(有 SDHC 的可能)還是 v1 */
    bool v2 = false;
    if (send_cmd(CMD8, 0x1AA) == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = xchg(0xff);
        /* 只有回聲 pattern 對上才算真的支援 */
        if (r7[2] == 0x01 && r7[3] == 0xAA) v2 = true;
        else { err = SD_ERR_CMD8; goto fail; }
    }

    /*
     * 等卡片完成初始化。HCS 位元只對 v2 有意義。
     * SD 規格給的上限是 1 秒,這裡用真正的時間留兩倍 —— 不是迴圈次數,
     * 因為每次 send_acmd 內含兩次 wait_r1,慢速下一輪就可能是 400ms。
     */
    ok = false;
    absolute_time_t acmd41_deadline = make_timeout_time_ms(2000);
    do {
        if (send_acmd(ACMD41, v2 ? (1u << 30) : 0) == 0) { ok = true; break; }
        sleep_ms(1);
    } while (!time_reached(acmd41_deadline));
    if (!ok) { err = SD_ERR_ACMD41; goto fail; }

    if (v2) {
        /* OCR 的 CCS 位元 = 1 表示位址以區塊為單位 */
        if (send_cmd(CMD58, 0) != 0) { err = SD_ERR_CMD58; goto fail; }
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xchg(0xff);
        sd_hc = (ocr[0] & 0x40) != 0;
    }

    cs_high();
    clock_idle(1);

    /* 初始化完了才能拉到全速 */
    spi_set_baudrate(SD_SPI, SD_SPI_FAST_HZ);
    return SD_OK;

fail:
    cs_high();
    clock_idle(1);
    return err;
}

const char *sd_result_str(sd_result_t r)
{
    switch (r) {
    case SD_OK:          return "OK";
    case SD_ERR_CMD0:    return "no response (card/wiring?)";
    case SD_ERR_CMD8:    return "bad CMD8 echo";
    case SD_ERR_ACMD41:  return "card never became ready";
    case SD_ERR_CMD58:   return "cannot read OCR";
    default:             return "unknown";
    }
}

/* 等資料權杖 0xFE。逾時回 false。 */
static bool wait_token(void)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t t = xchg(0xff);
        if (t == 0xFE) return true;
        if (t != 0xFF) return false;   /* 錯誤權杖,再等也沒用 */
    }
    return false;
}

bool sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!count) return true;

    uint32_t addr = sd_hc ? lba : lba * 512u;

    cs_low();
    /* 連續多個區塊用 CMD18,省掉每塊一次的指令來回 */
    uint8_t cmd = (count == 1) ? CMD17 : CMD18;
    if (send_cmd(cmd, addr) != 0) {
        cs_high();
        clock_idle(1);
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!wait_token()) {
            if (cmd == CMD18) send_cmd(CMD12, 0);
            cs_high();
            clock_idle(1);
            return false;
        }
        /* 持續送 0xff 出去把資料換回來 */
        spi_read_blocking(SD_SPI, 0xff, buf + i * 512u, 512u);
        xchg(0xff);   /* CRC16, 丟掉 */
        xchg(0xff);
    }

    if (cmd == CMD18) send_cmd(CMD12, 0);

    cs_high();
    clock_idle(1);
    return true;
}
