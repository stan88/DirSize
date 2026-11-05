<#
.SYNOPSIS
    Runs the Phase 5 Explorer probe steps one by one and saves the output.

.DESCRIPTION
    Run from an ordinary (NOT elevated) PowerShell. Each step waits for
    Enter before it starts, so Explorer can be set up first, and asks a few
    questions afterwards; the answers are saved with the results. Output goes
    to results next to this script in a test package, or bench-results in the
    repo. See docs/DESIGN-phase5.md section 8.

.EXAMPLE
    .\explorer-probe-test.ps1
    .\explorer-probe-test.ps1 -Steps 2,4
#>
param([int[]]$Steps = @(1, 2, 3, 4, 5, 6))

$ErrorActionPreference = 'Continue'
# The probe writes UTF-8; without this, non-ASCII folder names are garbled.
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$exe = Join-Path $PSScriptRoot 'explorer-probe.exe'
$inPackage = Test-Path $exe
if (-not $inPackage) { $exe = Join-Path $PSScriptRoot '..\build\Release\explorer-probe.exe' }
if (-not (Test-Path $exe)) { throw "Build first: $exe not found." }
$outDir = if ($inPackage) { Join-Path $PSScriptRoot 'results' } else { Join-Path $PSScriptRoot '..\bench-results' }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$answers = Join-Path $outDir "$stamp-answers.txt"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elevated) { Write-Warning 'This window is elevated; please use an ordinary window.' }

$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$os = "Windows $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)"
Set-Content -Path (Join-Path $outDir "$stamp-os.txt") -Value "$os, elevated: $elevated" -Encoding UTF8
Write-Host "OS: $os"

function Ask([string]$step, [string]$question) {
    $a = Read-Host "  [$step] $question"
    Add-Content -Path $answers -Value "[$step] $question`r`n    -> $a" -Encoding UTF8
}

function Step([string]$name, [string]$instructions, [string[]]$arguments) {
    Write-Host ''
    Write-Host "== $name" -ForegroundColor Cyan
    Write-Host $instructions
    Read-Host '按 Enter 開始，5 秒內把檔案總管切到前景' | Out-Null
    $file = Join-Path $outDir "$stamp-$name.txt"
    & $exe @arguments 2>&1 | Tee-Object -FilePath $file
    Write-Host "  -> $file"
}

$clicks = '在左邊樹狀窗格點幾個資料夾；在右邊清單點資料夾、點檔案、點空白處、選兩個；雙擊進入資料夾；按上一頁。也移動、縮放、縮到最小再還原檔案總管。90 秒後自動結束。'
if ($Steps -contains 1) {
    Step 'windows' '開兩個檔案總管視窗：一個選取一個資料夾，另一個什麼都不選。' @('windows')
}
if ($Steps -contains 2) {
    Step 'panel' $clicks @('dock', 'events', '--seconds', '90')
    Ask 'panel' '面板有沒有每次都出現、內容都對？（不對的話，是哪一種操作）'
    Ask 'panel' '面板有沒有正確貼在檔案總管右邊、移動縮放時有沒有跟上、會不會閃爍？'
}
if ($Steps -contains 7) {
    # Windows 11 tabs (Windows 10 has none).
    Step 'tabs-1' '只開一個檔案總管視窗，裡面開 3 個分頁，每個分頁在不同的資料夾。記下目前在前面的是第幾個分頁。（按 Enter 後不用切換視窗）' @('windows')
    Ask 'tabs-1' '剛才在前面的是第幾個分頁、哪個資料夾？'
    Step 'tabs-2' '切換到另一個分頁。（按 Enter 後不用切換視窗）' @('windows')
    Ask 'tabs-2' '剛才在前面的是第幾個分頁、哪個資料夾？'
    Step 'tabs-panel' '在 3 個分頁之間來回切換；每個分頁裡點資料夾、點空白處；開一個新分頁、關一個分頁。60 秒後自動結束。' @('dock', 'events', '--seconds', '60')
    Ask 'tabs-panel' '切換分頁後，面板顯示的是不是那個分頁的資料夾？開新分頁、關分頁時有沒有錯？'
}
if ($Steps -contains 3) {
    Step 'panel-owned' "和步驟 2 一樣的操作（面板改成檔案總管的 owned window）。$clicks" @('dock', 'owned', '--seconds', '90')
    Ask 'panel-owned' '面板有沒有每次都出現、內容都對？位置、移動、縮到最小再還原有沒有問題？'
}
if ($Steps -contains 4) {
    $hang = '先在右邊清單選一個資料夾讓面板出現。看到 HANG START 後，8 秒內在檔案總管點選、打字、拖曳視窗。40 秒後自動結束。'
    $q = '面板停止回應的 8 秒內，檔案總管有沒有跟著卡住？（有／沒有）'
    Step 'hang-plain' "第 1 種：不設 owner，不訂閱事件。$hang" @('dock', 'events', '--hang-after', '15', '--hang-for', '8', '--seconds', '40')
    Ask 'hang-plain' $q
    Step 'hang-nohooks' "第 2 種：不設 owner，也不用 WinEvent hook。$hang" @('dock', 'events', '--no-hooks', '--hang-after', '15', '--hang-for', '8', '--seconds', '40')
    Ask 'hang-nohooks' $q
    Step 'hang-owned' "第 3 種：owned window。$hang" @('dock', 'owned', '--hang-after', '15', '--hang-for', '8', '--seconds', '40')
    Ask 'hang-owned' $q
    Step 'hang-subscribe' "第 4 種：不設 owner，但訂閱選取事件（上一版的做法）。$hang" @('dock', 'events', '--subscribe', '--hang-after', '15', '--hang-for', '8', '--seconds', '40')
    Ask 'hang-subscribe' $q
}
if ($Steps -contains 6) {
    Step 'placement' '先選一個資料夾讓面板出現。依序：把檔案總管拖到靠近螢幕右邊緣（面板應改貼左邊外側）；拖到靠近左邊緣；把檔案總管拉寬到左右都放不下；最大化。每種情況都在檔案總管裡點幾下。90 秒後自動結束。' @('dock', 'events', '--seconds', '90')
    Ask 'placement' '靠右邊緣、靠左邊緣、左右都放不下、最大化，這四種情況面板各在哪裡？有沒有消失或被檔案總管蓋住？'
}
if ($Steps -contains 5) {
    Write-Host ''
    Write-Host '== verb' -ForegroundColor Cyan
    & $exe verb install
    Write-Host '在資料夾上按右鍵，也在資料夾裡的空白處按右鍵，找「DirSize probe」，點一次。'
    Ask 'verb' '「DirSize probe」出現在哪裡？（直接在選單裡／「顯示更多選項」裡／沒出現）點了之後有沒有跳出視窗顯示路徑？'
    & $exe verb remove
}
Write-Host ''
Write-Host "Results: $outDir"
