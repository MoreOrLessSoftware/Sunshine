# The do, undo, connect and choices scripts run as the signed-in user, since Windows keeps color profile settings
# per user, and save what they changed in the data folder. Let users write there.
# S-1-5-32-545 is the local Users group.

$ErrorActionPreference = 'Stop'

$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
& icacls.exe $dataDir /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to grant users write access to $dataDir"
}
Write-Output "Granted users write access to $dataDir"
