# DirSize

[English](README.md) | **繁體中文**

Windows 上快速、低資源的資料夾大小工具，顯示每個資料夾的大小。

> 狀態：早期開發中，目前還沒有可用的版本。

## 目標

- **快**：NTFS 磁碟區從主檔案表（MFT）建立索引，再用 USN 變更日誌保持同步，
  不必逐一走訪每個目錄。
- **省資源**：記憶體中的資料夾樹採緊湊結構，背景（低優先權）I/O，
  並把快取存到磁碟，重開機後不必整個重新掃描。
- **各種磁碟都能用**：其他檔案系統（exFAT、FAT32、卸除式磁碟、網路磁碟）
  改用逐目錄列舉加上變更通知。要掃描哪些類型的磁碟可以自行設定。

## 規劃中的元件

| 元件 | 用途 |
|---|---|
| 索引核心 | 磁碟區掃描、資料夾樹、變更追蹤、快取存檔 |
| 服務 | 在背景執行索引核心，透過 named pipe 回應查詢 |
| CLI | 命令列查詢與效能測試 |
| 前端 | 在檔案總管旁顯示資料夾大小 |

## 建置

需要 Visual Studio 2022，並安裝「**使用 C++ 的桌面開發**」工作負載
（MSVC、Windows SDK、CMake）。第一個元件完成後會補上建置說明。

## 來源聲明

DirSize 是依據 Microsoft 公開文件撰寫的獨立實作。
構想來自已停止維護的 *Folder Size for Windows*（SourceForge），
但不含它的任何程式碼。詳見 [docs/DEVLOG.md](docs/DEVLOG.md)。

## 參與開發

乾淨實作與隱私規則請見 [CONTRIBUTING.zh-TW.md](CONTRIBUTING.zh-TW.md)。
想翻譯介面，請見 [lang/README.md](lang/README.md)。

## 授權

[MIT](LICENSE)
