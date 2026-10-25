# 用 espgpu 跑 DSDA-Doom（或 prboom-plus）
#
#   .\tools\doom\run-dsda.ps1 -DoomDir C:\games\dsda-doom -Iwad doom2.wad
#   .\tools\doom\run-dsda.ps1 -DoomDir C:\games\dsda-doom -Mode passthrough   # 采 GL 调用清单
#
# 做三件事：把 opengl32.dll / espgpu.dll 拷到游戏目录、设好环境变量、启动 DOOM2 MAP01。
param(
    [Parameter(Mandatory = $true)][string]$DoomDir,
    [string]$Exe = "dsda-doom.exe",
    [string]$Iwad = "doom2.wad",
    [ValidateSet("espgpu", "trace", "passthrough")][string]$Mode = "espgpu",
    [string]$Res = "320x200",
    [int]$Fps = 24,
    [string]$Build = "default",
    [string[]]$Extra = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out = Join-Path $root "host\out\build\$Build"

$gl = Get-ChildItem -Path $out -Recurse -Filter "opengl32.dll" | Select-Object -First 1
$dev = Join-Path $out "espgpu.dll"
if (-not $gl) { throw "opengl32.dll not found under $out — run tools\build-host.ps1 first" }
if (-not (Test-Path $dev)) { throw "espgpu.dll not found in $out" }

Copy-Item $gl.FullName (Join-Path $DoomDir "opengl32.dll") -Force
Copy-Item $dev (Join-Path $DoomDir "espgpu.dll") -Force

$env:ESPGPU_GL_MODE = $Mode
$env:ESPGPU_RES = $Res
$env:ESPGPU_FPS = "$Fps"
$env:ESPGPU_GL_LOG = "1"
# SDL 默认 LoadLibrary("OPENGL32.DLL")，应用目录优先；显式指定一次更保险
$env:SDL_OPENGL_LIBRARY = (Join-Path $DoomDir "opengl32.dll")

Push-Location $DoomDir
try {
    & (Join-Path $DoomDir $Exe) -iwad $Iwad -warp 1 -skill 3 @Extra
} finally {
    Pop-Location
}

if ($Mode -ne "espgpu") {
    $trace = Join-Path $DoomDir "espgpu-gltrace.txt"
    if (Test-Path $trace) {
        Write-Host "GL inventory: $trace"
        Write-Host "regenerate coverage: python tools\doom\gen_coverage.py `"$trace`" > docs\gl-coverage-trace.md"
    }
}
