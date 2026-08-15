# rp2040-retro-loader

RP2040 掌機的開機載入器。冷開機進選單，列出 SD 卡上的 `.uf2`，選一個燒進 flash，然後交棒過去執行。

搭配 [rp2040-retro-handheld](https://github.com/pondahai/rp2040-retro-handheld) 的各個專題使用：infones、doom、apple2、makecode arcade。

---

## 1. Flash 佈局

```
0x10000000  +------------------------------+
            | boot2 (256B)                 |
            | 載入器  或  跳板               |  16KB (BOOT_REGION_SIZE)
            |   兩者擇一，看是怎麼燒進去的     |
0x10004000  +------------------------------+  APP_BASE
            | 專題本體 (無自己的 boot2)       |
            |   最前面就是向量表              |
            +------------------------------+
```

前面 16KB 這塊地在兩種情境下住著不同的東西：

| 情境 | 前 16KB | 開機流程 |
|---|---|---|
| 用載入器 | 載入器 | ROM → 載入器 → 選單 → 燒 SD 上的 UF2 → 跳 `0x10004000` |
| USB BOOTSEL 直接拖 UF2 | 跳板 | ROM → 跳板 → 跳 `0x10004000` |

**整個設計的關鍵是一句判斷**：載入器從 SD 讀 UF2 時，丟掉 `target_addr < 0x10004000` 的 block（`loader/flasher.c`）。那些 block 是跳板，寫下去等於把載入器自己蓋掉。專題本體 link 到 +16KB 就是為了製造這塊「可丟棄」的區段。

`common/boot_map.h` 是這些數字的單一事實來源。

---

## 2. 編譯

需要 pico-sdk（1.5 以上）。

```bash
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build
```

產出：

- `build/loader.uf2` — 載入器，用 USB BOOTSEL 燒進 Pico
- `build/trampoline.uf2` — 跳板，給合併工具用，不會單獨燒

build 過程會印出兩者的大小：

```
-- loader: 12480 / 16384 bytes (76%, 剩 3904)
-- trampoline: 3216 / 16384 bytes (19%, 剩 13168)
```

超過 16KB 的話 **build 會直接失敗**（`tools/check_size.cmake`）。這是刻意的——超出去只會蓋到專題本體，症狀是「燒進去之後開機沒反應」，很難查。

---

## 3. 改造一個專題（以 infones 為例）

專題本體必須重新編譯，不能只是把檔案往後搬——RP2040 是 XIP，所有資料位址在編譯時就寫死在機器碼裡了。

### 3.1 改 linker script

在專題的 `CMakeLists.txt` 加一行：

```cmake
pico_set_linker_script(infoNES ${CMAKE_CURRENT_LIST_DIR}/../../rp2040-retro-loader/app/memmap_app.ld)
```

`app/memmap_app.ld` 相對於 pico-sdk 的預設只有兩處不同：

1. `FLASH ORIGIN` 從 `0x10000000` 改成 `0x10004000`，長度扣掉 16KB
2. 丟掉 `.boot2`

### 3.2 為什麼要丟掉 boot2

專題自己那份 boot2 永遠不會被執行——ROM 只認 flash 最前面那 256 bytes，而那是載入器／跳板的地盤。留著不只白佔位置，它結尾還寫死「跳到 `0x10000100`」，萬一真的被執行會跳到別人家。

丟掉之後 `0x10004000` 第一個 byte 就是向量表，載入器與跳板都跳這個位址，沒有 `+0x100` 之類的特例要記。

### 3.3 合併出可單獨燒錄的 UF2

```bash
python tools/merge_uf2.py build/trampoline.uf2 infones.uf2 -o infones_standalone.uf2
```

合併版兩種用法都吃得下（USB 拖進去 / 放 SD 給載入器），所以 **SD 卡上放合併版就好**，不必準備兩個檔案。

工具會擋下位址搞錯的組合——例如專題還是用預設 linker script 編的。

---

## 4. 使用

1. `loader.uf2` 用 USB BOOTSEL 燒進 Pico（只需要一次）
2. SD 卡格式化成 FAT16 或 FAT32
3. **把 `.uf2` 放在根目錄**（不支援子目錄）
4. 開機 → 選單 → 上下選 → A 或 START 執行

| 按鍵 | 功能 |
|---|---|
| UP / DOWN | 移動光棒（長按會重複） |
| A / START | 燒錄並執行 |
| B | 重新掃描 SD 卡 |

執行之後要回選單只能重開機——載入器交棒後就不存在了，它的 RAM 已經被專題拿去用。

---

## 5. 為什麼是這些取捨

16KB 是硬限制，而 DOOM 那邊剩下的空間更少，所以放大保留區的代價很高。以下決定都是為了守住這個數字：

| 決定 | 省下 | 代價 |
|---|---|---|
| 不用 FatFs，自己寫 `fatlite.c` | 8–10KB | 只支援 FAT16/FAT32、只讀、只看根目錄 |
| 不實作寫入 | ~2KB | 載入器沒有能力弄壞 SD 卡（這其實是好事） |
| 不用 `snprintf`，自己寫 `sb_*` | 1.5–2KB | 只能印字串與十進位整數 |
| 只認根目錄 | 0.6–1KB | 使用者要自己整理 SD 卡 |
| 文字模式，不做 framebuffer | 150KB RAM | 畫面樸素、更新慢 |
| 全程 blocking SPI，不碰 DMA | — | 燒錄慢一些，但交棒時不會有 DMA 留在半路上弄髒專題 |

字型也換成自己的 1bpp `font8x8.h`，而不是 infones 的 `font_8x8.h`——後者是 2-bit packed 的抗鋸齒字型，解碼邏輯比字型本身還大。

---

## 6. 交棒做了什麼（`common/launch.c`）

正常開機時這幾件事是硬體與 boot2 幫忙做的，手動跳轉就得自己補：

1. **關掉所有中斷** — 否則跳過去之後，NVIC 裡還留著我們的 handler 位址，一個 timer 中斷就跳回一個已經不存在的函式
2. **清 XIP cache** — 剛換過一整塊 flash 內容，cache 裡可能還留著舊指令
3. **設 VTOR** — 告訴 CPU 向量表搬到 `0x10004000` 了
4. **設 MSP 再跳** — 用專題自己的堆疊頂端

不 reset 周邊是刻意的：pico-sdk 的 `runtime_init()` 在專題那頭開機時已經 reset 掉除了 QSPI/PLL/USB 以外的所有 block。我們只要保證在那之前沒有東西會插進來就好——前提是載入器不留下進行中的 DMA，所以載入器全程用 blocking SPI。

跳之前會先驗一下 `0x10004000` 的向量表像不像真的（堆疊頂端落在 SRAM、進入點落在 APP 區且是 Thumb）。空白 flash 讀出來是 `0xFFFFFFFF`，直接跳過去只會得到一台沒有反應的機器，而且沒有看門狗可以救。跳板遇到這種情況會改叫 `reset_usb_boot()`，讓使用者至少看得到 USB 磁碟可以重燒。

---

## 7. 硬體腳位

取自 `rp2040-ili9341-infones`，兩邊必須一致（見 `loader/board.h`）。

| 用途 | 腳位 |
|---|---|
| LCD (ILI9341, spi0) | DC 20, CS 17, CLK 18, MOSI 19, RST 21, BL 22 |
| SD (spi1) | CS 13, SCK 10, MOSI 11, MISO 12 |
| 按鍵（active-low） | UP 9, DOWN 5, LEFT 8, RIGHT 6, SELECT 28, START 4, A 2, B 3 |

---

## 8. 已知限制與尚未驗證的部分

**尚未實機驗證。** 這份程式碼是在沒有 pico-sdk、沒有硬體的環境寫的，沒有編譯過也沒有燒錄過。以下都還沒被證實：

- 載入器與跳板的實際大小是否真的在 16KB 內（`check_size.cmake` 會在 build 時告訴你）
- SD 卡初始化與 FAT 解析（`fatlite.c` 是新寫的，沒有跑過任何一張真卡）
- 交棒序列在實機上是否乾淨
- `app/memmap_app.ld` 是照 pico-sdk 1.5 的 `memmap_default.ld` 改的，若你的 SDK 版本不同需要重新比對

**設計上的限制：**

- SD 卡只認根目錄、只認第一個分割區、磁區必須是 512 bytes
- 非 ASCII 檔名會顯示成 `?`（字型只有 ASCII）
- 檔名最長 32 字元
- 最多列出 24 個檔案
- 燒錄只寫 UF2 涵蓋到的 sector。**前一個遊戲留在後面的資料不會被清掉**——如果某個專題把存檔放在 flash 尾端的固定位址，它可能讀到上一個遊戲的殘留資料。infones 有 NVRAM 機制，這點值得實機確認
- 燒錄中途失敗會讓 APP 區處於半個 image 的狀態，必須重選一個燒完整
