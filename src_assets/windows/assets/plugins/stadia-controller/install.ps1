# Downloads the Stadiacast receiver and lets it through Windows Firewall.
# https://github.com/MoreOrLessSoftware/stadiacast
#
# To install a local build instead of the release, put the release zip (or any zip holding stadiacast.exe) in
# the plugin data folder as stadiacast-local.zip before installing; its checksum is not verified.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot 'common.ps1')

$version = '1.1.0'
$url = "https://github.com/MoreOrLessSoftware/stadiacast/releases/download/v$version/stadiacast-$version-windows-x86_64.zip"
$sha256 = 'c49973d5ae0961216e020b94a8f254402a1b86266e58a1d6ff5413b545658eeb'

$zip = Join-Path $DataDir 'stadiacast.zip'
$local = Join-Path $DataDir 'stadiacast-local.zip'
$extract = Join-Path $DataDir 'extract'

# A receiver from an earlier install would hold the files.
Stop-Receiver

if (Test-Path $local) {
    Write-Output "Installing the local build $local (checksum not verified)"
    Copy-Item $local $zip -Force
}
else {
    if (-not $sha256) {
        throw "No checksum is pinned for Stadiacast $version yet. Put a local build in $local to install it."
    }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Output "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
    if ($actual -ne $sha256) {
        Remove-Item $zip -Force
        throw "Checksum mismatch for the Stadiacast download: expected $sha256, got $actual"
    }
}

if (Test-Path $extract) {
    Remove-Item $extract -Recurse -Force
}
Expand-Archive -Path $zip -DestinationPath $extract
Remove-Item $zip -Force
$exe = Get-ChildItem $extract -Recurse -Filter 'stadiacast.exe' | Select-Object -First 1
if (-not $exe) {
    throw 'The archive does not contain stadiacast.exe'
}
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Copy-Item $exe.FullName $StadiacastExe -Force
Remove-Item $extract -Recurse -Force
$installed = (& $StadiacastExe --version 2>&1 | Out-String).Trim()
Write-Output "Installed $installed to $StadiacastExe"

# Sunshine starts the receiver without a desktop, where Windows cannot ask whether to let it through the
# firewall, so allow it here. The controller reaches it on TCP 443, UDP 53 and 123, and UDP 40000 and up.
Get-NetFirewallRule -DisplayName $FirewallRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $FirewallRule -Direction Inbound -Action Allow -Program $StadiacastExe `
    -Profile Domain, Private -Description 'Lets a Stadia Controller on the local network reach the Stadiacast receiver.' | Out-Null
Write-Output "Allowed $StadiacastExe through Windows Firewall on private and domain networks"

# The virtual controller needs a driver; say so now rather than at the first stream.
switch ("$env:SUNSHINE_PLUGIN_OPT_GAMEPAD") {
    'vigem' {
        if (-not (Get-PnpDevice -FriendlyName '*Virtual Gamepad Emulation Bus*' -ErrorAction SilentlyContinue)) {
            Write-Output 'Warning: the ViGEmBus driver was not found. Install it, or choose the Xbox Series controller.'
        }
    }
    'off' {}
    default {
        $inf = Get-ChildItem (Join-Path $env:windir 'INF') -Filter 'oem*.inf' -ErrorAction SilentlyContinue |
            Where-Object { Select-String -Path $_.FullName -Pattern 'SunshineXgpad\' -SimpleMatch -Quiet }
        if (-not $inf) {
            Write-Output "Warning: the xgpad driver was not found. Install Sunshine's Xbox Series Controller Driver component for the virtual controller."
        }
    }
}
