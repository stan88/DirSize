DirSize shows the size of every folder on Windows: a window with a folder tree, a panel beside File Explorer, and "Open with DirSize" in the context menu.
DirSize 顯示 Windows 上每個資料夾的大小：有資料夾樹的獨立視窗、檔案總管旁的面板，以及右鍵選單的「用 DirSize 開啟」。

**New in 0.2.0** / **0.2.0 新增**

- Size unit: automatic, KB or GB ("⋯" menu, the panel's right-click menu, the installer). Number columns widen so no digit is cut off.
  顯示單位：自動、KB 或 GB（「⋯」選單、面板右鍵選單、安裝程式）。數字欄會自動加寬，不會切掉位數。
- The File Explorer panel lists files too.
  檔案總管面板也會列出檔案。
- Folders are listed before files; the list starts sorted by name.
  資料夾排在檔案前面；清單預設依名稱排序。
- Windows' own icons for folders, file types and drives.
  資料夾、檔案類型和磁碟機改用 Windows 的系統圖示。

**Download** / **下載**

- `DirSize-<version>-setup.exe`: the installer (the window, the File Explorer panel, the context menu and the indexing service). Administrator rights are needed.
  安裝程式（獨立視窗、檔案總管面板、右鍵選單和索引服務），需要系統管理員權限。
- `DirSize-<version>-portable.zip`: `dirsize.exe` only, no installation; without the service it scans folders itself, which is slower.
  只有 `dirsize.exe`，不用安裝；沒有服務時會自己逐目錄掃描，速度比較慢。

Windows 10 version 1809 or later, x64. / Windows 10 1809 以上，x64。

The files are **not code-signed** yet, so Windows shows "Unknown publisher" and SmartScreen may warn.
檔案**還沒有數位簽章**，所以 Windows 會顯示「未知的發行者」，SmartScreen 也可能跳出警告。

Built by GitHub Actions from the tagged source. / 由 GitHub Actions 從這個 tag 的原始碼建置。

[Code signing policy](https://github.com/stan88/DirSize#code-signing-policy) · [Privacy](https://github.com/stan88/DirSize#privacy) / [程式碼簽章政策與隱私](https://github.com/stan88/DirSize/blob/main/README.zh-TW.md)
