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
& "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1" *> $null

$root = Split-Path $PSScriptRoot -Parent
Set-Location (Join-Path $root "firmware")

if (-not (Test-Path "sdkconfig")) {
    Write-Host "No sdkconfig, running set-target esp32s3 first" -ForegroundColor Yellow
    idf.py set-target esp32s3
}

idf.py build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$args = @("-p", $Port, "flash")
if ($Monitor) { $args += "monitor" }

if ($Port -eq "") {
    Write-Host "Hint: pass -Port COMx if auto-detection fails." -ForegroundColor Yellow
}

idf.py @args
