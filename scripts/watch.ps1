<#
.SYNOPSIS
    Runs "dirsize-cli watch" on NTFS drives and saves the text output.

.DESCRIPTION
    Builds each drive's index, keeps it current with the USN journal for
    -Seconds, then compares it with a fresh full build. Run from an elevated
    PowerShell. Drives are watched one after another. Output goes to
    bench-results\ (not tracked by git).

.EXAMPLE
    .\scripts\watch.ps1 -Drives C,D -Seconds 300
#>
param(
    [string[]]$Drives = @('C', 'D'),
    [int]$Seconds = 300,
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
    $file = Join-Path $OutDir "$stamp-watch-$drive.txt"
    Write-Host "Watching ${drive}: for $Seconds s ..."
    $out = & $exe watch "${drive}:" $Seconds
    [IO.File]::WriteAllLines($file, [string[]]$out, [Text.UTF8Encoding]::new($false))
    $out | Select-String -Pattern 'failed|NEEDS RESCAN|replay|catch-ups|Resources|Memory now|Index tables|Maintained|Whole volume|Differing|never in journal  |closed since  |tracked open  |Result' |
        ForEach-Object { "  $_" }
    Write-Host "  -> $file"
}
