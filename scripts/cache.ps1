<#
.SYNOPSIS
    Runs "dirsize-cli cache save|load" on NTFS drives and saves the text output.

.DESCRIPTION
    save: builds each drive's index and saves it to
          %ProgramData%\DirSize\cache\<volume GUID>.cache.
    load: loads the cache, applies the journal changes made since the save,
          compares the result with a fresh full build, and saves it again.
    Run from an elevated PowerShell. Output goes to bench-results\ (not
    tracked by git). To test a restart, run save, restart Windows, run load.

.EXAMPLE
    .\scripts\cache.ps1 -Action save -Drives C,D
    .\scripts\cache.ps1 -Action load -Drives C,D
#>
param(
    [Parameter(Mandatory = $true)][ValidateSet('save', 'load')][string]$Action,
    [string[]]$Drives = @('C', 'D'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\bench-results')
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\dirsize-cli.exe'
if (-not (Test-Path $exe)) {
    throw "Build first: $exe not found."
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($drive in $Drives) {
    $file = Join-Path $OutDir "$stamp-cache-$Action-$drive.txt"
    Write-Host "cache $Action ${drive}: ..."
    $out = & $exe cache $Action "${drive}:"
    [IO.File]::WriteAllLines($file, [string[]]$out, [Text.UTF8Encoding]::new($false))
    $out | Select-String -Pattern 'failed|no usable cache|NEEDS RESCAN|not returned|Re-query|  id 0x|journal reasons|link:|saved|loaded|cached at|Resources|Memory now|catch-up since|replay|Maintained|Whole volume|Differing|never in journal  |closed since  |tracked open  |Result' |
        ForEach-Object { "  $_" }
    Write-Host "  -> $file"
}
