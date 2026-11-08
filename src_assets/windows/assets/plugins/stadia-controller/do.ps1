# Starts the receiver for the stream. A controller woken now connects and becomes a virtual Xbox controller.
# A receiver left running by "Start test" is restarted, so the stream uses the current options.
#
# A receiver that cannot start is reported in Sunshine's log but does not stop the stream: the client's own
# controllers still work. Use "Start test" on the Plugins page to see the reason.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

try {
    Stop-Receiver
    Start-Receiver
}
catch {
    Write-Output "Stadia Controller: the receiver could not start, streaming without it: $($_.Exception.Message)"
}
exit 0
