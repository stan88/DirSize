<#
.SYNOPSIS
    Runs the Phase 3 USN journal probe on NTFS drives and saves the text output.

.DESCRIPTION
    Run from an elevated PowerShell for the full probe; unelevated, only the
    access checks run. Output goes to bench-results\ (not tracked by git).

.EXAMPLE
    .\scripts\usn-probe.ps1 -Drives C,D
#>
param(
    [string[]]$Drives = @('C', 'D'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\bench-results')
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\usn-probe.exe'
if (-not (Test-Path $exe)) {
    throw "Build first: $exe not found."
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$tag = if ($isAdmin) { 'admin' } else { 'user' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($drive in $Drives) {
    $file = Join-Path $OutDir "$stamp-$tag-usn-$drive.txt"
    Write-Host "Probing ${drive}: ..."
    $out = & $exe $drive
    [IO.File]::WriteAllLines($file, [string[]]$out, [Text.UTF8Encoding]::new($false))
    $out | Select-String -Pattern 'open GENERIC_READ|History kept|Read whole journal|In index|IDs: error' |
        ForEach-Object { "  $_" }
    Write-Host "  -> $file"
}
