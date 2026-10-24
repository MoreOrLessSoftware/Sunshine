# Caps the selected RTSS profiles at the client's frame rate, remembering the previous limits for undo.ps1.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rtss.ps1')

Initialize-Rtss

# RTSS stores fractional limits as an integer over a denominator, e.g. 5994/100 for 59.94 FPS.
if ($env:SUNSHINE_CLIENT_FPS_X100) {
    $fpsX100 = [int]$env:SUNSHINE_CLIENT_FPS_X100
}
else {
    $fpsX100 = [int]$env:SUNSHINE_CLIENT_FPS * 100
}
if ($fpsX100 % 100 -eq 0) {
    $limit = $fpsX100 / 100
    $denominator = 1
}
else {
    $limit = $fpsX100
    $denominator = 100
}

$stateFile = Get-RtssStateFile
$previous = @{}
if (Test-Path $stateFile) {
    # A previous stream did not run undo.ps1; keep the limits saved before it.
    (Get-Content $stateFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object {
        $previous[$_.Name] = @{ limit = $_.Value.limit; denominator = $_.Value.denominator }
    }
}

foreach ($name in Get-RtssProfileNames) {
    if (-not $previous.ContainsKey($name)) {
        $previous[$name] = Get-RtssLimit $name
    }
    Set-RtssLimit $name $limit $denominator
    Write-Output "[RTSS profile $name] FPS limit $($previous[$name].limit)/$($previous[$name].denominator) -> $limit/$denominator"
}

$previous | ConvertTo-Json | Set-Content -Path $stateFile -Encoding UTF8
[RTSS]::UpdateProfiles()

# The limit only applies while RTSS runs. Start it after updating the profiles, since it reads them at startup.
if ($env:SUNSHINE_PLUGIN_OPT_START_RTSS -eq 'true' -and -not (Test-RtssRunning)) {
    $exe = Join-Path (Find-RtssDirectory) 'RTSS.exe'
    Write-Output "Starting $exe"
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
    Set-Content -Path (Get-RtssStartedFile) -Value (Get-Date -Format o)
}
