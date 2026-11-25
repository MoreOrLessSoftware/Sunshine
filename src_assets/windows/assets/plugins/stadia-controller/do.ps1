# Starts the receiver for the stream. A controller woken now connects and becomes a virtual Xbox controller.
# A receiver left running by "Start test" is restarted, so the stream uses the current options.
#
# The stream doesn't wait for the receiver to finish loading, which takes a few seconds; the controller is woken
# after the stream is up. A receiver that cannot start does not stop the stream: the client's own controllers still
# work. Use "Start test" on the Plugins page to see the reason.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

try {
    Stop-Receiver
    Start-Receiver -NoWait
}
catch {
    Write-Output "Stadia Controller: the receiver could not start, streaming without it: $($_.Exception.Message)"
}
exit 0
