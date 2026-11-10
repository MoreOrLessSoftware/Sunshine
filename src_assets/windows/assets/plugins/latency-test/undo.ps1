# Closes the test app when the stream's application ends. A normal close, like Esc: if an upload or analysis is still
# going, the app shows its progress and finishes it. Nothing to do if it isn't running.

$ErrorActionPreference = 'Continue'

try {
    . (Join-Path $PSScriptRoot 'common.ps1')
    foreach ($process in @(Get-PluginProcess -Names 'latency-test')) {
        $process.CloseMainWindow() | Out-Null
    }
} catch {
    Write-Output "Couldn't close latency-test: $_"
}
exit 0
