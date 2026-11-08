# Stops the receiver when the stream ends, removing the virtual controller.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Stop-Receiver
