# Installs the xgpad driver from the package in this plugin's driver folder. Runs as administrator.

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'xgpad-driver.ps1') -Action install
exit $LASTEXITCODE
