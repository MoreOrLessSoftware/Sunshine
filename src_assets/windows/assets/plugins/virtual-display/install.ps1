# Downloads sunshine-vdisplay and installs the SudoVDA virtual display driver.
# https://github.com/Zayrick/sunshine-vdisplay

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$version = '0.1.0'
$url = "https://github.com/Zayrick/sunshine-vdisplay/releases/download/v$version/vdisplay-v$version-windows-x64.zip"
$sha256 = '638579CECDF4675F5EB956E1CA5962A32EB747181B9303EAD5B6041DFB1272E5'

$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
$zip = Join-Path $dataDir 'vdisplay.zip'
$dest = Join-Path $dataDir 'vdisplay'

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# GitHub can answer with gateway errors for a minute or so at a time, so keep trying for about two minutes.
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

Invoke-Download $url $zip

$actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
if ($actual -ne $sha256) {
    Remove-Item $zip -Force
    throw "Checksum mismatch for vdisplay.zip: expected $sha256, got $actual"
}

if (Test-Path $dest) {
    Remove-Item $dest -Recurse -Force
}
Expand-Archive -Path $zip -DestinationPath $dest
Remove-Item $zip -Force

# vdisplay writes vdisplay.log and vdisplay.state next to the exe, and do.ps1 runs as the signed-in user.
# S-1-5-32-545 is the local Users group.
& icacls.exe $dest /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Failed to grant users write access to $dest"
}

# The option to stream on a new desktop needs the VirtualDesktop PowerShell module. Streaming works without it,
# so a problem here only warns. https://github.com/MScholtes/PSVirtualDesktop (MIT license)
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
    Expand-Archive -Path $desktopModuleZip -DestinationPath (Join-Path $dest 'virtualdesktop')
}
catch {
    Write-Output "Warning: could not install the VirtualDesktop module, so the new desktop option will not work: $_"
}
finally {
    Remove-Item $desktopModuleZip -Force -ErrorAction SilentlyContinue
}

. (Join-Path $PSScriptRoot 'vdisplay.ps1')

# Written before the driver is installed so that a newly loaded driver picks it up.
if ($env:SUNSHINE_PLUGIN_OPT_SDR_10BIT -eq 'true') {
    Set-SudoVdaSdrBits 10
}

# Adapters left behind by an earlier SudoVDA or Apollo install pick up the driver again when it is installed,
# leaving several adapters, and vdisplay can then attach the display to the wrong one. Remove the ones that
# are not working, and reuse a working adapter instead of creating another.
foreach ($device in Get-SudoVdaDevices | Where-Object { $_.Status -ne 'OK' }) {
    Write-Output "Removing leftover SudoVDA adapter $($device.InstanceId) (status: $($device.Status))"
    & pnputil.exe /remove-device $device.InstanceId | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Output "Warning: could not remove $($device.InstanceId) (pnputil exit code $LASTEXITCODE)"
    }
}

$working = @(Get-SudoVdaDevices | Where-Object { $_.Status -eq 'OK' })
if ($working.Count -gt 0) {
    Write-Output "The SudoVDA driver is already installed ($($working[0].InstanceId)); not installing it again"
}
else {
    Write-Output 'Installing the SudoVDA driver'
    $code = Invoke-Vdisplay @('install-driver')
    if ($code -ne 0) {
        throw "vdisplay install-driver failed with exit code $code"
    }
    $working = @(Get-SudoVdaDevices | Where-Object { $_.Status -eq 'OK' })
}

if ($working.Count -gt 1) {
    Write-Output "Warning: found $($working.Count) SudoVDA adapters ($(($working | ForEach-Object InstanceId) -join ', '))."
    Write-Output 'vdisplay may attach the virtual display to the wrong one. Remove the extras in Device Manager unless another program, such as Apollo, uses them.'
}

$null = Invoke-Vdisplay @('status')
