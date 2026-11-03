# 參與 DirSize 開發

[English](CONTRIBUTING.md) | **繁體中文**

感謝你的參與。DirSize 是以 MIT 授權開源的獨立實作，以下規則是為了維持這一點。

## 乾淨實作規則

- **不可閱讀、不可複製**已停止維護的 *Folder Size for Windows*（SourceForge，GPLv2）的原始碼，
  包括 SourceForge 的程式碼瀏覽頁、CVS 庫，以及任何鏡像或副本。
- 不可使用 GPL 授權或來源不明的 NTFS／MFT 解析程式碼，例如 ntfs-3g 的衍生作品，
  或網路上來源不明的標頭檔。
- 實作只參考公開文件：Microsoft Learn、WDK 文件、Microsoft 官方發布的 Windows SDK／WDK 標頭檔，
  以及 C++ 標準。
- 每用到一份文件，就記到 [docs/DEVLOG.md](docs/DEVLOG.md) 的「參考資料」表格，邊做邊記，不要最後才補。
- 引用第三方程式碼前，先確認授權與 MIT 相容，並記在 DEVLOG。

## 隱私

不要把私人資訊放進 repo：本機資料夾名稱或路徑、使用者名稱、硬體型號、已安裝的軟體、電子郵件。
記錄量測結果時，個人資料夾請用代號或只寫數字。原始量測輸出放在 `bench-results/`，該目錄已排除在 git 之外。

## 建置

需要 Visual Studio 2022，並安裝「**使用 C++ 的桌面開發**」工作負載。

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 程式碼

- C++20、Win32、MSVC。目標系統 Windows 10 以上（`_WIN32_WINNT=0x0A00`），靜態 CRT（`/MT`）。
- 解析 Windows 回傳的原始緩衝區時，一定要做邊界檢查。
