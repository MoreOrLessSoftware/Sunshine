# "Start test": runs the receiver without a stream, so the panel can show the controller connecting and its inputs.
# It keeps running until "Stop test", or until a stream starts and ends.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Start-Receiver
