# Lets the signed-in user's scripts save the display layout in the data folder, and downloads the VirtualDesktop
# PowerShell module for the option to stream on a new desktop.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# The do, undo and choices scripts run as the signed-in user, since display settings belong to the user's session.
# S-1-5-32-545 is the local Users group.
$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
& icacls.exe $dataDir /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to grant users write access to $dataDir"
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# The PowerShell Gallery can fail for a minute or so at a time, so keep trying for about two minutes.
function Invoke-Download([string]$Uri, [string]$OutFile) {
    $attempts = 10
    Write-Output "Downloading $Uri"
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -Uri $Uri -OutFile $OutFile -UseBasicParsing -TimeoutSec 60
            return
        }
        catch {
            if ($attempt -ge $attempts) {
                throw
            }
            Write-Output "Attempt $attempt of $attempts failed, retrying in 15 seconds: $_"
            Start-Sleep -Seconds 15
        }
    }
}

# Streaming works without the module, so a problem here only warns.
# https://github.com/MScholtes/PSVirtualDesktop (MIT license)
$desktopModuleVersion = '1.5.11'
$desktopModuleUrl = "https://www.powershellgallery.com/api/v2/package/VirtualDesktop/$desktopModuleVersion"
$desktopModuleSha256 = 'ECFF46B242F114086E199A9CBC6C5A1CB87409A5926459021F26EBD980409DDA'
$desktopModuleZip = Join-Path $dataDir 'virtualdesktop.zip'
try {
    Invoke-Download $desktopModuleUrl $desktopModuleZip
    $actual = (Get-FileHash -Path $desktopModuleZip -Algorithm SHA256).Hash
    if ($actual -ne $desktopModuleSha256) {
        throw "Checksum mismatch: expected $desktopModuleSha256, got $actual"
    }
    # -Force replaces the copy from an earlier install
    Expand-Archive -Path $desktopModuleZip -DestinationPath (Join-Path $dataDir 'virtualdesktop') -Force
    Write-Output 'Installed the VirtualDesktop module'
}
catch {
    Write-Output "Warning: could not install the VirtualDesktop module, so the new desktop option will not work: $_"
}
finally {
    Remove-Item $desktopModuleZip -Force -ErrorAction SilentlyContinue
}
