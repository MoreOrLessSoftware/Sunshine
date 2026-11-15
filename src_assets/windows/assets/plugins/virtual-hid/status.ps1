# Reports the installed Virtual HID Driver version for the panel, and a notice for the home page when this version
# of Sunshine can't use the driver. Runs as the signed-in user.
#
# DriverVersion and GamepadDriver stand in for the installed driver and the gamepad driver setting, to try the
# notices without changing either. Sunshine passes neither.

param(
    [string]$DriverVersion,
    [string]$GamepadDriver
)

$ErrorActionPreference = 'Stop'

# The oldest driver this version of Sunshine supports. Keep it equal to LIBVIRTUALHID_MINIMUM_VERSION in CMakeLists.txt.
$minimumVersion = '2026.914.1218.10'
$downloadUrl = 'https://github.com/LizardByte/libvirtualhid/releases/latest'

# Parse a dotted version such as 2026.914.1218.10, or return $null when it isn't one.
function ConvertTo-VersionParts([string]$Text) {
    if ($Text -notmatch '^\d+(\.\d+)*$') {
        return $null
    }
    return , ([int64[]]($Text.Split('.')))
}

# Development builds are numbered 0.0.x and work with any version of Sunshine.
function Test-DevelopmentVersion([string]$Text) {
    $parts = ConvertTo-VersionParts $Text
    return $null -ne $parts -and $parts.Count -ge 3 -and $parts[0] -eq 0 -and $parts[1] -eq 0
}

# Whether this version of Sunshine supports a driver version.
function Test-SupportedVersion([string]$Text) {
    if (Test-DevelopmentVersion $Text) {
        return $true
    }
    $parts = ConvertTo-VersionParts $Text
    $minimum = ConvertTo-VersionParts $minimumVersion
    if ($null -eq $parts) {
        return $false
    }
    $count = [Math]::Max($parts.Count, $minimum.Count)
    for ($i = 0; $i -lt $count; $i++) {
        $part = if ($i -lt $parts.Count) { $parts[$i] } else { 0 }
        $min = if ($i -lt $minimum.Count) { $minimum[$i] } else { 0 }
        if ($part -ne $min) {
            return $part -gt $min
        }
    }
    return $true
}

# The installed driver's version, from the driver key of its root device, or '' when it isn't installed.
function Get-InstalledVersion {
    $root = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\LIBVIRTUALHID'
    foreach ($device in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
        $driver = (Get-ItemProperty -LiteralPath $device.PSPath -Name Driver -ErrorAction SilentlyContinue).Driver
        if (-not $driver) {
            continue
        }
        $classKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$driver"
        $version = (Get-ItemProperty -LiteralPath $classKey -Name DriverVersion -ErrorAction SilentlyContinue).DriverVersion
        if ($version) {
            return [string]$version
        }
    }
    return ''
}

# The gamepad_driver setting, from sunshine.conf two folders above the data folder. Empty when it isn't set or
# can't be read, which Sunshine treats like 'all'.
function Get-GamepadDriverSetting {
    $configFile = Join-Path (Split-Path -Parent (Split-Path -Parent $env:SUNSHINE_PLUGIN_DATA_DIR)) 'sunshine.conf'
    try {
        foreach ($line in Get-Content -LiteralPath $configFile -ErrorAction Stop) {
            if ($line -match '^\s*gamepad_driver\s*=\s*(\S*)\s*$') {
                return $Matches[1]
            }
        }
    } catch {
        Write-Output "Can't read ${configFile}: $($_.Exception.Message)"
    }
    return ''
}

$version = if ($PSBoundParameters.ContainsKey('DriverVersion')) { $DriverVersion } else { Get-InstalledVersion }
$gamepadDriver = if ($PSBoundParameters.ContainsKey('GamepadDriver')) { $GamepadDriver } else { Get-GamepadDriverSetting }
$installed = [bool]$version
$development = $installed -and (Test-DevelopmentVersion $version)
$supported = $installed -and (Test-SupportedVersion $version)
# Sunshine doesn't use the driver when only ViGEmBus or only xgpad is selected, so its version doesn't matter then
$used = $gamepadDriver -ne 'vigembus' -and $gamepadDriver -ne 'xgpad'
Write-Output "Installed: '$version', supported: $supported, gamepad driver setting: '$gamepadDriver'"

$notices = @()
if ($installed -and -not $supported -and $used) {
    $notices += @{
        level  = 'warning'
        title  = 'Update Virtual HID Driver'
        text   = "Virtual HID Driver $version is installed, but this version of Sunshine needs $minimumVersion or newer, so Sunshine can't use it. Update the driver, then restart Sunshine."
        href   = $downloadUrl
        action = 'Download'
    }
} elseif (-not $installed -and $gamepadDriver -eq 'virtualhid') {
    $notices += @{
        level  = 'warning'
        title  = 'Virtual HID Driver is not installed'
        text   = 'Virtual HID Driver is selected as the only gamepad driver, but it is not installed. Install it and activate a license, or choose a different gamepad driver in the Input settings.'
        href   = $downloadUrl
        action = 'Download'
    }
}

$status = [ordered]@{
    installed       = $installed
    version         = $version
    minimum_version = $minimumVersion
    supported       = $supported
    development     = $development
    gamepad_driver  = $gamepadDriver
    download_url    = $downloadUrl
    notices         = $notices
}
ConvertTo-Json -InputObject $status -Depth 4 -Compress | Set-Content -LiteralPath $env:SUNSHINE_PLUGIN_STATUS_FILE -Encoding UTF8
