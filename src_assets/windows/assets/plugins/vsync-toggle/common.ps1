# Shared helpers for the NVIDIA (vsync-toggle) and AMD (driver registry) V-Sync backends.
# Dot-source this file.

$displayClassKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$nvidiaExe = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'vsynctoggle.exe'
$backendsFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'backends.txt'

# AMD "Wait for Vertical Refresh" values, stored as a UTF-16 digit in UMD\VSyncControl:
#   0 = Always off, 1 = Off unless application specifies, 2 = On unless application specifies, 3 = Always on
$amdModes = @{
    off = '0'
    default = '1'
    on = '3'
    # AMD has no Fast Sync equivalent in the registry, so force v-sync on instead.
    fast = '3'
}

function Get-VsyncBackends {
    if (-not (Test-Path $backendsFile)) {
        throw 'The plugin is not installed correctly; reinstall it.'
    }
    return @(Get-Content $backendsFile | Where-Object { $_ })
}

# Returns the registry paths of the UMD keys of every AMD display adapter.
function Get-AmdUmdKeys {
    Get-ChildItem $displayClassKey -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object {
        $provider = $_.GetValue('ProviderName')
        $umd = Join-Path $_.PSPath 'UMD'
        if ($provider -match 'Advanced Micro Devices|AMD' -and (Test-Path $umd)) {
            $umd
        }
    }
}

# Returns the VSyncControl digit of a UMD key, or $null when the value does not exist.
function Get-AmdVsync([string]$umd) {
    $value = (Get-Item $umd).GetValue('VSyncControl')
    if ($null -eq $value) {
        return $null
    }
    return [Text.Encoding]::Unicode.GetString([byte[]]$value).TrimEnd([char]0)
}

function Set-AmdVsync([string]$umd, [string]$digit) {
    Set-ItemProperty -Path $umd -Name VSyncControl -Value ([Text.Encoding]::Unicode.GetBytes($digit)) -Type Binary
}
