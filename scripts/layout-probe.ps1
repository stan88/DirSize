<#
.SYNOPSIS
    Runs the Phase 2 layout probe on NTFS drives and saves the text output.

.DESCRIPTION
    Each drive is probed -Runs times in a row. Run from an elevated
    PowerShell; without elevation FSCTL_QUERY_FILE_LAYOUT fails.

.EXAMPLE
    .\scripts\layout-probe.ps1 -Drives C,D
#>
param(
    [string[]]$Drives = @('C', 'D'),
    [int]$Runs = 2,
    [string]$OutDir = (Join-Path $PSScriptRoot '..\bench-results')
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\layout-probe.exe'
if (-not (Test-Path $exe)) {
    throw "Build first: $exe not found."
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$tag = if ($isAdmin) { 'admin' } else { 'user' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($drive in $Drives) {
    for ($run = 1; $run -le $Runs; $run++) {
        $file = Join-Path $OutDir "$stamp-$tag-layout-$drive-run$run.txt"
        Write-Host "Probing ${drive}: (run $run of $Runs) ..."
        $out = & $exe $drive
        [IO.File]::WriteAllLines($file, [string[]]$out, [Text.UTF8Encoding]::new($false))
        $out | Select-String -Pattern 'Enumeration ended|File entries|Enumerate time|Total time|Peak working set' |
            ForEach-Object { "  $_" }
    }
}
Write-Host "Results saved to $OutDir"
