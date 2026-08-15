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

/* 等卡片吐出非 0xff 的回應(R1)。逾時回 0xff。 */
static uint8_t wait_r1(void)
{
    for (int i = 0; i < 2000; i++) {
        uint8_t r = xchg(0xff);
        if (!(r & 0x80)) return r;
    }
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

bool sd_init(void)
{
    sd_hc = false;

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    cs_high();

    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);

    /* 初始化階段必須慢速(<=400kHz),卡片這時還沒切到高速時脈 */
    spi_init(SD_SPI, SD_SPI_SLOW_HZ);

    /* CS 拉高的狀態下先給 >=74 個時脈,卡片才會進 SPI 模式 */
    clock_idle(10);

    cs_low();
    bool ok = false;

    /* 進 idle state */
    for (int i = 0; i < 10; i++) {
        if (send_cmd(CMD0, 0) == 0x01) { ok = true; break; }
    }
    if (!ok) goto fail;

    /* CMD8 判斷是 v2 卡(有 SDHC 的可能)還是 v1 */
    bool v2 = false;
    if (send_cmd(CMD8, 0x1AA) == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = xchg(0xff);
        /* 只有回聲 pattern 對上才算真的支援 */
        if (r7[2] == 0x01 && r7[3] == 0xAA) v2 = true;
        else goto fail;
    }

    /*
     * 等卡片完成初始化。HCS 位元只對 v2 有意義。
     * SD 規格給的上限是 1 秒,這裡留兩倍。原本寫 20000(=20 秒)的問題是
     * 卡片沒反應時螢幕會沉默 20 秒,看起來像當機而不是失敗。
     */
    ok = false;
    for (int i = 0; i < 2000; i++) {
        if (send_acmd(ACMD41, v2 ? (1u << 30) : 0) == 0) { ok = true; break; }
        sleep_ms(1);
    }
    if (!ok) goto fail;

    if (v2) {
        /* OCR 的 CCS 位元 = 1 表示位址以區塊為單位 */
        if (send_cmd(CMD58, 0) != 0) goto fail;
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xchg(0xff);
        sd_hc = (ocr[0] & 0x40) != 0;
    }

    cs_high();
    clock_idle(1);

    /* 初始化完了才能拉到全速 */
    spi_set_baudrate(SD_SPI, SD_SPI_FAST_HZ);
    return true;

fail:
    cs_high();
    clock_idle(1);
    return false;
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
