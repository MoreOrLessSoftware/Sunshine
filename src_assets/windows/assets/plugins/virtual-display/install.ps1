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

Write-Output "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

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

. (Join-Path $PSScriptRoot 'vdisplay.ps1')

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
