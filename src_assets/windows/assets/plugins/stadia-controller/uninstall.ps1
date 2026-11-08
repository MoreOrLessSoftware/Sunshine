# Stops the receiver and removes its firewall rule. Sunshine deletes the plugin data folder afterwards.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Stop-Receiver
Get-NetFirewallRule -DisplayName $FirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Write-Output 'Removed the firewall rule'
