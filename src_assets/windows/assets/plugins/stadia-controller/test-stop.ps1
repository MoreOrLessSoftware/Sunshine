# "Stop test": stops the receiver and removes the virtual controller.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Stop-Receiver
