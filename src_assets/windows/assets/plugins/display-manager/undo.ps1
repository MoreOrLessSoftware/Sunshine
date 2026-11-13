# Puts back the display layout from before the stream, with each display's resolution and refresh rate, and removes
# the temporary desktop if there is one.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'display.ps1')
. (Join-Path $PSScriptRoot 'desktop.ps1')

$code = 0
try {
    Restore-DisplayLayout
}
catch {
    Write-Output "Warning: $_"
    $code = 1
}
Exit-StreamDesktop
exit $code
