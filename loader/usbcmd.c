/*
 * usbcmd.c - 手寫的最小 USB HID 裝置(見 usbcmd.h)
 *
 * 架構參考 pico-examples 的 usb/device/dev_lowlevel,砍到只剩:
 *   - EP0 控制傳輸: GET_DESCRIPTOR(device / config / HID report)、
 *     SET_ADDRESS、SET_CONFIGURATION、GET_STATUS、SET_IDLE、SET_REPORT
 *   - EP1 IN interrupt: 8 byte input report
 * 不給字串描述元(iProduct 等全為 0),Windows 會顯示成「USB 輸入裝置」,
 * 電腦端用 VID:PID 找。
 *
 * IRQ handler 直接定義成 isr_usbctrl,蓋掉 crt0 向量表的 weak 符號 ——
 * 不用 irq_set_exclusive_handler(),省下 hardware_irq 那一包。
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/resets.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/nvic.h"
#include "hardware/regs/intctrl.h"
#include "usbcmd.h"

#define usb_hw_set   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

#define PROTO_VER '1'

/* ------------------------------------------------------------ 描述元 */

static const uint8_t desc_device[18] = {
    18, 0x01,                   /* bLength, DEVICE */
    0x10, 0x01,                 /* USB 1.1 */
    0x00, 0x00, 0x00,           /* class 由介面決定 */
    64,                         /* EP0 max packet */
    USBCMD_VID & 0xFF, USBCMD_VID >> 8,
    USBCMD_PID & 0xFF, USBCMD_PID >> 8,
    0x00, 0x01,                 /* bcdDevice 1.00 */
    0, 0, 0,                    /* 不給字串 */
    1,                          /* 1 個 configuration */
};

static const uint8_t desc_report[] = {
    0x06, 0x00, 0xFF,           /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,                 /* Usage 1 */
    0xA1, 0x01,                 /* Collection (Application) */
    0x15, 0x00, 0x26, 0xFF, 0x00, /* Logical 0..255 */
    0x75, 0x08, 0x95, 0x08,     /* 8 x 8 bit */
    0x09, 0x01, 0x81, 0x02,     /* Input  (Data,Var,Abs) */
    0x09, 0x01, 0x91, 0x02,     /* Output (Data,Var,Abs) */
    0xC0,                       /* End Collection */
};

static const uint8_t desc_config[34] = {
    9, 0x02, 34, 0, 1, 1, 0, 0x80, 50,      /* config: 1 介面, bus powered, 100mA */
    9, 0x04, 0, 0, 1, 0x03, 0, 0, 0,        /* interface 0: HID, 1 個端點 */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(desc_report), 0, /* HID 1.11 */
    7, 0x05, 0x81, 0x03, 8, 0, 10,          /* EP1 IN interrupt, 8 bytes, 10ms */
};

/* ------------------------------------------------------------ 狀態 */

static volatile uint8_t pending_cmd;
static uint8_t  new_addr;           /* SET_ADDRESS 要等 status 階段結束才生效 */
static bool     want_report;        /* SET_REPORT 的 data 階段正在等 */
static bool     ep0_in_pid;         /* EP0 IN 的 DATA0/1 */
static bool     ep1_in_pid;
static const uint8_t *tx_ptr;       /* EP0 IN 還沒送完的部分 */
static uint16_t tx_left;

/* 寫 buffer control: 先寫除了 AVAIL 以外的位元,等幾個 clk_sys 再設 AVAIL
 * (datasheet 4.1.2.5.1 —— clk_sys 比 clk_usb 快時一次寫完會有競爭) */
static void buf_ctrl(io_rw_32 *reg, uint16_t len, bool pid, bool in)
{
    uint32_t v = len | (pid ? USB_BUF_CTRL_DATA1_PID : 0) | (in ? USB_BUF_CTRL_FULL : 0);
    *reg = v;
    __asm volatile ("nop\nnop\nnop" ::: "memory");
    *reg = v | USB_BUF_CTRL_AVAIL;
}

static void ep0_in_chunk(void)
{
    uint16_t n = tx_left > 64 ? 64 : tx_left;
    memcpy(usb_dpram->ep0_buf_a, tx_ptr, n);
    tx_ptr  += n;
    tx_left -= n;
    buf_ctrl(&usb_dpram->ep_buf_ctrl[0].in, n, ep0_in_pid, true);
    ep0_in_pid = !ep0_in_pid;
}

/* 控制讀取: 送資料,短於 wLength 就截 */
static void ep0_send(const uint8_t *p, uint16_t len, uint16_t wlen)
{
    tx_ptr  = p;
    tx_left = len < wlen ? len : wlen;
    ep0_in_chunk();
}

static void ep0_zlp(void)
{
    tx_left = 0;
    buf_ctrl(&usb_dpram->ep_buf_ctrl[0].in, 0, true, true);
}

static void ep0_stall(void)
{
    usb_hw->ep_stall_arm = USB_EP_STALL_ARM_EP0_IN_BITS | USB_EP_STALL_ARM_EP0_OUT_BITS;
    usb_dpram->ep_buf_ctrl[0].in  = USB_BUF_CTRL_STALL;
    usb_dpram->ep_buf_ctrl[0].out = USB_BUF_CTRL_STALL;
}

static void handle_setup(void)
{
    const volatile uint8_t *s = usb_dpram->setup_packet;
    uint8_t  rt   = s[0];
    uint8_t  req  = s[1];
    uint8_t  vhi  = s[3];
    uint16_t wlen = (uint16_t)(s[6] | (s[7] << 8));
    static const uint8_t zero2[2];

    ep0_in_pid  = true;     /* data 階段一律從 DATA1 開始 */
    want_report = false;
    tx_ptr      = 0;        /* 上一個控制傳輸若被新的 SETUP 打斷,丟掉殘留 */

    if (rt == 0x80 || rt == 0x81) {                 /* 標準 IN 請求 */
        if (req == 0x06) {                          /* GET_DESCRIPTOR */
            if (vhi == 0x01)      ep0_send(desc_device, sizeof desc_device, wlen);
            else if (vhi == 0x02) ep0_send(desc_config, sizeof desc_config, wlen);
            else if (vhi == 0x22) ep0_send(desc_report, sizeof desc_report, wlen);
            else if (vhi == 0x21) ep0_send(desc_config + 18, 9, wlen);
            else                  ep0_stall();      /* 字串等: 沒有 */
            return;
        }
        if (req == 0x00) { ep0_send(zero2, 2, wlen); return; }   /* GET_STATUS */
    } else if (rt == 0x00) {                        /* 標準 OUT 請求 */
        if (req == 0x05) { new_addr = s[2]; ep0_zlp(); return; } /* SET_ADDRESS */
        if (req == 0x09) {                                       /* SET_CONFIGURATION */
            usb_dpram->ep_ctrl[0].in = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER |
                                       (3u << EP_CTRL_BUFFER_TYPE_LSB) /* 3 = interrupt */ |
                                       offsetof(usb_device_dpram_t, epx_data);
            ep1_in_pid = false;
            ep0_zlp();
            return;
        }
    } else if (rt == 0x21) {                        /* HID class OUT */
        if (req == 0x0A) { ep0_zlp(); return; }     /* SET_IDLE */
        if (req == 0x09) {                          /* SET_REPORT: 等 data 階段 */
            want_report = true;
            buf_ctrl(&usb_dpram->ep_buf_ctrl[0].out, 64, true, false);
            return;
        }
    }
    ep0_stall();
}

void isr_usbctrl(void)
{
    uint32_t st = usb_hw->ints;

    if (st & USB_INTS_SETUP_REQ_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;
        handle_setup();
    }

    if (st & USB_INTS_BUFF_STATUS_BITS) {
        uint32_t b = usb_hw->buf_status;
        usb_hw_clear->buf_status = b;

        if (b & 1u) {                               /* EP0 IN 完成 */
            if (new_addr) {
                usb_hw->dev_addr_ctrl = new_addr;
                new_addr = 0;
            }
            if (tx_left) {
                ep0_in_chunk();
            } else if (tx_ptr) {
                /* 控制讀取的資料送完了 —— 收主機的 status ZLP */
                tx_ptr = 0;
                buf_ctrl(&usb_dpram->ep_buf_ctrl[0].out, 0, true, false);
            }
        }
        if (b & 2u) {                               /* EP0 OUT 完成 */
            if (want_report) {
                want_report = false;
                pending_cmd = usb_dpram->ep0_buf_a[0];
                ep0_zlp();
            }
        }
        /* bit 2 = EP1 IN 完成: 不需要做事 */
    }

    if (st & USB_INTS_BUS_RESET_BITS) {
        usb_hw_clear->sie_status = USB_SIE_STATUS_BUS_RESET_BITS;
        usb_hw->dev_addr_ctrl = 0;
        new_addr = 0;
    }
}

/* ------------------------------------------------------------ 介面 */

void usbcmd_start(void)
{
    reset_block(RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_USBCTRL_BITS);
    memset(usb_dpram, 0, sizeof(*usb_dpram));

    pending_cmd = 0;
    new_addr = 0;
    tx_ptr = 0;
    tx_left = 0;
    want_report = false;

    usb_hw->muxing    = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
    usb_hw->pwr       = USB_USB_PWR_VBUS_DETECT_BITS | USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;
    usb_hw->sie_ctrl  = USB_SIE_CTRL_EP0_INT_1BUF_BITS;
    usb_hw->inte      = USB_INTS_BUFF_STATUS_BITS | USB_INTS_BUS_RESET_BITS |
                        USB_INTS_SETUP_REQ_BITS;

    nvic_hw->icpr = 1u << USBCTRL_IRQ;
    nvic_hw->iser = 1u << USBCTRL_IRQ;

    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;   /* 電腦看到插入 */
}

void usbcmd_stop(void)
{
    nvic_hw->icer = 1u << USBCTRL_IRQ;
    usb_hw_clear->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS; /* 電腦看到拔除 */
    reset_block(RESETS_RESET_USBCTRL_BITS);
    nvic_hw->icpr = 1u << USBCTRL_IRQ;
}

uint8_t usbcmd_take(void)
{
    uint8_t c = pending_cmd;
    pending_cmd = 0;
    return c;
}

void usbcmd_reply(uint32_t four_chars)
{
    uint8_t *p = usb_dpram->epx_data;
    memset(p, 0, 8);
    memcpy(p, &four_chars, 4);
    p[4] = PROTO_VER;
    buf_ctrl(&usb_dpram->ep_buf_ctrl[1].in, 8, ep1_in_pid, true);
    ep1_in_pid = !ep1_in_pid;
}
