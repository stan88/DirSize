DirSize shows the size of every folder on Windows: a window with a folder tree, a panel beside File Explorer, and "Open with DirSize" in the context menu.
DirSize 顯示 Windows 上每個資料夾的大小：有資料夾樹的獨立視窗、檔案總管旁的面板，以及右鍵選單的「用 DirSize 開啟」。

**New in 0.3.0** / **0.3.0 新增**

- "Check for updates…" in the "⋯" menu. It connects only when you choose it: one request to GitHub for the latest version number (see [Privacy](https://github.com/stan88/DirSize#privacy)).
  「⋯」選單新增「檢查更新…」。只有你選它的時候才會連網：向 GitHub 查詢一次最新的版本號（見[隱私](https://github.com/stan88/DirSize/blob/main/README.zh-TW.md#隱私)）。

**Fixed in 0.2.1** / **0.2.1 修正**

- On a drive other than the system drive, every sub-folder could be marked "no access" after the drive was dismounted and mounted again (for example after sleep), although you could open them. Sizes were still correct.
  非系統磁碟在卸載又重新掛載後（例如睡眠喚醒後），子資料夾可能全部被標成「沒有權限」，但其實打得開。大小仍然正確。

**Download** / **下載**

- `DirSize-<version>-setup.exe`: the installer (the window, the File Explorer panel, the context menu and the indexing service). Administrator rights are needed.
  安裝程式（獨立視窗、檔案總管面板、右鍵選單和索引服務），需要系統管理員權限。
- `DirSize-<version>-portable.zip`: `dirsize.exe` only, no installation; without the service it scans folders itself, which is slower.
  只有 `dirsize.exe`，不用安裝；沒有服務時會自己逐目錄掃描，速度比較慢。

Windows 10 version 1809 or later, x64. / Windows 10 1809 以上，x64。

The files are **not code-signed** yet, so Windows shows "Unknown publisher" and SmartScreen may warn.
檔案**還沒有數位簽章**，所以 Windows 會顯示「未知的發行者」，SmartScreen 也可能跳出警告。

Built by GitHub Actions from the tagged source. / 由 GitHub Actions 從這個 tag 的原始碼建置。

**Checking a download** / **檢查下載的檔案**

- `SHA256SUMS.txt` lists the SHA-256 of both files. In PowerShell: `Get-FileHash .\DirSize-<version>-setup.exe`
  `SHA256SUMS.txt` 列出兩個檔案的 SHA-256。在 PowerShell 執行：`Get-FileHash .\DirSize-<version>-setup.exe`
- Each file has a build provenance attestation showing it was built by this repository's workflow. With the [GitHub CLI](https://cli.github.com/): `gh attestation verify DirSize-<version>-setup.exe -R stan88/DirSize`
  每個檔案都有建置來源證明，證明它由這個 repo 的 workflow 建置。用 [GitHub CLI](https://cli.github.com/) 驗證：`gh attestation verify DirSize-<version>-setup.exe -R stan88/DirSize`

[Code signing policy](https://github.com/stan88/DirSize#code-signing-policy) · [Privacy](https://github.com/stan88/DirSize#privacy) / [程式碼簽章政策與隱私](https://github.com/stan88/DirSize/blob/main/README.zh-TW.md)
