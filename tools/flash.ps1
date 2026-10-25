# Build and flash the ESP32-S3 firmware.
# Usage:
#   .\tools\flash.ps1                 # build + flash to the only serial port found
#   .\tools\flash.ps1 -Port COM5
#   .\tools\flash.ps1 -Monitor        # flash then open the log monitor
param(
    [string]$Port = "",
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"

$env:PYTHONUTF8 = "1"
# Dot-source (not "&"): the profile may define functions/aliases (idf.py) that a
# child scope would drop.
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1" *> $null

$root = Split-Path $PSScriptRoot -Parent
Set-Location (Join-Path $root "firmware")

if (-not (Test-Path "sdkconfig")) {
    Write-Host "No sdkconfig, running set-target esp32s3 first" -ForegroundColor Yellow
    idf.py set-target esp32s3
}

idf.py build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# $args is a PowerShell automatic variable; use our own name. Pass -p only when a
# port was given ("-p ''" makes idf.py fail instead of auto-detecting).
$idfArgs = @()
if ($Port -ne "") {
    $idfArgs += @("-p", $Port)
} else {
    Write-Host "Hint: pass -Port COMx if auto-detection fails." -ForegroundColor Yellow
}
$idfArgs += "flash"
if ($Monitor) { $idfArgs += "monitor" }

idf.py @idfArgs
if ($LASTEXITCODE -ne 0) { throw "flash failed" }
