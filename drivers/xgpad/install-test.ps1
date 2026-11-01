<#
.SYNOPSIS
  Installs (or removes) the xgpad test driver package. Run from an ELEVATED PowerShell.

.DESCRIPTION
  Install:
    1. Adds out\xgpad-test.cer to LocalMachine\Root and LocalMachine\TrustedPublisher
       (lets PnP accept the self-signed catalog; no test-signing mode needed for UMDF).
    2. pnputil /add-driver out\pkg\xgpad.inf
  Devices are created by out\xgpctl.exe (also elevated) and vanish when it exits.

  -Uninstall:
    removes any device nodes with a SunshineXgpad\* hardware ID, deletes the driver package from the driver store,
    and removes the test certificate from both machine stores.
#>
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated PowerShell.'
}

$out = Join-Path $PSScriptRoot 'out'
$cerPath = Join-Path $out 'xgpad-test.cer'
$infPath = Join-Path $out 'pkg\xgpad.inf'
$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cerPath

function Get-XgpadDriverPackages {
  # pnputil output: "Published Name: oemNN.inf" followed by "Original Name: xgpad.inf"
  $lines = pnputil /enum-drivers
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Original Name:\s+xgpad\.inf') {
      for ($j = $i - 1; $j -ge [Math]::Max(0, $i - 3); $j--) {
        if ($lines[$j] -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1]; break }
      }
    }
  }
}

if ($Uninstall) {
  # Same instance ID prefixes as platf::xgpad::is_xgpad_instance_id (software devices and their HID children).
  $prefixes = @(
    'SWD\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\',
    'HID\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&IG_00\',
    'SWD\SUNSHINEXGPAD\', 'HID\SUNSHINEXGPAD&IG_00\', 'SWD\VID_045E&PID_0B13&REV_0509\', 'HID\VID_045E&PID_0B13&REV_0509&IG_00\'
  )
  Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $id = $_.InstanceId.ToUpperInvariant(); [bool]($prefixes | Where-Object { $id.StartsWith($_) })
  } | ForEach-Object {
    Write-Host "Removing device $($_.InstanceId)"
    pnputil /remove-device "$($_.InstanceId)" | Out-Null
  }
  foreach ($oem in Get-XgpadDriverPackages) {
    Write-Host "Deleting driver package $oem"
    pnputil /delete-driver $oem /uninstall /force
  }
  foreach ($store in 'Root', 'TrustedPublisher') {
    $path = "Cert:\LocalMachine\$store\$($cert.Thumbprint)"
    if (Test-Path $path) {
      Write-Host "Removing certificate from LocalMachine\$store"
      Remove-Item $path
    }
  }
  Write-Host 'Uninstalled.'
  return
}

foreach ($store in 'Root', 'TrustedPublisher') {
  if (-not (Test-Path "Cert:\LocalMachine\$store\$($cert.Thumbprint)")) {
    Write-Host "Adding test certificate to LocalMachine\$store"
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
  }
}

# Replace any previous build of the package so the newest DLL is used.
foreach ($oem in Get-XgpadDriverPackages) {
  Write-Host "Removing previous driver package $oem"
  pnputil /delete-driver $oem /force | Out-Null
}

Write-Host "Adding driver package $infPath"
pnputil /add-driver $infPath /install
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) { throw "pnputil failed ($LASTEXITCODE)" }  # 259 = no matching devices yet
Write-Host "`nInstalled. Next (elevated): $out\xgpctl.exe 1 60"
