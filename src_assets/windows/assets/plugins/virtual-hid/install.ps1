# The status script runs as the signed-in user and writes its status to the data folder. Let users write there.
# S-1-5-32-545 is the local Users group.

$ErrorActionPreference = 'Stop'

$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
& icacls.exe $dataDir /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to grant users write access to $dataDir"
}
Write-Output "Granted users write access to $dataDir"
