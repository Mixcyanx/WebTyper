# WebTyper

WebTyper 是一款結合網頁介面與 nRF52840 開發板的無線文字輸入工具。使用者在手機或其他支援 Web Bluetooth 的裝置上準備文字，再由開發板模擬 USB 鍵盤，輸入到電腦目前游標所在的位置。目標電腦不需要執行專用接收程式。

**網頁介面 → BLE → nRF52840 → USB HID 鍵盤 → 電腦**

網頁提供深色文字輸入區、貼上與清空、傳送與停止，以及漢堡選單中的速度和輸入法切換設定。目前支援英文、數字與 ASCII 符號，不支援中文或 Emoji；輸入結果會受到目標電腦鍵盤配置與輸入法影響。

## 裝置外觀

| 裸板 | 加上 3D 列印外殼 |
| :---: | :---: |
| <img src="https://raw.githubusercontent.com/Mixcyanx/WebTyper/refs/heads/main/Images/S__18849795.jpg" alt="WebTyper nRF52840 裸板" width="320"> | <img src="https://raw.githubusercontent.com/Mixcyanx/WebTyper/refs/heads/main/Images/S__18857990.jpg" alt="WebTyper 加上 3D 列印外殼" width="320"> |

## 準備項目

- 本專案使用的 nRF52840 開發板，以及可傳輸資料的 USB 線或轉接器。
- Windows、macOS 或 Linux 電腦，用於安裝 Arduino IDE 與上傳韌體。
- 支援 Web Bluetooth 的瀏覽器；網頁必須透過 HTTPS 或 localhost 開啟。
- 本專案的 `firmware/WebTyper/WebTyper.ino` 和 `index.html`。

## 安裝 Arduino IDE

1. 從 [Arduino 官方軟體頁面](https://www.arduino.cc/en/software/)下載適合自己作業系統的 Arduino IDE 2.x。
2. 安裝並啟動 Arduino IDE。
3. 先完成下面的開發板套件安裝，再開啟專案韌體。

## 安裝開發板套件

### 開發板管理員網址

開啟 Arduino IDE 的「檔案 → 偏好設定」（macOS 為應用程式選單內的 Settings / Preferences），在「額外的開發板管理員網址」加入：

```text
https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
```

如果欄位已有其他網址，保留原本內容，透過清單編輯按鈕另加一行。

這是 Adafruit 官方套件索引網址，可參考 [Adafruit 安裝說明](https://learn.adafruit.com/bluefruit-nrf52-feather-learning-guide/arduino-bsp-setup)。網址提供的是安裝清單，並非完整離線套件；只保存這個 JSON 無法防止日後下載連結失效。

### 安裝指定版本

1. 開啟左側「開發板管理員」，或「工具 → 開發板 → 開發板管理員」。
2. 搜尋 `Adafruit nRF52`。
3. 選擇 Adafruit 發行的 nRF52 套件（介面可能顯示 **Adafruit nRF52** 或 **Adafruit nRF52 Boards**）。
4. 在版本選單選擇 **1.7.0**，再按「安裝」。等待開發板核心與編譯工具下載完成。

本專案固定記錄 1.7.0，方便重現現有環境；不要把「最新版」當作相同版本的替代品。

## 程式庫

本韌體直接使用 `Arduino.h`、`bluefruit.h` 與 `Adafruit_TinyUSB.h`。在本次已驗證的環境中，這些都由開發板套件提供，不需要另外安裝同名程式庫。

| 元件 | 本機備份版本 | 用途 |
| --- | --- | --- |
| Adafruit nRF52 核心 | 1.7.0 | Arduino 核心、板型與工具設定 |
| Adafruit Bluefruit nRF52 Libraries | 0.21.0 | BLE UART 與藍牙連線 |
| Adafruit TinyUSB Library | 3.6.0 | USB HID 鍵盤 |
| GNU Arm Embedded GCC | 9-2019q4 | 韌體編譯工具 |
| CMSIS | 5.7.0 | ARM 支援檔案 |

表格反映保存的本機版本，不保證未來重新下載或更新後仍相同。若 Arduino 顯示找到多個 `Adafruit_TinyUSB.h` 程式庫，請查看編譯輸出，確認實際使用開發板套件內的版本，避免額外安裝的版本覆蓋它。

## 選擇開發板與連接埠

將板子以 USB 接上電腦，設定如下：

| Arduino 選項 | 本專案配置 |
| --- | --- |
| 開發板套件 | Adafruit nRF52，1.7.0 |
| 開發板 | **Adafruit Feather nRF52840 Express** |
| SoftDevice | **S140 6.1.1** |
| 連接埠 | 選擇這塊板子實際出現的埠 |

使用者截圖中的埠是 `COM4`；其他電腦或重新進入 Bootloader 後可能不同，不能固定照填 COM4。Windows 通常顯示 COM 編號，macOS / Linux 則顯示對應的裝置路徑。

本機保存的套件識別名稱是 `Adafruit_Package`，官方新安裝通常是 `adafruit`。這是套件目錄／FQBN 的差異，Arduino 選單中的板型名稱仍應選上述 Feather nRF52840 Express。

## Arduino 編譯與上傳教學

1. 開啟 `firmware/WebTyper/WebTyper.ino`。請保留 `WebTyper` 資料夾與 `.ino` 檔案相同的名稱。
2. 依上表選擇開發板及實際連接埠。
3. 點左上角「驗證」勾號，等待編譯完成。驗證只會編譯，不會把韌體寫入板子。
4. 確認板子已接好、Serial Monitor 等程式沒有佔用連接埠。
5. 點左上角向右箭頭「上傳」，等待 IDE 顯示完成。
6. 上傳後板子重新啟動，即可由 WebTyper 網頁搜尋 **WebTyper_003** 並連線。
7. 在目標電腦開啟記事本等文字編輯器，切到英文輸入模式並放好游標。先以 `Hello WebTyper!` 做短文字測試，再傳送較長內容。

使用一般「上傳」即可，不需要使用「使用燒錄器上傳」或「燒錄 Bootloader」。Arduino 官方也提供[草稿上傳說明](https://support.arduino.cc/hc/en-us/articles/4733418441116-Upload-a-sketch-in-Arduino-IDE)。

### 找不到連接埠或上傳失敗

- 確認 USB 線／轉接器支援資料傳輸，換一個 USB 埠後重新選擇連接埠。
- 關閉其他佔用串口的程式，確認選到 nRF52840 而非其他板子。
- 若板子已使用支援雙擊重置的 Adafruit 相容 nRF52840 Bootloader，可快速按兩次 RESET 進入 Bootloader；選擇重新出現的連接埠後再次上傳。RESET 的位置與操作方式以這塊板子的硬體說明為準，不要隨意短接未確認的接點。
- 若板子沒有相容 Bootloader，安裝 Arduino 套件並不會自動補上；需要先確認實際板型、Bootloader 與燒錄方式，不能把 Feather 的 Bootloader 直接當成通用檔案。

雙擊重置的適用條件見 [Adafruit nRF52 Bootloader 文件](https://github.com/adafruit/Adafruit_nRF52_Bootloader)。

## 開啟 WebTyper

![WebTyper 網頁介面](https://raw.githubusercontent.com/Mixcyanx/WebTyper/refs/heads/main/ScreenShots/Galaxy-S22%2B-mixcyanx.github.io.png)

將 `index.html` 放到 HTTPS 網站，從支援 Web Bluetooth 的瀏覽器開啟。手機端連接開發板藍牙，開發板的 USB 則接到要輸入文字的電腦。

首屏可以直接連接裝置並貼上文字；右上角漢堡選單可調整輸入速度及輸入法切換快捷鍵。請先確認目標輸入框的游標與鍵盤配置，再按「開始傳送」。

備份保留各元件原有授權文件。保存或轉交整份套件時應一併保留這些文件；大型離線壓縮檔適合另存硬碟、雲端備份或專案 Release 附件，不宜直接加入一般 Git 原始碼紀錄。

## 參考來源

- [Arduino IDE 官方下載](https://www.arduino.cc/en/software/)
- [Adafruit 開發板套件安裝](https://learn.adafruit.com/bluefruit-nrf52-feather-learning-guide/arduino-bsp-setup)
- [Adafruit nRF52 核心原始碼與發行版](https://github.com/adafruit/Adafruit_nRF52_Arduino/releases)
- [Arduino IDE 上傳教學](https://support.arduino.cc/hc/en-us/articles/4733418441116-Upload-a-sketch-in-Arduino-IDE)
