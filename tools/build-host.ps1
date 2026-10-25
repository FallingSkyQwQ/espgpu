# Configure and build the Windows host side inside an MSVC environment.
# Usage:  .\tools\build-host.ps1 [-Config default|debug] [-Clean] [-Test]
#   -Test  run ctest afterwards (core rasterizer smoke test + espgpu.dll emulator end-to-end test)
param(
    [string]$Config = "default",
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$hostDir = Join-Path $root "host"

# --- MSVC environment ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}
# Only accept an installation that actually has the x64 C++ toolset
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the C++ x64 toolset (VC.Tools.x86.x64) found."
}
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

if ($Test) {
    $ctest = "ctest"
    $cmakeDir = Split-Path $cmake -Parent
    if ($cmakeDir -and (Test-Path (Join-Path $cmakeDir "ctest.exe"))) { $ctest = Join-Path $cmakeDir "ctest.exe" }
    & $ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

Write-Host ""
Write-Host "Build complete: $buildDir" -ForegroundColor Green
Get-ChildItem $buildDir -Filter "*.exe" | ForEach-Object { Write-Host "  $($_.Name)" }
Get-ChildItem $buildDir -Filter "*.dll" | ForEach-Object { Write-Host "  $($_.Name)" }
