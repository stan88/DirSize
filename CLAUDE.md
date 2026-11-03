# DirSize — 專案規則

## 獨立實作（最重要）

DirSize 是**獨立實作**，以 MIT 授權開源。

- **不可閱讀、不可複製**舊版 *Folder Size for Windows*（SourceForge，GPLv2）的原始碼。
  - 包括 SourceForge 的程式碼瀏覽頁、CVS 庫，以及任何鏡像或副本。本機的舊 CVS 庫已經刪除。
- 也不要參考授權不明或屬於 GPL 的第三方 NTFS／MFT 解析程式碼，例如 ntfs-3g 的衍生作品，或 GitHub 上來源不明的標頭檔。
- 實作只參考公開文件，例如 Microsoft Learn、WDK 文件和 C++ 標準文件。
- 每用到一份文件，就把連結記到 `docs/DEVLOG.md` 的「參考資料」，**邊做邊記**，不要最後才補。
- 引用第三方程式碼前，先確認授權與 MIT 相容，並記在 DEVLOG。

## 技術設定

- C++20、Win32，使用 MSVC（VS 2022）。
- 目標系統 Windows 10 以上：`_WIN32_WINNT=0x0A00`。
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
