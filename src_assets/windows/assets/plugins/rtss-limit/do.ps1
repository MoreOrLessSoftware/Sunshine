# Caps the selected RTSS profiles at the client's frame rate, remembering the previous limits for undo.ps1.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'rtss.ps1')

Initialize-Rtss

$clientLimit = Get-RtssClientLimit
$limit = $clientLimit.limit
$denominator = $clientLimit.denominator

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
