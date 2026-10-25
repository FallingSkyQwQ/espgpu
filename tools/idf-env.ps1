# Load the ESP-IDF v6.1 environment into the current PowerShell session.
# Usage:  . .\tools\idf-env.ps1
$env:PYTHONUTF8 = "1"
# Dot-source so functions/aliases defined by the profile survive into this session.
. "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1" *> $null
Set-Location (Join-Path $PSScriptRoot "..")
