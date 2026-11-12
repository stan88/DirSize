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

#### watch 補強：資源量測與逐檔比對

- **逐檔比對**：改成「更新後的索引 vs 全新建立」，以檔案 ID 比對，不再重新列資料夾。每個不同的檔案依日誌狀態分三組：
  - 從沒出現在日誌（`never in journal`）；
  - 出現過、之後已關閉（`closed since`）；
  - 仍在追蹤中（`tracked open`）。

  每組列出檔案數和位元組差異的總和。差異最大的 60 個檔案，另外列出路徑。
- **資源量測**：分三段量本程式的用量：
  - 建索引加重播；
  - 監看期間（大多在睡眠）；
  - 全新建立（此時記憶體裡同時有兩份索引）。

  每段記錄經過時間和 CPU 時間，CPU 分核心模式與使用者模式，並換算成單一核心的百分比；另外記錄 I/O 次數與位元組數、目前與峰值的工作集和 commit。
  - CPU 時間來自 [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)，單位是 100 奈秒，是所有執行緒的總和，所以可能超過經過時間。
  - I/O 來自 [GetProcessIoCounters](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters)／[IO_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-io_counters)。文件只說「read／write／other 的次數與位元組數」，**沒說 `DeviceIoControl` 算在哪一類，也沒說算的是快取還是實際磁碟讀寫**。所以這組數字只能當參考，不能當成磁碟負載。
  - 記憶體來自 [PROCESS_MEMORY_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters)：`PagefileUsage` 是 commit charge，`PeakPagefileUsage` 是它的峰值。

#### 第三次 watch 實測（資源量測＋逐檔比對，2026-11-05，提權，C:、D: 各 300 秒）

**資源用量**（本程式單一執行緒；百分比是相對於單一核心）：

| 階段 | C: | D: |
|---|---|---|
| 建索引＋重播：經過時間 | 5.4 s | 6.0 s |
| 建索引＋重播：CPU | 3.7 s（69%，幾乎都是核心模式） | 4.7 s（78%） |
| 索引本身 | 63.8 MB | 33.6 MB |
| 建完後的工作集 | 55 MB（建索引時峰值 128 MB） | 39 MB（峰值 102 MB） |
| 監看 300 秒：CPU | 0.141 s（0.05%） | 0.062 s（0.02%） |
| 監看期間的記憶體變化 | 工作集 +0.4 MB | 工作集 +0.2 MB |
| 全新建立（測試用，此時有兩份索引） | 3.9 s，CPU 93%，峰值工作集 179 MB | 5.0 s，CPU 94%，峰值 136 MB |

- 平常監看的成本幾乎可以忽略。**成本集中在整顆建索引**：約 4～6 秒裡大致佔滿一個核心，而且大多是核心模式，也就是時間花在檔案系統處理 `FSCTL_QUERY_FILE_LAYOUT` 上。
- 建索引時的記憶體峰值大約是索引本身的 2 倍，建完後會降下來。
- I/O 計數：建索引時 other ops 約 1,500 次、519 MB（C:），read／write 都是 0。依文件無法確定這代表什麼（見上一節），所以不拿來判斷磁碟負載。

**逐檔比對**：D: 完全相同。C: 有 42 個檔案不同，合計 −1,095,081 B：

| 組別 | 檔案數 | 位元組差異總和 | 判斷 |
|---|---|---|---|
| 從沒出現在日誌 | 33 | 1,107,307 | 建索引前就開著、一直在寫的檔案 |
| 出現過、已關閉 | 8 | 1,024,762 | 比較時間差（見下） |
| 仍在追蹤 | 1 | 894 | 比較時間差 |

- **從沒出現在日誌的 33 個**：全部是記錄檔、`-wal`、`.etl`、`.edb`、`.LOG2`、`.trc`、leveldb 檔案，而且除了一個被截短的檔案以外，都是更新後的索引比較小。這證實了上一次的推測：剩下的差異來自建索引之前就開著的檔案，日誌裡沒有它們的記錄。
- **出現過、已關閉的 8 個**：多半是同一個 MFT 記錄號、序號不同的成對項目，也就是檔案被刪除後重建（例如「寫入暫存檔再改名覆蓋」）。全新建立的是舊序號，更新後的索引是新序號，而且舊序號已經無法開啟（error 87）。這是比較方法本身的時間差：全新建立先完成，最後一次追日誌才在之後套用了這段時間的變動。這不是增量更新的錯誤。整顆的「檔案 −1」也是同樣的原因。
- **結論**：增量更新在日誌看得到的範圍內是正確的。看不到的只有建索引前就開著的檔案，要靠定期重掃，或是依第 4 階段的構想，把重掃前後大小不同的檔案加進追蹤清單。

### 2026-11-05 第 3 階段第 4 步：快取檔

設計與實作細節見 [DESIGN-phase3.md](DESIGN-phase3.md) 第 6 節。

- **新增**：
  - `crc32`：依 PNG 規格第 5.5 節的描述自己實作，沒有參考範例程式碼；
  - `index_cache`：格式與驗證，純位元組處理；
  - `cache_store`：位置、權限檢查、寫入與讀取；
  - `NtfsLiveVolume::FromCache` 和 `SaveCache`；
  - CLI `cache save|load <磁碟>` 和 `scripts/cache.ps1`。
- **查證**：
  - `icacls C:\ProgramData`：SYSTEM 和 Administrators 有完整權限，`CREATOR OWNER` 有完整權限（只繼承給子項目），`BUILTIN\Users` 可以讀取、執行，也可以建立資料夾和檔案。所以一般使用者可以搶先建立 `DirSize` 資料夾，成為它的擁有者。已用上述權限檢查處理。
  - CRC-32 的對照值用 Python 的 `zlib.crc32` 算出，因為 PNG 規格本身沒有提供測試向量；zlib 用的是和 PNG、gzip 相同的 CRC（RFC 1952）。`"123456789"` 是 `0xCBF43926`。
  - `GetSecurityInfo` 的文件沒有說明 NULL DACL 和沒有 DACL 的差別。程式遇到 NULL 一律拒絕，兩種情況都安全。
- **測試**：`live-tests` 從 99 項增加到 5,412 項。多出來的大多是「每一種截斷長度」和「每一個位元組改壞」都必須被拒絕。
  - **往返測試**：接進隨機等價測試。每個種子跑到第 150 批時存檔再載入，結果必須相同，再存一次的位元組也要完全相同；之後改用載入的索引繼續套用變動，60 個種子都通過。
  - **結構檢查**：先改壞內容、再重算檢查碼，這樣才測得到結構檢查本身，否則所有錯誤都只會被檢查碼擋下。案例包括上層超出範圍、循環、ID 順序、連結數不符、名稱超出範圍、保留欄位、溢位、版本、筆數乘法溢位。
  - **突變檢查**：一次拿掉一項檢查，看有沒有測試因此失敗。第一次有 4 項沒被抓到，補了測試之後，21 項都會讓測試失敗。其中一項是測試本身寫錯檔頭位移，改用具名常數，並加上 `static_assert` 確認。
  - **資料夾權限檢查**：用一般權限做得出來的情況都測了：
    - 不存在的路徑：回報「沒有快取」；
    - 檔案：不是資料夾，拒絕；
    - junction：重新導向點，拒絕；
    - 自己建立的資料夾：拒絕；
    - `ProgramData` 本身：擁有者是 SYSTEM，但 DACL 允許 Users，只有 DACL 檢查會拒絕它。

    同樣做了突變檢查，5 項檢查拿掉任何一項，都會有測試失敗。會「通過」的資料夾要用提權才建立得出來，所以這部分由實機測試涵蓋。
  - ASan 版也通過。
  - 隨機測試的「files changed」統計從 35,872 變成 35,908，原因是載入的索引已經整理過，沒有標記為刪除的資料夾。日誌之後再提到這些 ID 時，會被分類成檔案。結果不受影響，每一批都和全新建立逐項比對。

#### 快取實測：存檔 → 載入 → 重新開機 → 載入（2026-11-05，提權）

| 項目 | C: | D: |
|---|---|---|
| 快取檔大小 | 42.6 MB | 29.2 MB |
| 存檔（整理＋寫入並清到磁碟） | 0.40 s | 0.20 s |
| 載入（讀檔＋驗證＋重算合計） | 0.15 s | 0.10 s |
| 同一次開機內：補上存檔後的變動 | 0.014 s（450 個 ID） | 0.01 s |
| 重新開機後：補上關機、開機期間的變動 | 0.62 s（8,293 個 ID，28,491 筆記錄） | 0.34 s（3,311 個 ID） |
| 對照：開機後立刻整顆重建 | 15.6 s（CPU 54%） | 20.2 s（CPU 51%） |

- 一般權限讀取 `C:\ProgramData\DirSize` 被拒絕，資料夾權限有效。
- 「載入 → 存檔 → 再載入」：第二次載入讀到的位置，正好是第一次載入後存檔的位置。
- **重新開機後日誌接得上**：關機、開機這段期間的記錄都還在，沒有要求整顆重掃。之前「關機期間不會用掉日誌」只是依文件推論，這次實測確認了。
- 開機後立刻整顆重建比平常慢 3～4 倍（平常 4～5 s），因為磁碟正忙。有快取時，開機只需約 0.5～0.8 秒。
- 同一次開機內載入，D: 和全新建立完全相同。C: 差約 0.2～0.3 MB，都是「從沒出現在日誌」、一直開著寫的檔案，和 watch 的結論相同。

#### 找到的 bug：同一個 handle 第二次列目錄得到空清單

- **現象**：重新開機後載入，C: 少了 36,598,390,784 B，正好是根目錄 4 個檔案的大小：`hiberfil.sys`、`pagefile.sys`、`swapfile.sys`、`DumpStack.log.tmp`。四個都是開機時由系統打開、一直不關的檔案。
- **診斷**：加了兩項輸出：
  - 追日誌時，列出重查沒有回傳、日誌也沒有刪除記錄的 ID；
  - 比對時，對「更新後的索引沒有、全新建立有」的檔案，當場用 ID 重查一次，並檢查根目錄清單。

  結果：用 ID 重查**找得到**這 4 個檔案，大小、上層、名稱都正確，但根目錄清單裡**沒有**它們。
- **原因**：`NtfsLiveVolume` 重複使用同一個根目錄 handle 列清單。[FILE_INFO_BY_HANDLE_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class) 寫明：同一個 handle 再呼叫，會接著上次列完的位置繼續。所以從第二次追日誌開始，根目錄清單都是空的，被重查到的根目錄檔案依規則都不算。這 4 個檔案一直開著，`OpenFileTracker` 每次都會重查它們，所以第二次追日誌時就被拿掉了。全新建立每次都新開 handle，所以不受影響。
- **修正**：`DirectoryLister::List` 的第一次呼叫改用 `FileIdBothDirectoryRestartInfo`（或 `FileFullDirectoryRestartInfo`），文件寫的是「強制從頭開始列」。
- **測試**：新增 `TestListSameHandleTwice`：同一個 handle 列 300 個檔案的資料夾 5 次。修正前，除了第一次之外都是 0 個；修正後每次都是 300 個。`core-tests` 有 434 項檢查。
- **為什麼之前的測試沒抓到**：`live-tests` 的根目錄清單是模擬的，沒有用到真的 handle。watch 實測時，根目錄的檔案剛好很少變動。

#### 修正後再驗證：存檔 → 重新開機 → 載入（2026-11-05，提權）

| 項目 | C: | D: |
|---|---|---|
| 載入 | 0.19 s | 0.10 s |
| 補上關機、開機期間的變動 | 0.66 s（7,905 個 ID，26,027 筆記錄） | 0.32 s（9,123 個 ID） |
| 需要整顆重掃 | 沒有 | 沒有 |
| 維護的合計 vs 從頭重算 | 0 個資料夾不同 | 0 個資料夾不同 |
| 和全新建立比較 | 檔案 +3、資料夾 −1、位元組 +9,972,644 | 檔案 +1，位元組 0 |

- **根目錄那 4 個檔案不再遺失**：整顆的差異從 −36.6 GB 變成 +10 MB。
- C: 剩下的 71 個不同檔案：
  - 從沒出現在日誌：0 個；
  - 出現過、已關閉：54 個；
  - 仍在追蹤：17 個。

  其中「全新建立有、更新後沒有」的檔案，當場用 ID 重查時都已經不存在了，同一個 MFT 記錄號已換成更新的序號，也就是檔案已被刪除或取代。這是比較方法本身的時間差：開機後全新建立要 8 秒，期間磁碟一直在變，之後最後一次追日誌又套用了 748 個變動。更新後的索引反映的是比較晚的狀態。
- 「重查沒有回傳、日誌也沒有刪除記錄」的 ID，兩顆各 1 個。日誌原因含 `FILE_CREATE` 和 `CLOSE`，判斷是讀完日誌到重查之間，檔案就被刪除了，它的刪除記錄會在下一次追日誌時讀到。

### 2026-11-05 第 4 階段查證與設計草案

設計草案見 [DESIGN-phase4.md](DESIGN-phase4.md)。以下是查文件得到、會影響設計的事實：

- **服務**：
  - 主執行緒要儘快呼叫 `StartServiceCtrlDispatcherW`，文件寫的是 30 秒內；控制處理函式也要在 30 秒內返回。
  - SHUTDOWN 預設約 20 秒可以收尾；PRESHUTDOWN 會先收到，可以設定逾時時間。
  - 延遲自動啟動只保證在其他自動啟動的服務之後「再加一小段延遲」，沒有確切時間。
  - LocalSystem 有 SeBackupPrivilege，但預設是停用；LocalService 的權限清單裡沒有它。
  - `SERVICE_REQUIRED_PRIVILEGES_INFO` 可以讓 SCM 在啟動時移除不需要的權限。
  - `SERVICE_SID_TYPE_RESTRICTED` 會讓 token 變成寫入受限。
- **Named pipe**：
  - 預設的安全描述元會讓 Everyone 和匿名帳戶都能讀，所以要自己指定權限。
  - `FILE_FLAG_FIRST_PIPE_INSTANCE`：如果名稱已經被建立過，會回傳 `ERROR_ACCESS_DENIED`，可以用來防止名稱被搶先佔用。
  - `PIPE_REJECT_REMOTE_CLIENTS` 會自動拒絕遠端連線。
  - `ImpersonateNamedPipeClient` 失敗時，之後的動作會用服務自己的身分執行。服務是 SYSTEM，所以必須「失敗就拒絕」。
  - 冒充等級由前端決定，用 `CreateFileW` 的 `SECURITY_SQOS_PRESENT` 旗標指定。named pipe 預設是 SecurityImpersonation。
  - 第一次抓 `CreateNamedPipeW` 的頁面時，舊網址（`winbase`）回傳 404，正確的是 `namedpipeapi`。
- **資源**：
  - Job Object 的 CPU 硬上限，單位是每 10,000 個週期可用幾個，例如 20% 就設 2,000。**文件沒寫**的有：百分比以整台電腦還是單一核心計算；行程能不能把自己放進 job。
  - `PROCESS_MODE_BACKGROUND_BEGIN` 只能用在目前的行程，文件說和使用者互動的行程不應該用。
  - `THREAD_MODE_BACKGROUND_BEGIN` 只能用在目前的執行緒，並提醒要避免和其他執行緒共用資源，否則會發生優先順序反轉。
  - EcoQoS 用 `SetProcessInformation(ProcessPowerThrottling)` 開啟。
- **閒置偵測**：
  - `GetLastInputInfo` 只回報呼叫者自己 session 的輸入，服務在 session 0 用不到。
  - 服務可以用 `RegisterPowerSettingNotification` 搭配 `DEVICE_NOTIFY_SERVICE_HANDLE`，接收 `GUID_GLOBAL_USER_PRESENCE` 和 `GUID_ACDC_POWER_SOURCE`。文件寫明，session 0 的服務要用 GLOBAL 版本。
- **待實測**（文件沒說明）：
  - 識別等級的 token 能不能用在 `AccessCheck`。文件只說要傳 impersonation token，沒提到冒充等級；另外，`OpenThreadToken` 的 `OpenAsSelf` 可以在識別等級下取得 token。
  - 建索引和讀日誌是否需要 SeBackupPrivilege。

#### 實測程式 `tools/service_probe.cpp`（CMake target `service-probe`，不出貨）

**access**（一般權限，同一個使用者當服務端；2026-11-05）：

| 前端的冒充等級 | 用呼叫者身分開資料夾 | `AccessCheck`（讀資料夾的安全描述元後判斷） |
|---|---|---|
| SecurityImpersonation（預設） | 有權限：打開；DACL 拒絕：錯誤 5 | 允許、拒絕都正確 |
| SecurityIdentification | 一律失敗，錯誤 1346（`ERROR_BAD_IMPERSONATION_LEVEL`） | **允許、拒絕都正確** |

- 文件沒說明的一點，實測確認了：識別等級的 token（用 `OpenThreadToken` 的 `OpenAsSelf = TRUE` 取得）可以用在 `AccessCheck`。
- 識別等級下開檔會直接失敗，不會退回用服務自己的身分開。
- 一般權限讀不到 `ProgramData\DirSize\cache` 的安全描述元（錯誤 5），所以這一格沒有 `AccessCheck` 的結果。真正的服務以 SYSTEM 執行，到時候再測。

**backup**（提權，C:）：停用和啟用 SeBackupPrivilege 時，建索引（約 3.5 s）和讀日誌都成功，資料夾數相同，存取被拒都是 0；檔案數差 1，是兩次之間有檔案變動。**結論：服務不需要 SeBackupPrivilege。**

**cpu**（提權，C:，20 個邏輯處理器）：

- **Job 的 CPU 上限是以整台電腦計算**：
  - 第一次以整台電腦的百分比設定，50%、25%、10% 都限制不到單一執行緒：10% 下還能用到單一核心的 98%，建索引時間不變。
  - 建索引只用一個執行緒，所以本來就最多一個核心，在這台電腦上是 5%。
  - 把上限設成 0 來關閉（`ControlFlags = 0`）會回傳錯誤 87，所以第一次標成「不限」的那次建索引，其實還套著前一次的 10% 上限。這不影響結論，因為 10% 本來就限制不到單一執行緒。
  - 行程可以把自己放進 job（`AssignProcessToJobObject(GetCurrentProcess())` 成功），這點文件沒寫。
- **第二次以「單一核心」為單位設定**，`CpuRate = 比例 × 10000 ÷ 處理器數`。小工作在另一個執行緒上每 20 ms 量一次：

| 上限 | 單一執行緒實際分到 | 建索引經過時間 | 建索引 CPU 時間 | 小工作延遲 p99 | 最大 |
|---|---|---|---|---|---|
| 不放進 job | 94.8% | 4.03 s | 3.77 s | 0.14 ms | 0.14 ms |
| 1 個核心（`CpuRate` 500） | 94.8% | 6.45 s | 6.09 s | 0.32 ms | 0.40 ms |
| 1/2 個核心（250） | 56.8% | 12.35 s | 6.41 s | 258 ms | 311 ms |
| 1/4 個核心（125） | 27.6% | 22.60 s | 5.56 s | 421 ms | 510 ms |

- 上限真正卡住時（1/2、1/4 核心），**同一個行程的其他執行緒也會停住**：小工作最多延遲 0.3～0.5 秒。所以如果要設低於 1 個核心的上限，回答查詢的執行緒會跟著被拖慢。
- 1 個核心的上限下，建索引的 CPU 時間從 3.8 s 增加到 6.1 s，經過時間從 4.0 s 增加到 6.5 s。原因還沒查明。

#### 決定與第 2 步：pipe 協定與存取檢查（2026-11-05）

- **決定**（都採用 A，詳見 DESIGN-phase4.md 第 8 節）：
  - 8.1 呼叫者看得到子資料夾的大小，打不開的標「沒有權限」；
  - 8.2 所有本機固定式 NTFS 磁碟自動建索引；
  - 8.3 重掃後，把大小不同的檔案加進追蹤清單；
  - 8.4 整個服務 CPU 上限 1 個核心；
  - 8.5 前端用識別等級連線，服務用 `AccessCheck` 判斷。
- **新增**：
  - `src/service/protocol`：訊息格式與編解碼，純位元組處理；
  - `src/service/access`：
    - 路徑驗證；
    - 從 pipe 取得呼叫者的 token，任何一步失敗都拒絕，`RevertToSelf` 失敗就結束行程；
    - 以服務身分開資料夾，再用 `AccessCheck` 判斷能不能列出；
    - 用 `GetFinalPathNameByHandleW(VOLUME_NAME_GUID)` 取得 handle 所在的磁碟區。
- **測試** `service-tests`（154 項，不需要提權）：
  - 協定：往返；每一種截斷長度和多一個位元組都要被拒絕；每一項長度和範圍檢查都有對應的案例。
  - 路徑：接受和拒絕的各種寫法，包括 UNC、`\\?\`、`\\.\`、`GLOBALROOT`、相對路徑、`..`、資料流名稱、控制字元。
  - 存取：在同一個行程裡建 pipe，前端分別用識別、冒充、匿名三種等級連線，對「能列出」和「DACL 拒絕列出」兩個資料夾判斷。
- **測試發現的兩件事**：
  - **`C:\\` 被當成根目錄接受**：去掉結尾反斜線時沒有排除這種情況，是我的 bug，已修正。
  - **還沒讀任何訊息，就能取得前端的 token**：文件只說會冒充「最後讀到的那則訊息」的安全內容，沒說沒讀過訊息時會怎樣。實測可以取得，等級也正確，而且就是實際連線進來的前端。測試已改成記錄這個行為。
- **突變檢查**：一次拿掉一項檢查，看有沒有測試失敗。第一次有 3 項沒被抓到：
  - **token 等級檢查**：匿名等級在 `OpenThreadToken` 就失敗了，所以這項檢查走不到。改成抽出 `UsableClientToken` 直接測試。
  - **路徑長度上限**：原本的測試案例被別的檢查先擋下。補了一個剛好超過上限、資料長度也正確的案例。
  - **`.`、`..` 的檢查**：「結尾不能是句點」的規則已經包含這兩種情況，是預期中的重複檢查，保留著讓意圖清楚。

  其餘 24 項，拿掉任何一項都會有測試失敗。

#### 第 3 步：服務本體（2026-11-05）

- **新增**：
  - `src/service/volume_worker`：每顆磁碟一個執行緒，負責：
    - 啟動時載入快取並追日誌，沒有可用快取就重掃；
    - 每 5 秒追一次日誌；
    - 回答查詢：子資料夾依磁碟用量排序並分頁；
    - 需要重掃時，在背景模式的執行緒上建新索引，期間用舊索引回答，並標示「更新中」；
    - 重掃完成後，把大小不同的檔案加進追蹤清單（8.3）；
    - 依決定的時機存快取。
  - `src/service/pipe_server`：
    - 4 個 pipe 實例，overlapped I/O，每個等待都有逾時；
    - 每則請求都先檢查呼叫者能否列出那個資料夾（`AccessCheck`），子資料夾也逐一判斷是否標「沒有權限」；
    - 格式錯誤就中斷該連線。
  - `src/service/service_core`：
    - Job Object 上限 1 個核心，加上 EcoQoS；
    - 列出本機固定式 NTFS 磁碟；
    - 日誌寫在私有的 `ProgramData\DirSize\service.log`，超過 1 MB 時保留一份舊檔。
  - `src/service/service_main`（`dirsize-service`）：
    - 以服務執行，接受 STOP 和 PRESHUTDOWN；
    - `console` 主控台模式；
    - `install`：把執行檔複製到 `Program Files\DirSize` 再註冊，因為服務以 SYSTEM 執行，執行檔不能放在一般使用者寫得到的地方。註冊時設定：
      - 延遲自動啟動；
      - 失敗時 1 分鐘後重啟，最多兩次；
      - 必要權限只有 SeChangeNotifyPrivilege；
      - 服務 SID 用 UNRESTRICTED；
    - `uninstall`。
  - `src/client/pipe_client`：以識別等級連線，送出請求前確認 pipe 的伺服器行程就是 SCM 回報的 DirSize 服務。
  - CLI `status`、`query <資料夾>`，以及 `scripts/service-test.ps1`。
- **不採用的設計**：
  - 中斷連線前不呼叫 `FlushFileBuffers`：文件寫明它會等到前端讀完，故意不讀的前端就能卡住一個實例。改成寫完回應後，等前端關閉或逾時。
  - 不降低記憶體優先順序：索引一直在用，被調出記憶體反而會讓查詢變慢。
- **一般權限的冒煙測試**：以一般權限執行時打不開磁碟，所以所有磁碟都被略過，但 pipe 和權限檢查都測得到：
  - 服務沒有安裝時，一般的前端拒絕連線（錯誤 1060，服務不存在）。
  - 用 `--console` 測試旗標時：沒有索引的磁碟回「沒有索引」；`ProgramData\DirSize` 和 `System Volume Information` 回「沒有權限」；UNC、裝置路徑、`..` 回「路徑不接受」；不存在的路徑回「找不到」。
  - 已經有一個服務在執行時，第二個行程建立 pipe 會失敗，於是拒絕啟動。這次的錯誤是 231（所有實例都在使用中）。
- **測試發現**：一開始連第一個服務行程也建立失敗，錯誤 5。文件寫明，建立第二個以後的實例時，DACL 必須允許呼叫者 `FILE_CREATE_PIPE_INSTANCE`。以一般權限執行時 Administrators 群組不生效，而我刻意不給 Authenticated Users 這個權限。修正：DACL 加上擁有者（`OW`）的完整權限。以服務執行時擁有者是 SYSTEM，所以沒有放寬。記錄也改成分別寫出是第幾個實例失敗。
- **審查後修正**（提權實測前）：
  - **8.3 的前提不一定成立**：「重掃前後大小不同的檔案多半是一直開著的檔案」，只有在重掃前的索引是最新的時候才成立。但依第 5 節，重掃大多發生在索引已經過時之後，例如日誌接不上、變動太多、快取不能用，這時大小不同的多半是這段期間真的改過的檔案。追蹤的檔案每 5 秒都會重查，數量一多就會一直耗 CPU；實測 1 萬個 ID 約 0.4 秒。修正：
    - 大小不同的檔案超過 500 個，就判斷重掃前的索引已過時，全部不加進追蹤清單；
    - 追蹤清單整體上限 1,000 個，超過的捨棄，並寫進記錄；
    - 追蹤的檔案不再計入「變動太多就重掃」的判斷。
  - **重掃沒有頻率限制**：大量變動期間可能一次接一次地重掃。修正：連續重掃時，下一次至少要等 1、2、4、8 分鐘，之後固定 10 分鐘；穩定 30 分鐘後重新計算。等待期間繼續用舊索引回答，並標示「更新中」。
  - **關機時可能來不及存檔**：原本先等正在進行的重掃結束才存檔，而且一顆磁碟停好才停下一顆。剛開機時重掃要 8～20 秒，PRESHUTDOWN 預設只有 10 秒。修正：先通知所有磁碟停止；各磁碟先存檔，再等重掃結束。
  - **第一次建索引期間查詢回「找不到」**：改回「忙碌」，前端才不會把正常的啟動當成資料夾不存在。
  - **拒絕的原因要分得出來**：服務自己打不開資料夾，和 `AccessCheck` 拒絕，分別寫進記錄。一般權限的冒煙測試中，被拒絕的資料夾其實是服務自己打不開，`AccessCheck` 沒有跑到；要等提權實測才看得到 `AccessCheck` 拒絕。
  - **前端要求的 pipe 權限**：懷疑 `GENERIC_WRITE` 包含 `FILE_APPEND_DATA`，而這個位元在 pipe 上就是 `FILE_CREATE_PIPE_INSTANCE`，也就是我刻意不給 Authenticated Users 的權限。冒煙測試時前端剛好是 pipe 的擁有者，所以測不出來。新增測試：pipe 只給目前使用者 `0x0012019B`（和 Authenticated Users 相同），不給擁有者權限。**實測 `GENERIC_READ | GENERIC_WRITE` 和指定權限都能連線**，所以懷疑不成立。前端仍然只要求實際用到的權限，測試保留。
  - 寫這個測試時，測試裡的伺服器在「前端在 `ConnectNamedPipe` 之前就關閉」時直接結束，造成後面的連線等不到實例。這是測試程式的問題；正式的伺服器遇到這種情況會先中斷連線再繼續等。
- **已知限制**：本機程式只要每隔幾秒送一次請求，就能佔住 4 個實例，讓其他使用者暫時連不上。這只會讓服務暫時無法回應，不會耗 CPU。
- `service-tests` 增加到 209 項。

#### 主控台模式實測（2026-11-05：服務在系統管理員視窗，查詢在一般使用者視窗）

- **啟動**：CPU 上限 1 個核心（`CpuRate` 500，20 個邏輯處理器）；FAT32 的隨身碟被略過；2 顆 NTFS 磁碟都有索引。
  - C:：載入快取後補上存檔以來的 43,484 個變動，約 2 秒後就緒。
  - D:：存檔以來的變動超過 5 萬個，所以整顆重掃。重掃花 19.2 s（平常 5～7 s），完成後存快取；有 27 個檔案前後大小不同，加進追蹤清單。依設計，重掃在背景優先順序執行，加上 CPU 上限 1 個核心，所以會比較慢。但各因素各佔多少，沒有量過。
- **查詢**（一般使用者，透過 pipe）：
  - `C:\` 5.7 ms；`C:\Users` 2.4 ms；`C:\Windows` 4.2 ms（82 個子資料夾的前 10 個）；`D:\` 3.2 ms。
  - `System Volume Information`、`Config.Msi`、`PerfLogs`、一個服務帳戶的設定檔資料夾：有大小，標示「沒有權限」，符合 8.1。
  - 使用者自己的設定檔、`Default`、`Public` 可以查詢。
- **負面測試**：
  - `C:\ProgramData\DirSize`：**被 `AccessCheck` 拒絕**。服務打得開這個資料夾，但用呼叫者的 token 判斷後拒絕，記錄是 `query refused by AccessCheck for the caller`。
  - `System Volume Information`：服務自己打不開。主控台模式以系統管理員身分執行，不是 SYSTEM。
  - UNC、`\\?\` 裝置路徑、`..`：都回「路徑不接受」。
- **停止**（Ctrl+C）：兩顆磁碟都存了快取，在 0.3 s 內完成。
- **發現**：D: 的記錄寫「cache loaded, 0 changes since the save」，緊接著卻因為變動太多而重掃。原因是讀日誌時，一超過 5 萬個變動就提早結束，變動數還沒填進結果。已修正：提早結束時也填入當時的數量，記錄改成「at least N changed IDs」並寫出原因。

#### VM 實測：安裝成服務（2026-11-05）

測試機是乾淨的 VM：4 個邏輯處理器，一顆 C:（約 49 GB、46 萬個檔案）。用 `scripts/package-test.ps1` 產生的測試包安裝，不是正式的安裝程式。

| 項目 | 結果 |
|---|---|
| 服務設定（`sc qc`） | `AUTO_START (DELAYED)`，`LocalSystem`，執行檔在 `Program Files\DirSize` |
| 必要權限（`sc qprivs`） | 只有 `SeChangeNotifyPrivilege` |
| 失敗時（`sc qfailure`） | 重新計算 86400 s；`RESTART` 60000 ms 兩次 |
| CPU 上限 | `CpuRate` 2500，4 個邏輯處理器下正好 1 個核心 |
| 第一次啟動（沒有快取） | 整顆建索引 10.2 s，接著存快取 27 MB |
| 重開機兩次 | 關機前都存了快取；開機後都是從快取載入，0.1～0.25 s，分別補上 2,575 和 85 個變動 |
| 查詢（一般使用者，不加 `--console`） | 0.4～1.2 ms |
| 資源 | CPU 累計 0.11 s，工作集 43 MB，峰值 76 MB |

- **以 SYSTEM 身分、只保留 SeChangeNotifyPrivilege**，建索引和讀日誌都正常。之前「不需要 SeBackupPrivilege」是在開發機上以系統管理員身分測的，這次以真正的服務身分確認了。
- **前端確認伺服器身分**：不加 `--console` 時，前端比對 pipe 的伺服器行程和 SCM 回報的服務 PID，一致才查詢。這次第一次在真正的服務上測到，通過。
- **負面測試**：`C:\ProgramData\DirSize` 和 `System Volume Information` 都被 **`AccessCheck`** 拒絕。服務以 SYSTEM 執行時兩個都打得開，但會用呼叫者的 token 判斷後拒絕。UNC、裝置路徑、`..` 都回「路徑不接受」。VM 沒有 D:，所以查 `D:\` 回「找不到」。
- 有一次在系統管理員視窗執行測試，全部得到錯誤 2（找不到 pipe）。那時是重開機後、延遲啟動之前，服務在 1 分多鐘後才啟動。第 5 階段的前端遇到這種情況，要顯示「服務啟動中」，不能當成錯誤。

### 2026-11-05 第 5 階段查證與設計草案

設計草案見 [DESIGN-phase5.md](DESIGN-phase5.md)。使用者的需求：

- 除了獨立視窗，還要一個**跟著檔案總管的面板**：貼在檔案總管視窗旁邊，跟著目前資料夾切換。
- 面板的佈景主題要跟著系統設定。

查證結果（**沒有官方文件的部分要先實測**）：

- **有文件**：
  - 用 `IShellWindows` 列出檔案總管視窗，用 `SWC_EXPLORER` 區分出檔案總管；
  - 取得視窗目前資料夾：`IShellBrowser::QueryActiveShellView` → `IFolderView::GetFolder` → `IPersistFolder2::GetCurFolder`，最後得到絕對 PIDL；
  - 跟著視窗：`SetWinEventHook` 的前景、位置、縮到最小事件；`DWMWA_EXTENDED_FRAME_BOUNDS` 取得實際看得到的外框；
  - 主題：`UISettings` 和 `ColorValuesChanged`；高對比用 `SPI_GETHIGHCONTRAST`。
- **沒有文件**：
  - 檔案總管切換資料夾時會送出的事件：`DWebBrowserEvents2` 是 IE 的文件，沒有提到檔案總管；
  - **Windows 11 檔案總管的分頁**：找不到任何官方文件；
  - 跨行程 owned window 的注意事項；
  - ListView、TreeView 這些內建控制項的深色模式。
- **有限制**：
  - `DWMWA_USE_IMMERSIVE_DARK_MODE` 從 Windows 11 build 22000 起才支援，所以 Windows 10 上的標題列一律是淺色；
  - Windows 11 新的頂層右鍵選單，要用 `IExplorerCommand` 加上 sparse package，而且 package 必須用目標電腦信任的憑證簽章；
  - 傳統登錄動詞在 Windows 11 上，依文件推論會出現在「顯示更多選項」裡。
- **以下頁面打不開**（404）：IWebBrowser2 的 Document 屬性、`ShellFolderView` 物件頁、`DShellFolderViewEvents`、高對比模式總覽。

### 2026-11-05 第 5 階段前端：共用元件與第一版獨立視窗

- **建置確認**：C++/WinRT 用的是 Windows SDK 10.0.26100.0 內附的 `cppwinrt` 標頭檔。在 `/MT`、`/W4`、`/permissive-`、C++20 下可以編譯，沒有警告，連結 `WindowsApp.lib`。`<unknwn.h>` 放在 winrt 標頭之前。
- **主題**：
  - 依「Support Dark and Light themes in Win32 apps」：讀 `UISettings` 的前景色，用文件裡的 `IsColorLight` 公式判斷；前景色是亮色就是深色模式。
  - 切換通知用 `ColorValuesChanged`。文件沒說事件在哪個執行緒觸發，所以處理函式只 `PostMessage`，介面執行緒收到後重新讀取。
  - 文件沒有寫 `DWMWA_USE_IMMERSIVE_DARK_MODE` 支援哪個 build（DWMWINDOWATTRIBUTE 頁寫的是 Windows 11 build 22000 起），所以直接呼叫，失敗就忽略。
  - 高對比用 `SPI_GETHIGHCONTRAST` 和 `GetSysColor`。比例長條和百分比分開畫，不在長條上疊字。
- **DPI**：依「Setting the default DPI awareness for a process」，用 manifest 宣告 PerMonitorV2，不用 API。manifest 的 `supportedOS` 區段沒有加，因為 GUID 沒有從文件確認過，程式也不需要。
- **純邏輯**放在 `dirsize_ui_core`，由 `ui-tests` 測試：
  - 大小與數字格式；
  - 排序：`CompareStringEx` 加 `SORT_DIGITSASNUMBERS`，所以 file2 排在 file10 前面；
  - 配色；
  - 面板位置：從實測程式移植；
  - 面板規則：從實測程式移植，包括「換檢視後原本的選取當基準」。
- **逐目錄掃描可以取消**：
  - `WalkTree` 加上 `cancel` 參數，每開一個資料夾前檢查一次，core-tests 加了測試；
  - 前端換資料夾時就取消上一次的掃描；
  - 掃描期間，執行緒進入背景模式（`SetThreadPriority` 的 `THREAD_MODE_BACKGROUND_BEGIN`），文件說這會降低資源排程優先順序。
- **資料來源**（`FolderSource`，在工作執行緒）：
  - 先問服務，分頁取完全部子資料夾；
  - 服務回 `NotIndexed`、`Busy`、`PathRejected`，或連不上時，改成逐目錄掃描；
  - 連不上時用 SCM 分辨「未安裝」「已停止」「設成自動啟動但還沒啟動」；
  - 檔案直接用使用者自己的權限列出。
- **第一版獨立視窗**（`dirsize.exe [資料夾]`）：
  - 上方是路徑和狀態列，下方是自繪清單（名稱、大小、磁碟用量、比例、檔案數），點欄位標題排序；
  - Enter 進入，Backspace 上一層，Ctrl+O 選擇資料夾（`IFileOpenDialog`），F5 重新整理；
  - 所有介面文字都在 `.rc` 字串表裡。
- **開發機截圖檢查**（深色模式，未安裝服務，逐目錄掃描）：版面、深色標題列、比例長條都正常。截圖發現 `FormatCount` 在位數不是 3 的倍數時逗號放錯，例如「3,9,724」，原因是無號整數相減下溢。已修正，並補上 5 位數、2 位數等測試。ui-tests 75 項通過。
- **面板**（`dirsize.exe --panel`）：
  - `ExplorerTracker`：在自己的 STA 執行緒上執行，邏輯從實測程式移植。
    - 檔案總管在前景時，每 250 ms 檢查一次；
    - 記住目前的檢視，每 2 秒或檢視不見、前面分頁換了時，才重新列出；
    - Windows 11 分頁用 z-order 判斷；
    - 遇到 `RPC_E_DISCONNECTED`、`RPC_S_SERVER_UNAVAILABLE`、`CO_E_OBJNOTCONNECTED`，就丟掉 COM 物件，下次重新建立，處理檔案總管重新啟動的情況；
    - 結束時最多等 2 秒，等不到就不等了。
  - 面板規則用 `PanelRules`，已有單元測試。只有規則的結果改變時，才通知介面執行緒。
  - 面板視窗：
    - `WS_POPUP` 加 `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，不設 owner（VM 實測的結論）。點面板不會把檔案總管切到後面。
    - 位置用 `PlacePanel`；貼在內側時，每 250 ms 確認自己在檔案總管上面。
    - WinEvent 只收檔案總管那個行程的位置事件。
    - 清單只列子資料夾（名稱、磁碟用量、比例），用精簡欄寬。
  - 開發機上的自動檢查沒有完成：用程式開檔案總管視窗時，它沒有變成前景視窗，前景是另一個程式，所以面板照規則沒有出現。檢查完就關掉了那個測試用的視窗。要由使用者實際操作驗證。
- **使用者要求的三項**（2026-11-05）：
  - **圓角**：依「Apply rounded corners in desktop apps for Windows 11」，沒有外框的視窗不會自動變圓角，要用 `DWMWA_WINDOW_CORNER_PREFERENCE` 設 `DWMWCP_ROUND`。文件說這只是給系統的提示，而且**在 VM 裡、或視窗最大化、貼齊時不會變圓角**。DWMWINDOWATTRIBUTE 頁寫明 Windows 11 build 22000 起支援。
    - 設定成功時，外框交給 DWM 畫，用 `DWMWA_BORDER_COLOR` 設成主題的分隔線顏色；
    - 設定失敗時（Windows 10），面板維持自己畫的 1 px 直角外框。
  - **雙擊導航**：面板把路徑交給追蹤執行緒，由它用 `SHParseDisplayName` 和目前分頁的 `IShellBrowser::BrowseObject(SBSP_SAMEBROWSER | SBSP_ABSOLUTE)` 讓同一個檔案總管視窗切換資料夾；`SHParseDisplayName` 的文件要求在背景執行緒呼叫。下一次檢查看到新的檢視後，面板就跟著顯示新資料夾。
  - **調整大小**（使用者選了寬度和高度都可以調、要記住）：
    - `WS_THICKFRAME` 加上 `WM_NCCALCSIZE`（不顯示外框）；`WM_NCHITTEST` 只讓離檔案總管較遠的那一邊和下緣可以拖。清單在這些邊緣回傳 `HTTRANSPARENT`，交給面板處理。
    - 拖動期間不跟著檔案總管移動；放開後，以 DIP 為單位存進 `HKCU\Software\DirSize` 的 `PanelWidth`、`PanelHeight`。
    - 高度拉到跟檔案總管一樣高時存成 0，代表恢復成跟著檔案總管的高度。
    - 寬度限制 240～900 DIP，高度至少 160 DIP。
    - 位置的純函式加了高度參數和「哪一邊調寬度」的規則，ui-tests 81 項通過。
- **使用者在開發機（Windows 11）實測面板**：圓角與外框、雙擊導航、調整大小、面板跟著檔案總管，都正常。

### 2026-11-06 第 5 階段：獨立視窗的資料夾樹

- 前一步已 commit（5cb05f7）並 push。
- **原則：樹的子資料夾 = 清單的資料夾**。清單會叫樹「顯示到這個路徑」（Reveal），如果樹濾掉了清單有的資料夾，就會對不上。所以樹列子資料夾時，用的是服務索引的規則（`core/index.h`）：
  - 略過 `IsReparseTagNameSurrogate` 為真的資料夾 reparse point，也就是 junction、符號連結、掛載點；
  - 其他種類的 reparse point，以及隱藏、系統資料夾，都列出。
  - 標記從 `WIN32_FIND_DATAW::dwReserved0` 讀。文件寫明：有 `FILE_ATTRIBUTE_REPARSE_POINT` 時，這個欄位就是 reparse 標記。
  - **已知的不一致**（之前就存在，這次沒有改）：逐目錄掃描（`WalkTree`）略過**所有**資料夾 reparse point，所以服務不能用時，清單可能少幾個資料夾，例如雲端檔案資料夾。樹列的會比清單多，但不會少，所以 Reveal 不受影響。
- **純邏輯 `TreeModel`**（`dirsize_ui_core`，由 ui-tests 測試）：
  - 根節點是磁碟機；展開時才列子資料夾；同一個資料夾再列一次時，還在的子節點和它們的展開狀態都保留。
  - 名稱排序沿用清單的 `CompareNames`（`CompareStringEx` 加 `SORT_DIGITSASNUMBERS`）；比對名稱不分大小寫（`CompareStringOrdinal`）。
  - Reveal 會展開祖先；還沒列過的祖先，先列出來，等結果回來再繼續。
    - 中間某一層找不到，或列不出來，就停在找得到的最深一層；
    - 網路共用這類不在磁碟機下的路徑，清掉選取；
    - 磁碟機還沒出現在清單裡時就先等，`SetRoots` 帶來那個磁碟機後再繼續。
  - **只有使用者操作才回報「選取改變」**，Reveal 和列出結果都不回報。所以清單 → 樹 → 清單不會互相觸發。使用者一操作，進行中的 Reveal 就結束，晚到的列出結果不會把選取搶走。
- **背景列出 `TreeLoader`**（工作執行緒，生命週期的寫法同 `FolderSource`）：
  - 列資料夾用 `FindFirstFileExW`，加 `FindExInfoBasic`、`FIND_FIRST_EX_LARGE_FETCH`、`FindExSearchLimitToDirectories`。文件說最後這個旗標只是建議，所以每一筆都還是檢查屬性。
  - 要列的路徑排成佇列，同一個路徑不重複排。
  - 磁碟機分兩次送：先送 `GetLogicalDriveStringsW` 的磁碟機代號，再用 `GetVolumeInformationW` 補上標籤。網路磁碟機不查標籤，所以斷線的共用不會卡住列出。
  - 執行緒設 `SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`，空的讀卡機或光碟機只會回傳錯誤，不會跳出系統對話框。
  - **沒有文件依據的假設**：`FindFirstFileExW` 的文件沒有說「找不到符合的項目」時是哪個錯誤碼。磁碟機根目錄沒有 `.` 這一筆，所以空的根目錄可能什麼都找不到。程式把 `ERROR_FILE_NOT_FOUND` 當成「沒有子資料夾」，其他錯誤當成列不出來。
- **自己繪製的樹 `FolderTree`**：
  - 跟 SizeList 一樣，用主題的顏色自己畫；展開符號和資料夾、磁碟機圖示都用線條畫，不用圖片，高對比也看得清楚；
  - 點一下就切換清單；用鍵盤移動時，停 250 ms 才切換，按住方向鍵不會一路載入每個資料夾。按 Enter 立刻切換；
  - 方向鍵左右照檔案總管的習慣：右鍵展開或進到第一個子資料夾，左鍵收合或回到上一層；收合時如果選取在裡面，選取就移到收合的那個資料夾；
  - 有焦點時，選取列加一圈外框，用 Tab 切換時看得出鍵盤現在在哪一邊；
  - Reveal 的目標不在畫面內時，捲到中間。
- **獨立視窗**：
  - 左邊樹、右邊清單，中間的分隔線可以拖（`WM_SETCURSOR`、`SetCapture`、`WM_CAPTURECHANGED`）。寬度以 DIP 記，樹至少 120 DIP，清單至少留 240 DIP；
  - Tab、Shift+Tab 在樹和清單之間切換，視窗重新啟用時，焦點回到原本那一邊；
  - F5 會重新列磁碟機和目前的資料夾；
  - 新增字串 `IDS_DRIVE_LABEL`（"%1 (%2)"），提示列加上 Tab。
- **測試**：
  - ui-tests 175 → 177 項，全部通過；core 448 項、service 209 項也都通過；
  - **突變檢查**，12 種全部抓到。其中「Reveal 不展開已經列過的祖先」一開始沒抓到，補了「祖先已經列過但收合著」的測試才抓到。另一種會讓測試程式當掉，已經改成正常回報失敗；
  - 開發機截圖（深色，沒有安裝服務），用 `C:\Windows\System32\drivers` 啟動：樹自動展開 C:\ → Windows → System32，選取 drivers 並捲到中間；清單內容正確；工作集 15 MB，4 個執行緒，正常關閉。
- **沒做，之後再說**：分隔線寬度不記住；插拔磁碟機時樹不會自動更新（要按 F5）；無障礙（UI Automation）。
- **使用者回報：深色模式下，清單和樹的捲軸還是白色**（系統的 `WS_VSCROLL` 捲軸）。
  - 要讓系統捲軸變深色，只有沒有文件的做法（`SetWindowTheme` 加上未公開的佈景主題名稱），不符合設計第 5 節和「只用公開文件」的規則。
  - 所以改成自己畫捲軸：新增 `ScrollStrip`。它不是視窗，由清單和樹在自己的工作區右側畫，滑鼠和計時器訊息也由它們轉交；兩邊都拿掉了 `WS_VSCROLL`。
    - 拖動滑塊；點軌道就捲一頁，按住不放時，先停 350 ms，再每 60 ms 捲一頁，捲到游標的位置就停；
    - 平常是細的，滑鼠移上去或拖動時變寬、變亮。用 `TrackMouseEvent(TME_LEAVE)` 接收 `WM_MOUSELEAVE`，滑鼠離開時恢復；
    - 顏色是配色新增的 `scrollThumb` 和 `scrollThumbActive`。高對比時只用系統顏色：平常是 `COLOR_WINDOWTEXT`，啟用時是 `COLOR_HIGHLIGHT`；
    - 清單的捲軸從欄位標題下方開始，標題列的底色和分隔線照樣畫到最右邊；
    - 面板用的也是 SizeList，所以面板的捲軸一起改了。
  - 幾何計算 `scroll_geometry` 是純函式，由 ui-tests 測試：滑塊長度、最短長度、捲到最後一頁、拖動換算回列數（四捨五入、超出範圍時夾住，每個位置來回換算都一致）、點擊判斷的邊界。ui-tests 177 → 209 項，全部通過。
  - 開發機截圖（深色）：樹和清單的捲軸都是深色底配灰色細滑塊。拖動、點軌道、滑鼠移上去，要由使用者實際操作確認。
- **使用者回報：面板在點回左邊樹狀窗格後消失**。在右邊點一個資料夾，面板會出現；再點左邊樹狀窗格的資料夾，面板就消失，要等點了別的資料夾才會再出現。
  - 新增診斷參數 `dirsize --panel --trace`：追蹤元件的觀察有變化時，就在 `%TEMP%\DirSize-panel-trace.txt` 寫一行，內容包括焦點所在視窗的類別（`GetGUIThreadInfo`）、選取、檢視和資料夾、規則的決定。紀錄只留在本機。
  - 使用者在開發機（Windows 11）重現一次的紀錄：
    - 焦點從檢視（`DirectUIHWND`）移到 `SysTreeView32` 的同一次檢查，選取從 1 個變成 0 個，面板就隱藏了；
    - 之後約 8 秒都沒有導航。追蹤元件每 2 秒會完整檢查一次，所以點的是目前的資料夾：**在樹狀窗格點目前的資料夾不會導航，但會清掉右邊的選取**；
    - 選取一直是 0 個，規則看不到變化，所以面板一直沒有回來，直到下一次導航。
  - **修正規則**：選取變成 0 個時，如果焦點在樹狀窗格，就顯示目前資料夾。這和原本「在樹狀窗格點資料夾＝導航，顯示目前資料夾」一致。
    - 焦點有沒有在樹狀窗格，也算進選取的識別，所以焦點和選取在不同次檢查才變化時，結果也一樣；
    - 在右邊點空白處，照樣隱藏。
    - 樹狀窗格是 `SysTreeView32` 這一點是實測的，文件沒有寫。這個類別名稱本身就是有公開文件的 TreeView 控制項。
  - 把紀錄裡的順序寫成單元測試，另外補了「先清選取、下一次才換焦點」、非檔案系統資料夾、焦點在樹狀窗格時選到檔案等情況。ui-tests 209 → 220 項。兩種突變（拿掉新規則、焦點不算進識別）都抓得到。
  - **使用者第二次實測（開發機，`--trace`）**：
    - 在樹狀窗格點目前的資料夾，面板改成顯示目前資料夾，不再消失；
    - 在樹狀窗格點上一層，導航後顯示上一層；
    - 在右邊點資料夾就顯示它，點空白處就隱藏。
    - 和以前不同的兩點，使用者確認都是預期的行為：
      - 焦點從樹狀窗格回到右邊時，檔案總管自動選回的資料夾會顯示在面板上；
      - 沒有選取時，焦點一回到右邊，面板就隱藏。
- 9516bdb commit 並 push。

### 2026-11-06 第 5 階段：右鍵選單與清單選單

- **使用者決定**：
  - 「加入檔案總管右鍵選單」的開關，放在路徑列右上角的「⋯」按鈕；
  - 清單的右鍵選單（設計第 3 節）用系統選單，這次一起做。
  - 系統彈出選單在深色模式下是淺色，要改成深色只有沒有文件的做法，所以接受選單是淺色。
- **文件查證**（連結見參考資料）：
  - 靜態動詞：`shell\<動詞>\command`，動詞機碼的預設值就是選單文字，`"%1"` 要加引號。文件說可以註冊在 `HKEY_CURRENT_USER\Software\Classes`，不需要提升權限。
  - Predefined Shell Objects 表：`Directory`＝檔案資料夾，`Drive`＝「本機」裡的磁碟機（例如 "C:\"），`Directory\Background`＝資料夾裡的空白處。
  - **找到的文件只說明了 `%1`**，`%V`、`%L` 都沒有說明。實測程式當初在資料夾空白處用的是 `%V`，VM 上可以用，但沒有文件依據。**所以這次不註冊資料夾空白處**，只註冊 `Directory` 和 `Drive`。
  - 改完登錄要呼叫 `SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL)`；文件說不呼叫的話，可能要重新開機才會生效。
  - **磁碟機的參數**：`%1` 是 `C:\`，加上引號後命令列是 `"C:\"`。依 CommandLineToArgvW 和 C 執行階段的解析規則，奇數個反斜線後面接引號，引號會被當成字面字元，所以程式收到的是 `C:"`。路徑不可能含有引號，所以程式把結尾的引號換回反斜線（`PathFromArgument`）。單元測試用 `CommandLineToArgvW` 實際解析來驗證；開發機也實際用 `"C:\"` 啟動，確認會開到 C:\。
- **實作**：
  - `shell_verb`：在 `Directory\shell\DirSize` 和 `Drive\shell\DirSize` 寫入選單文字和 `command`。註冊到一半失敗時，把已經寫入的刪掉，不留下一半的設定。移除時用 `RegDeleteTreeW`，再用 `RegDeleteKeyW` 刪機碼本身。
  - 「⋯」按鈕：自己畫，滑鼠移上去有底色，高對比時加外框。選單裡有可勾選的「加入檔案總管的右鍵選單」，還有「選擇資料夾…」。改登錄失敗時，狀態列會顯示錯誤碼。
  - 清單的右鍵選單：
    - 資料夾：「開啟」（預設項目，和雙擊一樣）、「在檔案總管開啟」（`ShellExecuteW` 的 `explore`）；
    - 檔案：「在檔案總管中顯示」（`SHOpenFolderAndSelectItems`，會選取那個檔案）；
    - 空白處：「在檔案總管開啟」目前資料夾；
    - 每一種都有「重新整理」。
    - Shift+F10 或選單鍵會在選取的那一列開啟選單；這時 `WM_CONTEXTMENU` 的座標是 -1，依文件處理。
    - 選單開著的時候，訊息照樣會被處理，清單可能在這段時間重新載入。所以開選單前，先把目標路徑複製下來。
  - 面板的清單收到右鍵時，同樣會通知父視窗，但面板不處理這個通知。
- ui-tests 220 → 229 項，全部通過。新增的測試涵蓋命令列格式、經過 `CommandLineToArgvW` 解析後再換回路徑，包括磁碟機根目錄和含空白的路徑。
- **使用者在開發機（Windows 11）實測，全部正常**：
  - 用「⋯」勾選後，資料夾和磁碟機的右鍵選單都有「用 DirSize 開啟」，在「顯示更多選項」裡面，和設計 7.3 預期的一樣；
  - 磁碟機會開到正確的根目錄；
  - 取消勾選後，項目就消失；
  - 清單的右鍵選單每一項都正常。
  - 使用者問到 Windows 11 第一層選單。說明了需要 `IExplorerCommand` 加上簽章過的 sparse package，維持 7.3 的決定；如果之後要做，排到第 6 階段，跟安裝程式一起處理。
- 33c9f8f commit 並 push。

### 2026-11-06 第 5 階段驗證工具

- **`dirsize.exe --check <資料夾> <輸出檔>`**：
  - 走和視窗相同的 `FolderSource`，但不開視窗，結果由 message-only window 接收；
  - 在輸出檔附加一行：status、source（service／walk）、note（none／updating／starting／not-running／not-installed／not-indexed／indexing／not-local）、子資料夾數、磁碟用量、檔案數、毫秒數；
  - 狀態列畫在視窗上，腳本讀不到，有了這一行，「來源：DirSize 服務」「服務啟動中」「未安裝服務」就能自動判斷。
  - 開發機（沒有安裝服務）：`source=walk note=not-installed`；不存在的資料夾回報 `status=not-found`，結束碼 1。
- **`scripts/frontend-test.ps1`**：照設計第 8 節第 6 步的驗證引導，分成六部分：
  - Theme：開著視窗切換淺色、深色、對比佈景主題，每一步截圖並記下回答；
  - Dpi：記下視窗和面板的 `GetDpiForWindow`，並截圖；
  - Service：執行中 → 停止 → 再啟動，記下要幾秒服務才回答；
  - AfterBoot：登入後每 2 秒 `--check` 一次，直到服務回答；
  - NotInstalled：先移除服務，檢查完再裝回去，會先問過才移除；
  - Panel：每秒取樣面板行程的 CPU 時間、工作集、private bytes，以及 `Shell.Application.Windows().Count`，輸出成 CSV。
  - 已知的限制：服務設成自動啟動，所以手動停止後，note 會是 `starting`。這和「延遲啟動還沒開始」從 SCM 分不出來，前端照設計顯示「服務啟動中」。
- **`scripts/package-frontend-test.ps1`**：把 dirsize、dirsize-service、dirsize-cli 和腳本打包成 `dist\DirSize-frontend-test-*.zip`，附上中文步驟說明。

#### 第一輪驗證結果（VM：Windows 10 22H2 build 19045，2026-11-06）

| 項目 | 結果 |
|---|---|
| 服務執行中 | `source=service note=none`；使用者確認狀態列寫「來源：DirSize 服務」 ✓ |
| 停止服務 | `source=walk note=starting`，和設計的預期相同（自動啟動的服務停止後，和「延遲啟動還沒開始」分不出來）；逐目錄掃描 C:\Windows 花了 36 秒 |
| 再啟動 | 1 秒內服務就回答（有快取） ✓ |
| 沒有安裝服務 | `source=walk note=not-installed`，使用者確認狀態列和大小都正確；之後服務裝回去，狀態是 Running ✓ |
| 面板資源用量 | 185 秒內最多 9 個檔案總管檢視：CPU 531 ms（單一核心的 0.29%）；工作集最高 18 MB，private 3 MB；沒有跟丟、卡住、閃爍，檔案總管也沒有變慢 ✓ |
| 主題 | 淺色、深色、對比佈景主題，以及切回原本的主題，都立刻跟著變；面板也一致 ✓。高對比截圖：只用系統顏色，「⋯」按鈕有外框 |
| **開機後的延遲啟動** | **沒有量到**：腳本在開機後 200 秒才開始，服務已經在回答 |
| **跨 DPI** | **沒有驗證到**：移動前後的 `GetDpiForWindow` 都是 168，視窗沒有換到縮放比例不同的螢幕 |

- **觀察**：VM 的截圖裡，深色模式的標題列是深色的。可是 DWMWINDOWATTRIBUTE 頁寫 `DWMWA_USE_IMMERSIVE_DARK_MODE` 從 Windows 11 build 22000 起才支援，這台是 Windows 10 build 19045。這是實測結果，文件沒有寫；程式本來就是「呼叫失敗就忽略」，不受影響。
- **補救**：
  - 延遲啟動：新增 `-Parts ScheduleAfterBoot`，在 HKCU 的 `RunOnce` 登記下次登入時自動執行 AfterBoot。這樣一登入就開始記錄，不用手動搶時間。依「Run and RunOnce Registry Keys」：只執行一次，命令長度上限 260 字元，腳本會先檢查長度。
  - 跨 DPI：要有兩個縮放比例不同的螢幕，或者在測試中途改縮放比例。
- **使用者要求：狀態列移到視窗底部**。
  - 上方只留路徑和「⋯」按鈕，高度從 58 DIP 降到 40 DIP；
  - 底部新增 28 DIP 的狀態列：左邊是合計、來源和說明，右邊是按鍵提示（空間不夠就不顯示），背景用標題列的顏色，上緣有一條分隔線；
  - 樹、清單、分隔線都在上方和底部之間。開發機截圖確認正常。
- 86952ff commit。

#### 第二輪：開機後的延遲啟動（VM，用 ScheduleAfterBoot，2026-11-06）

- 登入後第 36 秒開始記錄。服務回答之前，每次 `--check` 都是 `source=walk note=starting`；開機後 136 秒服務開始回答，之後都是 `source=service note=none` ✓。
- **發現的問題**：第一張截圖裡，狀態列和清單只寫「讀取中…」。要等逐目錄掃描做完（第一次 29 秒，之後每次約 5 秒），才會顯示「服務啟動中，先逐目錄掃描」。設計第 2 節是要在掃描期間就說明原因，`IDS_WALKING`（「逐目錄掃描中…」）字串也早就有，只是程式沒用到。
  - **修正**：`FolderSource` 決定改成逐目錄掃描時，先送一個 `walking=true` 的通知，最後結果之後才到。
    - 獨立視窗：狀態列馬上顯示原因加上「逐目錄掃描中…」，清單顯示「逐目錄掃描中…」；重新整理時原本的清單內容保留；
    - 面板也一樣處理；
    - `--check` 跳過這個通知、等最後結果，並在輸出多記一欄 `walk-notice=N`。
  - 開發機（沒有安裝服務）：`--check` 輸出 `walk-notice=1`。用 C:\Windows 啟動 1.5 秒後截圖，狀態列寫「未安裝服務，改為逐目錄掃描 · 逐目錄掃描中…」。
- **觀察：不同權限時，逐目錄掃描和服務的差距**：
  - 這次腳本用一般權限執行，掃描 C:\Windows 得到 19.06 GB、202,536 個檔案，服務是 20.76 GB、203,955 個；
  - 第一輪用系統管理員執行時，兩者幾乎一樣（20.57 GB）。
  - 所以差距來自一般使用者讀不到的資料夾：逐目錄掃描算不到，服務依 8.1 會算進去。這是改用逐目錄掃描時的已知限制。

#### 第二輪：跨 DPI（開發機，Windows 11，2026-11-06）

- 開發機的兩個螢幕原本都是 175%。使用者把其中一個改成 100% 後執行 `-Parts Dpi`：視窗的 DPI 從 168 變成 96 ✓。
  - 移動後的截圖：字、列高、圖示、樹的縮排、捲軸、狀態列都一起換算成 100%，沒有模糊，也沒有被切掉；
  - 面板：使用者確認貼在檔案總管旁邊，大小和字都正確 ✓；
  - 視窗那一題，使用者回答了 n，事後確認是按錯了。
- **腳本的截圖問題**：在另一個 DPI 的螢幕上截圖，右邊和下面會多出白邊。原因是腳本用 `SetProcessDPIAware`，屬於「系統 DPI 感知」，量到的視窗大小被放大了 168/96 倍。改成 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，失敗時才退回原本的做法。
- 到這裡，第 5 階段第 8 節第 6 步的驗證項目都做完了。

### 2026-11-05 第 5 階段決定與實測程式

- 使用者同意 7.1～7.4 都照建議：Win32 原生 C++；Windows 11 分頁依實測，分辨不出就顯示「無法判斷」；右鍵選單只用傳統登錄動詞；面板顯示資料夾和檔案。
- **使用者修正需求**：面板不是「跟著目前資料夾」，而是**在檔案總管點選一個資料夾時，列出它底下各子資料夾的大小**。面板固定貼在檔案總管旁；點別的資料夾就換內容，點檔案或空白處就隱藏；不做「跟著目前資料夾」的面板。設計已改寫（DESIGN-phase5.md 第 1、4、7 節）。
- 查證：
  - `IShellView::GetItemObject` 的 `SVGIO_BACKGROUND` 搭配 `IID_IDispatch`，文件寫明會得到檢視的 ShellFolderView 物件；
  - `ShellFolderView.SelectionChanged` 文件只有 JScript 的寫法，沒寫 C++ 怎麼訂閱、也沒寫 DISPID；
  - `DShellFolderViewEvents`（`DIID_DShellFolderViewEvents`）在 SDK 的 `shldisp.h`，`DISPID_SELECTIONCHANGED`（200）在 `shdispid.h`；
  - `IFolderView2::GetSelection` 的文件寫「沒有選取時回 `S_FALSE`」。
- 實測程式 `tools/explorer_probe.cpp`（不出貨，一般權限）：`windows`、`events`、`dock owned|events --selection events|poll`（可加 `--hang`、`--seconds`）、`verb install|remove`。測試腳本 `scripts/explorer-probe-test.ps1`，打包腳本 `scripts/package-explorer-probe.ps1`。
- 開發機上的試跑（`windows`）：
  - 每個項目的 `IShellBrowser` 視窗類別是 `ShellTabWindowClass`，檢視視窗是 `SHELLDLL_DefView`，用 `SID_STopLevelBrowser` 取得；讀三個項目約 20 ms；
  - 三個視窗都能跨行程訂閱 `DShellFolderViewEvents`（`Advise` 成功）；
  - **什麼都沒選時，`GetSelection` 回 `0x80070490`（`ERROR_NOT_FOUND`），不是文件寫的 `S_FALSE`**。程式兩者都當成「選了 0 個」。
  - 點選後的實際事件、反應時間、CPU、多分頁，還沒測。

#### VM 實測：點選資料夾時的面板（2026-11-05）

測試機：Windows 10 22H2（build 19045），一般使用者權限。Windows 10 沒有檔案總管分頁，所以這次沒有測到分頁。

| 測試 | 時間 | CPU（本程式） | 檢查目前檢視（每 250 ms） | 讀選取 | `SelectionChanged` 事件 |
|---|---|---|---|---|---|
| 事件模式，不設 owner | 93 s | 0.42 s（0.45%） | 315 次，平均 13.1 ms，最長 1047 ms | 8 次，平均 0.8 ms | 1 次 |
| 每 250 ms 讀選取，不設 owner | 93 s | 0.39 s（0.42%） | 290 次，平均 9.0 ms，最長 112 ms | 290 次，平均 0.3 ms | （不訂閱） |
| 事件模式，owned window | 93 s | 0.61 s（0.65%） | 322 次，平均 14.0 ms，最長 203 ms | 32 次，平均 3.1 ms | 3 次 |
| 停止回應測試，owned | 43 s | 0.16 s | 127 次，平均 12.2 ms | 16 次 | 1 次 |
| 停止回應測試，不設 owner | 43 s | 0.19 s | 127 次，平均 15.7 ms | 23 次 | 0 次 |

- 訂閱本身都成功：五次測試共 74 次訂閱，沒有失敗。
- **`SelectionChanged`（DISPID 200）幾乎不來**：使用者點了很多次資料夾和檔案，每次測試只收到 0～3 次。同一段時間，標頭檔裡其他 DISPID（201、207、208、221）反而有來。在事件模式下，面板顯示的資料夾，幾乎都是換檢視後程式主動讀一次選取時才讀到的，例如按「上一頁」後檔案總管自動選回原本的資料夾。
  - **結論：在 Windows 10 上，不能靠 `SelectionChanged` 得知選取改變**。208 沒有官方文件，不用。
- **每 250 ms 讀選取的模式**：點資料夾會顯示，點檔案會隱藏，點選和換資料夾都抓得到。
- **成本主要花在「檢查目前檢視」**，不是讀選取：每次都要重新列出 `IShellWindows`、跨行程走過 `QueryService` 和 `QueryActiveShellView`，平均 9～16 ms；讀選取只要 0.3 ms。只有檔案總管在前景時才檢查，總共約 0.4～0.65% 的單核心。
- **有一次檢查卡了 1047 ms**，可能是檔案總管正在換資料夾。這些呼叫都在面板的執行緒上，卡住時面板也跟著不能動。
- 檔案總管縮到最小時，看得見的檢視是 0 個，面板隱藏，行為正確。
- **使用者回報**：
  - 面板停止回應的 8 秒內，**兩種方式（owned、不設 owner）檔案總管都跟著卡住**。這兩次測試都訂閱了 `DShellFolderViewEvents`，也都有 WinEvent hook。
    - 文件寫明 out-of-context 的 WinEvent 是排隊、非同步送達（SetWinEventHook、Out-of-Context Hook Functions）。
    - COM 跨行程呼叫時，呼叫端會不會等對方回應，查到的文件（Single-Threaded Apartments）沒有明寫。
    - 所以「是訂閱拖住檔案總管」只是推測。下一輪分成四種方式重測：不訂閱、不用 hook、owned、訂閱。
  - 面板不是每次點資料夾都出現，每個步驟都這樣。使用者主要是在**左邊樹狀窗格**點資料夾；在那裡點是切換目前資料夾，不是選取，所以照原本的規則不會觸發。
- **規則修正**（使用者確認）：切換資料夾時列出目前資料夾；在清單選一個資料夾就改列它；之後點檔案或空白處就隱藏。見 DESIGN-phase5.md 7.5、7.6。
- 實測程式改版：
  - 不再用事件，改成每 250 ms 讀一次選取；
  - 記住目前的檢視，只在它不見或被隱藏時、以及每 2 秒一次，重新列出 `IShellWindows`；
  - 加上 `--no-hooks`、`--subscribe` 選項，用來分辨卡住的原因；
  - 測試腳本會在每步結束後提問，把回答存進結果檔。

#### VM 實測第二輪：新規則與「誰拖住檔案總管」（2026-11-05）

同一台 Windows 10 22H2 VM。使用者的回答由測試腳本存檔。

| 測試 | 使用者回答 | CPU（本程式） | 重新列出檢視 | 讀選取 |
|---|---|---|---|---|
| 面板，不設 owner（90 s） | 每次都出現、內容正確；有貼在右邊，但**檔案總管靠近螢幕右邊緣時面板會消失** | 0.45 s（0.49%） | 40 次，平均 21 ms，最長 250 ms | 251 次，平均 4.9 ms，**最長 834 ms** |
| 面板，owned（90 s） | 沒問題 | 0.16 s（0.17%） | 45 次，平均 11 ms | 344 次，平均 1.5 ms，最長 271 ms |

面板停止回應 8 秒時，檔案總管會不會跟著卡住：

| 方式 | 檔案總管卡住？ |
|---|---|
| 不設 owner，有 WinEvent hook，不訂閱 | 沒有 |
| 不設 owner，不用 hook | 沒有 |
| **owned window** | **有** |
| 不設 owner，訂閱 `DShellFolderViewEvents`（事件不處理） | 沒有 |

- **owned window 會拖住檔案總管**，兩輪都一樣。文件沒有說明擁有者在別的行程時的行為，這是實測結果。**決定：面板不設 owner**，自己依 WinEvent 事件跟著檔案總管。
- 第一輪「不設 owner、有訂閱」的那次，使用者回報有卡住；這一輪同樣的組合沒有卡住。原因沒有查明。面板本來就不訂閱事件，所以不影響設計。
- 讀選取偶爾會花幾百 ms（最長 834 ms），這時面板的執行緒也跟著停住。**正式前端要把跨行程的 COM 呼叫放到另一個執行緒**。
- 右鍵選單：Windows 10 上，傳統登錄動詞直接出現在右鍵選單裡，點了之後會帶入資料夾路徑。Windows 11 還沒測。
- **面板位置的修正**（使用者確認的順序）：右邊外側 → 左邊外側 → 內側右邊，而且一定在螢幕的工作區域內。貼在內側時會和檔案總管重疊，所以每次檢查時，如果檔案總管跑到面板上面，就把面板放回它上面。「靠近右邊緣會消失」的原因，紀錄裡看不出來，因為沒有記下 z-order。可能是貼在內側後被檔案總管蓋住，這只是推測。新版會記下位置和重新放回上面的次數。

#### 實測第三輪：面板位置（VM）與 Windows 11（開發機）（2026-11-05）

- **VM（Windows 10）面板位置**：使用者依序試了靠右邊緣、靠左邊緣、左右都放不下、最大化，回答四種情況面板都**沒有消失、也沒有被蓋住**。
  - 紀錄裡依序出現右邊外側、左邊外側、內側右邊。
  - 「把面板放回檔案總管上面」做了 4 次：貼在內側時，檔案總管確實會跑到面板上面。這證實了上一輪「被蓋住」的推測。
  - CPU 0.45 s／91 s（0.49%）；讀選取平均 1.9 ms，最長 153 ms。
- **開發機（Windows 11，build 26300）**，一般權限：
  - 面板：每次都出現，內容正確；貼齊正常，不會閃爍。CPU 0.56 s／91 s（0.62%）；重新列出檢視平均 9.8 ms，讀選取平均 1.4 ms。
  - **右鍵選單**：傳統登錄動詞出現在「顯示更多選項」裡，點了會帶入資料夾路徑。和依文件推論的一致。
  - **分頁還沒測到**：這次是兩個視窗、各一個分頁，因為步驟 1 的提示寫的是開兩個視窗。測試腳本已補上分頁專用的步驟 7。
- **Windows 11 分頁（開發機，步驟 7 與後續逐次讀取）**：
  - 一個視窗開 3 個分頁時，`IShellWindows` 有 3 個項目，`get_HWND` 都是同一個頂層視窗。每個分頁各有一個 `ShellTabWindowClass` 的 `IShellBrowser` 視窗。
  - **3 個分頁的檢視視窗都回報「看得見」**（`IsWindowVisible` 為 1）。位置、大小、樣式、是否啟用、`DWMWA_CLOAKED` 也都相同。所以原本「只有一個看得見」的判斷分不出來，照 7.2 隱藏，使用者回報「沒看到面板」。
  - 唯一不同的是**在父視窗子視窗中的 z-order**。使用者依序把第 1、第 3、第 2 個分頁切到前面，每次讀取時，前面的分頁都是 3 個 `IShellBrowser` 視窗中 z-order 最上面的那個（3 次中 3 次），和視窗標題（「某資料夾 和 2 個其他索引標籤」）一致。
  - **這是實測的行為，沒有文件**。實作方式：看得見的檢視不只一個時，選 z-order 最上面的分頁；如果這些分頁視窗不在同一個父視窗下，或找不到，就隱藏，不猜。每次檢查也會用不需要 COM 的方式確認前面的分頁有沒有換，換了就重新列出。
  - **修正後重測（60 s）**：
    - 使用者回答：切換分頁後，面板顯示的是那個分頁的資料夾；開新分頁、關分頁也都正常。
    - 紀錄有 5 次前面分頁的變化。新分頁一開始是「本機」，不是檔案系統資料夾，面板隱藏。
    - CPU 0.39 s／61 s（0.64%）；重新列出檢視平均 43 ms，最長 212 ms（3～4 個分頁）；讀選取平均 3 ms。
    - 分頁越多，重新列出越慢，這部分正式前端也要放到另一個執行緒。
- **第 5 階段實測的結論**：
  - 面板用輪詢讀選取，不用事件；
  - 不設 owner；
  - 跨行程的 COM 呼叫放到另一個執行緒；
  - 位置依序是右邊外側、左邊外側、內側；
  - Windows 11 分頁用 z-order 判斷前面的分頁；
  - 右鍵選單用傳統登錄動詞（Windows 10 直接出現，Windows 11 在「顯示更多選項」裡）。

### 2026-11-06 第 6 階段查證與設計草案

- efacb2a push；第 5 階段完成。
- 草案 `docs/DESIGN-phase6.md`：安裝程式、多語系、版本與發行。連結見參考資料，元件「第 6 階段設計」。
- **查證重點**：
  - MSI 的 ServiceInstall／ServiceControl 表可以安裝、停止、刪除服務；延遲啟動用 MsiServiceConfig 表（Windows Installer 5.0 起）。
  - **WiX 不是 MIT**：原始碼是 MS-RL。README 寫「use of the WiX Toolset requires an Open Source Maintenance Fee」，OSMFEULA 第 1 節卻寫維護費只適用於年營收 1 萬美元以上的營利使用，兩者說法不一致。
  - Inno Setup 的授權寬鬆：允許任何用途，條件是保留著作權聲明、修改版要標示；沒有提到會限制產出的安裝程式。
  - Uninstall 機碼的頁面只列出 Windows Installer 會寫的值，沒有說明不用 MSI 時要寫哪些值。
  - **一個 exe 裡有多種語言的字串表時，LoadString 會選哪一種，LoadString 頁和 STRINGTABLE 頁都沒有寫**；FindResourceExW 頁 404。`SetThreadPreferredUILanguages` 的頁面說，LoadString 這類載入資源的函式會呼叫 GetThreadPreferredUILanguages。所以要先實測（草案 7.5、第 8 節第 1 步）。
  - 「一般使用者用管理員帳密提升權限時，HKCU 會是管理員的」，找到的 UAC 頁面沒有寫明。草案因此讓每個使用者自己的設定，由程式用一般權限寫入（7.2 A）。
  - SmartScreen：沒有信譽的檔案會顯示警告。UAC：沒有簽章的程式，提升權限的提示框是黃色背景。
- 使用者同意 7.1～7.6 都照建議：Inno Setup；右鍵選單由安裝程式寫 HKLM、面板自動啟動由程式寫 HKCU；先不簽章；移除時刪除快取；字串表放同一個 exe（先實測）；這個階段不做 Windows 11 第一層選單。

### 2026-11-06 第 6 階段實測：字串表的語言選擇（`tools/lang_probe.cpp`）

- 7.1～7.6 定案，7daf46e push。
- 實測程式：同一個 exe 放 zh-TW 和 en-US 兩份字串表；id 1 兩種都有，id 2 只有英文，id 3 只有中文。原始輸出在 `bench-results/`。
- 開發機（繁中 Windows 11，使用者 UI 語言 zh-TW，偏好清單 zh-TW;en-US）的結果：
  - 不做設定：拿到中文。
  - `SetThreadPreferredUILanguages` 設 en-US：拿到英文；設 zh-TW：拿到中文。清除設定後回到中文。**LoadString 確實照執行緒的偏好清單選**，和文件說的一致。
  - 設 ja-JP;en-US：第一個沒有，就拿第二個（英文）。
  - 設 en-GB：拿到**中文**，不是英文。合併後的清單是 en-GB;en;zh-TW…，「en」對不到 en-US 的字串表。所以**字串表的語言要完全相符**，程式要自己把語言對應成 zh-TW 或 en-US 再設定。
  - **缺字串不會改用另一種語言**：中文時 id 2 拿不到（回傳 0），英文時 id 3 拿不到。所以每種語言都要有全部字串，要加測試檢查。
  - 執行緒的設定**不會傳給新執行緒**（主執行緒設 en-US，新執行緒仍是中文）。`tree_loader` 在背景執行緒載入字串，所以要用 `SetProcessPreferredUILanguages`：設 en-US 後，主執行緒和新執行緒都拿到英文；執行緒自己再設 zh-TW 時，以執行緒的為準。
- 文件：GetThreadPreferredUILanguages 的 MUI_MERGE_SYSTEM_FALLBACK 會傳回「corresponds exactly to the language list used by the resource loader」的清單，合併順序是執行緒 → 行程 → 使用者 → 系統預設（上面印的「merged」就是用這個旗標取得的）。SetProcessPreferredUILanguages：Windows 7 起，建議程式啟動後盡早呼叫，最多五種語言；頁面沒有提到 LoadString。
- VM（Windows 顯示語言改成 English (United States)，系統預設 UI 語言仍是 zh-TW）的結果：
  - 不做設定：拿到**英文**。
  - 設 en-GB、ja-JP：拿到英文（照使用者的 en-US 退回）。
  - 設 zh-HK：拿到**英文**。合併清單是 zh-HK;zh-Hant;zh;en-US…，「zh-Hant」「zh」都對不到 zh-TW 的字串表，和開發機上 en-GB 的情形一樣。
  - 在主執行緒設 zh-TW 後，新執行緒仍是英文；設定整個程式時，新執行緒也照設定。
- 結論（7.5 A 可行）：中性語言（en、zh-Hant）對不到有地區的字串表，所以不能只靠 Windows 自己選。程式啟動時讀使用者的偏好清單，自己對應成 zh-TW 或 en-US，再用 `SetProcessPreferredUILanguages` 設定。
- 使用者決定：簡體中文（zh-CN、zh-SG）用英文，不用繁體。對應規則寫進設計第 5 節。
- 參考：[SetProcessPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-setprocesspreferreduilanguages)、[GetThreadPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getthreadpreferreduilanguages)、[GetUserPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserpreferreduilanguages)。

### 2026-11-06 第 6 階段：語系檔 `lang/*.json` 與語言選擇

- 使用者提議：開源專案要讓別人能貢獻語系檔。比較了「建置時轉換」和「執行時讀檔」，使用者選建置時轉換：仍然一個 exe，實測過的 LoadString 做法不變。
- **產生器** `cmake/gen_lang.cmake`（`cmake -P`，不需要其他工具）：
  - 讀 `src/ui/string_ids.h` 的 `#define IDS_`，和每個 `lang/<locale>.json`，產生 `build/generated/` 下的 `lang.rc`（每種語言一個 STRINGTABLE）、`languages.inc`（語言表）、`lang_expected.inc`（測試用的全部字串）。
  - 檢查：en-US 缺 ID、檔名和 `locale` 不同、未知的 key、占位符號和英文不同 → 建置失敗；其他語言缺字串 → 用英文並警告；去掉 UTF-8 BOM。
  - `LANGUAGE` 敘述寫 `winnt.h` 的常數名稱，不寫數字：LANGUAGE 敘述的頁面沒有說明可不可以用數字。
  - 用 `file(GLOB CONFIGURE_DEPENDS)` 找語系檔，新增檔案會在下次建置時加入。文件提醒這個旗標「may not work reliably on all generators」，在 VS 產生器上實測（只執行 `--build`，不重新設定）：新增 `lang/en-GB.json` 後，建置會先「Checking Build System」，再重新產生，`languages.inc` 出現 en-GB，ui-tests 265 項全過；刪掉檔案再建置，`lang.rc` 和 `languages.inc` 裡的 en-GB 都消失了。
- **選語言**：`PickLanguage`（`src/ui/language.cpp`，純邏輯）依序看使用者的顯示語言，第一個被某個檔案的 `locale` 或 `matches` 對到的就用；都沒有就用 en-US。`ApplyUiLanguage` 在 `wWinMain` 開頭（視窗、面板、`--check` 都會經過）用 `SetProcessPreferredUILanguages` 設定。
- 「⋯」選單新增「語言」：跟著 Windows，或選一種語言（名稱用 `GetLocaleInfoEx` 的 `LOCALE_SNATIVEDISPLAYNAME`），存在 HKCU\Software\DirSize 的 `Language`。
  - 第一版是「下次啟動生效」，使用者回報「語言切換無效」：設定有存進去，重開後也是英文，但提示「重新開啟後套用」會被下一次讀取資料夾的狀態蓋掉，看起來就像沒有作用。
  - 改成立刻切換：存檔後再呼叫一次 `ApplyUiLanguage`，再重新讀取目前的資料夾和磁碟機清單，讓標題、狀態列、清單訊息、磁碟機標籤重建（其他文字是重畫時才載入的）。SetProcessPreferredUILanguages 的頁面只說「建議」啟動後盡早呼叫，沒有禁止之後再呼叫；ui-tests 實測連續設定 en-US → zh-TW → en-US，主執行緒和新執行緒每次都拿到對應的語言。
  - 已經在執行的面板（`--panel`，另一個程序）不會跟著切換，要重新開啟。
- 串接的字串改成 `%1`：`IDS_TOTAL`、`IDS_FILE_COUNT`（原 `IDS_FILES_SUFFIX`）、新增 `IDS_STATUS_ALLOCATED`、`IDS_ERR_FAILED`、`IDS_ERR_VERB`；新增 `FormatPattern`（一次替換，參數裡的 `%2` 不會再被換）。新增 `IDS_MENU_LANGUAGE`、`IDS_MENU_LANG_AUTO`、`IDS_ERR_SETTING`。
- zh-TW.json 是從原本的 `dirsize.rc` 轉出來的，和原字串逐條比對過，除了上面改成 `%1` 的四條以外都相同。
- **測試**（ui-tests 271 項全過，4 個測試程式全過）：
  - `FormatPattern`、`TagMatches`、`PickLanguage`（zh-HK→zh-TW、zh-CN→en-US、en-GB→en-US、ja-JP;zh-TW 等）。
  - 每個語言的 `LANG_`／`SUBLANG_` 和 `LocaleNameToLCID(locale)` 一致。
  - ui-tests 也編入 `lang.rc`：每種語言設為執行緒偏好後，每個字串都載入得到，而且和 JSON 的內容完全相同（包括 tab）。
  - 故意改錯驗證：把 zh-TW 的 sublanguage 改成 SIMPLIFIED → 2 項失敗；把 `IDS_TOTAL` 的 `%1` 拿掉 → 建置失敗。
- 實機：預設是中文；HKCU 設 `Language=en-US` 後啟動，欄名、清單訊息、狀態列、提示都是英文。測完已刪除這個設定值。背景執行緒（磁碟機標籤）的兩種語言字串目前相同，畫面看不出差別；它用的語言靠的是實測程式的結果：整個程式的設定會套用到新執行緒。
- 從空的建置資料夾完整建置一次（平行建置）：產生器先執行，ui-tests 262 項全過（當時的數字）。
- VM 測試腳本只比對 `source=`、`note=` 這類固定代碼，不受介面語言影響。
- 已知、不是這次造成的：底部提示文字的寬度要小於狀態列的一半才會顯示，中文提示比較長，視窗不夠寬時會被隱藏。
- 給翻譯者的說明 `lang/README.md`（中英）；兩份 README 都加了連結。

### 2026-11-03 權限與部署方向（第 4 階段參考）

- 實測：`FSCTL_QUERY_FILE_LAYOUT` 在一般權限下無法使用（錯誤 1）；逐目錄掃描可以用，但讀不到部分資料夾。
- 方向：
  - **服務模式為主**：安裝時要系統管理員權限一次，註冊 Windows 服務，由服務在背景用高權限掃描。前端以一般權限透過 named pipe 查詢，日常使用不跳 UAC。
  - **逐目錄掃描為退路**：沒有服務或沒有管理員權限時使用，也可以做成免安裝版。
- **安全要求**：服務看得到所有資料夾，回應查詢前必須確認呼叫者本人有權限看該資料夾，否則一般使用者能查到其他使用者的資料夾名稱和大小。候選做法是 `ImpersonateNamedPipeClient`，第 4 階段再查文件確認。
- 待查：讀取 USN 日誌需要什麼權限（第 3 階段）。

### 2026-11-11　第 6 階段：執行檔的版本資訊

- **做了什麼**：`dirsize.exe`、`dirsize-cli.exe`、`dirsize-service.exe` 各加一份 `VERSIONINFO` 資源，檔案「內容」頁看得到版本。測試程式和探測工具不發行，沒有加。
- **做法**：
  - 一份範本 `cmake/version.rc.in`，由 `CMakeLists.txt` 的 `dirsize_add_version(目標 說明)` 用 `configure_file(... @ONLY)` 各產生一份到建置資料夾。
  - 版本號只有一個來源：`project(DirSize VERSION 0.1.0)`。改這一行，三個執行檔都會跟著變（文件：輸入檔改了，建置時會重新產生）。
  - 依文件：資源識別碼是 1（`VS_VERSION_INFO`）；`FILEVERSION` 是四個 16 位元數字，第四個填 0；常數要引入 `winver.h`。
  - 字串區塊用美式英文加 Unicode（`040904B0`，`Translation 0x0409, 1200`），內容全是 ASCII。依 5 的範圍，版本資訊不翻譯。
  - 文件標為必填的字串都有填：`CompanyName`、`FileDescription`、`FileVersion`、`InternalName`、`OriginalFilename`、`ProductName`、`ProductVersion`。`LegalCopyright` 和 LICENSE 的著作權行一致。
  - Debug 組態設 `VS_FF_DEBUG`：CMake 在 Debug 組態會給資源編譯器 `_DEBUG`（在產生的專案檔裡確認過）。
- **驗證**（Release 建置，沒有警告）：用 PowerShell 讀三個執行檔的 `VersionInfo`，檔案版本和產品版本都是 `0.1.0`，說明分別是 `DirSize`、`DirSize command-line tool`、`DirSize service`，原始檔名和實際檔名相同，語言是英文（美國），不是偵錯版。沒有加版本資訊的 `ui-tests.exe` 讀出來是空的，確認讀到的不是預設值。4 組測試全過。
- **待使用者確認**：`CompanyName` 目前填 `DirSize project`（文件要求必填，專案沒有公司）。

### 2026-11-11　第 6 階段查證：右鍵選單文字跟著使用者的語言

- **問題**：安裝程式把右鍵選單寫進 HKLM、給所有使用者（7.2 A），預設值裡的文字就只有一種語言。
- **文件**（Using Registry String Redirection）：動詞機碼的預設值保留一般文字，另外加 `MUIVerb` 值，寫成 `@<執行檔路徑>,-<字串資源 ID>`，選單文字就會依使用者的介面語言載入。Windows XP 起支援。逗號和減號之間不能有空白，不能用短檔名。
- **實測**（本機，用 PowerShell 呼叫 `SHLoadIndirectString`，對象是建置出來的 `dirsize.exe`，字串 ID 181）：
  - 不設定語言：`用 DirSize 開啟`。
  - 執行緒語言設成 en-US：`Open with DirSize`；設回 zh-TW：中文。
  - 不存在的 ID：失敗（`0x80004005`），沒有回傳文字。
- **結論**：
  - 安裝程式寫 HKLM 時，預設值寫英文，加上 `MUIVerb = @<安裝路徑>\dirsize.exe,-181`。字串 ID 181（`IDS_VERB_TEXT`）從此不能改號。
  - 選單是檔案總管載入的，用的是 Windows 的語言規則，不是 DirSize 自己的對應規則，也不看 DirSize 裡手動選的語言。依先前的實測，沒有對應字串表的語言（例如 zh-HK）會落到哪一種，由 Windows 決定。
  - 程式自己寫 HKCU 的那一份（不用安裝的版本）維持現狀：直接寫入當下介面語言的文字，和畫面一致。
  - 還沒驗證：實際在檔案總管的右鍵選單裡看到 `MUIVerb` 的文字。留到安裝程式做好後，在中文機和英文 VM 各看一次。
  - `SHLoadIndirectString` 文件提到 MUI 快取：資源內容改了但 ID 沒變時，快取不會更新，可以在後面加 `;v2`。翻譯文字日後修改時要留意。

### 2026-11-11　第 6 階段：面板自動啟動、安裝程式腳本（尚未編譯）

- **程式裡的設定**（設計 7.2 A）：
  - 「⋯」選單新增「登入時自動啟動檔案總管面板」：寫在 HKCU 的 `Run`，值名稱 `DirSizePanel`，內容是 `"<執行檔>" --panel`。依「Run and RunOnce Registry Keys」，命令超過 260 字元就不寫入、回報錯誤。
  - 右鍵選單如果已經由安裝程式寫在 HKLM，選單項目改顯示「由安裝程式設定」，不能在程式裡切換。
  - 新增兩條字串（兩個語系檔都有）。ui-tests 272 項、4 組測試全過。這兩項的畫面還沒有請使用者看過。
- **服務**：`dirsize-service install` 如果本身已經在 Program Files 的安裝位置，就只註冊、不複製（原本會複製到自己身上而失敗）。這個路徑要等安裝程式能編譯後才驗證得到。
- **安裝程式腳本** `installer/dirsize.iss`（Inno Setup），依官方說明文件寫成：
  - 需要系統管理員權限；只允許 x64 相容的系統；最低 Windows 10 1809（`MinVersion=10.0.17763`）。
  - 安裝位置固定在 Program Files 下的 `DirSize`，不讓使用者選：服務以 LocalSystem 執行，執行檔必須在一般使用者不能寫入的位置，而且服務是從這個固定位置註冊的。
  - 語言：原本寫了英文和繁體中文（`ChineseTraditional.isl`），編譯時發現這個檔不存在。結果和決定見下一則。
  - 右鍵選單是可勾選的工作，寫在 HKLM 的 `Directory` 和 `Drive`：預設值是英文，加上 `MUIVerb`（見上一則）。重新安裝時取消勾選，會把機碼刪掉。
  - 換檔案之前（`PrepareToInstall`）：用 `taskkill /IM dirsize.exe /F` 關掉執行中的視窗和面板（它們沒有未儲存的資料；面板沒有關閉按鈕），再用舊的 `dirsize-service uninstall` 停止並移除服務，檔案才換得掉。因此不使用 Inno Setup 內建的關閉應用程式功能（`CloseApplications=no`）。
  - 檔案複製完：執行 `dirsize-service install`，成功就 `sc start DirSize`；失敗會顯示訊息，但安裝繼續（沒有服務時，程式仍可用自己讀取的方式運作）。
  - 最後一頁可以勾選開啟 DirSize、啟動面板。依文件，這類項目預設用啟動安裝程式的原使用者身分執行。
  - 移除：關掉程式 → 移除服務 → 刪除檔案和登錄 → 刪除 `ProgramData` 下的 `DirSize` 資料夾（7.4 A）。
  - 輸出檔名用 `DirSize-<版本>-setup.exe`。文件提醒不要取名 `setup.exe`：Windows 會對這個檔名套用相容性處理，載入額外的 DLL。
- **打包腳本** `scripts/package-installer.ps1`：從 `CMakeLists.txt` 讀版本號，檢查三個執行檔的版本資訊和它一致，再呼叫 `ISCC.exe`，另外產生只含 `dirsize.exe` 的免安裝 zip。輸出到 `dist\`（不進 git）。
- **已知的限制**：各使用者自己開的「登入時自動啟動面板」寫在各自的 HKCU，移除程式清不到其他帳號的這一項。移除後，那一項會指向不存在的檔案。
- **還沒做**：這台建置機沒有安裝 Inno Setup，腳本**還沒編譯過，也沒執行過**。裝好後要先編譯，再到 VM 做設計第 8 節第 5 步的驗證。

### 2026-11-12　第 6 階段：安裝程式第一次編譯

- 用 winget 安裝 Inno Setup 6.7.3（`JRSoftware.InnoSetup`，安裝在使用者的 `AppData\Local\Programs` 下；`package-installer.ps1` 本來就會找這個位置）。
- 官方安裝包的 `Languages\` 資料夾**沒有** `ChineseTraditional.isl`。繁體中文只出現在 Inno Setup 網站的非官方翻譯，是第三方譯者的作品，授權沒有確認。
- **決定**：安裝精靈只用英文，拿掉 `zhtw` 語言和它的自訂訊息。程式本身和右鍵選單（`MUIVerb`）仍然依使用者的語言顯示，不受影響。
- 拿掉 `zhtw` 後，ISCC 編譯成功，`[Code]`、`[Registry]`、`[Files]`、版本資訊都沒有錯誤。

### 2026-11-12　第 6 階段：程式圖示

- 原本沒有任何圖示：exe、視窗、捷徑、「新增或移除程式」都是 Windows 預設的圖示，安裝檔則是 Inno Setup 的圖示。
- 圖案由使用者選定：資料夾上面疊一個圓餅圖。原創，用 `scripts/make-icon.py`（Pillow）畫出來，輸出 `src/app/dirsize.ico`。這個 .ico 也放進 git，建置時不需要 Python。
- 尺寸：16、20、24、32、40、48、64、256。包含 Microsoft 對 Win32 建議的最少尺寸（16、24、32、48、256），以及 Inno Setup 建議的 64。每個尺寸都用 16 倍大小畫好再縮小。24 px 以下的外框加粗。
- `dirsize.rc`：`1 ICON "dirsize.ico"`。exe 裡只有這一個圖示資源。文件沒有寫檔案總管選哪一個圖示當 exe 圖示，只有一個就不必管這個規則。
- 主視窗類別的 `hIcon`／`hIconSm` 用 `LoadImageW` 載入，尺寸取 `GetSystemMetrics` 的 `SM_CXICON` 和 `SM_CXSMICON`。面板是 `WS_EX_TOOLWINDOW`，不會出現在工作列，所以不設定圖示。
- **出錯過一次**：第一版用 `LoadIconMetric`。安裝到 VM 後一啟動就出現「序數 380 無法在動態連結程式庫 …dirsize.exe 上找到」。用 dumpbin 查，`dirsize.exe` 是用**序數 380** 從 `COMCTL32.dll` 匯入這個函式。這個序數只有第 6 版 comctl32 才有，而我們的 manifest 沒有要求第 6 版，系統載入的是舊版，所以找不到。`LoadIconMetric` 的文件頁沒有提到這個條件。改用 user32 的 `LoadImageW`，不再連結 comctl32。重新建置後確認：沒有 COMCTL32 匯入；視窗開得起來；類別上的大小圖示 handle 都不是 0。當初沒有先實際啟動一次就打包，以後打包前要先啟動一次。
- 安裝程式：`SetupIconFile` 指向同一個 .ico。
- 驗證：建置成功，ui-tests 272 項全過。從 `dirsize.exe` 和 `DirSize-0.1.0-setup.exe` 用 `ExtractAssociatedIcon` 取出的都是新圖示。

### 2026-11-12　第 6 階段：安裝時列出所有預設選項

- 起因：使用者在 VM 上裝好後，面板沒有出現。原因是「現在啟動面板」預設不勾，「登入時自動啟動面板」也只能裝完在程式裡開。使用者要求：程式的預設選項都要在安裝時列出，並且預設啟用。
- 安裝程式的「工作」頁現在有兩項，都預設勾選：
  - 右鍵選單：沿用原本的設定，寫在 HKLM。
  - **登入時自動啟動面板**：新增。
- 自動啟動仍然寫在各使用者自己的 HKCU（設計 7.2）。新增 `dirsize.exe --set-autostart`：開啟這個設定後就結束，不開視窗，結束碼是 Win32 錯誤碼。安裝程式在 `[Run]` 用 `runasoriginaluser` 執行它。依 Inno Setup 文件，這個旗標會用「啟動安裝程式的那位使用者」的身分執行，不用核准 UAC 的管理員身分。文件也寫了例外：如果安裝程式是用「以系統管理員身分執行」啟動的，這個旗標就沒有作用。
- 參數的判斷改成只比對第一個參數（`CommandLineToArgvW`），不用子字串搜尋，以免資料夾名稱裡剛好有同樣的文字。舊的 `--check`、`--panel` 還是用子字串搜尋，這次沒有改。
- 完成頁的「現在啟動面板」改成預設勾選。
- 重新安裝時，依 Inno 的 `UsePreviousTasks`（預設 yes），工作頁會沿用上次的勾選。
- 移除程式時，加上 `reg delete` 刪掉**執行移除程式的那個帳號**的 HKCU `DirSizePanel`。使用者自己核准 UAC 時，這就是使用者本人的帳號。用另一個管理員帳密提權時，刪的是那個管理員帳號的設定，對其他帳號沒有作用，這仍然是已知限制。
- 本機驗證：`--set-autostart` 的結束碼是 0，HKCU Run 寫入 `"<exe>" --panel`，沒有開出視窗。測試後已還原登錄。一般啟動正常。ui-tests 272 項通過。VM 上還沒驗證。
- （同日稍後）`--set-autostart` 改成通用的 `--setting <autostart|share> <on|off>`，見下一則。工作取消勾選時會寫入 off，所以重新安裝時取消勾選也會生效。

### 2026-11-12　第 6 階段：「顯示比例」選項；最右欄被切掉的修正

- **使用者要求**：比例欄（長條＋百分比）要不要顯示，改成選項。決定：
  - 主視窗和面板一起套用。
  - 預設顯示。
  - 開關放在主視窗的「⋯」選單，面板按右鍵也有同一項。
- **設定**：HKCU `Software\DirSize` 的 `ShowShare`（DWORD），沒有這個值就是顯示。程式在 `src/ui/settings.h/.cpp`。
- **欄位**：`WindowColumns`／`PanelColumns`（`list_model`，純函式）。如果目前依「比例」排序，隱藏比例欄後改成依「磁碟用量」排序。兩者的排序依據相同，所以順序不變（`FallbackSortColumn`）。ui-tests 新增 16 項，共 288 項。
- **即時更新**：主視窗和面板是不同的行程。改設定後，用 `RegisterWindowMessage("DirSize.SettingsChanged")` 加上 `PostMessage(HWND_BROADCAST)` 通知所有頂層視窗。面板是沒有擁有者的 popup，依文件會收到。本機實測：主視窗不用重開，比例欄就會出現或消失。面板的部分還沒實測。
- **面板右鍵選單**：面板本身不取得焦點（`WS_EX_NOACTIVATE`）。依 TrackPopupMenu 的說明，選單的擁有者不在前景時，點選單外面不會關閉選單。所以顯示選單前先把面板帶到前景，結束後把前景還給檔案總管。這個行為還沒在實機上看過。
- **安裝程式**：新增「顯示比例」工作，預設勾選。安裝程式用 `--setting share on|off` 以原使用者身分寫入。
- **最右欄被切掉**：
  - 使用者的面板截圖裡「99.6%」右邊被切掉。本機主視窗的「檔案數」欄也一樣（「25」只剩一半），但標題列沒有被切。
  - 原因：自繪捲動條 `ScrollStrip::Paint` 在捲動條沒有出現時，仍然把自己那一條（12 DIP）塗成背景色，蓋住最右欄文字的最後幾個像素。標題列不在那一條的範圍內，所以標題沒事。
  - 修正：捲動條沒出現時什麼都不畫（兩個擁有者都會先把整個 client 區塗成背景色）。修正後截圖確認「25」「3,264」「100.0%」都完整顯示。
  - 我一開始以為是百分比的文字太寬，改成量測寬度。看到「檔案數」欄也被切之後，才發現猜錯了，已經改回原本的寫法。
- **不顯示比例時面板變窄**（使用者要求）：
  - 存檔的寬度（`PanelWidth`）一律是「有比例欄」的寬度。比例欄隱藏時，面板寬度扣掉比例欄（112 DIP）。在隱藏狀態下拖曳調整寬度，存檔時再加回去，所以開關切換來回，寬度都會一樣。
  - 隱藏時的最小寬度是 160 DIP。直接扣掉會變成 128，放不下名稱、磁碟用量和捲動條。
  - 計算放在 `list_model`（`PanelShownWidthDip`／`PanelSavedWidthDip`，欄寬 `ColumnWidthDip` 也從 `size_list.cpp` 搬到這裡），ui-tests 共 392 項，包含每 7 DIP 一個寬度的往返檢查。
  - 本機實測（顯示縮放 175%）：面板寬度 583 → 387 → 583 px，差 196 px，就是 112 DIP。截圖確認比例欄消失，標題文字會加上省略號。
  - 另外看到的既有問題（沒有修改）：英文介面下，面板的「Size on disk ▾」標題在 80 DIP 的窄欄裡左邊被切掉。現在可以用下面的欄界線拖曳把這一欄拉寬。

### 2026-11-12　第 6 階段：名稱被截斷的三項改善

- **起因**：使用者截圖。面板裡的名稱被截斷，而且不知道怎麼把欄位拉寬。使用者選了三項：拖曳邊緣更好找、欄界線可以拖曳、滑鼠停留時顯示全名。
- **面板的拖曳帶**：
  - 原本的情況：面板邊緣其實可以拖曳。清單會把 `WM_NCHITTEST` 轉給面板，回傳 `HTTRANSPARENT`。但可拖曳的範圍只有 6 DIP，而且看不見；面板在檔案總管右側時，這 6 DIP 還跟清單 12 DIP 寬的捲動條重疊。
  - 現在：在可拉伸側和下緣各留一條 8 DIP 的拖曳帶，清單不再蓋住這兩條，也就不會跟捲動條重疊。每條拖曳帶中間畫一段 32 DIP 的短線，平時用框線色，滑鼠移上去時改用強調色（`WM_NCMOUSEMOVE`／`TrackMouseEvent(TME_NONCLIENT)`）。面板換到檔案總管的另一側時，拖曳帶也跟著換邊。
- **欄界線拖曳**（`SizeList`）：
  - 標題列的每條分隔線都畫出來，控制它右邊那一欄的寬度（名稱欄吃掉剩下的空間），寬度限制在 40～400 DIP。拖曳時游標是 `IDC_SIZEWE`。在分隔線上雙擊會恢復預設寬度。
  - 欄寬存在 HKCU `PanelColumns`／`WindowColumns`，格式是 `Allocated=96;Share=150`。解析時會跳過名稱欄、不認得的欄名、不合法的數字和超出範圍的寬度，所以登錄值損壞時只會失去那幾項（`ParseColumnWidths`，ui-tests 有涵蓋）。
  - 隱藏比例欄時，面板縮減的寬度改成「比例欄目前的寬度」，也就是使用者拉過的寬度，不再固定是 112。
  - 本機實測：把訊息直接送給主視窗的清單，讓 Files 欄的分隔線往左拖 100 px（175%），存檔結果是 `Files=141`，跟計算的 84 + 100 × 96 / 168 一致。
- **全名提示**（`tip_window.h/.cpp`，自己畫）：
  - 系統內建的 tooltip 在深色模式下仍然是淺色，所以自己畫，用調色盤的顏色。提示框是 `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST`，不會搶焦點，點擊會穿透，所以在面板上也能用。
  - 名稱被截斷的那一列，滑鼠停留時間到了（`TME_HOVER`）就在那一列下方顯示完整名稱，文字跟名稱對齊。捲動、滾輪、點擊、換到別列、離開清單時都會收起來。主視窗和面板都有這個功能。
  - 除錯過程：第一版提示框一出現就馬上消失。我暫時在程式裡加記錄檔，發現顯示提示框時 Windows 會補送一個 `WM_MOUSEMOVE`，而 `UpdateTip` 因為我在 `WM_MOUSEHOVER` 後把追蹤狀態清掉了，就把提示框隱藏，造成來回重複。改成「同一列不做任何事」後就正常了。另外把提示框改放在那一列的下方，不蓋住游標。除錯用的記錄已經移除。
  - 本機實測（用 `SetCursorPos` 移動真正的游標，測完移回原位）：截斷的那一列會顯示完整名稱；沒截斷的那一列不顯示；游標離開後提示框收起。
- ui-tests 418 項通過。
- **欄寬調整改成檔案總管的方式**（使用者回饋：調整一欄時其他欄不應該跟著變，應該是面板寬度跟著變）：
  - 分隔線改成「欄的右緣」，控制它左邊那一欄，往右拖就變寬。原本的設計是控制右邊那一欄、往右拖變窄，跟一般習慣相反，已經改掉。
  - **面板**：被拖的那一欄變寬或變窄多少，面板就跟著變寬或變窄多少（`kListGrowParent`，用 SendMessage 讓版面立刻跟上），其他欄維持原寬度。名稱欄右緣也可以拖，拖了多少就是面板寬度改變多少。拖曳中面板可能往左長，所以拖曳距離用螢幕座標計算。存檔的面板寬度跟著更新。在分隔線上雙擊恢復預設寬度，面板也會跟著縮放。
  - **主視窗**（使用者選「像檔案總管」）：還沒拖過任何欄時，名稱欄照舊填滿。第一次拖曳時，把名稱欄固定在當下的寬度，之後每一欄都有自己的寬度，由左往右排。總寬度小於清單時右邊留白；大於清單時出現水平捲動條。`ScrollStrip` 新增水平方向，水平捲動條用另一個計時器 ID，兩條捲動條才能各自連續捲動。水平捲動也可以用 Shift＋滾輪或 `WM_MOUSEHWHEEL`。在名稱欄的分隔線上雙擊，名稱欄會回到自動填滿。
  - 欄寬設定改成也接受 `Name`（上限 1200 DIP）。只有主視窗用得到，面板的名稱欄永遠是填滿。
  - 版面配置拆成純函式 `LayoutColumns`，ui-tests 共 441 項，其中一項是「只有被拖的那一欄改變」。
  - 本機實測（用 SendMessage 送拖曳訊息，175%）：
    - 拖「大小」欄的右緣 +35 px → 存成 `Name=149;Size=110`，名稱欄固定在原本的 149 DIP，其他欄往右移，出現水平捲動條。
    - 再拖名稱欄 +400 px → `Name=378`。
    - Shift＋滾輪可以往右捲。
    - 以上都有截圖確認。面板的部分沒在本機實測。
- **還沒驗證**：面板拖曳帶的實際畫面和懸停效果，以及面板跟著欄寬變寬變窄。
- **面板右鍵選單時出現外框**（VM 截圖，Windows 10）：
  - 現象：打開右鍵選單時，面板周圍多了一圈灰色的粗外框。
  - 原因：為了讓選單在點擊外面時能關閉，選單出現前會把面板帶到前景。面板因此被啟用，`WM_NCACTIVATE` 交給 DefWindowProc 處理，它依 `WS_THICKFRAME` 畫出了外框。
  - 修正：面板自己處理 `WM_NCACTIVATE`，不畫任何東西，直接回傳 TRUE。依文件，處理這個訊息時必須回傳 TRUE，啟用狀態的切換才會完成；面板不會被最小化，所以不需要依文件交給 DefWindowProc。
  - 開發機是 Windows 11（圓角，由 DWM 畫邊框），看不到這個現象，要在 VM 上確認。

### 2026-11-12　第 6 階段：安裝程式加入繁體中文

- 使用者要求安裝程式要有繁中版，推翻同日稍早「只用英文」的決定。
- **查證**（Inno Setup 翻譯頁和 issrc 原始碼庫）：
  - 現在的翻譯頁把 Chinese (Traditional) 列在官方翻譯下，維護者是 GoneTone，適用 6.5.0 以上版本。
  - 但 **6.7.3 發行標籤**（`is-6_7_3`）裡，這個檔放在 `Files/Languages/Unofficial/`，當時還是非官方翻譯。它是後來才在 `main` 分支移到官方位置，所以 6.7.3 的安裝包沒有附帶。這解釋了本機 `Languages\` 資料夾裡找不到它的原因。
  - 我先前一度說它「其實是官方翻譯」，後來查證才知道：以 6.7.3 來說，它當時確實還是非官方的。
- **兩個版本比較**（都用 6.7.3 編譯）：
  - `main` 版（commit `6ef3219`，2026-08-04，沒有 BOM）：零警告。
  - 6.7.3 時期的非官方版（有 BOM）：至少有 3 條訊息不被這個版本認得，另外有 3 條缺漏，會改用英文，而且這只是前幾條警告。
  - 依 Inno 文件，6.3.0 起支援沒有 BOM 的 UTF-8 `.isl`；6.5.0 起，官方翻譯已經全部拿掉 BOM。
  - 採用 `main` 版。
- **授權**：
  - `.isl` 檔本身沒有版權或授權文字，只列了譯者，並註明部分翻譯參考了網路資源。
  - 它放在 issrc 原始碼庫裡，該庫採用 Inno Setup 授權：寬鬆授權，允許修改與再散布，條件是保留版權聲明、修改版要標明。這份授權跟 MIT 相容，因為這個檔是獨立的第三方檔案，不會併入 DirSize 的程式碼。
  - 這份授權沒有特別提到翻譯檔。「翻譯檔受原始碼庫授權涵蓋」是推論，文件沒有明講。使用者看過這些事實後決定放進 repo。
- **放法**：
  - `installer/lang/ChineseTraditional.isl` 和 `INNO-SETUP-LICENSE.txt`（同一個 commit 的 `license.txt`）完全不改內容。
  - `installer/lang/README.md` 記錄來源 commit、SHA-256 和授權說明。
  - 新增 `.gitattributes`，讓這兩個檔不做換行轉換，SHA-256 才能保持一致（這台機器的 `core.autocrlf=true`）。
- **`.iss` 的修改**：
  - 加回 `zhtw` 語言，自訂訊息兩種語言都有。
  - 加上 `ShowLanguageDialog=auto`：依 Windows 顯示語言自動選擇語言，兩種都不符合時才會詢問。
  - 用 6.7.3 編譯零警告。
- **還沒驗證**：繁中安裝精靈的實際畫面。安裝程式需要系統管理員權限，一啟動就會跳出 UAC，所以在開發機上沒有執行，要在中文和英文的 VM 上各看一次。
- **VM 測試**：中文 Windows 上，安裝精靈卻全部顯示英文。
  - 依文件，`UsePreviousLanguage` 預設是 yes，已經安裝過時會沿用上一次安裝的語言。VM 之前裝的是只有英文的版本，所以我推測是沿用了英文。
  - 這個原因還沒有證實：已經請使用者查 `(Get-UICulture).Name` 和解除安裝登錄機碼裡的 `Inno Setup: Language`，結果還沒回來。
  - 使用者同意後，加上 `UsePreviousLanguage=no`，讓安裝精靈一律跟著 Windows 介面語言，跟程式本身的行為一致。
  - 重新編譯：零警告。檔案大小跟上一版一樣，所以改用 SHA-256 分辨兩版。
  - 結果：使用者在中文 Windows 的 VM 上確認，安裝精靈已經是繁體中文。那兩項登錄值沒有查，所以「是沿用了上次的語言」這個原因還是推測，但加上 `UsePreviousLanguage=no` 之後問題就消失了。

### 2026-11-12　第 6 階段：VM 驗證結果（使用者回報）

- 面板右鍵選單：不再出現灰色外框（`WM_NCACTIVATE` 的修正有效）。
- 面板拖曳帶：看得到提示線，拖得動。
- 在面板標題列拖曳欄位分隔線：面板寬度跟著改變，其他欄維持原本的寬度。
- 名稱被截斷時，在面板裡停留滑鼠會顯示完整名稱。
- **沒有驗證**：
  - 英文 Windows 上安裝精靈是不是英文：使用者目前沒有英文 Windows 的 VM。
  - 右鍵選單點外面會不會關閉、關閉後焦點有沒有回到檔案總管：這兩項沒有另外回報。

### 2026-11-12　第 6 階段：VM 驗證（設計第 8 節第 5 步）

- **一般使用者＋管理員帳密安裝**：使用者回報 OK。面板的自動啟動和比例欄設定都寫在標準使用者自己的 HKCU，右鍵選單正常，重新登入後面板會自動啟動。
- **面板開著時升級**：安裝沒有失敗。升級後「快取還在、服務重新啟動」沒有另外確認。
- **解除安裝**：服務、`ProgramData\DirSize`（快取和記錄檔）、兩個右鍵選單機碼都清掉了。但 `Program Files\DirSize` **留下一個空資料夾**，沒有要求重新開機，也沒有殘留的行程。
  - 用 `unins000.exe /LOG=…` 取得記錄：資料夾裡的檔案都刪了，但記錄裡**沒有**刪除這個資料夾的那一行（`ProgramData\DirSize` 有），結尾是 `Removed all? Yes`。代表解除安裝程式根本沒有打算刪這個資料夾。
  - 推測：安裝前這個資料夾就已經存在（上一次留下來的；最早可能是第 4 階段 VM 測試時 `dirsize-service install` 自己複製檔案建立的），所以 Inno 不把它當成自己建立的資料夾。
  - 驗證：先手動刪除空資料夾，再安裝一次、解除安裝一次。這次記錄裡有 `Deleting directory: …\Program Files\DirSize`，資料夾也確實不見了。**推測證實，程式不需要修改。** 只有從舊的手動測試環境升級上來的機器，才會留下這個空資料夾。
- **還沒驗證**：英文 Windows 上的安裝精靈；升級後快取是否保留、服務是否重新啟動；面板右鍵選單點外面會不會關閉、焦點會不會回到檔案總管。在這台開發機上，從背景開啟的檔案總管沒有讓面板出現（追蹤記錄是空的），原因沒有查明，留到 VM 上確認。標題列、工作列和高 DPI 下的顯示還沒有實際看過。

### 2026-11-12　第 6 階段：GitHub Actions 自動建置

- **費用**：GitHub 的計費文件寫明，公開 repo 使用標準 GitHub-hosted runner 免費，也不算進每月的額度；要付費的是私人 repo 和 larger runner。DirSize 是公開 repo，用標準 runner。
- **順序**：先做 CI，再申請簽章。SignPath 的免費開源簽章只簽 CI 從原始碼建出來的檔案，所以 CI 是申請的前提。
- `.github/workflows/build.yml`：push 到 main、PR、手動觸發時執行。步驟依序是設定、建置 Release、ctest、`package-installer.ps1 -NoBuild`。安裝程式和免安裝 zip 存成這次執行的 artifact，保留 30 天。
- runner 固定用 `windows-2025`，不用 `windows-latest`，以免標籤換成別的映像。官方的軟體清單（Windows2025-Readme.md，映像版本 20261101）列有 Visual Studio Enterprise 2022 17.14、CMake 3.31.6（在 PATH 上）、Inno Setup 6.7.1、Windows SDK 10.0.26100。
- `package-installer.ps1` 原本把 cmake.exe 寫死成 VS Community 的路徑，runner 上的是 Enterprise 版。改成先用 PATH 上的 cmake.exe，找不到才用原來的路徑。在本機重跑一次，安裝程式和 zip 都正常產生。
- 使用的 action：`actions/checkout@v7`、`actions/upload-artifact@v7`（各自 releases 的最新版是 v7.0.1、v7.0.2）。workflow 的權限只開 `contents: read`。

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
| cli watch | [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes) |
| cli watch | [GetProcessIoCounters](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters) |
| cli watch | [IO_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-io_counters) |
| cli watch | [PROCESS_MEMORY_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters) |
| 快取 | [SHGetKnownFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath) |
| 快取 | [KNOWNFOLDERID（FOLDERID_ProgramData）](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid) |
| 快取 | [ConvertStringSecurityDescriptorToSecurityDescriptorW](https://learn.microsoft.com/en-us/windows/win32/api/sddl/nf-sddl-convertstringsecuritydescriptortosecuritydescriptorw) |
| 快取 | [Security Descriptor String Format](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-string-format) |
| 快取 | [ACE Strings](https://learn.microsoft.com/en-us/windows/win32/secauthz/ace-strings) |
| 快取 | [SID Strings](https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-strings) |
| 快取 | [GetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-getsecurityinfo) |
| 快取 | [GetAce](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-getace) |
| 快取 | [ACL_SIZE_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-acl_size_information) |
| 快取 | [ACE_HEADER](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-ace_header)；`ACCESS_ALLOWED_ACE` 的 `SidStart` 依 SDK 標頭檔 `winnt.h` |
| 快取 | [IsWellKnownSid](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-iswellknownsid)、[WELL_KNOWN_SID_TYPE](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-well_known_sid_type) |
| 快取 | [GetVolumeNameForVolumeMountPointW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumenameforvolumemountpointw) |
| 快取 | [GetVolumeInformationW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw)（比較用，沒有採用） |
| 快取 | [FSCTL_GET_NTFS_VOLUME_DATA](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_get_ntfs_volume_data)、[NTFS_VOLUME_DATA_BUFFER](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-ntfs_volume_data_buffer) |
| 快取 | [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) |
| 快取 | [CreateDirectoryW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createdirectoryw) |
| 快取 | [PNG Specification (Third Edition) 5.5 CRC algorithm](https://www.w3.org/TR/png-3/#5CRC-algorithm) |
| 第 3 階段修正 | [FILE_INFO_BY_HANDLE_CLASS（FileIdBothDirectoryRestartInfo）](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-file_info_by_handle_class) |
| 第 4 階段設計 | [StartServiceCtrlDispatcherW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-startservicectrldispatcherw) |
| 第 4 階段設計 | [RegisterServiceCtrlHandlerExW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-registerservicectrlhandlerexw) |
| 第 4 階段設計 | [LPHANDLER_FUNCTION_EX](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nc-winsvc-lphandler_function_ex) |
| 第 4 階段設計 | [SERVICE_STATUS](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_status) |
| 第 4 階段設計 | [Service Control Handler Function](https://learn.microsoft.com/en-us/windows/win32/services/service-control-handler-function) |
| 第 4 階段設計 | [SERVICE_DELAYED_AUTO_START_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_delayed_auto_start_info) |
| 第 4 階段設計 | [SERVICE_FAILURE_ACTIONSW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_failure_actionsw) |
| 第 4 階段設計 | [SC_ACTION](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-sc_action) |
| 第 4 階段設計 | [SERVICE_FAILURE_ACTIONS_FLAG](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_failure_actions_flag) |
| 第 4 階段設計 | [LocalSystem Account](https://learn.microsoft.com/en-us/windows/win32/services/localsystem-account) |
| 第 4 階段設計 | [LocalService Account](https://learn.microsoft.com/en-us/windows/win32/services/localservice-account) |
| 第 4 階段設計 | [SERVICE_REQUIRED_PRIVILEGES_INFOW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_required_privileges_infow) |
| 第 4 階段設計 | [SERVICE_SID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_sid_info) |
| 第 4 階段設計 | [CreateServiceW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-createservicew) |
| 第 4 階段設計 | [ChangeServiceConfig2W](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-changeserviceconfig2w) |
| 第 4 階段設計 | [CreateNamedPipeW](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createnamedpipew) |
| 第 4 階段設計 | [Pipe Names](https://learn.microsoft.com/en-us/windows/win32/ipc/pipe-names) |
| 第 4 階段設計 | [Named Pipe Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights) |
| 第 4 階段設計 | [ImpersonateNamedPipeClient](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-impersonatenamedpipeclient) |
| 第 4 階段設計 | [GetNamedPipeClientProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeclientprocessid) |
| 第 4 階段設計 | [GetNamedPipeServerProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeserverprocessid) |
| 第 4 階段設計 | [AccessCheck](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-accesscheck) |
| 第 4 階段設計 | [MapGenericMask](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-mapgenericmask) |
| 第 4 階段設計 | [Impersonation Levels](https://learn.microsoft.com/en-us/windows/win32/secauthz/impersonation-levels) |
| 第 4 階段設計 | [Impersonation Tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/impersonation-tokens) |
| 第 4 階段設計 | [Client Impersonation](https://learn.microsoft.com/en-us/windows/win32/secauthz/client-impersonation) |
| 第 4 階段設計 | [Verifying Client Access with ACLs in C++](https://learn.microsoft.com/en-us/windows/win32/secauthz/verifying-client-access-with-acls-in-c--) |
| 第 4 階段設計 | [OpenThreadToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openthreadtoken) |
| 第 4 階段設計 | [CreateFileW（SECURITY_SQOS_PRESENT）](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) |
| 第 4 階段設計 | [JOBOBJECT_CPU_RATE_CONTROL_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information) |
| 第 4 階段設計 | [AssignProcessToJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-assignprocesstojobobject) |
| 第 4 階段設計 | [SetPriorityClass](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass) |
| 第 4 階段設計 | [SetThreadPriority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) |
| 第 4 階段設計 | [SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation) |
| 第 4 階段設計 | [GetLastInputInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastinputinfo) |
| 第 4 階段設計 | [GetSystemTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes) |
| 第 4 階段設計 | [Power Setting GUIDs](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids) |
| 第 4 階段設計 | [RegisterPowerSettingNotification](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerpowersettingnotification) |
| 第 4 階段實作 | [ConnectNamedPipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-connectnamedpipe) |
| 第 4 階段實作 | [DisconnectNamedPipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-disconnectnamedpipe) |
| 第 4 階段實作 | [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/fileio/cancelioex-func) |
| 第 4 階段實作 | [GetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult) |
| 第 4 階段實作 | [ReadFile（訊息模式的 ERROR_MORE_DATA）](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile) |
| 第 4 階段實作 | [Service Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights) |
| 第 4 階段實作 | [QueryServiceStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryservicestatusex) |
| 第 4 階段實作 | [SERVICE_STATUS_PROCESS](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_status_process) |
| 第 4 階段實作 | [OpenSCManagerW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-openscmanagerw) |
| 第 4 階段實作 | [DeleteService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-deleteservice) |
| 第 4 階段實作 | [ControlService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-controlservice) |
| 第 4 階段實作 | [GetLogicalDrives](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlogicaldrives) |
| 第 4 階段實作 | [GetDriveTypeW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew) |
| 第 4 階段實作 | [CreateJobObjectW](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-createjobobjectw) |
| 第 4 階段實作 | [SetInformationJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-setinformationjobobject) |
| 第 4 階段實作 | [SetConsoleCtrlHandler](https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler) |
| 第 4 階段實作 | [HandlerRoutine](https://learn.microsoft.com/en-us/windows/console/handlerroutine) |
| 第 4 階段實作 | [SERVICE_PRESHUTDOWN_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_preshutdown_info) |
| 第 4 階段實作 | [SERVICE_DESCRIPTIONW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/ns-winsvc-service_descriptionw) |
| 第 5 階段設計 | [IShellWindows](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nn-exdisp-ishellwindows) |
| 第 5 階段設計 | [IShellWindows::Item](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-item) |
| 第 5 階段設計 | [IShellWindows::OnNavigate](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/nf-exdisp-ishellwindows-onnavigate) |
| 第 5 階段設計 | [ShellWindowTypeConstants](https://learn.microsoft.com/en-us/windows/win32/api/exdisp/ne-exdisp-shellwindowtypeconstants) |
| 第 5 階段設計 | [DWebBrowserEvents2（IE，封存）](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa768283(v=vs.85)) |
| 第 5 階段設計 | [NavigateComplete2（IE，封存）](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa768285(v=vs.85)) |
| 第 5 階段設計 | [IWebBrowser2 HWND（IE，封存）](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa752126(v=vs.85)) |
| 第 5 階段設計 | [IShellBrowser::QueryActiveShellView](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellbrowser-queryactiveshellview) |
| 第 5 階段設計 | [IFolderView::GetFolder](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview-getfolder) |
| 第 5 階段設計 | [IPersistFolder2::GetCurFolder](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ipersistfolder2-getcurfolder) |
| 第 5 階段設計 | [ShellFolderView](https://learn.microsoft.com/en-us/windows/win32/shell/shellfolderview) |
| 第 5 階段設計 | [ShellFolderView.Folder](https://learn.microsoft.com/en-us/windows/win32/shell/shellfolderview-folder) |
| 第 5 階段設計 | [ShellFolderView.SelectionChanged](https://learn.microsoft.com/en-us/windows/win32/shell/shellfolderview-selectionchanged) |
| 第 5 階段設計 | [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook) |
| 第 5 階段設計 | [Event Constants](https://learn.microsoft.com/en-us/windows/win32/winauto/event-constants) |
| 第 5 階段設計 | [SetWindowLongPtrW（GWLP_HWNDPARENT）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowlongptrw) |
| 第 5 階段設計 | [Window Features（Owned Windows）](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features) |
| 第 5 階段設計 | [DWMWINDOWATTRIBUTE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute) |
| 第 5 階段設計 | [Context Menu Handlers](https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers) |
| 第 5 階段設計 | [Integrate a packaged app with File Explorer](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/integrate-packaged-app-with-file-explorer) |
| 第 5 階段設計 | [Grant package identity by packaging with external location](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/grant-identity-to-nonpackaged-apps) |
| 第 5 階段設計 | [Support Dark and Light themes in Win32 apps](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-windows-themes) |
| 第 5 階段設計 | [High Contrast Parameter](https://learn.microsoft.com/en-us/windows/win32/winauto/high-contrast-parameter) |
| 第 5 階段實測 | [SHGetNameFromIDList](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-shgetnamefromidlist) |
| 第 5 階段實測 | [IOleWindow::GetWindow](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-iolewindow-getwindow) |
| 第 5 階段實測 | [IServiceProvider::QueryService](https://learn.microsoft.com/en-us/windows/win32/api/servprov/nf-servprov-iserviceprovider-queryservice(refguid_refiid_void)) |
| 第 5 階段實測 | [DwmGetWindowAttribute](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetwindowattribute) |
| 第 5 階段實測 | [SetProcessDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext) |
| 第 5 階段實測 | [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow) |
| 第 5 階段實測 | [GetWindow（GW_HWNDPREV、GW_OWNER）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow) |
| 第 5 階段實測 | [HKEY_CLASSES_ROOT（HKCU\Software\Classes 合併檢視）](https://learn.microsoft.com/en-us/windows/win32/sysinfo/hkey-classes-root-key) |
| 第 5 階段實測 | [SHChangeNotify](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify) |
| 第 5 階段實測 | [IShellView::GetItemObject](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellview-getitemobject) |
| 第 5 階段實測 | [_SVGIO](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_svgio) |
| 第 5 階段實測 | [IFolderView2::GetSelection](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview2-getselection) |
| 第 5 階段實測 | [IShellFolderViewDual](https://learn.microsoft.com/en-us/windows/win32/api/shldisp/nn-shldisp-ishellfolderviewdual) |
| 第 5 階段實測 | [IConnectionPointContainer::FindConnectionPoint](https://learn.microsoft.com/en-us/windows/win32/api/ocidl/nf-ocidl-iconnectionpointcontainer-findconnectionpoint) |
| 第 5 階段實測 | [IConnectionPoint::Advise](https://learn.microsoft.com/en-us/windows/win32/api/ocidl/nf-ocidl-iconnectionpoint-advise) |
| 第 5 階段實測 | [IShellItem::GetDisplayName](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellitem-getdisplayname) |
| 第 5 階段實測 | [Out-of-Context Hook Functions](https://learn.microsoft.com/en-us/windows/win32/winauto/out-of-context-hook-functions) |
| 第 5 階段實測 | [In-Context and Out-of-Context Hook Functions](https://learn.microsoft.com/en-us/windows/win32/winauto/in-context-and-out-of-context-hook-functions) |
| 第 5 階段實測 | [WinEvents overview](https://learn.microsoft.com/en-us/windows/win32/winauto/winevents-overview) |
| 第 5 階段實測 | [Single-Threaded Apartments](https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments) |
| 第 5 階段實測 | Windows SDK 10.0.26100.0 `um/shldisp.h`：`DShellFolderViewEvents`（`DIID_DShellFolderViewEvents`）；`um/shdispid.h`：`DISPID_SELECTIONCHANGED` |
| 第 5 階段實測 | Windows SDK 10.0.26100.0 `um/shlguid.h`：`SID_STopLevelBrowser`、`SID_SShellBrowser` 的定義；`um/exdisp.h`：`CLSID_ShellWindows`、`SWC_EXPLORER` |
| 第 5 階段前端 | [UISettings](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings) |
| 第 5 階段前端 | [UISettings.ColorValuesChanged](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings.colorvalueschanged) |
| 第 5 階段前端 | [UIColorType](https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uicolortype) |
| 第 5 階段前端 | [Setting the default DPI awareness for a process](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process) |
| 第 5 階段前端 | [SetThreadPriority（THREAD_MODE_BACKGROUND_BEGIN）](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) |
| 第 5 階段前端 | [SystemParametersInfoForDpi](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfofordpi) |
| 第 5 階段前端 | [WM_DPICHANGED](https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged) |
| 第 5 階段前端 | [CompareStringEx（SORT_DIGITSASNUMBERS）](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringex) |
| 第 5 階段前端 | [LoadStringW（cchBufferMax 為 0 時回傳唯讀指標）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadstringw) |
| 第 5 階段前端 | [IFileOpenDialog／FOS_PICKFOLDERS](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_fileopendialogoptions) |
| 第 5 階段前端 | [CreateAcceleratorTableW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createacceleratortablew) |
| 第 5 階段前端 | [QueryServiceConfigW](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryserviceconfigw) |
| 第 5 階段前端 | [COM Error Codes（CO_E_OBJNOTCONNECTED）](https://learn.microsoft.com/en-us/windows/win32/com/com-error-codes-1) |
| 第 5 階段前端 | [RPC Return Values（RPC_S_SERVER_UNAVAILABLE）](https://learn.microsoft.com/en-us/windows/win32/rpc/rpc-return-values) |
| 第 5 階段前端 | [Apply rounded corners in desktop apps for Windows 11](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/ui/apply-rounded-corners) |
| 第 5 階段前端 | [IShellBrowser::BrowseObject](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellbrowser-browseobject) |
| 第 5 階段前端 | [SHParseDisplayName](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shparsedisplayname) |
| 第 5 階段前端 | [ILFree](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-ilfree) |
| 第 5 階段前端 | [RegGetValueW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-reggetvaluew) |
| 第 5 階段前端 | [RegSetKeyValueW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetkeyvaluew) |
| 第 5 階段前端 | [WM_NCCALCSIZE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize) |
| 第 5 階段前端 | [WM_NCHITTEST](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-nchittest) |
| 第 5 階段前端 | [WM_GETMINMAXINFO](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-getminmaxinfo) |
| 資料夾樹 | [WIN32_FIND_DATAW（dwReserved0 是 reparse 標記）](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-win32_find_dataw) |
| 資料夾樹 | [FINDEX_SEARCH_OPS（FindExSearchLimitToDirectories 只是建議）](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ne-minwinbase-findex_search_ops) |
| 資料夾樹 | [FindFirstFileExW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfileexw) |
| 資料夾樹 | [SetThreadErrorMode](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setthreaderrormode) |
| 資料夾樹 | [GetLogicalDriveStringsW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlogicaldrivestringsw) |
| 資料夾樹 | [GetDriveTypeW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypew) |
| 資料夾樹 | [GetVolumeInformationW（標籤）](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationw) |
| 資料夾樹 | [CompareStringOrdinal](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringordinal) |
| 資料夾樹 | [WM_SETCURSOR](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-setcursor) |
| 資料夾樹 | [WM_CAPTURECHANGED](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-capturechanged) |
| 自繪捲軸 | [TrackMouseEvent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackmouseevent) |
| 自繪捲軸 | [WM_MOUSELEAVE](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseleave) |
| 自繪捲軸 | [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture) |
| 自繪捲軸 | [RoundRect](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-roundrect) |
| 自繪捲軸 | [SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer) |
| 面板規則 | [GetGUIThreadInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguithreadinfo) |
| 面板規則 | [GUITHREADINFO（hwndFocus）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-guithreadinfo) |
| 面板規則 | [About Tree-View Controls（WC_TREEVIEW = "SysTreeView32"）](https://learn.microsoft.com/en-us/windows/win32/controls/tree-view-controls) |
| 右鍵選單 | [Verbs and File Associations](https://learn.microsoft.com/en-us/windows/win32/shell/fa-verbs) |
| 右鍵選單 | [Creating Shortcut Menu Handlers（HKCU\Software\Classes 不需提升權限）](https://learn.microsoft.com/en-us/windows/win32/shell/context-menu-handlers) |
| 右鍵選單 | [Shell Extension Handlers（Predefined Shell Objects：Directory、Directory\Background、Drive）](https://learn.microsoft.com/en-us/windows/win32/shell/handlers) |
| 右鍵選單 | [SHChangeNotify（SHCNE_ASSOCCHANGED）](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify) |
| 右鍵選單 | [CommandLineToArgvW（反斜線與引號的規則）](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw) |
| 右鍵選單 | [Parsing C command-line arguments](https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments) |
| 右鍵選單 | [RegSetKeyValueW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regsetkeyvaluew) |
| 右鍵選單 | [RegDeleteTreeW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regdeletetreew) |
| 清單選單 | [WM_CONTEXTMENU（鍵盤開啟時座標為 -1）](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-contextmenu) |
| 清單選單 | [TrackPopupMenu（TPM_RETURNCMD）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu) |
| 清單選單 | [SetMenuDefaultItem](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setmenudefaultitem) |
| 清單選單 | [SHOpenFolderAndSelectItems](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shopenfolderandselectitems) |
| 清單選單 | [ShellExecuteW（explore）](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew) |
| 驗證工具 | [Run and RunOnce Registry Keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys) |
| 驗證工具 | [Message-Only Windows（HWND_MESSAGE）](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#message-only-windows) |
| 驗證工具 | [GetDpiForWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow) |
| 驗證工具 | [PrintWindow（PW_RENDERFULLCONTENT）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-printwindow) |
| 驗證工具 | [SetProcessDpiAwarenessContext](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext) |
| 第 6 階段設計 | [ServiceInstall Table](https://learn.microsoft.com/en-us/windows/win32/msi/serviceinstall-table) |
| 第 6 階段設計 | [ServiceControl Table](https://learn.microsoft.com/en-us/windows/win32/msi/servicecontrol-table) |
| 第 6 階段設計 | [MsiServiceConfig Table（SERVICE_CONFIG_DELAYED_AUTO_START，Windows Installer 5.0 起）](https://learn.microsoft.com/en-us/windows/win32/msi/msiserviceconfig-table) |
| 第 6 階段設計 | [Major Upgrades](https://learn.microsoft.com/en-us/windows/win32/msi/major-upgrades) |
| 第 6 階段設計 | [Uninstall Registry Key](https://learn.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key) |
| 第 6 階段設計 | [WiX Toolset README、LICENSE.TXT（MS-RL）、OSMFEULA.txt](https://github.com/wixtoolset/wix) |
| 第 6 階段設計 | [Inno Setup License](https://jrsoftware.org/files/is/license.txt) |
| 第 6 階段設計 | [User Interface Language Management](https://learn.microsoft.com/en-us/windows/win32/intl/user-interface-language-management) |
| 第 6 階段設計 | [SetThreadPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-setthreadpreferreduilanguages) |
| 第 6 階段設計 | [STRINGTABLE resource](https://learn.microsoft.com/en-us/windows/win32/menurc/stringtable-resource) |
| 第 6 階段設計 | [MUI Resource Management](https://learn.microsoft.com/en-us/windows/win32/intl/mui-resource-management) |
| 第 6 階段設計 | [How User Account Control works](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works) |
| 第 6 階段設計 | [Microsoft Defender SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/) |
| 第 6 階段設計 | [SetProcessPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-setprocesspreferreduilanguages) |
| 第 6 階段設計 | [GetThreadPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getthreadpreferreduilanguages) |
| 第 6 階段 | [CMake string(JSON)（3.19 起；錯誤用 ERROR_VARIABLE 回報）](https://cmake.org/cmake/help/latest/command/string.html) |
| 第 6 階段 | [CMake file(GLOB CONFIGURE_DEPENDS)、file(READ HEX)](https://cmake.org/cmake/help/latest/command/file.html) |
| 第 6 階段 | [LocaleNameToLCID](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-localenametolcid) |
| 第 6 階段 | [LOCALE_SNATIVEDISPLAYNAME（Windows 7 起）](https://learn.microsoft.com/en-us/windows/win32/intl/locale-snative-constants) |
| 第 6 階段 | [LANGUAGE statement](https://learn.microsoft.com/en-us/windows/win32/menurc/language-statement) |
| 第 6 階段 | [VERSIONINFO resource](https://learn.microsoft.com/en-us/windows/win32/menurc/versioninfo-resource) |
| 第 6 階段 | [StringFileInfo BLOCK statement](https://learn.microsoft.com/en-us/windows/win32/menurc/stringfileinfo-block) |
| 第 6 階段 | [VarFileInfo BLOCK statement](https://learn.microsoft.com/en-us/windows/win32/menurc/varfileinfo-block) |
| 第 6 階段 | [CMake configure_file（@ONLY）](https://cmake.org/cmake/help/latest/command/configure_file.html) |
| 第 6 階段 | [Using Registry String Redirection（MUIVerb）](https://learn.microsoft.com/en-us/windows/win32/intl/using-registry-string-redirection) |
| 第 6 階段 | [SHLoadIndirectString](https://learn.microsoft.com/en-us/windows/win32/api/shlwapi/nf-shlwapi-shloadindirectstring) |
| 第 6 階段設計 | [GetUserPreferredUILanguages](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserpreferreduilanguages) |
| 安裝程式 | [Inno Setup：[Setup] AppId](https://jrsoftware.org/ishelp/topic_setup_appid.htm) |
| 安裝程式 | [Inno Setup：[Setup] PrivilegesRequired](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm) |
| 安裝程式 | [Inno Setup：[Setup] ArchitecturesAllowed](https://jrsoftware.org/ishelp/topic_setup_architecturesallowed.htm) |
| 安裝程式 | [Inno Setup：[Setup] ArchitecturesInstallIn64BitMode](https://jrsoftware.org/ishelp/topic_setup_architecturesinstallin64bitmode.htm) |
| 安裝程式 | [Inno Setup：[Setup] MinVersion](https://jrsoftware.org/ishelp/topic_setup_minversion.htm) |
| 安裝程式 | [Inno Setup：[Setup] DisableDirPage](https://jrsoftware.org/ishelp/topic_setup_disabledirpage.htm) |
| 安裝程式 | [Inno Setup：[Setup] CloseApplications（使用 Restart Manager）](https://jrsoftware.org/ishelp/topic_setup_closeapplications.htm) |
| 安裝程式 | [Inno Setup：[Setup] AppMutex（沒有採用）](https://jrsoftware.org/ishelp/topic_setup_appmutex.htm) |
| 安裝程式 | [Inno Setup：[Setup] OutputBaseFilename（不要取名 setup）](https://jrsoftware.org/ishelp/topic_setup_outputbasefilename.htm) |
| 安裝程式 | [Inno Setup：[Setup] UninstallDisplayIcon](https://jrsoftware.org/ishelp/topic_setup_uninstalldisplayicon.htm) |
| 安裝程式 | [Inno Setup：[Setup] WizardStyle](https://jrsoftware.org/ishelp/topic_setup_wizardstyle.htm) |
| 安裝程式 | [Inno Setup：[Languages] section](https://jrsoftware.org/ishelp/topic_languagessection.htm) |
| 安裝程式 | [Inno Setup：[Registry] section](https://jrsoftware.org/ishelp/topic_registrysection.htm) |
| 安裝程式 | [Inno Setup：[Run] 與 [UninstallRun]（RunOnceId、postinstall）](https://jrsoftware.org/ishelp/topic_runsection.htm) |
| 安裝程式 | [Inno Setup：[UninstallDelete] section](https://jrsoftware.org/ishelp/topic_uninstalldeletesection.htm) |
| 安裝程式 | [Inno Setup：Pascal Scripting 事件函式（PrepareToInstall、CurStepChanged）](https://jrsoftware.org/ishelp/topic_scriptevents.htm) |
| 安裝程式 | [Inno Setup：Pascal Scripting Exec](https://jrsoftware.org/ishelp/topic_isxfunc_exec.htm) |
| 安裝程式 | [Inno Setup 翻譯清單（實測 6.7.3：繁體中文不在官方附帶的檔案裡，見 2026-11-12）](https://jrsoftware.org/files/istrans/) |
| 安裝程式 | [Inno Setup：SetupIconFile](https://jrsoftware.org/ishelp/topic_setup_setupiconfile.htm) |
| 安裝程式 | [Inno Setup 授權（license.txt）](https://jrsoftware.org/files/is/license.txt) |
| 安裝程式 | [issrc：ChineseTraditional.isl（commit 6ef3219）](https://github.com/jrsoftware/issrc/blob/6ef32198ef1f7b7b375cd4b6b90896c2a58eb4c2/Files/Languages/ChineseTraditional.isl) |
| 安裝程式 | [Inno Setup：Unicode（6.3 起 UTF-8 不需要 BOM）](https://jrsoftware.org/ishelp/topic_unicode.htm) |
| 安裝程式 | [Inno Setup：UsePreviousLanguage](https://jrsoftware.org/ishelp/topic_setup_usepreviouslanguage.htm) |
| 安裝程式 | [Inno Setup：LanguageDetectionMethod](https://jrsoftware.org/ishelp/topic_setup_languagedetectionmethod.htm) |
| 安裝程式 | [Inno Setup：ShowLanguageDialog](https://jrsoftware.org/ishelp/topic_setup_showlanguagedialog.htm) |
| 安裝程式 | [Inno Setup：命令列參數（/LANG）](https://jrsoftware.org/ishelp/topic_setupcmdline.htm) |
| 安裝程式 | [Inno Setup：[LangOptions]（LanguageCodePage）](https://jrsoftware.org/ishelp/topic_langoptionssection.htm) |
| 安裝程式 | [Inno Setup 6 更新紀錄（6.3.0 UTF-8、6.5.0 移除 BOM）](https://jrsoftware.org/files/is6-whatsnew.htm) |
| 安裝程式 | [Inno Setup：[Run] 與 [UninstallRun]（runasoriginaluser、unchecked）](https://jrsoftware.org/ishelp/topic_runsection.htm) |
| 安裝程式 | [Inno Setup：[Tasks]](https://jrsoftware.org/ishelp/topic_taskssection.htm) |
| 安裝程式 | [Inno Setup：UsePreviousTasks](https://jrsoftware.org/ishelp/topic_setup_useprevioustasks.htm) |
| 前端 | [CommandLineToArgvW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw) |
| 前端 | [RegisterWindowMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerwindowmessagew) |
| 前端 | [PostMessageW（HWND_BROADCAST）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postmessagew) |
| 前端 | [TrackPopupMenu（擁有者要在前景）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenu) |
| 前端 | [WM_NCACTIVATE（自行處理時回傳 TRUE）](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-ncactivate) |
| 圖示 | [App icon construction（Win32 至少 16、24、32、48、256）](https://learn.microsoft.com/en-us/windows/apps/design/style/iconography/app-icon-construction) |
| 圖示 | [ICON resource](https://learn.microsoft.com/en-us/windows/win32/menurc/icon-resource) |
| 圖示 | [LoadIconMetric（試過後不用，見 2026-11-12）](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-loadiconmetric) |
| 圖示 | [LoadImageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadimagew) |
| 圖示 | [GetSystemMetrics（SM_CXICON、SM_CXSMICON）](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics) |
| 安裝程式 | [taskkill（/im、/f）](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/taskkill) |
| 安裝程式 | [Controlling a Service Using SC（sc start）](https://learn.microsoft.com/en-us/windows/win32/services/controlling-a-service-using-sc) |
| 測試 | [/fsanitize](https://learn.microsoft.com/en-us/cpp/build/reference/fsanitize) |
| 測試 | [AddressSanitizer 建置參考](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-building) |
| 建置 | [CMAKE_MSVC_RUNTIME_LIBRARY](https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_RUNTIME_LIBRARY.html) |
| CI | [GitHub Actions billing（公開 repo 的標準 runner 免費）](https://docs.github.com/en/billing/concepts/product-billing/github-actions) |
| CI | [Windows Server 2025 runner 映像的軟體清單](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-Readme.md) |
| CI | [actions/checkout](https://github.com/actions/checkout) |
| CI | [actions/upload-artifact](https://github.com/actions/upload-artifact) |
