# Suspends the window in the foreground, or resumes the one suspended last, wherever it is.
# Run by the plugin's "Suspend" client command; its output goes to the plugin's log.
#
# Once something is suspended, a notification on this PC shows it, with a Resume button, and the command in
# Moonlight's menu changes to the plugin's "Resume command label" option.

param()

$ErrorActionPreference = 'Stop'
$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
. (Join-Path $PSScriptRoot 'common.ps1')

$hotkey = $env:SUNSHINE_PLUGIN_OPT_HOTKEY
$minimize = $env:SUNSHINE_PLUGIN_OPT_MINIMIZE -ne 'false'

Register-Integration
Register-Tasks $minimize

# What was suspended before, unless it has been resumed or closed since (by Nyrna itself, say)
$before = Read-State
if ($before -and -not (Test-Suspended $before.pid)) {
    Write-Output "$(Get-Date -Format o) $($before.name) is no longer suspended"
    Clear-Suspended
    $before = $null
}

# Pictured now, while it's still on screen: it may be minimized as it's suspended
$app = $null
$hasImage = $false
if (-not $before) {
    $app = Get-ForegroundApp
    if ($app) {
        Write-Output "$(Get-Date -Format o) In front: $($app.Name) ($($app.ProcessName), PID $($app.Pid))"
        $hasImage = Save-WindowImage $app.Hwnd
    }
}

Invoke-NyrnaToggle $hotkey $minimize

# Nyrna minimizes the window before suspending it, so this can take a moment
for ($i = 0; $i -lt 16; $i++) {
    Start-Sleep -Milliseconds 250
    if ($before -and -not (Test-Suspended $before.pid)) {
        Write-Output "$(Get-Date -Format o) Resumed $($before.name)"
        Clear-Suspended
        exit 0
    }
    if ($app -and (Test-Suspended $app.Pid)) {
        Write-Output "$(Get-Date -Format o) Suspended $($app.Name)"
        Set-Suspended $app $hotkey $minimize $hasImage
        try {
            Show-Toast $app $hasImage
        }
        catch {
            Write-Output "Couldn't show the notification: $($_.Exception.Message)"
        }
        exit 0
    }
}

Write-Output "$(Get-Date -Format o) Nothing was seen being suspended or resumed"
