<#
.SYNOPSIS
    Builds Release and packs the Phase 5 front-end verification package for
    another machine (a VM).

.DESCRIPTION
    A zip with dirsize.exe, dirsize-service.exe, dirsize-cli.exe,
    frontend-test.ps1 and step-by-step notes (not an installer: Phase 6).
    Output: dist\DirSize-frontend-test-<stamp>.zip (not tracked by git).

.EXAMPLE
    .\scripts\package-frontend-test.ps1
#>
param([switch]$NoBuild)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not $NoBuild) {
    & $cmake --build (Join-Path $root 'build') --config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$release = Join-Path $root 'build\Release'
$exes = 'dirsize.exe', 'dirsize-service.exe', 'dirsize-cli.exe'
foreach ($exe in $exes) {
    if (-not (Test-Path (Join-Path $release $exe))) { throw "missing $exe; build first" }
}

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain) -ne $null
$version = if ($dirty) { "$commit + uncommitted changes" } else { $commit }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$name = "DirSize-frontend-test-$stamp"
$dist = Join-Path $root 'dist'
$folder = Join-Path $dist $name
New-Item -ItemType Directory -Force -Path $folder | Out-Null
foreach ($exe in $exes) { Copy-Item (Join-Path $release $exe) $folder }
Copy-Item (Join-Path $root 'scripts\frontend-test.ps1') $folder

$notes = @"
DirSize 前端驗證包（第 5 階段，不是正式安裝程式）
版本：$version
打包時間：$stamp

需求：Windows 10 1809 以上，至少一顆 NTFS 磁碟。不需要另外安裝執行階段。
結果都存在這個資料夾的 results\ 裡，做完請整個傳回來。

== 準備 ==

1. 解壓縮到 VM，例如 C:\DirSizeFrontendTest。
2. 開 PowerShell，切到該資料夾：
     cd C:\DirSizeFrontendTest
     Get-ChildItem | Unblock-File                  # 解除「從網路下載」標記
     Set-ExecutionPolicy -Scope Process Bypass     # 只影響這個視窗

== A. 服務（「系統管理員」PowerShell） ==

3. 安裝服務（已經裝過舊版的話，先 .\dirsize-service.exe uninstall）：
     .\dirsize-service.exe install
     Start-Service DirSize
   等 30 秒左右讓它建索引。
4. 執行：
     .\frontend-test.ps1 -Parts Service
   會檢查：執行中 → 停止 → 再啟動，並截一張獨立視窗的圖。

== B. 開機後的延遲啟動（一般 PowerShell） ==

5. 先登記「下次登入時自動執行」：
     .\frontend-test.ps1 -Parts ScheduleAfterBoot
   然後重新開機並登入，PowerShell 視窗會自動開啟，每 2 秒檢查一次，
   直到服務開始回答（服務是延遲啟動，手動開視窗通常來不及）。

== C. 沒有安裝服務（「系統管理員」PowerShell） ==

6. 執行（會先移除服務，檢查完再裝回去）：
     .\frontend-test.ps1 -Parts NotInstalled

== D. 面板的 CPU 和記憶體（一般 PowerShell） ==

7. 執行：
     .\frontend-test.ps1 -Parts Panel -Seconds 180
   照提示開 8～10 個檔案總管視窗，切換、點資料夾、捲動。

== E. 主題、高 DPI（一般 PowerShell，開發機也可以做） ==

8. 執行：
     .\frontend-test.ps1 -Parts Theme,Dpi
   照提示切換淺色、深色、對比佈景主題，以及把視窗拖到另一個螢幕或改縮放比例。

== 請回傳 ==
- results\ 資料夾裡的所有檔案（回答在 *-frontend-answers.txt，截圖是 *.png）
"@
# UTF-8 with BOM so Notepad shows the Chinese text correctly.
[IO.File]::WriteAllText((Join-Path $folder 'README-測試步驟.txt'), $notes, [Text.UTF8Encoding]::new($true))

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
Write-Host "package: $zip"
Write-Host "version: $version"
