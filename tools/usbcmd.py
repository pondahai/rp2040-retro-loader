"""跟停在選單的載入器說話(USB HID,見 loader/usbcmd.h)。

    python tools/usbcmd.py ping
    python tools/usbcmd.py mount     # 叫載入器啟動 SDMOUNT.UF2

需要 hidapi:pip install hidapi

只有載入器**顯示選單時**才找得到裝置;正在跑別的專題時不會出現。
這是 S3 的驗證工具,正式的電腦端工具 retro-sync 是 S4。
"""
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("需要 hidapi:pip install hidapi")

VID, PID = 0xCAFE, 0x4010
REPLIES = {
    b"PRTL": "pong(載入器在選單)",
    b"OK  ": "找到 SDMOUNT.UF2,啟動中",
    b"NONE": "SD 卡上沒有 SDMOUNT.UF2",
}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("ping", "mount"):
        sys.exit(__doc__)
    cmd = {"ping": b"P", "mount": b"M"}[sys.argv[1]]

    if not hid.enumerate(VID, PID):
        sys.exit("找不到載入器(USB %04X:%04X)。掌機有停在選單嗎?" % (VID, PID))

    dev = hid.device()
    dev.open(VID, PID)
    try:
        # 第一個 byte 是 report ID(本裝置沒有,填 0),後面 8 byte 是 output report
        dev.write(b"\x00" + cmd + b"\x00" * 7)
        data = bytes(dev.read(8, timeout_ms=2000))
    finally:
        dev.close()

    if not data:
        sys.exit("沒有回覆")
    tag, ver = data[:4], chr(data[4]) if data[4] else "?"
    print(f"{REPLIES.get(tag, tag)}  (協定 v{ver})")
    if tag == b"NONE":
        sys.exit(1)


if __name__ == "__main__":
    main()
