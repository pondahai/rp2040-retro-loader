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

### 2.1 環境

實際驗證過的組合（VS Code 的 Raspberry Pi Pico 擴充套件裝在 `~/.pico-sdk/` 底下就有這些）：

| 元件 | 版本 |
|---|---|
| pico-sdk | 2.2.0 |
| GCC arm-none-eabi | 14.2.1（`toolchain/14_2_Rel1`） |
| CMake | 3.31.5 |
| Ninja | 1.12.1 |
| picotool | 2.2.0-a4 |

SDK 1.x 沒有測過。`app/memmap_app.ld` 是從 2.2.0 的 `memmap_default.ld` 生成的，換版本要重跑 `tools/gen_app_ld.py`（見 §3.1）。

### 2.2 指令

Windows（Git Bash / PowerShell 皆可，路徑照抄）：

```bash
P="$HOME/.pico-sdk"
cmake -B build -G Ninja \
  -DPICO_SDK_PATH="$P/sdk/2.2.0" \
  -DPICO_TOOLCHAIN_PATH="$P/toolchain/14_2_Rel1" \
  -DCMAKE_MAKE_PROGRAM="$P/ninja/v1.12.1/ninja.exe" \
  -Dpicotool_DIR="$P/picotool/2.2.0-a4/picotool" \
  -Dpioasm_DIR="$P/tools/2.2.0/pioasm"
```

```bash
ninja -C build
```

`PICO_SDK_PATH` 不指定的話會退回 `../../pico-sdk`，也就是假設倉庫旁邊有一份 pico-sdk clone。多數情況你會需要明確指定。

### 2.3 產出與大小

- `build/loader.uf2` — 載入器，用 USB BOOTSEL 燒進 Pico
- `build/trampoline.uf2` — 跳板，給合併工具用，不會單獨燒

build 過程會印出兩者的大小（以下是實測值，非估計）：

```
-- loader: 13544 / 16384 bytes (82%, 剩 2840)
-- trampoline: 5536 / 16384 bytes (33%, 剩 10848)
```

超過 16KB 的話 **build 會直接失敗**（`tools/check_size.cmake`）。這是刻意的——超出去只會蓋到專題本體，症狀是「燒進去之後開機沒反應」，很難查。

載入器只剩 2840 bytes 餘裕，加功能前先看這個數字。

---

## 3. 改造一個專題（以 infones 為例）

專題本體必須重新編譯，不能只是把檔案往後搬——RP2040 是 XIP，所有資料位址在編譯時就寫死在機器碼裡了。

### 3.1 改 linker script

在專題的 `CMakeLists.txt` 加一行：

```cmake
pico_set_linker_script(infoNES ${CMAKE_CURRENT_LIST_DIR}/../../rp2040-retro-loader/app/memmap_app.ld)
```

`app/memmap_app.ld` 不是手寫的，是用 `tools/gen_app_ld.py` 從 SDK 自己那份 `memmap_default.ld` 生成的，只動兩處：

1. `FLASH ORIGIN` 從 `0x10000000` 改成 `0x10004000`，長度扣掉 16KB
2. 丟掉 `.boot2` 與它的 `ASSERT`

換 SDK 版本時重跑：

```bash
python tools/gen_app_ld.py ~/.pico-sdk/sdk/2.2.0
```

**不要手改這個檔案。** `memmap_default.ld` 在 SDK 版本之間差異不小（2.x 多了 `.tdata`/`.tbss`、`.embedded_block`，`NOLOAD` 取代了 `COPY`，`__etext` 的定義方式也換了）。手抄的版本很快會跟 SDK 脫節，而症狀是「編得過但開機掛掉」這種最難查的那種。生成腳本會在版面對不上時直接報錯，不會安靜地生出壞檔案。

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

### 3.4 改造其他專題時的檢查清單

infones 是第一個改造的專題，以下是實際踩出來的順序。改 doom / apple2 / arcade 時照著走。

**① 換 linker script** — 用 `tools/gen_app_ld.py` 生成，不要手寫（§3.1）。

**② 確認丟掉 `.boot2` 是安全的**

SDK 的 `crt0.S` 裡有一段會引用 `__boot2_entry_point`。在 SDK 2.2.0 它被 `#if !PICO_RP2040 && PICO_EMBED_XIP_SETUP && !PICO_NO_FLASH` 包住，RP2040 上不會編進去，所以 discard 安全。**換 SDK 版本時要重新確認**：

```bash
grep -n -B4 "__boot2_entry_point" $PICO_SDK_PATH/src/rp2_common/pico_crt0/crt0.S
```

**③ ⚠️ 找出專題寫死的 flash 位址——最容易漏掉、症狀最難查的一項**

很多專題會在 flash 裡自己劃地盤：存 ROM、存檔、字型、資源。這些位址是絕對值，**不會**跟著 linker script 一起位移，於是本體往後推 16KB 之後，尾巴就可能壓上去。

```bash
grep -rn "XIP_BASE\|0x10[0-9a-fA-F]\{6\}\|flash_range_erase\|flash_range_program" --include=*.c --include=*.cpp --include=*.h .
```

找到之後把那些位址**一起往上推 16KB**，維持跟預設編譯相同的相對佈局。

**④ 加一道 build 期的佈局檢查**

這種重疊不會在編譯時報錯，只會在實機上變成黑畫面。infones 的做法是 `software/infones/check_flash_layout.cmake`：算出 image 結束位址，跟資料區起點比對，重疊就讓 build 失敗。每個專題都值得抄一份。

**⑤ 燒錄用 `picotool load -v`，不要拖曳** — 見 §3.5 坑 3。

---

### 3.5 案例：infones 踩到的三個坑

**坑 1：交棒時中斷沒有重新打開**

`launch_app()` 用 `cpsid i` 關中斷，而 pico-sdk 的 `crt0.S` 從頭到尾沒有碰過 PRIMASK——正常開機時它本來就是 0，SDK 沒有理由去清它。帶著 PRIMASK=1 跳過去，專題就在「所有中斷被遮蔽」的狀態下啟動：`sleep_ms()` 靠 alarm + WFE，永遠不會返回。

infones 的 `display_init()` 第一行就是 `sleep_ms(100)`，而背光是那個函式**最後**才點亮的，所以症狀是連背光都不亮，看起來像完全沒開機。

已修（`common/launch.c` 的 `cpsie i`），這是所有專題共用的路徑，不必逐一處理。

**坑 2：`NES_FILE_ADDR` 沒有跟著位移**

infones 把選中的 ROM 燒在 `0x10080000`，NVRAM 存檔槽則從那裡往下長（`NES_FILE_ADDR - SRAM_SIZE * (slot+1)`）。

| | image 結束 | 存檔槽 slot0 | 結果 |
|---|---|---|---|
| 預設 | `0x1007d078` | `0x1007e000` | 餘裕 3,976 bytes |
| 偏移（未修） | `0x10080f78` | `0x1007e000` | **重疊 12,152 bytes** |
| 偏移（ROM 區移到 `0x10084000`） | `0x10080f78` | `0x10082000` | 餘裕 4,232 bytes |

未修時，infones 開機會把自己的 `.data` 初值讀成 ROM；選遊戲時 `menu.cpp` 還會 erase 掉那份初值。

值得注意的是**預設編譯的餘裕本來就只有約 4KB**——這個相鄰關係一直沒有被檢查過，只是「剛好」沒撞到。位移 16KB 就撞了。

**坑 3：拖曳燒錄會安靜地截斷**

把 1MB 的合併版 UF2 拖進 `RPI-RP2` 磁碟，結果只有前面 22 塊跳板寫進去，本體完全沒寫（讀回 `0x10004000` 全是 `0xFF`），而且沒有任何錯誤訊息。

診斷方式：

```bash
picotool info -a
```

```bash
picotool save -r 0x10004000 0x10004020 readback.bin
```

改用有驗證的燒錄（`-v` 驗證，`-x` 順便執行）：

```bash
picotool load -v -x infones_standalone.uf2
```

裝置正在跑 app 時先踢回 BOOTSEL：

```bash
picotool reboot -f -u
```

> infones 的 `fds_plan.md` 7.6 記著「picotool 在 Windows 踢不動板子，要用 1200 baud touch」。用 picotool 2.2.0-a4 實測 `reboot -f -u` **可以**踢動；反而是韌體卡死時 CDC 不服務，1200 baud touch 會整個卡住開不了埠。

這個坑的代價很大：它讓「跳板沒問題」看起來像「跳板壞了」。跳板的空白 flash 偵測（`app_present()` → `reset_usb_boot()`）反而是揭穿它的關鍵——裝置一直退回 BOOTSEL，才讓人想到去讀 `0x10004000`。

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
| **開機時按住 SELECT** | **直接進 BOOTSEL** |

最後一項是逃生口，很重要：載入器沒有 USB（stdio 為了省 16KB 預算關掉了），
所以 `picotool` 沒有 reset interface 可用——一旦燒進去，本來只能靠實體
BOOTSEL 按鈕才能重燒，機殼按不到那顆按鈕的話就等於磚了。

這個檢查放在 `main()` 最前面，LCD 和 SD 都還沒初始化，所以就算它們壞掉也救得回來。

執行之後要回選單只能重開機——載入器交棒後就不存在了，它的 RAM 已經被專題拿去用。

### 4.1 軟重置會直接穿透，不顯示選單

| 開機原因 | `watchdog_hw->reason` | 載入器行為 |
|---|---|---|
| 冷開機（上電） | 0 | 顯示選單 |
| 按實體 RESET（拉 RUN 腳） | 0 | 顯示選單 |
| 軟重置（watchdog、`picotool reboot`） | ≠0 | **直接跳到 `0x10004000`**，不顯示選單、不重燒 |
| 軟重置但開機時按住 B | ≠0 | 顯示選單（保險） |

**為什麼需要這個。** 有些專題用「重置晶片」來切換自己的狀態。infones 就是這樣：
它的選單選好遊戲後把 ROM 燒進 flash，然後 `watchdog_enable()` 重置，
開機時再用 `watchdog_caused_reboot()` 判斷這次要直接進遊戲。
（`menu.cpp` 的註解說重置是為了拿到乾淨的 core1/DMA 狀態，聲音才正常。）

載入器搬進 `0x10000000` 之後，那次重置就不再回到 infones，而是掉進載入器的
選單——使用者得再選一次、再等重燒 1MB。**是載入器改變了「重置」的意義，
所以由載入器負責把它修回來**，而不是要求每個專題改寫自己的狀態機。

**為什麼這樣就對了。** `watchdog_hw->reason` 是唯讀的（`io_ro_32`），軟體清不掉；
而載入器是用「跳」的、自己從不重置晶片。所以這個值會原封不動地傳給專題，
它讀到的跟沒有載入器時一模一樣。

**風險與逃生口。** 如果某個專題當機後被 watchdog 重置，就會被無限穿透回去，
永遠看不到載入器選單。三條退路：拔電、按 RESET、或**開機時按住 B**。
（B 在選單裡是「重新掃描」，開機時則是「強制顯示選單」。）

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

## 8. 開發過程

設計是從一個問題往回推出來的：**同一份 UF2，要能單獨燒錄，也要能被載入器載入，而載入器不能把自己蓋掉。**

推導順序大致是：

1. 載入器要活在 flash 最前面（ROM 只認那裡），所以專題本體必須讓開 → 偏移
2. 但偏移後的 image 單獨燒錄就開不了機（前面是空的）→ 需要跳板填那塊地
3. 跳板跟載入器住同一塊地，兩者擇一 → 載入器讀 UF2 時必須丟掉跳板的 block
4. 而「偏移」不能靠搬 bytes：RP2040 是 XIP，資料位址在編譯時就寫死在機器碼裡 → 專題必須重新編譯
5. 重編之後專題自己那份 boot2 就沒用了（也不可能被執行）→ 從 linker script 丟掉

### 過程中踩到的坑

**`memmap_default.ld` 不能憑印象手寫。** 第一版是照 SDK 1.5 的記憶寫的，拿 2.2.0 的原檔一比，差了 `.tdata`/`.tbss`、`.embedded_block`、`NOLOAD` vs `COPY`、`__etext` 的定義方式。這種錯誤編得過但開機會掛，所以改成用 `tools/gen_app_ld.py` 從 SDK 原檔生成，版面對不上就直接報錯。

**include guard 撞名。** `board.h` 用 `LCD_H` 表示螢幕高度，而 `lcd.h` 的 guard 也叫 `LCD_H` → `#ifndef LCD_H` 永遠為假，整個標頭被靜靜跳過，錯誤訊息卻是一整排「`LCD_COLS` undeclared」。guard 現在叫 `LOADER_LCD_H`。

**16KB 比想像中緊。** 除了預期中的 FatFs（8–10KB），還有兩個沒預料到的：`snprintf` 即使 newlib nano 版也要 1.5–2KB，infones 的 `font_8x8.h` 是 2-bit packed 抗鋸齒格式、解碼邏輯比字型本身還大。兩個都換成自己寫的之後才進到 82%。

---

## 9. 驗證狀態

### 已驗證（在本機實際跑過）

- **編譯通過**：SDK 2.2.0 + GCC 14.2.1，loader 13544 bytes、trampoline 5536 bytes，都在 16KB 內
- **`memmap_app.ld` 正確**：用一個最小測試專案編出來，`objdump` 確認 `.text` 落在 `0x10004000`、**沒有 `.boot2` 段**、向量表在最前面（SP `0x20042000`、Reset `0x100040f7`，Thumb bit 已設）——正好符合 `app_present()` 的檢查條件
- **合併與丟棄規則**：`merge_uf2.py` 把真實的 trampoline.uf2 + 偏移版 app.uf2 合成 47 塊，流水號重編正確；模擬載入器的規則後，22 塊跳板被丟棄、25 塊從 `0x10004000` 開始寫入
- **工具會擋錯**：用預設 linker script 編的 app 會被 `merge_uf2.py` 拒絕

### 已驗證（實機）

**跳板這條路完整跑通了**：合併版 `infoNES_standalone.uf2` 用 `picotool load -v` 燒進掌機，
冷開機 → 跳板交棒 → infones 選單顯示 → 選一個 NES → 寫進 `0x10084000` →
watchdog 重開 → 跳板再次交棒 → **遊戲正常遊玩**。

也就是說底層假設全部成立：偏移編譯、丟掉 boot2、`cpsie i` 交棒、
ROM 區跟著位移，以及同一份 UF2 能被 USB 直接燒錄。

燒錄後讀回 flash 確認（`picotool save`）：

| 位址 | 讀回 | |
|---|---|---|
| `0x10000000` | `00 b5 32 4b ...` | 跳板 boot2 |
| `0x10004000` | `00 20 04 20 f7 40 00 10` | infones 向量表 |
| `0x10080000` | 非 `0xFF` | 本體尾巴完好 |
| `0x10084000` | `4e 45 53 1a ...` | 選中的 NES ROM |

### 尚未驗證（載入器本身）

**載入器（選單 + SD + 燒錄）還沒有在實機上跑過任何一行。** 目前驗證到的只有跳板那條路。

- SD 卡初始化與 FAT 解析——`sdspi.c` 與 `fatlite.c` 都是新寫的，沒碰過任何一張真卡
- ILI9341 文字模式的實際顯示（初始化序列照 infones 抄的，但沒點亮過）
- 從 SD 讀 UF2 並寫進 flash 的整段流程
- 按鍵的長按重複手感

### 一個還沒解釋清楚的現象

實機測試途中出現過一次：選了遊戲之後黑畫面，`picotool reboot` 也還是黑畫面，
但**拔電再上電就完全正常**，之後選遊戲也能玩。

當時已經確認 flash 內容完全正確（上表那四個位置都對），USB 穩定列舉、
沒有重置迴圈。所以不是 flash 損壞，也不是 watchdog 重置迴圈。

真正的原因**沒有查出來**。可能是先前那版（ROM 區還沒位移、會蓋到本體尾巴）
留下的殘留狀態，加上 SD 卡上的 `currentloadedrom.txt` 仍指著一個遊戲，
使得開機直接走「執行遊戲」而不是「顯示選單」。但這只是推測，沒有證據。

**如果之後再遇到「選了遊戲就黑畫面、但冷開機正常」，這一段可以當線索。**
值得注意的是 watchdog 重開機不會清掉 RAM 與部分周邊狀態，而冷開機會。

### 設計上的限制

- SD 卡只認根目錄、只認第一個分割區、磁區必須是 512 bytes
- 非 ASCII 檔名會顯示成 `?`（字型只有 ASCII）
- 檔名最長 32 字元
- 最多列出 24 個檔案
- 燒錄只寫 UF2 涵蓋到的 sector。**前一個遊戲留在後面的資料不會被清掉**——如果某個專題把存檔放在 flash 尾端的固定位址，它可能讀到上一個遊戲的殘留資料。infones 有 NVRAM 機制，這點值得實機確認
- 燒錄中途失敗會讓 APP 區處於半個 image 的狀態，必須重選一個燒完整
