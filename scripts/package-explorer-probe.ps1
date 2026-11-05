<#
.SYNOPSIS
    Builds the Phase 5 Explorer probe and packs it for another machine (e.g. a VM).

.DESCRIPTION
    Nothing is installed: explorer-probe.exe runs from any folder with
    ordinary rights (static CRT). Only "verb install" writes to HKCU, and
    the test script removes it again. Output:
    dist\DirSize-explorer-probe-<stamp>.zip (not tracked by git).

.EXAMPLE
    .\scripts\package-explorer-probe.ps1
#>
param([switch]$NoBuild)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not $NoBuild) {
    & $cmake --build (Join-Path $root 'build') --config Release --target explorer-probe
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$probe = Join-Path $root 'build\Release\explorer-probe.exe'
if (-not (Test-Path $probe)) { throw 'missing explorer-probe.exe; build first' }

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain) -ne $null
$version = if ($dirty) { "$commit + uncommitted changes" } else { $commit }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$name = "DirSize-explorer-probe-$stamp"
$dist = Join-Path $root 'dist'
$folder = Join-Path $dist $name
New-Item -ItemType Directory -Force -Path $folder | Out-Null
Copy-Item $probe, (Join-Path $root 'scripts\explorer-probe-test.ps1') $folder

$notes = @"
DirSize 檔案總管實測包（第 5 階段，不出貨）
版本：$version
打包時間：$stamp

不需要安裝，也不需要系統管理員權限。只有步驟 5 會在目前使用者的 HKCU
加一個右鍵選單項目，測完立刻自動移除。這一版面板只列子資料夾名稱，
大小要等正式前端向服務查詢。

== 測試步驟 ==

1. 解壓縮到 VM，例如 C:\DirSizeProbe。

2. 開「一般」PowerShell（不要用系統管理員）：
     cd C:\DirSizeProbe
     Get-ChildItem | Unblock-File
     Set-ExecutionPolicy -Scope Process Bypass
     .\explorer-probe-test.ps1

3. 腳本會一步一步提示，每步按 Enter 才開始；按 Enter 後 5 秒內把檔案總管
   切到前景。每步結束後會問幾個問題，回答會存進 results\。
   面板的規則：
   - 切換資料夾（左邊樹狀窗格、雙擊進入、上一頁）→ 列出目前資料夾的子資料夾
   - 在右邊清單選一個資料夾 → 改列出它的子資料夾
   - 之後點檔案、點空白處、選多個 → 隱藏
   - 「本機」這類不是資料夾的位置 → 隱藏
   1 列出檔案總管視窗（一個選一個資料夾，另一個什麼都不選）
   2 面板（不設 owner）：照提示操作 90 秒
   3 面板（owned window）：同樣操作 90 秒
   4 面板停止回應 8 秒時，檔案總管會不會跟著卡住：四種方式，各 40 秒
   5 右鍵選單「DirSize probe」出現的位置
   6 面板位置：靠螢幕右邊緣、左邊緣、左右都放不下、最大化

   這一輪只需要跑步驟 6：.\explorer-probe-test.ps1 -Steps 6

   只想重跑某幾步：.\explorer-probe-test.ps1 -Steps 2,4

== 請回傳 ==
- results\ 資料夾裡的所有檔案（回答都在 *-answers.txt）
"@
# UTF-8 with BOM so Notepad shows the Chinese text correctly.
[IO.File]::WriteAllText((Join-Path $folder 'README-測試步驟.txt'), $notes, [Text.UTF8Encoding]::new($true))

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
Write-Host "package: $zip"
Write-Host "version: $version"
