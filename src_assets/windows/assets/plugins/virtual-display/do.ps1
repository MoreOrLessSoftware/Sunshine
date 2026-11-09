# Creates a virtual display matching the client's resolution and frame rate.
# vdisplay reads the resolution, client name and (by default) HDR from the SUNSHINE_CLIENT_* variables.
# The monitor identity comes from its name, so a fixed name gives every client the same monitor.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vdisplay.ps1')
. (Join-Path $PSScriptRoot 'desktop.ps1')

$invariant = [Globalization.CultureInfo]::InvariantCulture

if ($env:SUNSHINE_CLIENT_FPS_X100) {
    $fps = [decimal]$env:SUNSHINE_CLIENT_FPS_X100 / 100
}
else {
    $fps = [decimal]$env:SUNSHINE_CLIENT_FPS
}
$displayFps = $fps * [decimal]$env:SUNSHINE_PLUGIN_OPT_FPS_MULTIPLIER

$vdisplayArgs = @('start', '--fps', $displayFps.ToString($invariant))
if ($env:SUNSHINE_PLUGIN_OPT_SHARED -eq 'true') {
    $vdisplayArgs += @('--name', 'Sunshine')
}
if ($env:SUNSHINE_PLUGIN_OPT_EXCLUSIVE -eq 'true') {
    $vdisplayArgs += '--exclusive'
}
switch ($env:SUNSHINE_PLUGIN_OPT_HDR) {
    'on' { $vdisplayArgs += @('--hdr', 'true') }
    'off' { $vdisplayArgs += '--no-hdr' }
}

if ($env:SUNSHINE_PLUGIN_OPT_NEW_DESKTOP -eq 'true') {
    Enter-StreamDesktop
}

# Sunshine does not run the undo script of a do script that failed, so clean up the desktop here.
try {
    Write-Output "Starting virtual display at $($displayFps.ToString($invariant)) Hz for client $env:SUNSHINE_CLIENT_NAME"
    $process = Start-Vdisplay $vdisplayArgs

    # vdisplay waits for Windows to add the new monitor to the desktop, but depending on the saved display layout
    # Windows may never do that on its own. If the display is not ready shortly after starting, switch to extend
    # mode so Windows activates it. vdisplay restores the previous layout when the display is removed.
    if (-not $process.WaitForExit(2000)) {
        Write-Output 'Virtual display not active yet; switching Windows to extend mode'
        & (Join-Path $env:windir 'System32\DisplaySwitch.exe') /extend
    }
    $process.WaitForExit()

    $code = $process.ExitCode
    if ($code -ne 0) {
        throw "vdisplay start failed with exit code $code; see $(Split-Path $vdisplayExe)\vdisplay.log"
    }
}
catch {
    Exit-StreamDesktop
    throw
}

$settle = [int]$env:SUNSHINE_PLUGIN_OPT_SETTLE_SECONDS
if ($settle -gt 0) {
    Write-Output "Waiting $settle seconds for the display layout to settle"
    Start-Sleep -Seconds $settle
}
