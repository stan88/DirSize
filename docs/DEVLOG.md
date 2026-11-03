# 開發紀錄

本專案是**獨立實作**。這份紀錄寫下每個元件參考了哪些資料，用來證明程式碼並非取自其他專案。

## 原則

- **不閱讀、不複製**舊版 *Folder Size for Windows*（SourceForge，brio1337，GPLv2）的原始碼。
- 實作時只參考公開文件：Microsoft Learn（Win32 API、檔案系統控制碼）、C++ 標準文件，以及一般演算法知識。
- 每個元件完成時，要在下方「參考資料」列出用過的文件連結。
- 引用第三方程式碼前，必須先確認授權與 MIT 相容，並記錄在這裡。

## 決策紀錄

### 2026-11-03 專案成立

- **授權**：MIT。
- **名稱**：DirSize。在套件庫（Chocolatey/winget）上可能需要改用 `dirsize-win` 之類的套件名稱，因為已有同名的命令列工具。
- **技術**：原生 C++20／Win32，使用 Visual Studio 2022。
- **磁碟支援**：所有格式。
  - NTFS：用 `FSCTL_ENUM_USN_DATA` 列舉 MFT 建立索引，用 USN Journal 增量更新。
  - 其他格式：逐目錄列舉，搭配 `ReadDirectoryChangesW` 監看。
  - 可以設定要掃描哪些磁碟類型，預設不掃網路磁碟和光碟。
- **隔離**：舊版 CVS 庫留在 `D:\Project\foldersize`，和本專案分開。
  - 開發前看過舊版架構的人，只會使用公開層級的概念：背景服務加快取、named pipe 查詢、追蹤 Explorer 視窗。
  - 這些概念都不受著作權保護。

## 開發階段

1. 量測基準：做一個簡易的逐目錄掃描器，取得時間與記憶體數據。
2. 掃描核心：MFT 列舉加上緊湊的資料夾樹。
3. USN Journal 增量更新，以及快取存檔。
4. Windows 服務與 named pipe 查詢。
5. 前端。
6. 多國語言與安裝程式。

### 2026-11-03 第 1 階段：基準掃描器

- 新增 `src/core/dir_walker.*`：逐目錄掃描器，所有檔案系統都適用。之後也會當作非 NTFS 磁碟的掃描方式。
  - **列目錄的方式**：用 `CreateFileW(FILE_LIST_DIRECTORY, FILE_FLAG_BACKUP_SEMANTICS)` 開啟目錄，再呼叫 `GetFileInformationByHandleEx(FileFullDirectoryInfo)`，每次讀 64 KB 緩衝區。
  - **磁碟上大小**：直接取用回傳資料中的 `AllocationSize`，不用逐檔另外查詢。
  - **走訪方式**：用明確的工作堆疊取代遞迴，目錄再深也不會讓呼叫堆疊溢位。
  - **連結處理**：遇到目錄類的 reparse point（junction、symlink）不追進去，避免重複計算。硬連結的檔案會依連結數各算一次。
  - **長路徑**：一律轉成 `\\?\` 延伸路徑，不受 MAX_PATH（260 字元）限制。
- 新增 `src/core/path_util.*`：路徑轉換與串接。
- 新增 `src/cli/main.cpp`：`dirsize-cli scan <path> [--top N] [--json]`。
  - 輸出時間、檔案數、資料夾數、邏輯大小、磁碟上大小、存取被拒數、峰值記憶體。
  - 以 UTF-8 輸出。
- **驗證**：
  - `D:\Project\MediTrack`：檔案 601、位元組 187,570,673、資料夾 197，與 PowerShell `Get-ChildItem -Recurse -Force` 的結果完全一致。
  - 邊界測試：中文名稱、756 字元深層路徑、junction 略過，三項都正確。

#### 基準數據（2026-11-03，未提權執行，Release 版）

測試環境：
- C:、D: 是 NVMe SSD（Samsung 990 EVO Plus／980），NTFS。
- P: 是卸除式 exFAT 磁碟。

| 磁碟 | 項目數（檔案＋資料夾） | 第 1 次 | 第 2 次（快取已熱） | 峰值記憶體 | 存取被拒 |
|---|---|---|---|---|---|
| C:（NTFS，442 GB 已用） | 約 117 萬 | 45.2 s | 12.3 s | 12.3 MB | 588 個資料夾 |
| D:（NTFS，736 GB 已用） | 約 94 萬 | 7.0 s | 4.5 s | 5.6 MB | 3 個資料夾 |
| P:（exFAT，卸除式） | 約 21 萬 | 13.0 s | 11.1 s | 5.4 MB | 0 |

注意事項：
- 「第 1 次」不是真正的冷啟動（沒有重開機），只代表當時系統快取裡的資料不多。
- 未提權執行，所以 C: 有 588 個資料夾讀不到，總量會偏低。
- C:\Windows 的檔案數在測試期間持續增加：第 1 次 255,884，第 2 次 275,048，之後重測 366,108，資料夾數都維持在 68,745 左右。
  - 重測顯示，增加的部分集中在 `C:\Windows\SoftwareDistribution`（186,172 個檔案）。這是 Windows Update 的下載目錄，代表當時 Windows Update 正在下載。
  - 所以數字變動是磁碟內容真的在變，不是掃描器漏算。
- `Program Files (x86)` 的磁碟上大小比邏輯大小還小，因為裡面有壓縮檔案。這證明 `AllocationSize` 的處理是正確的。
- 峰值記憶體偏低，是因為基準掃描器只保留第一層子資料夾的合計，沒有建立完整的資料夾樹。第 2 階段建完整樹之後，記憶體才是真正要比較的指標。
- P: 第 2 次只快了約 15%（13.0 秒 → 11.1 秒）。**推測**是卸除式裝置本身速度慢、系統快取的效果有限，但還沒驗證。不論原因為何，這類磁碟都最需要「快取存檔＋增量更新」。

### 2026-11-03 第 2 階段前提修正

- 查證 [USN_RECORD_V2](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2)：裡面有檔案參照號碼、父目錄參照號碼、屬性、名稱，**但沒有檔案大小**。
  - 因此**只靠 `FSCTL_ENUM_USN_DATA` 列舉 MFT，算不出資料夾大小**。原本的第 2 階段規劃要修正。
- 下一步要查證的候選做法：
  1. `FSCTL_QUERY_FILE_LAYOUT`：Windows 8 以上，定義在 WDK `ntifs.h`。依文件說明，它可以一次列出所有檔案，加上旗標還能附帶串流（stream）資訊。參考：[QUERY_FILE_LAYOUT_OUTPUT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_query_file_layout_output)。
     - 待確認：串流資訊裡有沒有大小欄位、需要什麼權限、實際速度如何。
  2. 直接讀取並解析 `$MFT`：只能用微軟公開文件記載的結構。工作量最大。
  3. 混合做法：用 USN 列舉建立樹狀結構，大小仍然逐目錄讀取。效益待評估。
- MFT 類的做法都需要系統管理員權限。為了公平比較，基準也要用提權方式重測，並啟用 `SeBackupPrivilege`，否則提權後仍會有讀不到的資料夾。
- 硬連結的計算規則還沒決定，目前每個連結各算一次。決定後，第 1 階段和第 2 階段兩種掃描方式要一致。

### 2026-11-03 提權基準：啟用 SeBackupPrivilege

- `src/cli/main.cpp`：啟動時用 `OpenProcessToken` → `LookupPrivilegeValueW(SE_BACKUP_NAME)` → `AdjustTokenPrivileges` 啟用 `SeBackupPrivilege`。
  - `AdjustTokenPrivileges` 在權限不存在時仍回傳成功，要再檢查 `GetLastError()` 是否為 `ERROR_NOT_ALL_ASSIGNED`。
  - 文字輸出多一行「Backup privilege」，JSON 多一個 `backupPrivilege` 欄位。
- 新增 `scripts/bench.ps1`：對整個磁碟連跑數次，JSON 結果存到 `bench-results/`（已加入 `.gitignore`）。

#### 基準數據（2026-11-03，提權執行，Release 版）

測試環境同上一節。三個磁碟的 `backupPrivilege` 都是 `true`。

| 磁碟 | 項目數（檔案＋資料夾） | 第 1 次 | 第 2 次 | 峰值記憶體 | 存取被拒 |
|---|---|---|---|---|---|
| C:（NTFS） | 約 148 萬（1,284,605＋199,544） | 15.0 s | 12.7 s | 13.4 MB | 0 |
| D:（NTFS） | 約 94 萬（823,934＋114,946） | 4.8 s | 4.1 s | 6.3 MB | 0 |
| P:（exFAT，卸除式） | 約 21 萬（178,520＋29,771） | 11.8 s | 11.1 s | 6.3 MB | 0 |

與未提權基準比較：
- 存取被拒的資料夾全部歸零（C: 588 → 0，D: 3 → 0）。
- C: 項目數從約 117 萬增加到約 148 萬。增加的部分有兩個來源：原本讀不到的資料夾，以及前述 Windows Update 下載造成的檔案增加。這次沒有分開量，**無法拆分**各占多少。
- C: 第 1 次從 45.2 s 變成 15.0 s。兩次測試前的快取狀態不同（都不是冷啟動），所以**不能當成提權讓掃描變快的證據**。
- 快取已熱時，C: 項目數多了約 27%，時間只從 12.3 s 變成 12.7 s。
- `System Volume Information` 在 C: 算到 17.5 GB。提權並啟用備份權限後才讀得到。是否要計入總量，留到前端設計時決定。
- 這組數據是第 2 階段 MFT 類做法的比較基準（同樣需要系統管理員權限）。

## 參考資料

| 元件 | 文件 |
|---|---|
| dir_walker | [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex) |
| dir_walker | [FILE_FULL_DIR_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_full_dir_info) |
| dir_walker | [FILE_INFO_BY_HANDLE_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class) |
| dir_walker | [CreateFileW（開啟目錄需要 FILE_FLAG_BACKUP_SEMANTICS）](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) |
| dir_walker | [Reparse Points](https://learn.microsoft.com/en-us/windows/win32/fileio/reparse-points) |
| dir_walker | [File Attribute Constants](https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants) |
| path_util | [Naming Files, Paths, and Namespaces（`\\?\` 延伸路徑）](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file) |
| path_util | [GetFullPathNameW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew) |
| cli | [GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo) |
| cli | [GetTokenInformation / TOKEN_ELEVATION](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation) |
| cli | [WideCharToMultiByte](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-widechartomultibyte) |
| cli | [AdjustTokenPrivileges](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges) |
| cli | [LookupPrivilegeValueW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lookupprivilegevaluew) |
| cli | [OpenProcessToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken) |
| cli | [Privilege Constants（SE_BACKUP_NAME）](https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants) |
| 建置 | [CMAKE_MSVC_RUNTIME_LIBRARY](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html) |
