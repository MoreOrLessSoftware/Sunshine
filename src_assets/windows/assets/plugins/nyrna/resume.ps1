# Resumes what toggle.ps1 recorded as suspended. Run by the "Nyrna Resume" scheduled task, which the notification's
# Resume button starts through the sunshine-nyrna: link; its output goes to the plugin's log.
#
# It only toggles while that app really is still suspended, so a notification left over from earlier can't suspend
# whatever happens to be in front instead.

param(
    [Parameter(Mandatory = $true)][string]$DataDir
)

$ErrorActionPreference = 'Stop'
$dataDir = $DataDir
. (Join-Path $PSScriptRoot 'common.ps1')

$log = Join-Path (Split-Path $dataDir) 'nyrna.log'
function Write-Log([string]$text) {
    try {
        Add-Content -Path $log -Value "$(Get-Date -Format o) $text"
    }
    catch {
    }
}

$state = Read-State
if (-not $state -or -not (Test-Suspended $state.pid)) {
    Write-Log 'Nothing suspended to resume'
    Clear-Suspended
    exit 0
}

try {
    # Its lines are already timestamped
    Invoke-NyrnaToggle $state.hotkey ([bool]$state.minimize) | ForEach-Object { Add-Content -Path $log -Value $_ }
}
catch {
    Write-Log "Couldn't toggle: $($_.Exception.Message)"
    exit 1
}

for ($i = 0; $i -lt 16; $i++) {
    Start-Sleep -Milliseconds 250
    if (-not (Test-Suspended $state.pid)) {
        Write-Log "Resumed $($state.name)"
        Clear-Suspended
        exit 0
    }
}
Write-Log "$($state.name) is still suspended"
exit 1
