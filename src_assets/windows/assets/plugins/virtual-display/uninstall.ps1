# Removes any leftover virtual display and, if requested, the SudoVDA driver.
# Sunshine deletes the plugin data directory once this script succeeds.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdisplay.ps1')

if (-not (Test-Path $vdisplayExe)) {
    Write-Output 'vdisplay is not installed'
    exit 0
}

$null = Invoke-Vdisplay @('stop')

if ($env:SUNSHINE_PLUGIN_OPT_REMOVE_DRIVER -eq 'true') {
    Write-Output 'Removing the SudoVDA driver'
    $code = Invoke-Vdisplay @('uninstall-driver')
    if ($code -ne 0) {
        throw "vdisplay uninstall-driver failed with exit code $code"
    }
}
