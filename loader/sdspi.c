/*
 * sdspi.c - SD 卡 SPI 驅動 (只讀)
 *
 * 指令序列照 infones 的 drivers/sdcard/sdcard.c(ChaN 的 FatFs 範例)抄,
 * 那份在這塊硬體上已經證明可用。自己想的版本踩了兩個坑:
 *
 *   1. 漏掉 MISO 上拉 —— infones 的註解直接寫著 "pull up of MISO is MUST"
 *   2. 每個指令前沒有等卡片忙碌結束 —— 卡片在忙的時候會把 DO 拉低,
 *      這時送下一個指令等於對牛彈琴。症狀是 CMD0/CMD8 都成功,
 *      ACMD41 卻永遠不會就緒。
 *
 * 這裡只保留載入器用得到的部分: 初始化 + 讀 512-byte 區塊。
 * 寫入完全不實作 —— 載入器不該有能力弄壞 SD 卡。
 */
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "board.h"
#include "sdspi.h"

#define CMD0   0            /* GO_IDLE_STATE */
#define CMD8   8            /* SEND_IF_COND */
#define CMD12  12           /* STOP_TRANSMISSION */
#define CMD17  17           /* READ_SINGLE_BLOCK */
#define CMD18  18           /* READ_MULTIPLE_BLOCK */
#define CMD55  55           /* APP_CMD */
#define CMD58  58           /* READ_OCR */
#define ACMD41 (0x80 + 41)  /* SD_SEND_OP_COND, 高位元代表「要先送 CMD55」 */

static bool sd_hc;   /* true = SDHC/SDXC, 位址以區塊為單位而非 byte */

static uint8_t xchg(uint8_t v)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &v, &r, 1);
    return r;
}

/*
 * 等卡片就緒。卡片忙的時候會把 DO 壓在低電位,回到 0xFF 才是可以收指令了。
 * 少了這一步,後面的指令會在卡片還忙的時候送出去,回應就全錯。
 */
static bool wait_ready(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t d;
    do {
        d = xchg(0xff);
    } while (d != 0xff && !time_reached(deadline));
    return d == 0xff;
}

static void deselect(void)
{
    gpio_put(SD_PIN_CS, 1);
    xchg(0xff);   /* dummy clock, 讓卡片釋放 DO */
}

static bool select_card(void)
{
    gpio_put(SD_PIN_CS, 0);
    xchg(0xff);   /* dummy clock, 讓卡片開始驅動 DO */
    if (wait_ready(500)) return true;
    deselect();
    return false;
}

/* 回傳 R1。bit7 = 1 表示沒收到有效回應。 */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    /* ACMD 要先送一個 CMD55 */
    if (cmd & 0x80) {
        cmd &= 0x7f;
        uint8_t res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }

    /* CMD12 是用來中止多區塊讀取的,不能在這裡重新 select */
    if (cmd != CMD12) {
        deselect();
        if (!select_card()) return 0xff;
    }

    uint8_t buf[6];
    buf[0] = 0x40 | cmd;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    /* CRC 只有 CMD0/CMD8 需要正確,之後卡片就不檢查了 */
    buf[5] = (cmd == CMD0) ? 0x95 : (cmd == CMD8) ? 0x87 : 0x01;
    spi_write_blocking(SD_SPI, buf, 6);

    if (cmd == CMD12) xchg(0xff);   /* CMD12 後面會多吐一個 byte */

    /* 回應最多在 10 個 byte 之內出現 */
    uint8_t res;
    int n = 10;
    do {
        res = xchg(0xff);
    } while ((res & 0x80) && --n);

    return res;
}

sd_result_t sd_init(void)
{
    sd_result_t err = SD_OK;
    sd_hc = false;

    /*
     * 腳位設定順序與上拉照 infones 抄。MISO 的上拉不能省 ——
     * 沒有它,卡片未驅動匯流排的空檔會讓 MISO 浮空,讀回來的是雜訊。
     */
    gpio_init(SD_PIN_SCK);
    gpio_init(SD_PIN_MISO);
    gpio_pull_up(SD_PIN_MISO);
    gpio_init(SD_PIN_MOSI);
    gpio_pull_up(SD_PIN_MOSI);

    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);

    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);

    /* 初始化階段慢速。infones 用 100kHz,不是規格上限的 400kHz。 */
    spi_init(SD_SPI, SD_SPI_SLOW_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    sleep_ms(10);

    /* >=74 個時脈,卡片才會進 SPI 模式 */
    gpio_put(SD_PIN_CS, 0);
    for (int i = 0; i < 10; i++) xchg(0xff);

    /* 進 idle state。這一步失敗代表卡片完全沒回應。 */
    if (send_cmd(CMD0, 0) != 0x01) {
        err = SD_ERR_CMD0;
        goto fail;
    }

    /* CMD8 判斷是 v2 卡(有 SDHC 的可能)還是 v1 */
    bool v2 = false;
    if (send_cmd(CMD8, 0x1AA) == 0x01) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = xchg(0xff);
        /* 回聲 pattern 對上才算真的支援這個電壓範圍 */
        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            v2 = true;
        } else {
            err = SD_ERR_CMD8;
            goto fail;
        }
    }

    /*
     * 等卡片完成初始化。回應 0 才算好,0x01 是「還在初始化」。
     * HCS 位元只對 v2 有意義。SD 規格給的上限是 1 秒,這裡留兩倍 ——
     * 用真正的時間,不是迴圈次數(慢速時脈下一輪就可能是好幾百 ms)。
     */
    bool ready = false;
    absolute_time_t deadline = make_timeout_time_ms(2000);
    do {
        if (send_cmd(ACMD41, v2 ? (1u << 30) : 0) == 0) { ready = true; break; }
    } while (!time_reached(deadline));
    if (!ready) {
        err = SD_ERR_ACMD41;
        goto fail;
    }

    if (v2) {
        /* OCR 的 CCS 位元 = 1 表示位址以區塊為單位 */
        if (send_cmd(CMD58, 0) != 0) {
            err = SD_ERR_CMD58;
            goto fail;
        }
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xchg(0xff);
        sd_hc = (ocr[0] & 0x40) != 0;
    }

    deselect();
    spi_set_baudrate(SD_SPI, SD_SPI_FAST_HZ);   /* 初始化完了才能拉到全速 */
    return SD_OK;

fail:
    deselect();
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

/* 等資料權杖 0xFE。 */
static bool wait_token(void)
{
    absolute_time_t deadline = make_timeout_time_ms(500);
    uint8_t t;
    do {
        t = xchg(0xff);
        if (t == 0xFE) return true;
        if (t != 0xFF) return false;   /* 錯誤權杖,再等也沒用 */
    } while (!time_reached(deadline));
    return false;
}

bool sd_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!count) return true;

    /* SDHC/SDXC 以區塊定址,舊卡以 byte 定址 */
    uint32_t addr = sd_hc ? lba : lba * 512u;

    /* 連續多個區塊用 CMD18,省掉每塊一次的指令來回 */
    uint8_t cmd = (count == 1) ? CMD17 : CMD18;

    /* send_cmd 會自己處理 CS,回來時 CS 仍然是低的,可以直接收資料 */
    if (send_cmd(cmd, addr) != 0) {
        deselect();
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!wait_token()) {
            if (cmd == CMD18) send_cmd(CMD12, 0);
            deselect();
            return false;
        }
        spi_read_blocking(SD_SPI, 0xff, buf + i * 512u, 512u);
        xchg(0xff);   /* CRC16, 丟掉 */
        xchg(0xff);
    }

    if (cmd == CMD18) send_cmd(CMD12, 0);

    deselect();
    return true;
}
