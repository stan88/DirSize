# DirSize

[English](README.md) | **繁體中文**

Windows 上快速、低資源的資料夾大小工具，顯示每個資料夾的大小。

> 狀態：0.1.0，尚未發行。安裝程式已經可以建置和安裝，但還沒有發布正式版本，
> 需要自行建置（見下方）。

## 功能

- **獨立視窗**：左邊是資料夾樹；右邊列出選取資料夾的子資料夾和檔案，
  包含大小、磁碟用量、佔總量的比例和檔案數。可以依任一欄排序，也可以調整欄寬。
- **檔案總管面板**：在檔案總管選取一個資料夾，旁邊的窄面板就會列出它各個子資料夾的大小。
- **右鍵選單**：資料夾和磁碟機上的「用 DirSize 開啟」。
- 跟著 Windows 的佈景主題（淺色、深色、高對比）和顯示語言
  （英文、繁體中文；可以新增其他語言，見 [lang/README.md](lang/README.md)）。

## 運作方式

- **服務**（`dirsize-service.exe`，以 LocalSystem 執行）：從主檔案表（MFT）
  為固定式 NTFS 磁碟區建立索引，再用 USN 變更日誌保持同步，
  不必逐一走訪資料夾就能提供大小。索引存在 `C:\ProgramData\DirSize`，
  重新開機後不必整個重新掃描。服務以低優先權執行，並限制 CPU 用量。
- **前端**（`dirsize.exe`，獨立視窗和面板）：向服務查詢。服務沒有索引的地方
  （其他檔案系統、網路磁碟，或沒有安裝服務），就自己逐目錄掃描，速度比較慢。
- **CLI**（`dirsize-cli.exe`）：查詢和效能量測；不帶參數執行就會列出指令。

## 系統需求

Windows 10 1809 以上，x64。

## 安裝

執行 `DirSize-<版本>-setup.exe`（安裝服務需要系統管理員權限）。
安裝程式提供下列選項，預設全部勾選：

- 在檔案總管的右鍵選單加入「用 DirSize 開啟」（所有使用者）
- 登入時自動啟動檔案總管面板
- 顯示比例欄

之後可以在程式的「⋯」選單修改（比例欄也可以在面板的右鍵選單切換）。

安裝程式**還沒有數位簽章**，所以 Windows 會顯示「未知的發行者」，
從網路下載時 SmartScreen 也可能跳出警告。

解除安裝會移除服務、程式檔案、右鍵選單和索引快取，以及執行解除安裝的
那個帳號的登入自動啟動設定。其他個人設定（`HKEY_CURRENT_USER\Software\DirSize`）會保留。

另外也會產生只含 `dirsize.exe` 的免安裝 zip；沒有服務時，它一律自己逐目錄掃描。

## 建置

需要 Visual Studio 2022，並安裝「**使用 C++ 的桌面開發**」工作負載
（MSVC、Windows SDK、CMake）。在 Git Bash 執行：

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" -S . -B build -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build build --config Release
```

測試：`ctest --test-dir build -C Release`（`ctest.exe` 和 `cmake.exe` 在同一個資料夾）。

安裝程式：先安裝 [Inno Setup](https://jrsoftware.org/isinfo.php) 6.5 以上，
再用 PowerShell 執行 `scripts\package-installer.ps1`。它會建置 Release，
並把安裝程式和免安裝 zip 輸出到 `dist\`。

每次 push 和 PR，GitHub Actions（`.github/workflows/build.yml`）都會做同樣的事。
發行新版：改 `CMakeLists.txt` 的 `project(VERSION)`（版本號只在這裡設定），
commit 後推 `v<版本>` tag。tag 和 `CMakeLists.txt` 不一致時建置會停止；
一致就建立附上兩個檔案的草稿 Release，再手動發布。

## 來源聲明

DirSize 是依據 Microsoft 公開文件撰寫的獨立實作。
構想來自已停止維護的 *Folder Size for Windows*（SourceForge），
但不含它的任何程式碼。詳見 [docs/DEVLOG.md](docs/DEVLOG.md)。

安裝程式的繁體中文訊息是 Inno Setup 的官方翻譯，原封不動收錄，
詳見 [installer/lang/README.md](installer/lang/README.md)。

## 程式碼簽章政策（Code signing policy）

正在申請 SignPath Foundation 給開源專案的免費簽章；0.1.0 沒有簽章。
通過後，發行的檔案會這樣簽署：Free code signing provided by
[SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org)
（由 SignPath.io 免費提供簽章，憑證屬於 SignPath Foundation）。

- 只簽由 GitHub Actions 從這個 repo 的原始碼建置的檔案；每次發行都要先手動核准才會簽章。
- 提交與審查（Committers and reviewers）：[stan88](https://github.com/stan88)
- 核准（Approvers）：[stan88](https://github.com/stan88)

## 隱私

This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or operating it.
（除非使用者或安裝、操作的人明確要求，本程式不會把任何資訊傳到其他連網的系統。）
DirSize 沒有任何網路程式碼；服務只回應同一台電腦上的程式（它的具名管道會拒絕遠端連線）。

## 參與開發

乾淨實作與隱私規則請見 [CONTRIBUTING.zh-TW.md](CONTRIBUTING.zh-TW.md)。
想翻譯介面，請見 [lang/README.md](lang/README.md)。

## 授權

[MIT](LICENSE)
