<#
.SYNOPSIS
    Runs dirsize-cli against whole drives and saves the JSON results.

.DESCRIPTION
    Each drive is scanned -Runs times in a row. The first run reflects the
    file-system cache as it was; later runs are warm. Run from an elevated
    PowerShell to include folders that normal users cannot read.

.EXAMPLE
    .\scripts\bench.ps1 -Drives C,D,P
#>
param(
    [string[]]$Drives = @('C', 'D'),
    [int]$Runs = 2,
    [string]$OutDir = (Join-Path $PSScriptRoot '..\bench-results')
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\build\Release\dirsize-cli.exe'
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
        $file = Join-Path $OutDir "$stamp-$tag-$drive-run$run.json"
        Write-Host "Scanning ${drive}: (run $run of $Runs) ..."
        $json = & $exe scan "${drive}:\" --top 10 --json
        [IO.File]::WriteAllLines($file, [string[]]$json, [Text.UTF8Encoding]::new($false))
        $r = ($json -join "`n") | ConvertFrom-Json
        '{0}: run {1}  {2,8:N1} s  {3,10:N0} files  {4,8:N0} folders  denied {5}  backupPrivilege {6}' -f `
            $drive, $run, $r.elapsedSeconds, $r.files, $r.folders, $r.accessDenied, $r.backupPrivilege
    }
}
Write-Host "Results saved in $((Resolve-Path $OutDir).Path)"
