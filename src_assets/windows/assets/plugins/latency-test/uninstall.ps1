# Stops the plugin's programs and removes its firewall rule. Sunshine deletes the data directory, with the exes in it,
# once this succeeds. Results are in the user's Documents folder and stay there.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Get-PluginProcess | Stop-Process -Force
Remove-FirewallRules
Write-Output 'Stopped latency-test and removed its firewall rule'
