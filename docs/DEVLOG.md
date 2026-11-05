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

### 2026-11-03 第 2 階段核心實作

- 使用者決定：硬連結預設每個連結各算一次（A）；只計入資料夾列舉看得到的項目（含隱藏檔、系統檔），中繼檔案、`$Extend`、命名串流不計入。
- 新增核心模組：`index`（資料結構與彙總）、`layout_parser`（版面輸出解析，純解析、無 I/O）、`layout_index_builder`、`ntfs_index`、`walk_index`、`dir_listing`。第 1 階段的 `dir_walker` 保留給 `scan`。
- 逐目錄掃描改用 [FileIdBothDirectoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_both_dir_info)：同時取得 64 位元檔案 ID，NTFS 掃描的根目錄判斷也用同一個函式。檔案系統不支援時退回 `FileFullDirectoryInfo`。文件說明 FAT 的檔案 ID 可能隨重組改變，所以逐目錄掃描用掃描路徑本身連結上層，ID 只當資料保存。
- **資料夾 reparse point 的規則改變**：第 1 階段略過所有資料夾 reparse point。現在只略過 [IsReparseTagNameSurrogate](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-isreparsetagnamesurrogate) 為真的（文件：代表系統中另一個具名項目，例如掛載的資料夾，包括 junction 和符號連結）。其他種類（例如雲端檔案的資料夾）裡面是真的檔案，檔案總管也會顯示，所以計入。標記用 [FILE_ATTRIBUTE_TAG_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_attribute_tag_info)（`FileAttributeTagInfo`）讀取；讀不到時當作不跟隨。
- 測試：`tests/core_tests.cpp`，122 項檢查，不需提權。包含解析器的每一種截斷長度、錯誤 offset、未知版本、負數大小，以及一個真實的暫存資料夾（硬連結、隱藏系統檔、命名串流、junction）。也確認列舉回傳的檔案 ID 等於開啟 handle 後 `GetFileInformationByHandle` 的 ID，也就是版面資料用的 ID。
- AddressSanitizer：CMake 選項 `DIRSIZE_ASAN`。依 [/fsanitize](https://learn.microsoft.com/en-us/cpp/build/reference/fsanitize) 與 [ASan 建置參考](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building)：`/MT` 可用，但 VS 17.7 起執行檔需要 `clang_rt.asan_dynamic-x86_64.dll`（在 MSVC 工具的 bin 資料夾）。ASan 版測試 0 個錯誤。
- `dirsize-cli verify X:`：兩種引擎各建一次索引，逐資料夾比對，並逐一比對根目錄直屬檔案（用來查 1.63 GB 落差）。排除量分成「根目錄列舉看不到的檔案」和「隱藏資料夾底下的檔案」兩類。也輸出跟隨的 reparse 種類、根目錄 ID 對到幾個、索引表格大小與程序記憶體。`scripts/verify.ps1` 需在提權的 PowerShell 執行。

#### 驗證結果（2026-11-03，提權，C:、D: 各一次）

`verify` 先跑 NTFS 掃描，**接著**跑逐目錄掃描，所以逐目錄是熱快取，兩者的時間不能直接比冷熱。

| 項目 | C: | D: |
|---|---|---|
| 資料夾數（兩種掃描） | 200,373／200,373 | 115,049／115,049 |
| 檔案數（NTFS／逐目錄） | 981,818／982,086 | 824,182／824,182 |
| 邏輯大小差（NTFS − 逐目錄） | +23,190,118 B | +61,981,414 B |
| 配置大小差 | +972,207,264 B | +65,218,288 B |
| NTFS 掃描時間（列舉＋建索引） | 8.58 s（8.29＋0.29） | 19.25 s（18.83＋0.42） |
| 逐目錄掃描時間 | 32.5 s | 16.2 s |
| 索引表格 | 50.39 MB | 33.57 MB |
| 程序峰值工作集 | 130.22 MB | 101.74 MB |

- 解析器：兩顆磁碟都是 0 個異常項目、0 個未知版本、0 個截斷；列舉都以 `ERROR_HANDLE_EOF` 結束。
- **根目錄 ID 判斷正確**：C: 根目錄列舉 23 項，在版面資料中對到 23 項；D: 22／22。
- **1.63 GB 落差已確認**：C: 根目錄列舉看不到的檔案有 10 個，合計 1,631,545,888 B，和之前的落差相同。根目錄直屬、列舉看得到的檔案，兩種掃描逐一比對大小全部一致（C: 9／9，D: 11／11）。D: 這類檔案 10 個，合計 6.56 GB。
- 隱藏資料夾（`$Extend` 等）：兩顆各 8 個，底下的檔案 C: 159 個（180 MB）、D: 6 個（21 MB），不計入。
- junction／符號連結等略過的資料夾：C: 276 個、D: 3,186 個，兩種掃描數字相同。
- D:：檔案數、資料夾數完全一致。位元組差異集中在兩個資料夾：一個虛擬機器的磁碟檔資料夾（60 MB），一個開發工具的快取資料夾（2 MB），都是使用中的檔案。
- C:：
  - 檔案數差 −268，幾乎全在使用者 AppData 下一個應用程式的資料夾（−267），另有兩個 ±1、−2 的資料夾。這些資料夾在掃描期間正在使用；NTFS 掃描比逐目錄早約 30 秒，**推測**是這段時間新增的檔案，未驗證。
  - 邏輯大小差和上次同時比對相同，集中在 Temp、資料庫伺服器記錄檔、瀏覽器與應用程式快取等寫入中的檔案，NTFS 掃描較多。
- 記憶體：C: 索引表格 50.39 MB，符合設計估計的 50–60 MB。峰值工作集包含 4 MB 版面緩衝區和建索引時的暫存陣列。

**未解釋，待查：**
- C: 配置大小差 +972 MB，遠大於邏輯大小差 +23 MB；D: 兩者接近。`verify` 目前只依邏輯大小往下鑽，看不出差在哪個資料夾，要加上依配置大小比對。
- C: `System Volume Information` 直屬檔案：檔案數相同，NTFS 掃描的邏輯大小少 234,881,024 B（剛好 224 MiB），方向和其他差異相反。
- C: 有 1 個資料夾帶 reparse 屬性，但讀到的標記是 `0x00000000`，兩種掃描都跟隨了它。還不知道是哪個資料夾。
- 兩顆磁碟各有 4 個找不到上層資料夾的檔案（unreachable），還沒查是哪些。
- 冷快取速度：D: 的 NTFS 掃描這次 19.25 s，比逐目錄還慢，但兩者快取狀態不同，要重開機後分別測。

**`verify` 補強（為了查上面的項目）：**
- 逐資料夾比對也比配置大小，往下鑽時取邏輯大小差和配置大小差較大的一個。
- 「直屬檔案」有差異的資料夾，重新列舉一次，逐一列出不同的檔案（每個資料夾最多 5 個）和屬性。
- 新增「Diagnostics」段落：列出跟隨的 reparse 資料夾路徑、屬性和標記，以及找不到上層資料夾的檔案和其上層 ID 的路徑（[OpenFileById](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openfilebyid)＋[GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)）。索引只保留最多 32 筆樣本。

#### 第二次驗證（2026-11-03，補強後的 `verify`）

- **逐目錄掃描回報的大小會落後**：差異的檔案幾乎都是記錄檔、`.etl`、`.tmp`、資料庫 `-wal` 這類寫入中的檔案。很多檔案目錄列舉回報 0 B，版面資料是實際大小，例如：
  - 資料庫伺服器的錯誤記錄檔：列舉 0 B，版面 95,080 B。
  - 系統 `LogFiles\WMI` 的一個 `.etl`：列舉 262,144 B，版面 16,777,216 B。
  - 所以兩邊差異不是 NTFS 掃描算錯，而是逐目錄掃描讀到的目錄項目大小比較舊。檔案本身目前的大小哪邊對，下一版 `verify` 用 `FileStandardInfo` 直接讀檔案來確認。
- **C: 配置大小差 +1.21 GB**（這次）：其中 +830 MB 在 `Windows\ServiceProfiles\NetworkService\AppData\Local\Microsoft\Windows` 底下，邏輯大小只差 0.88 MB。比對最多只往下 6 層，沒看到是哪些檔案，下一版改成 14 層。
- **System Volume Information 的 −224 MiB 這次沒有出現**，判斷是當時正在變動的檔案。
- **標記 0 的 reparse 資料夾**：在使用者 Temp 底下一個即時通訊軟體的暫存資料夾。掃描時兩種方式都看到 reparse 屬性，但用 handle 讀到的屬性只有 `0x10`（資料夾）、標記 0。照規則當作一般資料夾計入，結果正確；為什麼屬性不一致還沒查文件。
- **找不到上層資料夾的 4 個檔案**：兩顆磁碟都是 ID `0x0c`～`0x0f`（序號等於記錄號），沒有任何名稱，用 ID 開啟回傳錯誤 5。它們沒有名稱，不屬於任何資料夾，排除是對的。
- D:：檔案數、資料夾數完全一致。這次另外看到一個 pnpm `node_modules` 資料夾邏輯大小相同、配置大小差 2.7 MB（pnpm 大量使用硬連結），同樣受 6 層限制看不到檔案。
- 掃描期間有檔案被替換（同名但 ID 不同：列舉看得到的 ID 不在版面資料裡，版面資料的舊 ID 也不在列舉裡），屬於正常變動。

**`verify` 再補強**：有差異的檔案多印「now」，也就是開啟檔案後 [FILE_STANDARD_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info)（`FileStandardInfo`）讀到的目前大小；往下比對的層數從 6 改成 14。

#### 第三次驗證（2026-11-03，加上「now」）

有差異的檔案中，開啟檔案讀到的目前大小（`FileStandardInfo`）：

| 磁碟 | 和 NTFS 掃描相同 | 和逐目錄掃描相同 | 都不同 |
|---|---|---|---|
| C: | 138 | 0 | 5 |
| D: | 82 | 0 | 0 |

- 「都不同」的 5 個都是寫入中的記錄檔，目前大小比兩次掃描都大。
- **結論：NTFS 掃描的大小和檔案本身一致；逐目錄掃描讀到的目錄項目大小會落後，常常是 0 B。** 兩種掃描的差異來自逐目錄掃描，不是 NTFS 掃描算錯。這也代表沒有管理員權限時的逐目錄掃描，對寫入中的檔案會少算。
- **C: 那 830 MB 配置大小**：是系統傳遞最佳化（Delivery Optimization）快取裡的 3 個 `content.bin`。屬性 `0x220`（稀疏檔）。逐目錄掃描回報配置 0 B；NTFS 掃描和開啟檔案讀到的一樣，例如 533,790,720 B。
- **D: pnpm 的 2.7 MB**：數百個幾百位元組的小檔案，逐目錄掃描回報的配置大小約等於檔案大小（例如 440 B），NTFS 掃描和開啟檔案讀到的都是 4,096 B。

第 2 階段核心的 NTFS 掃描結果，可以視為正確。剩下沒查的：標記 0 的 reparse 資料夾為什麼屬性前後不一致、冷快取速度。

### 2026-11-05 第 3 階段查證：USN 日誌（文件階段）

SDK 標頭檔：Windows SDK 10.0.26100.0 `um/winioctl.h`，查 `READ_USN_JOURNAL_DATA_V0/V1`、`USN_RECORD_V2/V3/V4`、`USN_REASON_*`、`QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID`、`FILE_REFERENCE_RANGE`。

文件記載的事實：
- [FSCTL_READ_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_read_usn_journal)：對 `\\.\X:` 磁碟區 handle 呼叫。頁面沒寫需要什麼存取權限或特殊權限，也沒列錯誤碼。
- [Walking a Buffer of Change Journal Records](https://learn.microsoft.com/en-us/windows/win32/fileio/walking-a-buffer-of-change-journal-records)：輸出緩衝區開頭 8 bytes 是下一次要讀的 USN，後面才是記錄；用 `RecordLength` 前進。
- [READ_USN_JOURNAL_DATA_V1](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-read_usn_journal_data_v1)：
  - `StartUsn` 為 0 從第一筆開始；不是 0 但小於第一筆時回傳 `ERROR_JOURNAL_ENTRY_DELETED`。
  - `ReturnOnlyOnClose` 為 0 時，每個變動第一次記錄時就回傳，檔案關閉時再回傳一次。
  - `BytesToWaitFor` 為 0 時不等待，讀到日誌結尾就成功返回；回傳的下一個 USN 等於 `StartUsn` 表示沒有新記錄。
  - `UsnJournalID` 不符時呼叫失敗。日誌停止後重新啟動，或刪除後重建，會換新的 ID，期間的變動可能沒有記錄。
  - `MinMajorVersion`／`MaxMajorVersion` 的允許值頁面沒寫。
- [USN_RECORD_V2](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2)：**沒有檔案大小欄位**。`Reason` 是檔案開啟後累積的旗標，關閉時會有一筆帶 `USN_REASON_CLOSE`；改名或搬移會產生兩筆記錄。記錄在緩衝區內以 64 位元對齊；檔名不一定以 `\0` 結尾，要用 `FileNameLength`。
- [USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_journal_data_v0)：`FirstUsn` 是目前可讀的第一筆，`LowestValidUsn` 之前的變動可能沒有記錄（日誌不連續）。
- [Creating, Modifying, and Deleting a Change Journal](https://learn.microsoft.com/en-us/windows/win32/fileio/creating-modifying-and-deleting-a-change-journal)：日誌超過 `MaximumSize + AllocationDelta` 時，NTFS 在檢查點會從開頭修剪；刪除中讀取會回傳 `ERROR_JOURNAL_DELETE_IN_PROGRESS`，刪除會跨重開機持續。
- [Using the Change Journal Identifier](https://learn.microsoft.com/en-us/windows/win32/fileio/using-the-change-journal-identifier)：寫著所有變更日誌操作都要系統管理員權限（Administrators 群組）。但這句的上下文是取得日誌 ID，**讀取是否真的需要提權要實測**。舊記錄可能無法使用時，NTFS 會換新 ID。
- [Change Journals](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journals)：概觀，沒有寫保留多久或誰能讀。
- [QUERY_FILE_LAYOUT_INPUT（ntifs）](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_query_file_layout_input)：
  - `QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID` 只回傳 `FileReferenceRanges` 範圍內的檔案，**Windows 10 1809 起**才有。
  - 範圍不能重疊；範圍是否包含結尾、最多幾個範圍，頁面沒寫。
  - 篩選範圍在設定 `RESTART` 的那次呼叫時快取。
  - 另一個網址 `.../api/winioctl/ns-winioctl-query_file_layout_input` 回傳 404。
- [FSCTL_QUERY_FILE_LAYOUT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/fsctl-query-file-layout)：頁面的狀態表把 `STATUS_END_OF_FILE` 的說明寫成「緩衝區未對齊」，和 `STATUS_INVALID_USER_BUFFER` 重複，判斷是文件錯誤；最低版本寫 Windows 8.1 Update，ntifs 頁面寫 Windows 8，兩頁不一致。

要實測（文件沒寫）：一般權限能否讀日誌、日誌保留多久、FILEID 篩選的比對方式與速度、USN 記錄的檔案 ID 是否和索引一致。

#### 實測程式 `tools/usn_probe.cpp`（CMake target `usn-probe`，不出貨）

- 量測項目：三種存取權限開磁碟區後能否查詢／讀取日誌；日誌保留多久（最舊記錄的時間、每天的記錄數）；USN 記錄的檔案 ID 是否和第 2 階段索引一致；FILEID 篩選怎麼比對 ID、速度和 `OpenFileById`＋`FileStandardInfo` 逐檔讀取的比較。
- 記錄只要求第 2 版（`MinMajorVersion`＝`MaxMajorVersion`＝2），解析時檢查長度與邊界。
- `scripts/usn-probe.ps1` 對 C:、D: 各跑一次，輸出存到 `bench-results/`。
- **未提權（C:、D: 相同）**：
  - `GENERIC_READ` 開磁碟區：錯誤 5（存取被拒）。
  - `FILE_READ_ATTRIBUTES` 或存取權限 0 開得了，但 `FSCTL_QUERY_USN_JOURNAL`、`FSCTL_READ_USN_JOURNAL` 都回傳錯誤 1（`ERROR_INVALID_FUNCTION`）。
  - 所以**一般權限讀不到 USN 日誌**，和 `FSCTL_QUERY_FILE_LAYOUT` 一樣。沒有服務時的逐目錄掃描無法用日誌保持最新。

#### 提權實測（2026-11-05，C:、D: 各一次）

**存取權限**：提權後用 `GENERIC_READ` 開磁碟區，查詢、讀取都可以；用 `FILE_READ_ATTRIBUTES` 或 0 開，即使提權也是錯誤 1。所以讀日誌要「提權＋`GENERIC_READ`」。

**日誌大小與保留時間**：

| 項目 | C: | D: |
|---|---|---|
| `MaximumSize`／`AllocationDelta` | 32 MB／8 MB | 32 MB／8 MB |
| 可讀範圍 | 41.2 MB | 35.5 MB |
| 記錄數 | 354,379 | 409,544 |
| 不同的檔案 ID | 85,689 | 124,199 |
| 保留的歷史 | **10.3 小時** | 24.1 小時 |
| 讀完整個日誌 | 0.075 s | 0.079 s |

- `LowestValidUsn` 兩顆都是 0。沒有讀到第 2 版以外的記錄，也沒有格式錯誤的記錄。
- C: 的日誌只保留約 10 小時。服務停掉或電腦關機超過這個時間，就接不上，只能整顆重掃。
- `USN_REASON_CLOSE` 記錄約佔四到六成。設 `ReturnOnlyOnClose = 0`，所以開著的檔案也有記錄。

**USN 記錄的檔案 ID 和索引的關係**：
- 只算建索引之前的記錄。D:：124,199 個 ID 中，19,596 個在索引裡，104,602 個「MFT 記錄號相同但序號不同」，1 個不在索引。104,602 剛好等於 `USN_REASON_FILE_DELETE` 的記錄數，也就是檔案刪除後記錄號被新檔案重用。
- C:：29,627 個在索引裡，50,559 個是同記錄號、不同序號，5,503 個不在索引。抽樣 2,000 個不在索引的 ID 用 `OpenFileById` 開啟，1,989 個回傳錯誤 87（已刪除），11 個開得了（不計入的項目或掃描後新增的）。
- **結論：USN 記錄的 `FileReferenceNumber` 和索引的檔案 ID 是同一套編號**（含序號）。

**FILEID 篩選（`QUERY_FILE_LAYOUT_FILTER_TYPE_FILEID`）**：
- **只比對 MFT 記錄號（低 48 位元），不比對序號**：只給記錄號 `[ra, ra]` 也回傳完整 ID。所以記錄號被重用時會拿到新檔案，必須比對回傳的完整 ID。
- **範圍包含結尾**：`[a, b]` 回傳 a 到 b（包含 b），`[a, b-1]` 不含 b。中間有其他記錄號時也一起回傳。
- 範圍不必排序，依給的順序回傳。
- 新建的暫存檔立刻查得到；刪除後查同一個 ID，0 筆，沒有錯誤。

**速度**（熱快取，剛建完索引；每個範圍一個 ID）：

| ID 數 | C: 篩選 | C: 逐檔開啟 | D: 篩選 | D: 逐檔開啟 |
|---|---|---|---|---|
| 10 | 0.000 s | 0.002 s | 0.000 s | 0.001 s |
| 1,000 | 0.020 s | 0.212 s | 0.024 s | 0.109 s |
| 10,000 | 0.413 s | 1.771 s | 0.341 s | 1.165 s |
| 100,000 | 20.557 s | 沒測 | 22.237 s | 沒測 |

- 逐檔開啟指 `OpenFileById`＋`FileStandardInfo`。篩選比逐檔開啟快 3～10 倍。
- 一次給 100,000 個範圍時，時間是 10,000 個的約 50 倍，不是線性。整顆重掃 C: 只要約 9 秒，所以一次只給大約 1 萬個範圍，分批送。分批是否就能變回線性，實作時再量。
- 逐檔開啟有 1 個錯誤 5。這支實測程式沒有啟用 `SeBackupPrivilege`。

**寫入量**：C: 的 `NextUsn` 在 11-03 21:34 的 verify 是 18,962,126,688，11-05 08:09 的實測是 19,234,366,904，約 34.6 小時增加 272 MB，平均每小時約 7.9 MB。D: 同期間約 59 MB，每小時約 1.7 MB。C: 實測時保留的 41 MB 橫跨 10.3 小時，比平均速度算出的約 5 小時長；這段時間包含晚上，**推測**是關機或閒置時沒有寫入，未驗證。

設計草案寫在 [docs/DESIGN-phase3.md](DESIGN-phase3.md)，另外查證：
- [Windows 10 Home and Pro 生命週期](https://learn.microsoft.com/en-us/lifecycle/products/windows-10-home-and-pro)：1809 的結束日期 2020-11-11；Windows 10 整體 2025-10-14 結束支援。
- [FSCTL_CREATE_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_create_usn_journal)：可以修改現有日誌的大小，不必刪除；頁面沒寫呼叫者需要什麼權限。
- [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)：沒有說是原子操作；錯誤 1176、1177 時檔名會停在中間狀態；`REPLACEFILE_WRITE_THROUGH` 不支援。
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)：`MOVEFILE_REPLACE_EXISTING` 取代既有檔案；`MOVEFILE_WRITE_THROUGH` 在檔案實際搬到磁碟後才返回。
- [System Error Codes 1000-1299](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-)：1178 `ERROR_JOURNAL_DELETE_IN_PROGRESS`、1179 `ERROR_JOURNAL_NOT_ACTIVE`、1181 `ERROR_JOURNAL_ENTRY_DELETED`。

### 2026-11-05 第 3 階段設計確認

- 使用者決定（[DESIGN-phase3.md](DESIGN-phase3.md) 第 8 節）：
  - 最低版本改成 Windows 10 1809，舊版只做整顆重掃。已更新 `CLAUDE.md`、`CONTRIBUTING.md`、`CONTRIBUTING.zh-TW.md`；README 沒有寫版本，不用改。
  - 不改系統的日誌大小，接不上就在背景整顆重掃。
  - 重掃期間先顯示舊快取，標示「更新中」。

### 2026-11-05 第 3 階段實作：增量更新

- **共用規則**：可見性判斷和計入規則拆成 `ClassifyChildDir`、`FileLinkCounted`（`index.h`），整顆建立（`LayoutIndexBuilder::Finish`）、逐目錄掃描和增量更新都用同一套。拆完後原本 122 項檢查全部通過。
- `DirEntry` 加上 `prevSibling`，放在原本的對齊空位，仍是 64 bytes；移除子資料夾變成 O(1)。
- **`live_index`**：可修改的索引。一批變動分三輪套用：
  1. 扣掉變動檔案的舊貢獻。
  2. 改資料夾結構：先建立，上層先於下層；再處理搬移和改名；變成看不到的資料夾**最後**才移除；刪除由最深的開始。
  3. 加上變動檔案的新貢獻。
  - 新增的項目放在新增區，用雜湊表查；刪除只做標記。
  - 不一致時一律要求整顆重掃，不嘗試修補：刪除的資料夾不是空的、搬移形成循環、合計會變成負數、資料夾變成看得到但不是新建的、版面資料格式錯誤。
  - 增量更新不維護排除統計、`rootListingMatched`、跟隨的 reparse 種類和診斷樣本，這些都只代表上次整顆掃描。
- **`usn_parser`**：解析 `FSCTL_READ_USN_JOURNAL` 的輸出，純解析、檢查邊界，只接受第 2 版；`RecordLength` 必須是 8 的倍數（文件：記錄以 64 位元對齊）。`RecordRanges` 把 ID 去掉序號、去除重複、合併連續記錄號。
- **`ntfs_live`**：`NtfsLiveVolume::CatchUp()` 檢查日誌 ID 和 `FirstUsn`／`LowestValidUsn`，讀到日誌結尾，每批最多 1 萬個範圍重查，再列舉根目錄、套用。變動 ID 超過 5 萬個就改成整顆重掃。
- **`dirsize-cli watch X: [秒數]`**：建索引後先重播掃描期間的變動，接著每 5 秒追一次日誌，時間到再和全新的整顆建立逐資料夾比對。`scripts/watch.ps1` 需在提權的 PowerShell 執行。
- **測試**：新增 `live-tests`（99 項檢查，ASan 版也通過）。
  - 隨機等價測試：60 個固定 seed，每個 300 批隨機操作，每批後用 ID 比對增量結果和整顆重建結果，也比對維護的合計和從頭重算的合計。
  - 隨機操作包括：檔案的新增、改大小、刪除、搬移；硬連結的新增與移除，含同一資料夾兩個連結；記錄號重用；資料夾的新增、改名、搬移、刪除、互換位置；變成 junction；搬進 `$Extend`；根目錄列舉看不到的項目。
  - 固定案例：資料夾變成看得到要求重掃；不一致的批次要求重掃；記錄號重用；重複套用結果不變；搬出資料夾後原資料夾變成 junction。
  - **刻意改壞程式驗證測試有效**：9 種改法（少扣硬連結、搬移不扣舊上層、忽略根目錄規則、只標記最上層死亡等）全部被抓到。
  - 隨機測試抓到一個真錯誤：同一批裡 B 從 A 搬出、A 又變成 junction 時，原本先處理 A 會把 B 一起移除，再誤判 B「變成看得到」。改成搬移全部完成後才移除看不到的子樹，並加了固定案例。

#### 第一次 watch 實測（2026-11-05，提權，C:、D: 各 300 秒）

| 項目 | C: | D: |
|---|---|---|
| 建索引 | 5.0 s＋0.2 s | 9.1 s＋0.7 s |
| 重播掃描期間的變動 | 81 筆記錄、19 個 ID | 1 筆、1 個 ID |
| 300 秒內追日誌 | 60 次，共 1,333 個 ID，最慢 0.014 s | 60 次，共 1 個 ID |
| 需要整顆重掃 | 沒有 | 沒有 |
| 維護的合計 vs 從頭重算 | 0 個資料夾不同 | 0 個資料夾不同 |
| 和全新整顆建立比較 | 檔案 +1，位元組 −764,046 | 檔案 0，位元組 −156 |

- 每次追日誌都只花幾毫秒：讀取、重查、套用各在 0.003 s 以內。
- **差異的原因：開著的檔案在日誌裡看不到後續成長。** 差異幾乎都是記錄檔、`.etl`、`-wal`，而且多半是更新後的索引比檔案目前大小小。例如：
  - 系統 `LogFiles\WMI` 的 `Wifi.etl`：更新後的索引 6,307,840 B，全新建立和目前大小約 6,389,760 B。
  - D: 一個執行中虛擬機器的記錄檔：差 156 B。
  - 文件寫明：`Reason` 是檔案開啟後累積的旗標；`ReturnOnlyOnClose = 0` 時，每種變動只在第一次記錄時回傳，關閉時再回傳一次（[READ_USN_JOURNAL_DATA_V1](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-read_usn_journal_data_v1)、[USN_RECORD_V2](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2)）。所以檔案一直開著寫的話，第一筆記錄之後就沒有新記錄，要到關閉才有。
  - 也有反方向的：登錄檔的 `.LOG1`、`.LOG2` 更新後的索引比較大，判斷是開著的期間被截短，原因相同。
- 比較本身也有誤差：全新建立在最後一次追日誌之前完成，之後才寫入的檔案只有更新後的索引有，例如 C: 那 1 個檔案。

**改善**：新增 `OpenFileTracker`。日誌裡出現過、但還沒看到 `USN_REASON_CLOSE` 或刪除的檔案，每次追日誌時一併重查，直到關閉。這只能涵蓋**建索引之後才開啟**的檔案；建索引之前就開著、之後一直寫的檔案（例如開機就開始寫的 `.etl`），日誌裡沒有它們的記錄，要靠定期整顆重掃修正。

#### 第二次 watch 實測（加上 OpenFileTracker，2026-11-05，提權，C:、D: 各 300 秒）

| 項目 | C: | D: |
|---|---|---|
| 建索引 | 3.4 s＋0.4 s | 7.2 s＋0.6 s |
| 重播掃描期間的變動 | 80 筆記錄、25 個 ID | 1 筆、1 個 ID |
| 300 秒內追日誌 | 60 次，共 4,890 個 ID，最慢 0.065 s | 60 次，共 0 個 ID |
| 同時追蹤的開啟中檔案 | 最多 59 個 | 最多 1 個 |
| 需要整顆重掃 | 沒有 | 沒有 |
| 維護的合計 vs 從頭重算 | 0 個資料夾不同 | 0 個資料夾不同 |
| 和全新整顆建立比較 | 檔案 +1，位元組 −1,956,141，88 個資料夾不同 | **完全相同** |

- 最慢的一次追日誌：一次刪掉約 3,300 個檔案（3,348 個 ID、3,150 個範圍），重查 0.062 s，套用 0.002 s，同時移除 2 個資料夾。
- C: 的差異比第一次大（第一次 −764,046 B）。兩次的使用狀況不同，所以這個數字不能拿來判斷 OpenFileTracker 有沒有效果。
- 差異仍然都是更新後的索引比全新建立小，而且都是一直開著寫的檔案：
  - 系統的 `LogFiles\WMI` 下的 `.etl`；
  - `System32\config\DEFAULT.LOG1`；
  - 使用者登錄檔的 `ntuser.dat.LOG2`；
  - 使用者資料夾內各應用程式的 `.log`、`-wal` 與 leveldb 檔案。
- 例：使用者資料夾內一個 `-wal` 檔，在更新後的索引是 65,952 B。所在資料夾的差是 −226,600 B，而這個檔案目前的大小是 292,552 B（65,952＋226,600，剛好相等）。這表示全新建立讀到的是目前大小，所以 `FSCTL_QUERY_FILE_LAYOUT` 對開著的檔案也會回傳目前大小。漏掉的是「要重查哪些檔案」，不是重查的結果。
- **尚未證實**：剩下的差異是否全部來自「建索引之前就開著」的檔案。要證實，需要逐一確認每個不同的檔案都沒有出現在日誌裡。目前的輸出做不到這點，原因有兩個：
  - watch 的逐檔明細沿用 verify 的比較，比的是「更新後的索引 vs 資料夾清單」，不是「更新後 vs 全新建立」；
  - 輸出也沒有標示該 ID 是否出現在日誌、是否正在追蹤。
  
  下一步會補上這兩項。

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
| 第 2 階段實作 | [FILE_ID_BOTH_DIR_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_both_dir_info) |
| 第 2 階段實作 | [FILE_INFO_BY_HANDLE_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class) |
| 第 2 階段實作 | [FILE_ATTRIBUTE_TAG_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_attribute_tag_info) |
| 第 2 階段實作 | [IsReparseTagNameSurrogate](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-isreparsetagnamesurrogate) |
| 第 2 階段驗證 | [GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew) |
| 第 2 階段驗證 | [FILE_STANDARD_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_standard_info) |
| 第 3 階段查證 | [FSCTL_READ_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_read_usn_journal) |
| 第 3 階段查證 | [READ_USN_JOURNAL_DATA_V1](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-read_usn_journal_data_v1) |
| 第 3 階段查證 | [USN_RECORD_V2](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_record_v2) |
| 第 3 階段查證 | [Change Journals](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journals) |
| 第 3 階段查證 | [Walking a Buffer of Change Journal Records](https://learn.microsoft.com/en-us/windows/win32/fileio/walking-a-buffer-of-change-journal-records) |
| 第 3 階段查證 | [Creating, Modifying, and Deleting a Change Journal](https://learn.microsoft.com/en-us/windows/win32/fileio/creating-modifying-and-deleting-a-change-journal) |
| 第 3 階段查證 | [Using the Change Journal Identifier](https://learn.microsoft.com/en-us/windows/win32/fileio/using-the-change-journal-identifier) |
| 第 3 階段設計 | [Windows 10 Home and Pro 生命週期](https://learn.microsoft.com/en-us/lifecycle/products/windows-10-home-and-pro) |
| 第 3 階段設計 | [FSCTL_CREATE_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_create_usn_journal) |
| 第 3 階段設計 | [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew) |
| 第 3 階段設計 | [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw) |
| 第 3 階段設計 | [System Error Codes 1000-1299](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-) |
| 測試 | [/fsanitize](https://learn.microsoft.com/en-us/cpp/build/reference/fsanitize) |
| 測試 | [AddressSanitizer 建置參考](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building) |
| 建置 | [CMAKE_MSVC_RUNTIME_LIBRARY](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html) |
