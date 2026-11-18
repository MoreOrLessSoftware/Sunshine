<#
.SYNOPSIS
  Installs or removes the Sunshine xgpad driver (virtual Xbox Series controllers). Requires elevation.

.DESCRIPTION
  Used by the xgpad plugin's install and uninstall scripts, so installing the plugin installs the driver and
  uninstalling it removes the driver, and by the Sunshine uninstaller through uninstall-xgpad-driver.bat.

  install:
    1. Removes certificates and driver packages left behind by older xgpad versions.
    2. Adds the release's public signing certificate (xgpad.cer) to LocalMachine\Root and
       LocalMachine\TrustedPublisher. Its private key was destroyed when the release was built, so the
       certificate cannot be used to sign anything else.
    3. Adds the driver package to the driver store (pnputil /add-driver /install).

  uninstall:
    Removes xgpad device nodes, the driver package and the certificate(s).

.PARAMETER Action
  install or uninstall.

.PARAMETER PackageDir
  Folder containing xgpad.inf, xgpad.dll, xgpad.cat and xgpad.cer. Defaults to the driver folder next to
  this script.

.PARAMETER LogFile
  File to append a transcript to, for callers whose output isn't kept, such as the Sunshine uninstaller.
  Plugin scripts leave it out, since their output already goes to the plugin's log.
#>
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('install', 'uninstall')]
  [string]$Action,
  [string]$PackageDir = '',
  [string]$LogFile = ''
)
$ErrorActionPreference = 'Stop'

# $PSScriptRoot is empty in param() defaults when Windows PowerShell runs a script with -File (as the
# uninstaller's .bat wrapper does), so resolve the package folder here.
$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $PackageDir) {
  $PackageDir = Join-Path $scriptDir 'driver'
}

# Windows PowerShell started from a PowerShell 7 process inherits PowerShell 7's module paths and loads its
# incompatible modules (no Cert: drive). Use the Windows PowerShell defaults.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
  $env:PSModulePath = @(
    (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'WindowsPowerShell\Modules'),
    [Environment]::ExpandEnvironmentVariables([Environment]::GetEnvironmentVariable('PSModulePath', 'Machine'))
  ) -join ';'
}

# The uninstaller runs this hidden or in a window that closes, so it asks for a log
$transcript = $false
if ($LogFile) {
  try {
    New-Item -ItemType Directory -Force (Split-Path -Parent $LogFile) | Out-Null
    Start-Transcript -Path $LogFile -Append | Out-Null
    $transcript = $true
  } catch {
    Write-Output "Can't write the log to ${LogFile}: $($_.Exception.Message)"
  }
}
Write-Output ("[{0}] xgpad-driver.ps1 -Action {1} as {2} (64-bit process: {3}, PowerShell {4}, TEMP={5})" -f `
    (Get-Date -Format s), $Action, [Security.Principal.WindowsIdentity]::GetCurrent().Name,
    [Environment]::Is64BitProcess, $PSVersionTable.PSVersion, $env:TEMP)

# pnputil only exists in the 64-bit System32; a 32-bit host has to go through Sysnative.
$pnputil = if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
  Join-Path $env:windir 'Sysnative\pnputil.exe'
} else {
  Join-Path $env:windir 'System32\pnputil.exe'
}

# Subjects used by release and test signing certificates (see drivers/xgpad/build.ps1)
$certSubjectPatterns = @('CN=Sunshine xgpad Driver *', 'CN=Sunshine xgpad Test Signing')

# Instance ID prefixes of xgpad device nodes (keep in sync with platf::xgpad::is_xgpad_instance_id)
$instancePrefixes = @(
  'SWD\{00001812-0000-1000-8000-00805F9B34FB}_DEV_VID&02045E_PID&0B13_REV&0509\',
  'HID\{00001812-0000-1000-8000-00805F9B34FB}&DEV&VID_045E&PID_0B13&REV_0509&IG_00\',
  'SWD\SUNSHINEXGPAD\', 'HID\SUNSHINEXGPAD&IG_00\', 'SWD\VID_045E&PID_0B13&REV_0509\', 'HID\VID_045E&PID_0B13&REV_0509&IG_00\',
  # DualSense pads: "&REV_0100" never appears in a real DualSense's instance IDs
  'SWD\VID_054C&PID_0CE6&REV_0100\', 'HID\VID_054C&PID_0CE6&REV_0100\', 'HID\VID_054C&PID_0CE6&REV_0100&IG_'
)

function Get-XgpadDriverPackages {
  # pnputil /enum-drivers prints "Published Name: oemNN.inf" shortly before "Original Name: xgpad.inf"
  $lines = @(& $pnputil /enum-drivers)
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Original Name:\s+xgpad\.inf') {
      for ($j = $i - 1; $j -ge [Math]::Max(0, $i - 3); $j--) {
        if ($lines[$j] -match 'Published Name:\s+(oem\d+\.inf)') { $Matches[1]; break }
      }
    }
  }
}

function Remove-XgpadDevices {
  Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $id = $_.InstanceId.ToUpperInvariant()
    [bool]($instancePrefixes | Where-Object { $id.StartsWith($_) })
  } | ForEach-Object {
    Write-Output "Removing device $($_.InstanceId)"
    & $pnputil /remove-device "$($_.InstanceId)" | Out-Null
  }
}

function Remove-XgpadDriverPackages {
  foreach ($oem in Get-XgpadDriverPackages) {
    Write-Output "Deleting driver package $oem"
    & $pnputil /delete-driver $oem /uninstall /force | Out-Null
  }
}

function Remove-XgpadCertificates([string]$KeepThumbprint = '') {
  foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object {
      $subject = $_.Subject
      $_.Thumbprint -ne $KeepThumbprint -and [bool]($certSubjectPatterns | Where-Object { $subject -like $_ })
    } | ForEach-Object {
      Write-Output "Removing certificate $($_.Subject) [$($_.Thumbprint)] from LocalMachine\$store"
      Remove-Item $_.PSPath
    }
  }
}

$exitCode = 0
try {
if ($Action -eq 'install') {
  $infPath = Join-Path $PackageDir 'xgpad.inf'
  $cerPath = Join-Path $PackageDir 'xgpad.cer'
  if (-not (Test-Path $infPath) -or -not (Test-Path $cerPath)) {
    throw "xgpad driver package not found in $PackageDir"
  }
  $cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cerPath

  # Clean up older versions first so only this release's package and certificate remain trusted
  Remove-XgpadDriverPackages
  Remove-XgpadCertificates -KeepThumbprint $cert.Thumbprint

  foreach ($store in 'Root', 'TrustedPublisher') {
    if (-not (Test-Path "Cert:\LocalMachine\$store\$($cert.Thumbprint)")) {
      Write-Output "Trusting $($cert.Subject) [$($cert.Thumbprint)] in LocalMachine\$store"
      Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }
  }

  Write-Output "Adding driver package $infPath"
  & $pnputil /add-driver $infPath /install
  # 259 (ERROR_NO_MORE_ITEMS): added, but no matching device exists yet (devices are created by Sunshine)
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 259) {
    throw "pnputil /add-driver failed with exit code $LASTEXITCODE"
  }
  Write-Output 'xgpad driver installed'
} else {
  Remove-XgpadDevices
  Remove-XgpadDriverPackages
  Remove-XgpadCertificates
  Write-Output 'xgpad driver removed'
}
} catch {
  Write-Output "ERROR: $($_.Exception.Message)"
  Write-Output $_.ScriptStackTrace
  $exitCode = 1
} finally {
  if ($transcript) {
    Stop-Transcript | Out-Null
  }
}
exit $exitCode
