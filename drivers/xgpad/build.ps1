<#
.SYNOPSIS
  Builds the xgpad UMDF2 driver package and the xgpctl test tool (no WDK install needed).

.DESCRIPTION
  - Fetches the Microsoft.Windows.WDK.x64 NuGet package into .wdk\ on first run.
  - Compiles xgpad.dll (UMDF 2.15) and xgpctl.exe with MSVC (VS 2022).
  - Creates/reuses a self-signed code-signing certificate in CurrentUser\My, signs the DLL,
    generates the catalog with Inf2Cat and signs it.
  Output: out\pkg\{xgpad.inf,xgpad.dll,xgpad.cat}, out\xgpad-test.cer, out\xgpctl.exe
  Installing the package needs install-test.ps1 from an elevated shell.
#>
param(
  [string]$WdkVersion = '10.0.26100.6584',
  [string]$CertSubject = 'CN=Sunshine xgpad Test Signing'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out'
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
  cmd /c "`"$vcvars`" >nul 2>&1 && $cmdline"
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

# --- Signing ---------------------------------------------------------------------------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date).AddDays(7) } |
  Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert) {
  Write-Host "Creating self-signed certificate '$CertSubject' in CurrentUser\My ..."
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertSubject -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2)
}
Export-Certificate -Cert $cert -FilePath (Join-Path $out 'xgpad-test.cer') | Out-Null
$signtool = Join-Path $wdkBin 'x64\signtool.exe'
if (-not (Test-Path $signtool)) { $signtool = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\$wdkVer\x64\signtool.exe" }

& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My "$pkg\xgpad.dll"
if ($LASTEXITCODE -ne 0) { throw 'signtool (dll) failed' }

Remove-Item "$pkg\xgpad.cat" -ErrorAction SilentlyContinue
& (Join-Path $wdkBin 'x86\Inf2Cat.exe') /driver:$pkg /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }
& $signtool sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My "$pkg\xgpad.cat"
if ($LASTEXITCODE -ne 0) { throw 'signtool (cat) failed' }

Write-Host "`nBuilt:"
Get-ChildItem $pkg, (Join-Path $out 'xgpctl.exe'), (Join-Path $out 'xgpad-test.cer') | Format-Table Name, Length, LastWriteTime
Write-Host "Certificate thumbprint: $($cert.Thumbprint)"
