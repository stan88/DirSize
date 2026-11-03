<#
.SYNOPSIS
    Runs "dirsize-cli verify" on NTFS drives and saves the text output.

.DESCRIPTION
    Builds the Phase 2 folder index with both engines and compares them.
    Run from an elevated PowerShell; the NTFS engine needs elevation.

.EXAMPLE
    .\scripts\verify.ps1 -Drives C,D
#>
param(
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
    $file = Join-Path $OutDir "$stamp-verify-$drive.txt"
    Write-Host "Verifying ${drive}: ..."
    $out = & $exe verify $drive
    [IO.File]::WriteAllLines($file, [string[]]$out, [Text.UTF8Encoding]::new($false))
    $out | Select-String -Pattern 'failed|Time |Memory:|Whole volume|Differing folders|Root files listed|Result:' |
        ForEach-Object { "  $_" }
}
Write-Host "Results saved to $OutDir"
