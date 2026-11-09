# Removes the virtual display, restores the previous display layout, and removes the temporary desktop if there is one.

. (Join-Path $PSScriptRoot 'vdisplay.ps1')
. (Join-Path $PSScriptRoot 'desktop.ps1')

Write-Output 'Stopping virtual display'
$code = Invoke-Vdisplay @('stop')
Exit-StreamDesktop
exit $code
