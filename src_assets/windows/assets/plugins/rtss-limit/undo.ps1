# Restores the RTSS limits saved by do.ps1, and closes RTSS if do.ps1 started it.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rtss.ps1')

$stateFile = Get-RtssStateFile
if (Test-Path $stateFile) {
    Initialize-Rtss

    (Get-Content $stateFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object {
        Set-RtssLimit $_.Name ([int]$_.Value.limit) ([int]$_.Value.denominator)
        Write-Output "[RTSS profile $($_.Name)] FPS limit restored to $($_.Value.limit)/$($_.Value.denominator)"
    }

    [RTSS]::UpdateProfiles()
    Remove-Item $stateFile -Force
}
else {
    Write-Output 'No saved RTSS limits to restore'
}

$startedFile = Get-RtssStartedFile
if (Test-Path $startedFile) {
    Write-Output 'Closing RTSS, which was started for the stream'
    # RTSS has no command to exit, so stop it along with the hook loaders it starts.
    Get-Process -Name RTSS, RTSSHooksLoader, RTSSHooksLoader64 -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item $startedFile -Force
}
