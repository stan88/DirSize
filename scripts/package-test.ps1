<#
.SYNOPSIS
    Builds Release and packs a test package for another machine (e.g. a VM).

.DESCRIPTION
    Not an installer (that is Phase 6): a zip with dirsize-service.exe,
    dirsize-cli.exe, service-test.ps1 and step-by-step test notes. The
    executables use the static CRT, so nothing else needs to be installed.
    "dirsize-service install" copies itself to Program Files\DirSize and
    registers the service. Output: dist\DirSize-test-<stamp>.zip (not
    tracked by git).

.EXAMPLE
    .\scripts\package-test.ps1
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
foreach ($exe in 'dirsize-service.exe', 'dirsize-cli.exe') {
    if (-not (Test-Path (Join-Path $release $exe))) { throw "missing $exe; build first" }
}

$commit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = (git -C $root status --porcelain) -ne $null
$version = if ($dirty) { "$commit + uncommitted changes" } else { $commit }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$name = "DirSize-test-$stamp"
$dist = Join-Path $root 'dist'
$folder = Join-Path $dist $name
New-Item -ItemType Directory -Force -Path $folder | Out-Null
Copy-Item (Join-Path $release 'dirsize-service.exe'), (Join-Path $release 'dirsize-cli.exe') $folder
Copy-Item (Join-Path $root 'scripts\service-test.ps1') $folder

$notes = @"
DirSize 服務測試包（不是正式安裝程式）
版本：$version
打包時間：$stamp

需求：Windows 10 1809 以上，至少一顆 NTFS 磁碟。不需要另外安裝執行階段。

== 測試步驟 ==

1. 把這個資料夾解壓縮到 VM，例如 C:\DirSizeTest。

2. 開「系統管理員」PowerShell，切到該資料夾：
     cd C:\DirSizeTest
     Get-ChildItem | Unblock-File                  # 解除「從網路下載」標記
     Set-ExecutionPolicy -Scope Process Bypass     # 只影響這個視窗
     .\dirsize-service.exe install
     Start-Service DirSize
     Get-Service DirSize                           # 應為 Running

3. 等 30 秒左右（第一次沒有快取，要整顆建索引），再開「一般」PowerShell
   （不要用系統管理員；最好另外建一個標準使用者帳戶登入來測）：
     cd C:\DirSizeTest
     Set-ExecutionPolicy -Scope Process Bypass
     .\service-test.ps1
   結果存在 results\ 資料夾。

4. 回到系統管理員 PowerShell，看服務記錄與資源用量：
     Get-Content C:\ProgramData\DirSize\service.log -Tail 60
     Get-Process dirsize-service | Format-List CPU, WorkingSet64, PeakWorkingSet64
     sc.exe qc DirSize
     sc.exe qprivs DirSize
     sc.exe qfailure DirSize

5. 重新開機。服務是「自動（延遲開始）」，登入後等 2～3 分鐘，重做步驟 3、4。
   這次記錄應該是「cache loaded」，不是整顆重建。

6. 測完要移除時（系統管理員 PowerShell）：
     .\dirsize-service.exe uninstall
   快取與記錄在 C:\ProgramData\DirSize，要清乾淨就手動刪除該資料夾。

== 請回傳 ==
- results\ 資料夾裡的檔案（步驟 3、5 各一份）
- 步驟 4 的輸出（兩次）
"@
# UTF-8 with BOM so Notepad shows the Chinese text correctly.
[IO.File]::WriteAllText((Join-Path $folder 'README-測試步驟.txt'), $notes, [Text.UTF8Encoding]::new($true))

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $zip
Write-Host "package: $zip"
Write-Host "version: $version"
