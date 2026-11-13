# Sets up the physical display for the stream: optionally moves to a new desktop, turns on the chosen display (and off
# the others when asked), makes it the primary display, and sets the client's resolution and the closest refresh rate.
# A display problem only warns, since the stream still works on the displays as they are.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'display.ps1')
. (Join-Path $PSScriptRoot 'desktop.ps1')

if ($env:SUNSHINE_PLUGIN_OPT_NEW_DESKTOP -eq 'true') {
    Enter-StreamDesktop
}

try {
    Set-StreamDisplay
}
catch {
    Write-Output "Warning: could not set up the display, putting back the previous layout: $_"
    try {
        Restore-DisplayLayout
    }
    catch {
        Write-Output "Warning: $_"
    }
}
