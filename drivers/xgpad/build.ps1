<#
.SYNOPSIS
  Builds the xgpad UMDF2 driver package and the xgpctl test tool (no WDK install needed).

.DESCRIPTION
  - Fetches the Microsoft.Windows.WDK.x64 NuGet package into .wdk\ on first run.
  - Compiles xgpad.dll (UMDF 2.15) and xgpctl.exe with MSVC (VS 2022).
  - Creates/reuses a self-signed code-signing certificate in CurrentUser\My, signs the DLL,
    generates the catalog with Inf2Cat and signs it.
  Output: <OutDir>\pkg\{xgpad.inf,xgpad.dll,xgpad.cat}, <OutDir>\xgpad-test.cer (or xgpad.cer with
  -Release), <OutDir>\xgpctl.exe, <OutDir>\xgpdiag.exe
  Installing a test build needs install-test.ps1 from an elevated shell.

  -Release (used by the Sunshine installer build):
    Signs with a fresh, one-time self-signed certificate and deletes it, including its private key,
    from the certificate store afterwards. Only the public certificate (xgpad.cer) is kept; the
    installer trusts it, and nothing else can ever be signed with it.
  -DriverVersion stamps DriverVer so newer Sunshine releases replace older driver packages.
#>
param(
  [string]$WdkVersion = '10.0.26100.6584',
  [string]$CertSubject = 'CN=Sunshine xgpad Test Signing',
  [string]$OutDir = '',
  [switch]$Release,
  [string]$DriverVersion = ''
)
$ErrorActionPreference = 'Stop'

# Windows PowerShell started from a PowerShell 7 process (e.g. via msys2/ninja) inherits PowerShell 7's module
# paths and loads its incompatible modules: the Cert: drive and New-SelfSignedCertificate then fail.
if ($PSVersionTable.PSEdition -eq 'Desktop') {
  $env:PSModulePath = @(
    (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'WindowsPowerShell\Modules'),
    [Environment]::ExpandEnvironmentVariables([Environment]::GetEnvironmentVariable('PSModulePath', 'Machine'))
  ) -join ';'
}

# $PSScriptRoot is empty in param() defaults under Windows PowerShell -File, so resolve paths here.
$root = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$out = if ($OutDir) { $OutDir } else { Join-Path $root 'out' }
$pkg = Join-Path $out 'pkg'
$obj = Join-Path $out 'obj'
$wdkRoot = Join-Path $root ".wdk\$WdkVersion"
New-Item -ItemType Directory -Force $pkg, $obj | Out-Null

# --- WDK from NuGet --------------------------------------------------------------------------------------------
$wdk = Join-Path $wdkRoot 'c'
if (-not (Test-Path (Join-Path $wdk 'Include\wdf\umdf\2.15\wdf.h'))) {
  New-Item -ItemType Directory -Force $wdkRoot | Out-Null
  $nupkg = Join-Path $wdkRoot 'wdk.zip'
  Write-Host "Downloading Microsoft.Windows.WDK.x64 $WdkVersion ..."
  Invoke-WebRequest -UseBasicParsing -OutFile $nupkg `
    "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$WdkVersion/microsoft.windows.wdk.x64.$WdkVersion.nupkg"
  Expand-Archive -Force $nupkg $wdkRoot
  Remove-Item $nupkg
}
$wdkInc = Join-Path $wdk 'Include'
$wdkVer = (Get-ChildItem $wdkInc -Directory | Where-Object Name -like '10.*' | Sort-Object Name | Select-Object -Last 1).Name
$umdfInc = Join-Path $wdkInc 'wdf\umdf\2.15'
$umdfLib = Join-Path $wdk 'Lib\wdf\umdf\x64\2.15'
$wdkBin = Join-Path $wdk "bin\$wdkVer"

# hidport.h lives in the km folder; copy just that header so km headers don't shadow um ones.
$hidInc = Join-Path $obj 'hidinc'
New-Item -ItemType Directory -Force $hidInc | Out-Null
Copy-Item (Join-Path $wdkInc "$wdkVer\km\hidport.h") $hidInc -Force

# --- MSVC ------------------------------------------------------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vsPath) { $vsPath = "$env:ProgramFiles\Microsoft Visual Studio\2022\Community" }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

function Invoke-Msvc([string]$cmdline) {
  # Full path: under msys2 (the Sunshine build environment) "cmd" resolves to msys' /usr/bin/cmd shim.
  & "$env:SystemRoot\System32\cmd.exe" /c "`"$vcvars`" >nul 2>&1 && $cmdline"
  if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $cmdline" }
}

$defs = '/DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=15 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00'
Write-Host 'Compiling xgpad.dll ...'
Invoke-Msvc ("cl /nologo /c /W4 /WX- /O2 /MT /Zi /GS $defs /I`"$umdfInc`" /I`"$hidInc`" " +
  "/Fo`"$obj\xgpad.obj`" /Fd`"$obj\xgpad.pdb`" `"$root\driver\xgpad.c`"")
Invoke-Msvc ("link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /OUT:`"$pkg\xgpad.dll`" /PDB:`"$out\xgpad.pdb`" " +
  "`"$obj\xgpad.obj`" /LIBPATH:`"$umdfLib`" WdfDriverStubUm.lib ntdll.lib kernel32.lib")
Remove-Item "$pkg\xgpad.exp", "$pkg\xgpad.lib" -ErrorAction SilentlyContinue

Write-Host 'Compiling xgpctl.exe ...'
Invoke-Msvc "cl /nologo /W3 /O2 /MT /EHsc /std:c++17 /Fo`"$obj\xgpctl.obj`" /Fe`"$out\xgpctl.exe`" `"$root\tools\xgpctl.cpp`""

Write-Host 'Compiling xgpdiag.exe ...'
Invoke-Msvc "cl /nologo /W3 /O2 /MT /EHsc /std:c++17 /Fo`"$obj\xgpdiag.obj`" /Fe`"$out\xgpdiag.exe`" `"$root\tools\xgpdiag.cpp`""

Copy-Item "$root\driver\xgpad.inf" $pkg -Force

if ($DriverVersion) {
  # DriverVer needs four numeric parts, each 0..65535
  $parts = @($DriverVersion -split '[^0-9]+' | Where-Object { $_ -ne '' } | Select-Object -First 4)
  while ($parts.Count -lt 4) { $parts += '0' }
  $parts = $parts | ForEach-Object { [Math]::Min([int64]$_, 65535) }
  $driverVer = "$(Get-Date -Format 'MM/dd/yyyy'),$($parts -join '.')"
  $infPath = Join-Path $pkg 'xgpad.inf'
  (Get-Content $infPath) -replace '^DriverVer\s*=.*$', "DriverVer   = $driverVer" | Set-Content $infPath -Encoding ascii
  Write-Host "DriverVer = $driverVer"
}

# --- Signing ---------------------------------------------------------------------------------------------------
if ($Release) {
  $subject = "CN=Sunshine xgpad Driver $(Get-Date -Format 'yyyyMMddHHmmss')"
  Write-Host "Creating one-time signing certificate '$subject' ..."
  # Signatures are not timestamped, so the certificate must outlive the release.
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(30)
  $cerFile = Join-Path $out 'xgpad.cer'
} else {
  $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date).AddDays(7) } |
    Sort-Object NotAfter | Select-Object -Last 1
  if (-not $cert) {
    Write-Host "Creating self-signed certificate '$CertSubject' in CurrentUser\My ..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -KeyUsage DigitalSignature `
      -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2)
  }
  $cerFile = Join-Path $out 'xgpad-test.cer'
}
Export-Certificate -Cert $cert -FilePath $cerFile | Out-Null
$signtool = Join-Path $wdkBin 'x64\signtool.exe'
if (-not (Test-Path $signtool)) { $signtool = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$wdkVer\x64\signtool.exe" }

try {
  & $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My "$pkg\xgpad.dll"
  if ($LASTEXITCODE -ne 0) { throw 'signtool (dll) failed' }

  Remove-Item "$pkg\xgpad.cat" -ErrorAction SilentlyContinue
  & (Join-Path $wdkBin 'x86\Inf2Cat.exe') /driver:$pkg /os:10_X64 /uselocaltime
  if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }
  & $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My "$pkg\xgpad.cat"
  if ($LASTEXITCODE -ne 0) { throw 'signtool (cat) failed' }
} finally {
  if ($Release) {
    # Destroy the one-time certificate and its private key; only the exported public .cer remains.
    Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
    Write-Host 'One-time signing certificate and private key deleted'
  }
}

Write-Host "`nBuilt:"
Get-ChildItem $pkg, (Join-Path $out 'xgpctl.exe'), $cerFile | Format-Table Name, Length, LastWriteTime
Write-Host "Certificate thumbprint: $($cert.Thumbprint)"
