# Removes the notification and the sunshine-nyrna: link. Sunshine deletes the plugin's data directory, with Nyrna
# in it, once this succeeds. Something still suspended stays suspended; resume it first.

$ErrorActionPreference = 'Stop'
$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
. (Join-Path $PSScriptRoot 'common.ps1')

$state = Read-State
if ($state -and (Test-Suspended $state.pid)) {
    Write-Output "$($state.name) is still suspended; resume it with Nyrna or Task Manager"
}

Remove-Toast
Unregister-Integration
Unregister-Tasks
Write-Output 'Removed the notification, the sunshine-nyrna: link and the scheduled task'
