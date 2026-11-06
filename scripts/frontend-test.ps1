<#
.SYNOPSIS
    Phase 5 verification (docs/DESIGN-phase5.md section 8, step 6), guided
    step by step; results are saved for review.

.DESCRIPTION
    Parts (-Parts):
      Theme       light, dark, high contrast, switched while the window is
                  open: a screenshot of the window after each, and answers.
      Dpi         the window and the panel on monitors with different
                  scaling: screenshots, the window's DPI, answers.
      Service     with the DirSize service installed (elevated window):
                  running -> sizes from the service; stopped; started again
                  (how long until the service answers).
      ScheduleAfterBoot  registers AfterBoot to run once at the next sign-in
                  (HKCU RunOnce), so it starts before the delayed start.
      AfterBoot   right after signing in, during the service's delayed
                  start: dirsize --check every 2 s until the service answers.
      NotInstalled  uninstalls the service, checks, installs it again
                  (elevated window).
      Panel       CPU and memory of dirsize --panel, sampled every second,
                  while many Explorer windows and tabs are open.
    The Service, AfterBoot and NotInstalled parts read what the status line
    would say through "dirsize.exe --check <folder> <file>" (the same data
    source as the window, without a window).

    Output: results\ next to this script in a test package, or
    bench-results\ in the repo (not tracked by git).

.EXAMPLE
    .\frontend-test.ps1 -Parts Theme,Dpi
    .\frontend-test.ps1 -Parts Service          # elevated window
    .\frontend-test.ps1 -Parts AfterBoot        # right after signing in
    .\frontend-test.ps1 -Parts Panel -Seconds 180
#>
param(
    [ValidateSet('Theme', 'Dpi', 'Service', 'ScheduleAfterBoot', 'AfterBoot', 'NotInstalled', 'Panel')]
    [string[]]$Parts = @('Theme', 'Dpi'),
    [string]$Folder = $env:windir,
    [int]$Seconds = 120
)

$ErrorActionPreference = 'Continue'

$exe = Join-Path $PSScriptRoot 'dirsize.exe'
$inPackage = Test-Path $exe
if (-not $inPackage) { $exe = Join-Path $PSScriptRoot '..\build\Release\dirsize.exe' }
if (-not (Test-Path $exe)) { throw "Build first: $exe not found." }
$exe = (Resolve-Path $exe).Path
$serviceExe = Join-Path (Split-Path $exe) 'dirsize-service.exe'
$outDir = if ($inPackage) { Join-Path $PSScriptRoot 'results' } else { Join-Path $PSScriptRoot '..\bench-results' }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outDir = (Resolve-Path $outDir).Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$answers = Join-Path $outDir "$stamp-frontend-answers.txt"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$os = "Windows $($cv.DisplayVersion) build $($cv.CurrentBuild).$($cv.UBR)"
Add-Content -Path $answers -Value "OS: $os, elevated: $elevated, parts: $($Parts -join ',')" -Encoding UTF8
Write-Host "OS: $os"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class FtWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
}
"@
[FtWin]::SetProcessDPIAware() | Out-Null  # screenshots in physical pixels

function Ask([string]$part, [string]$question) {
    $a = Read-Host "  $question"
    Add-Content -Path $answers -Value "[$part] $question -> $a" -Encoding UTF8
}

function Note([string]$part, [string]$text) {
    Write-Host "  $text"
    Add-Content -Path $answers -Value "[$part] $text" -Encoding UTF8
}

function Shot([IntPtr]$hwnd, [string]$name) {
    if ($hwnd -eq [IntPtr]::Zero -or -not [FtWin]::IsWindowVisible($hwnd)) {
        Write-Host "  (window not visible: no screenshot $name)"
        return
    }
    $r = New-Object FtWin+RECT
    [FtWin]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -le 0 -or $h -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [FtWin]::PrintWindow($hwnd, $dc, 2) | Out-Null  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $file = Join-Path $outDir "$stamp-$name.png"
    $bmp.Save($file)
    $bmp.Dispose()
    Write-Host "  screenshot: $file"
}

function Check([string]$part, [string]$folder) {
    $file = Join-Path $outDir "$stamp-check.txt"
    $before = if (Test-Path $file) { (Get-Content $file -Encoding UTF8).Count } else { 0 }
    $p = Start-Process $exe -ArgumentList '--check', "`"$folder`"", "`"$file`"" -Wait -PassThru
    $lines = @(Get-Content $file -Encoding UTF8)
    $line = if ($lines.Count -gt $before) { $lines[-1] } else { "(no result, exit $($p.ExitCode))" }
    Add-Content -Path $answers -Value "[$part] check: $line" -Encoding UTF8
    return $line
}

function StartWindow() {
    $p = Start-Process $exe -ArgumentList "`"$Folder`"" -PassThru
    for ($i = 0; $i -lt 50 -and $p.MainWindowHandle -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    Start-Sleep -Seconds 2
    return $p
}

function StopWindow($p) {
    if ($p -and -not $p.HasExited) {
        $p.CloseMainWindow() | Out-Null
        if (-not $p.WaitForExit(3000)) { $p.Kill() }
    }
}

function Wait([string]$text) { Read-Host "  $text，完成後按 Enter" | Out-Null }

# The running "dirsize.exe --panel" process, started if there is none.
# Returns @(process, startedHere).
function EnsurePanel() {
    $running = Get-CimInstance Win32_Process -Filter "Name='dirsize.exe'" |
        Where-Object { $_.CommandLine -match '--panel' } | Select-Object -First 1
    if ($running) { return @((Get-Process -Id $running.ProcessId), $false) }
    $p = Start-Process $exe -ArgumentList '--panel' -PassThru
    Start-Sleep -Seconds 2
    return @($p, $true)
}

# ---- parts --------------------------------------------------------------------

function Part-Theme {
    Write-Host "`n== 主題（獨立視窗開著時切換） =="
    $p = StartWindow
    foreach ($t in @(
            @{ name = 'light'; text = '到「設定 > 個人化 > 色彩」選「淺色」' },
            @{ name = 'dark'; text = '選「深色」' },
            @{ name = 'contrast'; text = '到「設定 > 協助工具 > 對比佈景主題」選一個（例如「沙漠」或「夜空」）並套用' },
            @{ name = 'contrast-off'; text = '把對比佈景主題改回「無」' })) {
        Wait $t.text
        Start-Sleep -Seconds 2
        $p.Refresh()
        Shot $p.MainWindowHandle "theme-$($t.name)"
        Ask "theme-$($t.name)" '視窗有沒有立刻跟著變（不用重開）？文字、清單、樹、捲軸、比例長條、標題列都看得清楚嗎？（y/n＋說明）'
    }
    $panel, $startedPanel = EnsurePanel
    Wait '在檔案總管點一個資料夾，讓面板出現（淺色或深色都可以；也可以在面板開著時切換主題看看）'
    Shot ([FtWin]::FindWindowW('DirSizePanel', $null)) 'theme-panel'
    Ask 'theme-panel' '面板的顏色和獨立視窗一致嗎？切換主題時面板也跟著變嗎？（y/n＋說明）'
    if ($startedPanel) { Stop-Process -Id $panel.Id -ErrorAction SilentlyContinue }
    StopWindow $p
}

function Part-Dpi {
    Write-Host "`n== 高 DPI 與多螢幕 =="
    $p = StartWindow
    Note 'dpi' "start: window DPI $([FtWin]::GetDpiForWindow($p.MainWindowHandle))"
    Shot $p.MainWindowHandle 'dpi-start'
    Wait '把 DirSize 視窗拖到另一個縮放比例不同的螢幕（只有一個螢幕的話：到「設定 > 系統 > 顯示器」改縮放比例）'
    Start-Sleep -Seconds 1
    Note 'dpi' "moved: window DPI $([FtWin]::GetDpiForWindow($p.MainWindowHandle))"
    Shot $p.MainWindowHandle 'dpi-moved'
    Ask 'dpi' '字、列高、圖示、捲軸、分隔線有沒有跟著縮放，沒有模糊、沒有被切掉？（y/n＋說明）'
    $panelProcess, $startedPanel = EnsurePanel
    Wait '把檔案總管拖到另一個螢幕，點一個資料夾讓面板出現'
    $panel = [FtWin]::FindWindowW('DirSizePanel', $null)
    if ($panel -ne [IntPtr]::Zero) { Note 'dpi' "panel DPI $([FtWin]::GetDpiForWindow($panel))" }
    Shot $panel 'dpi-panel'
    Ask 'dpi-panel' '面板有沒有貼在檔案總管旁邊、大小和字都正確？（y/n＋說明）'
    if ($startedPanel) { Stop-Process -Id $panelProcess.Id -ErrorAction SilentlyContinue }
    StopWindow $p
}

function Part-Service {
    Write-Host "`n== 服務：執行中 → 停止 → 再啟動 =="
    if (-not $elevated) { Write-Warning '這一部分要停止和啟動服務，請用「系統管理員」PowerShell。'; return }
    $svc = Get-Service DirSize -ErrorAction SilentlyContinue
    if (-not $svc) { Write-Warning '沒有安裝 DirSize 服務（先用 dirsize-service.exe install）。'; return }
    if ($svc.Status -ne 'Running') { Start-Service DirSize; Start-Sleep -Seconds 5 }
    Note 'service' "running: $(Check 'service' $Folder)"
    Note 'service' '預期：source=service note=none（或 updating）'
    Stop-Service DirSize
    Start-Sleep -Seconds 2
    Note 'service' "stopped: $(Check 'service' $Folder)"
    Note 'service' '預期：source=walk；服務設成自動啟動，所以 note=starting（停止和「延遲啟動還沒開始」分不出來）'
    Start-Service DirSize
    $t0 = Get-Date
    for ($i = 0; $i -lt 90; $i++) {
        $line = Check 'service' $Folder
        if ($line -match 'source=service') { break }
        Start-Sleep -Seconds 2
    }
    Note 'service' ("started again: {0:N0} s until the service answered: $line" -f ((Get-Date) - $t0).TotalSeconds)
    $p = StartWindow
    Shot $p.MainWindowHandle 'service-window'
    Ask 'service' '狀態列是不是寫「來源：DirSize 服務」？（y/n）'
    StopWindow $p
}

function Part-ScheduleAfterBoot {
    Write-Host "`n== 下次登入時自動執行 AfterBoot =="
    # HKCU RunOnce: runs once at the next sign-in of this user, then the
    # value is deleted (Run and RunOnce Registry Keys). A command line is
    # limited to 260 characters there.
    $script = $PSCommandPath
    $command = "powershell.exe -NoExit -ExecutionPolicy Bypass -File `"$script`" -Parts AfterBoot -Folder `"$Folder`""
    if ($command.Length -gt 260) { Write-Warning "命令超過 260 字元，請把測試包放到較短的路徑：$command"; return }
    Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce' -Name 'DirSizeAfterBoot' -Value $command
    Note 'schedule' "RunOnce registered: $command"
    Write-Host '  現在請重新開機並登入，PowerShell 視窗會自動開啟並開始記錄。'
}

function Part-AfterBoot {
    Write-Host "`n== 開機後的延遲啟動（請在登入後馬上執行） =="
    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
    Note 'boot' ("seconds since boot: {0:N0}" -f ((Get-Date) - $boot).TotalSeconds)
    $p = StartWindow
    Shot $p.MainWindowHandle 'boot-window-first'
    for ($i = 0; $i -lt 150; $i++) {
        $line = Check 'boot' $Folder
        Write-Host ("  {0,4:N0} s after boot: $line" -f ((Get-Date) - $boot).TotalSeconds)
        if ($line -match 'source=service') { break }
        Start-Sleep -Seconds 2
    }
    Note 'boot' ("first answer from the service {0:N0} s after boot" -f ((Get-Date) - $boot).TotalSeconds)
    Ask 'boot' '一開始的視窗狀態列，是不是寫「服務啟動中，先逐目錄掃描」？（看截圖 boot-window-first；y/n）'
    StopWindow $p
}

function Part-NotInstalled {
    Write-Host "`n== 沒有安裝服務 =="
    if (-not $elevated) { Write-Warning '這一部分要移除和重新安裝服務，請用「系統管理員」PowerShell。'; return }
    if (-not (Test-Path $serviceExe)) { Write-Warning "找不到 $serviceExe"; return }
    $installed = [bool](Get-Service DirSize -ErrorAction SilentlyContinue)
    if ($installed) {
        $ok = Read-Host '  會移除 DirSize 服務，檢查後再裝回去。繼續嗎？(y/n)'
        if ($ok -ne 'y') { return }
        & $serviceExe uninstall
    }
    Note 'notinstalled' "not installed: $(Check 'notinstalled' $Folder)"
    Note 'notinstalled' '預期：source=walk note=not-installed'
    $p = StartWindow
    Shot $p.MainWindowHandle 'notinstalled-window'
    Ask 'notinstalled' '狀態列是不是寫「未安裝服務，改為逐目錄掃描」，而且有顯示大小？（y/n）'
    StopWindow $p
    if ($installed) {
        & $serviceExe install
        Start-Service DirSize
        Note 'notinstalled' "reinstalled: $((Get-Service DirSize).Status)"
    }
}

function Part-Panel {
    Write-Host "`n== 面板的 CPU 和記憶體（$Seconds 秒） =="
    $panel, $started = EnsurePanel
    Write-Host '  開始後請：開 8～10 個檔案總管視窗（Windows 11 可以再開幾個分頁），'
    Write-Host '  在各視窗之間切換、點資料夾、捲動；最後留幾秒什麼都不做。'
    Wait '準備好'
    $shell = New-Object -ComObject Shell.Application
    $csv = Join-Path $outDir "$stamp-panel-samples.csv"
    'second,cpu_ms,working_set_mb,private_mb,explorer_views' | Set-Content -Path $csv -Encoding UTF8
    $panel.Refresh()
    $cpu0 = $panel.TotalProcessorTime.TotalMilliseconds
    $peakWs = 0; $peakPriv = 0; $maxViews = 0
    $t0 = Get-Date
    for ($s = 1; $s -le $Seconds; $s++) {
        Start-Sleep -Seconds 1
        $panel.Refresh()
        if ($panel.HasExited) { Note 'panel' 'panel exited during the run'; break }
        $views = try { $shell.Windows().Count } catch { -1 }
        $ws = [math]::Round($panel.WorkingSet64 / 1MB, 1)
        $priv = [math]::Round($panel.PrivateMemorySize64 / 1MB, 1)
        $cpu = [math]::Round($panel.TotalProcessorTime.TotalMilliseconds - $cpu0)
        "$s,$cpu,$ws,$priv,$views" | Add-Content -Path $csv -Encoding UTF8
        $peakWs = [math]::Max($peakWs, $ws); $peakPriv = [math]::Max($peakPriv, $priv); $maxViews = [math]::Max($maxViews, $views)
        Write-Progress -Activity 'panel' -Status "$s / $Seconds s, Explorer views $views, CPU $cpu ms, WS $ws MB" -PercentComplete (100 * $s / $Seconds)
    }
    Write-Progress -Activity 'panel' -Completed
    $wall = ((Get-Date) - $t0).TotalMilliseconds
    $panel.Refresh()
    $cpuTotal = $panel.TotalProcessorTime.TotalMilliseconds - $cpu0
    Note 'panel' ("CPU {0:N0} ms over {1:N0} s = {2:N2}% of one core; peak working set {3} MB, peak private {4} MB; up to {5} Explorer views" -f `
            $cpuTotal, ($wall / 1000), (100 * $cpuTotal / $wall), $peakWs, $peakPriv, $maxViews)
    Note 'panel' "samples: $csv"
    Ask 'panel' '過程中面板有沒有跟丟、卡住、閃爍，或檔案總管變慢？（y/n＋說明）'
    if ($started) { Stop-Process -Id $panel.Id -ErrorAction SilentlyContinue }
}

foreach ($part in $Parts) { & "Part-$part" }
Write-Host "`nanswers: $answers"
