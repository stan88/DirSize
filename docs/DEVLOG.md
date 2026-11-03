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
- **隔離**：舊版 CVS 庫放在本機另一個獨立目錄，和本專案分開（之後已刪除）。
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
  - 一個測試用資料夾：檔案 601、位元組 187,570,673、資料夾 197，與 PowerShell `Get-ChildItem -Recurse -Force` 的結果完全一致。
  - 邊界測試：中文名稱、756 字元深層路徑、junction 略過，三項都正確。

#### 基準數據（2026-11-03，未提權執行，Release 版）

測試環境：
- C:、D: 是 NVMe SSD，NTFS。
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

### 2026-11-03 第 2 階段查證：FSCTL_QUERY_FILE_LAYOUT（文件階段）

文件有寫的：
- 對**磁碟區 handle** 發出請求。要列出整個磁碟區，就重複呼叫直到回傳 `STATUS_END_OF_FILE`。第一次要帶 `QUERY_FILE_LAYOUT_RESTART`；收到 `STATUS_END_OF_FILE` 之後，要再列一次也要帶它。
- **ReFS 不支援**。最低版本：控制碼頁面寫 Windows 8.1 Update，結構頁面寫 Windows 8，兩頁不一致。我們的目標是 Windows 10，所以都沒問題。
- 輸入旗標：`INCLUDE_NAMES`、`INCLUDE_STREAMS`、`INCLUDE_EXTRA_INFO`、`INCLUDE_EXTENTS`（需同時設 STREAMS）、`INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED`（包含常駐串流與未配置的屬性，需同時設 STREAMS）。
  - 小檔案的資料常駐在 MFT 記錄裡，不占叢集。要算這類檔案，**很可能需要設最後這個旗標**。這點要等實測確認。
- 輸出標頭 `QUERY_FILE_LAYOUT_OUTPUT` 後面接一串 `FILE_LAYOUT_ENTRY`。名稱、串流、額外資訊、extent 等子項目用 offset 串起來，offset 為 0 表示結束。
- NTFS 一律設 `QUERY_FILE_LAYOUT_SINGLE_INSTANCED`：**每個檔案只回一個 `FILE_LAYOUT_ENTRY`**，每個串流只回一個 `STREAM_LAYOUT_ENTRY`。對硬連結「只算一次」的規則有利。
- 錯誤碼有 `STATUS_ACCESS_DENIED`（「呼叫者無法存取該磁碟區」）。

文件**沒寫**、仍待確認的：
- **`FILE_LAYOUT_ENTRY`、`FILE_LAYOUT_NAME_ENTRY`、`FILE_LAYOUT_INFO_ENTRY`、`STREAM_LAYOUT_ENTRY`、`STREAM_EXTENT_ENTRY` 在 Microsoft Learn 上找不到參考頁面**。`ntifs.h` 索引頁只列出 INPUT／OUTPUT 兩個結構。
  - 所以三個關鍵問題，文件都還答不出來：串流項目有沒有大小欄位、名稱項目有沒有父目錄參照、各欄位的確切配置。
- 需要哪種權限和開啟方式：文件只寫「磁碟區 handle」，沒寫存取權限或特權。
- 文件只描述 kernel 端的 `ZwFsControlFile`／`FltFsControlFile` 呼叫方式，沒提到使用者模式的 `DeviceIoControl`。

### 2026-11-03 第 2 階段查證：SDK 標頭檔

使用者同意參考 Microsoft 官方 SDK／WDK 標頭檔（規則已補進 `CLAUDE.md`）。來源：Windows SDK **10.0.26100.0** 的 `um/winioctl.h`，只讀了 `FSCTL_QUERY_FILE_LAYOUT` 這一段的定義。

- `FSCTL_QUERY_FILE_LAYOUT` 定義在**使用者模式**的 `winioctl.h`，存取需求是 `FILE_ANY_ACCESS`。所以應該可以用 `DeviceIoControl` 呼叫，實測確認。
- **檔案大小有**：`STREAM_LAYOUT_ENTRY`（版本 2）有 `AllocationSize`（配置大小）和 `EndOfFile`（邏輯大小），另有 `AttributeTypeCode` 和 `StreamIdentifier`。旗標 `STREAM_LAYOUT_ENTRY_RESIDENT` 表示常駐串流。
- **父目錄有**：`FILE_LAYOUT_NAME_ENTRY` 有 `ParentFileReferenceNumber`、檔名（不以 null 結尾），旗標分成 `PRIMARY` 和 `DOS`（8.3 短檔名）。每個硬連結各有一筆名稱項目。
- `FILE_LAYOUT_ENTRY` 有 `FileReferenceNumber`、`FileAttributes`（可以判斷目錄和 reparse point）、名稱與串流的 offset。
- 標頭檔對 `INCLUDE_STREAMS_WITH_NO_CLUSTERS_ALLOCATED` 的註解：涵蓋常駐屬性、長度 0 的非常駐屬性、只有稀疏空洞的壓縮／稀疏屬性；只能用在整個磁碟區的查詢。**要算到小檔案，就必須設這個旗標。**
- 待實測：
  - `AttributeTypeCode` 的值：SDK 沒有定義各屬性代碼，WDK 也沒安裝。所以先在實測中列出實際出現的代碼和識別字，對照第 1 階段掃描器的結果，判斷哪一種是檔案資料串流。
  - 需要什麼權限。
  - 速度與記憶體。

### 2026-11-03 第 2 階段實測程式

- 新增 `tools/layout_probe.cpp`（CMake target `layout-probe`）：只用來量測，不屬於核心，也不會出貨。
  - 列出整個磁碟區，印出串流統計（代碼、是否常駐、識別字），以及每個第一層資料夾的大小合計。
  - 合計同時用兩種規則計算：每個連結各算一次（第 1 階段規則）、每個檔案只算一次。
  - 根目錄 ID 由 `GetFileInformationByHandle` 取得，不寫死。8.3 短檔名（旗標只有 `DOS`）不計，避免重複。
- 新增 `scripts/layout-probe.ps1`：對 C:、D: 各跑兩次，輸出存到 `bench-results/`。
- **未提權試跑（D:）**：
  - 用 `GENERIC_READ` 開磁碟區，錯誤 5（存取被拒）。
  - 用 `FILE_READ_ATTRIBUTES` 開得了，但 `DeviceIoControl(FSCTL_QUERY_FILE_LAYOUT)` 回傳錯誤 1（`ERROR_INVALID_FUNCTION`）。
  - 所以**一般權限不能用**，至少要提權。

#### 提權實測（2026-11-03，C:、D: 各兩次）

第一次提權執行：NTFS 回傳的 `STREAM_LAYOUT_ENTRY` 是**第 1 版**，但 SDK 只定義第 2 版。程式遇到版本不符就停下，所以沒有結果。

改成用有文件記載的 API 驗證：
- 用 `OpenFileById` 開啟抽樣檔案，再用 `GetFileInformationByHandleEx(FileStreamInfo)` 讀出每個串流的名稱、大小、配置大小，當作標準答案。
- 第 1 版的項目先套用第 2 版的欄位位置解讀，再拿來比對。

**欄位配置驗證結果：**
- 抽樣 D: 423 個、C: 523 個（含 C: 100 個壓縮檔、兩顆磁碟各 29 個稀疏檔、各 100 個多串流檔）。
- 全部和標準答案一致：每個串流的 `EndOfFile`、`AllocationSize` 都相符，0 個不符。
- 另有 7 個檔案無法用 `OpenFileById` 開啟（錯誤 2、5、87），不在比對範圍內。
- 結論：**第 1 版的大小欄位位置與 SDK 第 2 版相同**（`AllocationSize`@16、`EndOfFile`@24）。只驗證了大小欄位；屬性代碼和識別字的位置，是從 hex dump 和比對結果間接確認的。
- 加上 `INCLUDE_STREAM_INFORMATION` 旗標後，回傳的仍是第 1 版。
- 檔案資料串流：屬性代碼 `0x80`、識別字為空。這是從比對結果判斷的，不是推測：522／523 個 `::$DATA` 都對應到它。
  - 剩下 1 個對應到代碼 `0x30`，判斷是大小剛好相同的巧合。
  - 其他屬性帶有檔案系統自己給的識別字，例如 `::$STANDARD_INFORMATION`、`::$FILE_NAME`，可以直接看出是哪種屬性。
- 命名串流（ADS）也是代碼 `0x80`，識別字不為空。
- 兩顆磁碟都沒有找不到上層資料夾的孤兒連結；C: 有 813,597 個 8.3 短檔名被略過（D: 沒有）。

**和第 1 版逐目錄掃描（提權）比對各第一層資料夾的邏輯大小：**
- D:：10 個第一層資料夾中，9 個**位元組完全一致**，配置大小也一致。
  - 剩下 1 個是開發用的專案資料夾，差 2,667,656 位元組。量測期間在裡面編譯程式，內容本來就在變。
- C:：
  - Program Files (x86) 幾乎一致：邏輯 10,847,006,171 對 10,846,975,856，配置 9,660,526,904 對 9,660,495,096。**壓縮檔的配置大小算法正確。**
  - Windows 少 2.3 GB，Users 少 1.0 GB，另有幾個資料夾（ProgramData、System Volume Information 等）有增減。
  - 兩次量測隔了約 35 分鐘，C: 內容在這段時間有變動（之前已觀察到 Windows Update 在下載）。所以**差異的原因還不能確定**，要做「同一時間連續跑兩種掃描、比對各資料夾檔案數」才能確認。
- 硬連結：C: 有 47,790 個檔案有多個連結。
  - 每個連結各算一次：492.3 GB。每個檔案只算一次：475.2 GB。差 17 GB。
- 命名串流（ADS）：使用者資料夾裡的總量很小（Program Files (x86) 約 322 MB 最多）。但中繼檔案有非常大的稀疏命名串流：
  - D: 根目錄有 8 個，合計約 1 TB。
  - `$Extend` 裡 C: 有 18.8 GB，D: 有 62.6 GB，**很可能**是 USN 日誌。
  - 這些都不能直接算進總量。第 1 版掃描本來就不包含 ADS。

**速度與記憶體：**

| 磁碟 | 第 1 次 | 第 2 次 | 第 1 版逐目錄（提權，熱快取） |
|---|---|---|---|
| C: | 4.49 s | 4.63 s | 12.7 s |
| D: | 17.5 s | 4.57 s | 4.1 s |

- 上表「第 1 次／第 2 次」是 pass 1 的列舉時間。只計列舉，不含後面的比對和加總。
- C: 快了約 2.8 倍。D: 熱快取時和逐目錄差不多。D: 第 1 次 17.5 s，**推測**是 MFT 資料還不在快取裡，尚未驗證。
- 峰值記憶體 C: 463 MB、D: 309 MB。這是因為實測程式把每個檔案都存成 `unordered_map` 加 `vector`／`wstring`，**不代表正式版的記憶體用量**。第 2 階段必須設計緊湊的資料結構。

**待決定／待驗證：**
- 硬連結規則：每個連結各算一次，或每個檔案只算一次。C: 兩者差 17 GB。
- 中繼檔案（`$` 開頭）和命名串流要不要計入。
- C: 各資料夾差異的原因：同一時間連續跑兩種掃描，比對各資料夾檔案數。
- 冷快取速度：要重開機後測。

#### 同時比對（2026-11-03，`layout-probe --compare`）

做法：新方法列舉完成後，**立刻**用第 1 階段的掃描器掃同一顆磁碟。兩者都用第 1 階段規則計算：只算未命名資料串流、每個連結各算一次、不進入目錄 reparse point。然後逐個資料夾比對檔案數、資料夾數、位元組；有差異就自動往下鑽到差最多的子資料夾，最多 5 層。

結果：
- **C: 上次約 3 GB 的差異，原因是磁碟內容在兩次量測之間變了，不是新方法算錯。**
  - 第 1 階段這次在 C: 只數到 971,260 個檔案，早上是 1,284,605 個。其中 C:\Windows 從 492,816 個降到 183,388 個，符合之前觀察到的 Windows Update 下載檔案已被清掉。
  - 兩種方法的總檔案數相差 74 個，大多是 `$Extend` 裡的 71 個中繼檔案。這些檔案逐目錄掃描看不到，算法差異本來就在預期中。
- **各資料夾的檔案數、資料夾數幾乎全部一致。** Users +8、ProgramData −1，判斷是掃描期間檔案有新增刪除。
- **剩下的位元組差異，全部是新方法比第 1 階段多**，集中在正在寫入的檔案：
  - 資料庫伺服器的交易記錄檔資料夾：檔案數相同，差 70 MB。
  - `AppData\Local\Temp` 直屬的檔案：差 68 MB。
  - 瀏覽器快取等資料夾：數 MB 到 12 MB。
  - **推測**：目錄列舉回傳的大小（來自目錄項目）沒有即時反映寫入中檔案的大小，`FSCTL_QUERY_FILE_LAYOUT` 讀到的是檔案本身目前的大小。目前**沒有文件可以證實**這點，如果之後需要，可以抽樣這些檔案，用 `GetFileInformationByHandleEx(FileStandardInfo)` 驗證。
  - C: 的標準答案比對有 2 個抽樣不符，都是正在變大的檔案，標準答案比新方法還大。這也說明差異來自檔案在變動。
- D: 對照組：只有開發用的專案資料夾差 2 MB（量測期間在編譯），其他資料夾完全一致。

**結論：`FSCTL_QUERY_FILE_LAYOUT` 算出的資料夾大小正確，可以作為第 2 階段 NTFS 掃描的基礎。**

- **待查**：C: 根目錄直屬檔案（不屬於任何第一層資料夾）兩種方式差約 1.63 GB，新方法較多。**推測**是逐目錄掃描看不到的根目錄系統檔案，尚未證實。這會在第 2 階段的驗證模式中確認。

### 2026-11-03 第 2 階段設計草案

- 設計寫在 [docs/DESIGN-phase2.md](DESIGN-phase2.md)，等使用者確認。
- 硬連結的比較數據：C: 已用空間 476.7 GB（`Get-Volume` 查詢，比掃描晚幾分鐘）。
  - 配置大小每個連結各算一次：486.5 GB，**超過**已用空間。
  - 每個檔案只算一次：470.2 GB。
  - D: 也一樣：每個連結算一次的合計超過已用空間約 171 MB，每個檔案只算一次則在已用空間之內。
- 查證 [FSCTL_QUERY_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_query_usn_journal)：對 `\\.\X:` 磁碟區 handle 呼叫，只支援 NTFS。輸出 [USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_journal_data_v0)，含 `UsnJournalID`、`FirstUsn`、`NextUsn`、`LowestValidUsn`。
  - 設計上要在掃描前記下 `NextUsn`，第 3 階段從這裡接續。
  - 文件沒寫需要什麼權限，實作時實測。

### 2026-11-03 權限與部署方向（第 4 階段參考）

- 實測：`FSCTL_QUERY_FILE_LAYOUT` 在一般權限下無法使用（錯誤 1）；逐目錄掃描可以用，但讀不到部分資料夾。
- 方向：
  - **服務模式為主**：安裝時要系統管理員權限一次，註冊 Windows 服務，由服務在背景用高權限掃描。前端以一般權限透過 named pipe 查詢，日常使用不跳 UAC。
  - **逐目錄掃描為退路**：沒有服務或沒有管理員權限時使用，也可以做成免安裝版。
- **安全要求**：服務看得到所有資料夾，回應查詢前必須確認呼叫者本人有權限看該資料夾，否則一般使用者能查到其他使用者的資料夾名稱和大小。候選做法是 `ImpersonateNamedPipeClient`，第 4 階段再查文件確認。
- 待查：讀取 USN 日誌需要什麼權限（第 3 階段）。

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
| 第 2 階段查證 | [FSCTL_QUERY_FILE_LAYOUT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-query-file-layout) |
| 第 2 階段查證 | [QUERY_FILE_LAYOUT_INPUT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_query_file_layout_input) |
| 第 2 階段查證 | [QUERY_FILE_LAYOUT_OUTPUT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_query_file_layout_output) |
| 第 2 階段查證 | Windows SDK 10.0.26100.0 `um/winioctl.h`：`FSCTL_QUERY_FILE_LAYOUT` 段落（`FILE_LAYOUT_ENTRY`、`FILE_LAYOUT_NAME_ENTRY`、`FILE_LAYOUT_INFO_ENTRY`、`STREAM_LAYOUT_ENTRY`、各旗標） |
| 第 2 階段查證 | [OpenFileById](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openfilebyid) |
| 第 2 階段查證 | [FILE_ID_DESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_descriptor) |
| 第 2 階段查證 | [FILE_STREAM_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_stream_info) |
| 第 2 階段設計 | [FSCTL_QUERY_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_query_usn_journal) |
| 第 2 階段設計 | [USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_journal_data_v0) |
| 建置 | [CMAKE_MSVC_RUNTIME_LIBRARY](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html) |
