# 交接文件

給接手這個專案的新對話串。**先讀這份，再依指引去讀 README 的對應章節與程式碼。**

最後更新：2026-08-15

---

## 1. 一句話現況

RP2040 掌機的開機載入器：冷開機進選單、從 SD 卡選一個 `.uf2` 燒進 flash、交棒執行。
**載入器與 infones 的整條鏈已通過實機驗證**，載入器 14256 / 16384 bytes（87%）；
目前只改造過 infones 一個專題，doom / apple2 / arcade 尚未動工。

---

## 2. 這個專案牽涉兩個倉庫

| 倉庫 | 分支 | 狀態 |
|---|---|---|
| [pondahai/rp2040-retro-loader](https://github.com/pondahai/rp2040-retro-loader) | `main` | 本專案 |
| [pondahai/rp2040-ili9341-infones](https://github.com/pondahai/rp2040-ili9341-infones) | `main` | [PR #12](https://github.com/pondahai/rp2040-ili9341-infones/pull/12) 已合併（merge commit `0072bcd`） |

兩邊要一起看才完整。infones 那邊的改動是「偏移編譯模式」，預設關閉，不影響原本的 build。

---

## 3. 先讀哪些章節

| 位置 | 內容 | 什麼時候讀 |
|---|---|---|
| `README.md` §1 | flash 佈局與整個設計的關鍵那一句 | **必讀**，不懂這段後面都看不懂 |
| `README.md` §3.4 | 改造專題的檢查清單 | 要改 doom / apple2 就必讀 |
| `README.md` §3.5 | infones 踩到的三個坑 | 同上，這是清單的由來 |
| `README.md` §4.1 | 軟重置穿透的行為與理由 | 要碰 `main()` 的開機流程就必讀 |
| `README.md` §9 | 驗證狀態（已驗證 / 未驗證分開列） | 動手前先確認你要碰的東西驗證到什麼程度 |
| `common/boot_map.h` | 所有位址的單一事實來源 | 要動位址就必讀 |

---

## 4. 設計的一句話

**載入器從 SD 讀 UF2 時，丟掉 `target_addr < 0x10004000` 的 block。**

那些 block 是跳板（讓同一份 UF2 也能被 USB 直接燒錄），寫下去等於把載入器自己蓋掉。
專題本體 link 到 +16KB 就是為了製造這塊「可丟棄」的區段。整套設計都繞著這一句轉。

```
0x10000000  載入器 或 跳板（16KB，兩者擇一）
0x10004000  專題本體（無自己的 boot2，最前面就是向量表）
```

---

## 5. 程式碼地圖

| 檔案 | 內容 |
|---|---|
| `common/boot_map.h` | `APP_BASE` / `BOOT_REGION_SIZE`，**改位址從這裡開始** |
| `common/launch.c` | 交棒序列（關中斷 → 清 XIP cache → VTOR → MSP → **`cpsie i`** → 跳）＋ `app_present()` |
| `loader/main.c` | 開機流程、SELECT/B 的判斷、選單 UI、`sb_*` 字串工具 |
| `loader/flasher.c` | UF2 解析與燒錄。**丟棄跳板那一句在這裡** |
| `loader/sdspi.c` | SD 驅動，序列照 infones 的 FatFs 範例抄 |
| `loader/fatlite.c` | 唯讀 FAT16/32，只看根目錄 |
| `loader/lcd.c` | ILI9341 文字模式 40×30 |
| `loader/board.h` | 腳位，**必須跟 infones 一致** |
| `trampoline/trampoline.c` | 32 行，只做交棒 |
| `app/memmap_app.ld` | **生成的，不要手改**，見 `tools/gen_app_ld.py` |
| `tools/merge_uf2.py` | 跳板 + 本體 → 單一 UF2，並用 `0xFF` 補滿中間的空隙（`pad_gap()`，見 README §3.5 坑 3） |
| `tools/check_size.cmake` | 超過 16KB 讓 build 失敗 |

---

## 6. 環境（很重要，之前查漏過）

**本機可以編譯也可以燒錄。** 工具鏈在 `C:\Users\Dell\.pico-sdk\`，
不在 PATH 上 —— 只查 PATH 會誤判成「沒有工具鏈」，我第一次就是這樣搞錯的。

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

infones 的完整編譯指令記在它自己的 `software/infones/README.md`。

---

## 7. 燒錄與除錯的坑（全部實際踩過）

**① 位址不連續的 UF2 拖曳會安靜截斷。** 合併版拖進 `RPI-RP2` 只有前 22 塊
跳板寫進去，本體完全沒寫，且無錯誤訊息。**不是大小問題**——同尺寸的連續 UF2
拖曳正常。`merge_uf2.py` 現在預設用 `0xFF` 補滿跳板與 `APP_BASE` 之間的空隙，
補完之後拖曳可行（已實機確認）。機制未查明，見 README §3.5 坑 3。

開發時仍建議用有驗證的燒法：

```bash
picotool load -v -x <檔案>
```

**② `picotool load -x` 與 `picotool reboot` 都是軟重置。**
新版載入器會因此**直接穿透到 app**，看不到選單。要看選單就燒完不加 `-x`，
然後**拔電冷開機**。

**③ 載入器沒有 USB**（stdio 為省空間關掉），所以 `picotool` 踢不動它。
要進 BOOTSEL 只能 **開機時按住 SELECT**（逃生口，在 `main()` 最前面）。
跑 infones 的時候倒是可以用 `picotool reboot -f -u`。

**④ 讀 flash 內容是最有效的診斷。**

```bash
picotool save -r 0x10004000 0x10004020 readback.bin
```

`0x10004000` 應該是 `00 20 04 20 f7 40 00 10`（SP + Reset 向量）。全 `0xFF` 代表沒寫進去。

**⑤ infones 的 serial 要設 `DtrEnable = $true`** 才有輸出（見 infones `fds_plan.md` 7.6）。
但韌體卡死時 CDC 不服務，開埠會整個卡住 —— 那時候 1200 baud touch 也沒用，
改用 `picotool reboot -f -u`。

---

## 8. 待辦事項（建議順序）

### 8.1 補完未驗證的項目 ⚠️ 成本很低，建議先做

- 按實體 RESET 是否確實回到載入器選單（理論上 RUN 腳是 power-on reset）
- 開機按住 B 強制顯示選單的保險
- 多個 `.uf2` 時的捲動與長按重複

前兩項使用者順手就能測，測完更新 README §9。

### 8.2 ~~改造第二個專題（doom 或 apple2）~~ ✅ apple2 已完成

2026-08-15 完成 [pondahai/PicoApple2](https://github.com/pondahai/PicoApple2) 的改造，
跳板路線與載入器路線都實機跑通。詳見 README §9 的「第二個專題」一段。

**最大的收穫是清單本身不夠用**：它假設專題是 pico-sdk CMake 專案，而 PicoApple2 是
arduino-cli + arduino-pico，換 linker script 的機制完全不同；更要命的是 arduino-pico
的向量表不在 image 最前面（前面壓著 `.ota` + `.partition`，共 `0x3000`），
只改 `ORIGIN` 會讓載入器跳到一堆資料上。這一類坑清單原本沒提。

反倒是清單裡「最容易漏掉」的第 ③ 項在這裡完全不適用——PicoApple2 的磁碟映像走 SD 卡，
flash 上只有 image 本身。

**改 doom 之前先讀 README §9 那一段。**

### 8.2b 改造 doom / arcade

**照 README §3.4 的清單走。** 那份清單就是為此寫的，第二個專題也會反過來檢驗
清單夠不夠用。

**最容易漏掉的是第 ③ 項**：專題寫死的 flash 位址不會跟著 linker script 位移。
infones 就是踩在這裡（`NES_FILE_ADDR`），症狀是黑畫面且極難查。

> **DOOM 特別注意**：它的空間比 infones 更緊。動手前先確認 image 尾端離
> 它自己的資料區還剩多少 —— 位移 16KB 之後可能直接撞上。

### 8.3 ~~合併 infones 的 PR #12~~ ✅ 已完成

2026-08-15 合併（`0072bcd`），四個 commit 保留。合併後從乾淨狀態重編預設模式，
確認輸出與改動前逐項一致。

infones 那邊另有一件未完成的事：把存檔從 flash 搬到 SD 卡，
見該倉庫的 `nvram_sd_plan.md` 與其 `HANDOVER.md` §4.5。
動機是一個目前就存在的缺陷——實際上只有一個存檔槽，換遊戲會覆蓋上一款的進度。

### 8.4 尚未查明的一件事

實機測試途中出現過：選了遊戲之後**黑畫面**，冷開機才恢復。

`watchdog reason` 解釋了「為什麼沒進 infones 選單」（軟重置 → infones 直接走
執行遊戲那條路），但**沒有解釋「為什麼遊戲畫面是黑的」**——當時 ROM 已經
正確寫在 `0x10084000`，flash 四個關鍵位址讀回來全對，USB 穩定列舉、沒有重置迴圈。

**這一半仍然沒有原因。** 如果之後再遇到，線索是：watchdog 重置不會清 RAM
與部分周邊狀態，冷開機會。

---

## 9. 專案慣例與地雷

**`common/boot_map.h` 是位址的單一事實來源，但 linker script 沒辦法 `#include` 它。**
`app/memmap_app.ld` 裡的 `0x10004000` / `2032k` 只能靠人工同步。改一邊記得改另一邊。

**`app/memmap_app.ld` 不要手改。** 它是 `tools/gen_app_ld.py` 從 SDK 自己那份
`memmap_default.ld` 生成的。SDK 版本之間這個檔案差異不小（2.x 多了
`.tdata`/`.tbss`、`.embedded_block`，`NOLOAD` 取代 `COPY`，`__etext` 定義方式也換了），
手抄的版本會安靜地跟 SDK 脫節，症狀是「編得過但開機掛掉」。換 SDK 就重跑腳本。

**16KB 是硬限制，不是目標值。** `tools/check_size.cmake` 會擋。加功能前先看
build 印出來的剩餘 bytes（目前只剩 2128）。DOOM 那邊空間更緊，放大保留區的代價很高。

**不要在載入器裡用 `snprintf`。** newlib 的格式化即使 nano 版也要 1.5–2KB。
用 `main.c` 裡的 `sb_*`。

**不要在載入器裡開 DMA。** 交棒時留在半路上的 DMA 會弄髒專題。全程 blocking SPI
是刻意的。

**`launch.c` 的 `cpsie i` 不能拿掉。** pico-sdk 的 `crt0.S` 從頭到尾沒碰過 PRIMASK，
帶著中斷遮蔽跳過去，專題會卡死在第一個 `sleep_ms()`。

**`lcd.h` 的 include guard 是 `LOADER_LCD_H`，不能用 `LCD_H`** —— `board.h` 拿
`LCD_H` 當螢幕高度，撞名會讓整個標頭被靜靜跳過。

**SD 驅動的序列照抄 infones，不要自己想。** 我自己重寫了一版，連續踩兩個坑
（漏掉 MISO 上拉、指令之間沒等卡片忙完），花掉好幾輪實機測試。
infones 那份是這塊硬體上已經證明可用的。

---

## 10. 開新對話串的提示詞

貼這段就能接上：

```
專案 pondahai/rp2040-retro-loader（RP2040 掌機的開機載入器）。

請先讀 repo 根目錄的 HANDOVER.md，再依它的指引讀 README.md 的相關章節。
相關的還有 pondahai/rp2040-ili9341-infones 的 feature/loader-offset-build 分支（PR #12）。

這次要做的是：<填入 HANDOVER.md 第 8 節的其中一項>

本機可以編譯也可以燒錄，工具鏈在 ~/.pico-sdk（不在 PATH 上）。
實機測試需要使用者操作（看螢幕、按按鍵、拔電），請明確說出
哪些部分沒有驗證過。
```

接續實機測試時，記得補上**測試結果**——那是新對話串永遠不知道、只有使用者
能提供的資訊。例如：

```
冷開機進載入器選單正常，選 infones 後燒錄約 15 秒，進 infones 選單，
選遊戲後直接進遊戲沒有閃過載入器。但按實體 RESET 之後畫面是黑的。
```

---

## 11. 分支與歷史

`main` 上的 commit 依序記錄了整個過程，訊息裡寫了「為什麼」而不只是「改了什麼」：

| commit | 內容 |
|---|---|
| `b279612` | 初版（未編譯、未驗證） |
| `c839dd0` | 編譯通過，`memmap_app.ld` 改為從 SDK 生成 |
| `159bee6` | 交棒補 `cpsie i` |
| `340bc95` | README 補檢查清單與三個坑 |
| `e36e56f` | 階段顯示、SELECT 逃生口 |
| `38a8850` | SD 補 MISO 上拉、逾時改用真正的時間 |
| `d644ccb` | SD 指令間補 `wait_ready` |
| `d318c1e` | 軟重置穿透 + B 保險 |
| `f0072ae` | README 標記整條鏈已實機驗證 |
| `4004727` | 加入 HANDOVER.md |
| （本次） | `merge_uf2.py` 補空隙，讓合併版能被拖曳燒錄 |

`git log` 看得到完整脈絡。每個修正都對應一次實機症狀，訊息裡有記症狀與推理。
