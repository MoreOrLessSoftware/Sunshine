# Removes the virtual display and restores the previous display layout.

. (Join-Path $PSScriptRoot 'vdisplay.ps1')

Write-Output 'Stopping virtual display'
exit (Invoke-Vdisplay @('stop'))
