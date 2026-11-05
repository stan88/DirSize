# DirSize — 專案規則

## 獨立實作（最重要）

DirSize 是**獨立實作**，以 MIT 授權開源。

- **不可閱讀、不可複製**舊版 *Folder Size for Windows*（SourceForge，GPLv2）的原始碼。
  - 包括 SourceForge 的程式碼瀏覽頁、CVS 庫，以及任何鏡像或副本。本機的舊 CVS 庫已經刪除。
- 也不要參考授權不明或屬於 GPL 的第三方 NTFS／MFT 解析程式碼，例如 ntfs-3g 的衍生作品，或 GitHub 上來源不明的標頭檔。
- 實作只參考公開文件，例如 Microsoft Learn、WDK 文件和 C++ 標準文件。
  - Microsoft 官方發布的 Windows SDK／WDK 標頭檔（例如 `winioctl.h`、`ntifs.h`）也可以參考，用於查 Learn 沒有說明頁的結構或常數定義。在 DEVLOG 記下標頭檔名稱與 SDK 版本。
- 每用到一份文件，就把連結記到 `docs/DEVLOG.md` 的「參考資料」，**邊做邊記**，不要最後才補。
- 引用第三方程式碼前，先確認授權與 MIT 相容，並記在 DEVLOG。

## 隱私（開源 repo）

- repo 內的文件與程式碼**不要放開發者的私人資訊**：本機資料夾名稱與路徑（例如 `D:\某專案`）、使用者名稱、硬體型號、已安裝的軟體、電子郵件。
- 量測結果寫進 DEVLOG 時，個人資料夾一律用代號或只寫數字。Windows、Program Files、Users 這類系統資料夾名稱可以保留。
- 原始量測輸出放 `bench-results/`（已排除在 git 外），不要貼進文件。

## 技術設定

- C++20、Win32，使用 MSVC（VS 2022）。
- 目標系統 Windows 10 1809 以上（第 3 階段決定，增量更新需要 1809 起才有的功能）。`_WIN32_WINNT=0x0A00` 不變：SDK 沒有區分 1809 的值，新功能在執行時檢查。
- 靜態 CRT（`/MT`），使用者不需要安裝 VC++ Redistributable。

## 建置（Git Bash）

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -S . -B build -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build build --config Release
```

## Git

- commit 前要先經過使用者同意。
- commit 訊息不加 AI 署名 trailer。
