<#
.SYNOPSIS
    Queries the DirSize service through its pipe and saves the output.

.DESCRIPTION
    Run from an ordinary (NOT elevated) PowerShell, while the service runs
    either as "dirsize-service console" in an elevated window (use -Console)
    or as the installed service. Checks:
      - status and a few folder queries answer;
      - folders this user may not list are refused (negative tests);
      - malformed paths are rejected.
    Output goes to bench-results in the repo, or results next to this
    script in a test package.

.EXAMPLE
    .\scripts\service-test.ps1 -Console
    .\scripts\service-test.ps1
#>
param(
    [switch]$Console,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Continue'
# In a test package the CLI sits next to this script; in the repo it is in the build folder.
$exe = Join-Path $PSScriptRoot 'dirsize-cli.exe'
$inPackage = Test-Path $exe
if (-not $inPackage) { $exe = Join-Path $PSScriptRoot '..\build\Release\dirsize-cli.exe' }
if (-not (Test-Path $exe)) { throw "Build first: $exe not found." }
if ($OutDir -eq '') {
    $OutDir = if ($inPackage) { Join-Path $PSScriptRoot 'results' } else { Join-Path $PSScriptRoot '..\bench-results' }
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elevated) { Write-Warning 'This window is elevated; the negative tests need an ordinary window.' }

$flag = @()
if ($Console) { $flag = @('--console') }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$file = Join-Path $OutDir "$stamp-service-test.txt"
$lines = New-Object System.Collections.Generic.List[string]

function Run([string]$title, [string[]]$arguments) {
    $lines.Add("== $title")
    $out = & $exe @arguments 2>&1
    $lines.Add("exit code $LASTEXITCODE")
    foreach ($l in $out) { $lines.Add([string]$l) }
    $lines.Add('')
}

$lines.Add("Elevated: $elevated, console mode: $Console")
Run 'status' (@('status') + $flag)
Run 'query C:\ (top 20)' (@('query', 'C:\', '--count', '20') + $flag)
Run 'query C:\Users' (@('query', 'C:\Users') + $flag)
Run 'query C:\Windows (top 10)' (@('query', 'C:\Windows', '--count', '10') + $flag)
Run 'query D:\ (top 10)' (@('query', 'D:\', '--count', '10') + $flag)
Run 'NEGATIVE: C:\ProgramData\DirSize (SYSTEM and Administrators only)' (@('query', 'C:\ProgramData\DirSize') + $flag)
Run 'NEGATIVE: C:\System Volume Information' (@('query', 'C:\System Volume Information') + $flag)
Run 'NEGATIVE: UNC path' (@('query', '\\localhost\C$') + $flag)
Run 'NEGATIVE: device path' (@('query', '\\?\C:\Windows') + $flag)
Run 'NEGATIVE: dot-dot' (@('query', 'C:\Windows\..\Users') + $flag)
if (-not $Console) {
    Run 'without --console against the installed service (must connect)' @('status')
}

[IO.File]::WriteAllLines($file, $lines, [Text.UTF8Encoding]::new($false))
$lines | Select-String -Pattern '^==|exit code|service:|cannot|warning|^[A-Z]:  ' | ForEach-Object { "  $_" }
Write-Host "  -> $file"
