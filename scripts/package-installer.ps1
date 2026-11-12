<#
.SYNOPSIS
    Builds Release and makes the installer and the portable zip.

.DESCRIPTION
    Needs Inno Setup 6.5 or later (ISCC.exe). The version comes from
    project(VERSION) in CMakeLists.txt.
    Output (not tracked by git):
      dist\DirSize-<version>-setup.exe
      dist\DirSize-<version>-portable.zip   (dirsize.exe and the license)

.EXAMPLE
    .\scripts\package-installer.ps1
#>
param([switch]$NoBuild, [string]$Iscc)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
# cmake.exe on PATH (the GitHub runner), else the copy in Visual Studio 2022.
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue | ForEach-Object Source | Select-Object -First 1
if (-not $cmake) {
    $cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}

$match = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(DirSize VERSION (\d+\.\d+\.\d+)'
if (-not $match) { throw 'cannot find the version in CMakeLists.txt' }
$version = $match.Matches[0].Groups[1].Value

if (-not $Iscc) {
    $candidates = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path $_) }
    if (-not $candidates) { throw 'ISCC.exe not found. Install Inno Setup 6.5 or later, or pass -Iscc <path>.' }
    $Iscc = @($candidates)[0]
}

if (-not $NoBuild) {
    & $cmake --build (Join-Path $root 'build') --config Release
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}
$release = Join-Path $root 'build\Release'
foreach ($exe in 'dirsize.exe', 'dirsize-service.exe', 'dirsize-cli.exe') {
    $path = Join-Path $release $exe
    if (-not (Test-Path $path)) { throw "missing $exe; build first" }
    $built = (Get-Item $path).VersionInfo.FileVersion
    if ($built -ne $version) { throw "$exe has version '$built', expected $version; rebuild" }
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

& $Iscc "/DAppVersion=$version" "/DBinDir=$release" (Join-Path $root 'installer\dirsize.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }

# The portable zip: the standalone window only (no service, no installer).
$zip = Join-Path $dist "DirSize-$version-portable.zip"
$stage = Join-Path $dist "DirSize-$version-portable"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item (Join-Path $release 'dirsize.exe') $stage
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Get-ChildItem $dist -Filter "DirSize-$version-*" | ForEach-Object { '{0}  {1:N0} bytes' -f $_.Name, $_.Length }
