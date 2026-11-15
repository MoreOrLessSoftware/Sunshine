# Removes the xgpad driver, its device nodes and its certificate. Runs as administrator.

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'xgpad-driver.ps1') -Action uninstall
exit $LASTEXITCODE
