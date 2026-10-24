# Configure and build the Windows host side inside an MSVC environment.
# Usage:  .\tools\build-host.ps1 [-Config default|debug] [-Clean]
param(
    [string]$Config = "default",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$hostDir = Join-Path $root "host"

# --- MSVC environment ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}
$vsPath = & $vswhere -latest -products * -property installationPath
Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null

# --- CMake / Ninja: prefer the copies bundled with ESP-IDF ---
$cmake = "C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe"
$ninja = "C:\Espressif\tools\ninja\1.12.1\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
if (-not (Test-Path $ninja)) { $ninja = "ninja" }

$buildDir = Join-Path $hostDir "out\build\$Config"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

$buildType = "Release"
if ($Config -eq "debug") { $buildType = "Debug" }

& $cmake -S $hostDir -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$buildType" `
    "-DCMAKE_MAKE_PROGRAM=$ninja"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host ""
Write-Host "Build complete: $buildDir" -ForegroundColor Green
Get-ChildItem $buildDir -Filter "*.exe" | ForEach-Object { Write-Host "  $($_.Name)" }
Get-ChildItem $buildDir -Filter "*.dll" | ForEach-Object { Write-Host "  $($_.Name)" }
