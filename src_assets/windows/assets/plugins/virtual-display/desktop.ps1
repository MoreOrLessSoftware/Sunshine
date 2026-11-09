# Shared helpers for the optional temporary Windows desktop. Dot-source this file.
#
# The do and undo scripts are separate processes and the VirtualDesktop module does not expose desktop IDs, so the
# temporary desktop is found again by its unique name, and the original desktop by its position and name.
# Problems here only log a warning, since the stream still works on the current desktop.
# https://github.com/MScholtes/PSVirtualDesktop

$desktopModule = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'vdisplay\virtualdesktop\VirtualDesktop.psd1'
$desktopStateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'vdisplay\stream-desktop.json'

# Returns the saved state, or $null if there is none or it cannot be read.
function Read-DesktopState {
    try {
        return Get-Content -LiteralPath $desktopStateFile -Raw -ErrorAction Stop | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

# Returns the numbers of the desktops with exactly this name. The module's own name search matches parts of names.
function Get-DesktopNumberByName([string]$Name) {
    return @(Get-DesktopList | Where-Object { $_.Name -ceq $Name } | ForEach-Object { $_.Number })
}

# Switches desktops, without the animation on the Windows versions that can skip it.
function Switch-StreamDesktop($Desktop) {
    $switchArgs = @{ Desktop = $Desktop }
    if ((Get-Command Switch-Desktop).Parameters.ContainsKey('NoAnimation')) {
        $switchArgs.NoAnimation = $true
    }
    Switch-Desktop @switchArgs
}

# Returns the desktop the stream started from, or $null if it no longer exists.
function Find-OriginalDesktop($State) {
    $desktops = @(Get-DesktopList)
    $found = @($desktops | Where-Object { $_.Number -eq $State.original_index -and $_.Name -ceq $State.original_name })
    if ($found.Count -eq 0) {
        $found = @($desktops | Where-Object { $_.Name -ceq $State.original_name })
    }
    if ($found.Count -eq 0) {
        return $null
    }
    return Get-Desktop -Index $found[0].Number
}

# Creates a temporary desktop and switches to it. Reuses the one left behind if an earlier stream was cut short.
function Enter-StreamDesktop {
    $ErrorActionPreference = 'Stop'
    $new = $null
    try {
        if (-not (Test-Path -LiteralPath $desktopModule)) {
            Write-Output 'Warning: new desktop support is not installed, streaming on the current desktop. Uninstall and reinstall this plugin to add it.'
            return
        }
        Import-Module $desktopModule -DisableNameChecking

        $state = Read-DesktopState
        if ($state) {
            $leftover = @(Get-DesktopNumberByName $state.temp_name)
            if ($leftover.Count -gt 0) {
                Write-Output "Reusing desktop $($state.temp_name) from an earlier stream"
                Switch-StreamDesktop (Get-Desktop -Index $leftover[0])
                return
            }
        }

        $original = Get-CurrentDesktop
        $originalIndex = Get-DesktopIndex -Desktop $original
        $originalName = Get-DesktopName -Desktop $original
        $name = "Sunshine Stream $([guid]::NewGuid().ToString('N').Substring(0, 8))"

        $new = New-Desktop
        Set-DesktopName -Desktop $new -Name $name
        @{ temp_name = $name; original_index = $originalIndex; original_name = $originalName } |
            ConvertTo-Json | Set-Content -LiteralPath $desktopStateFile -Encoding UTF8
        Switch-StreamDesktop $new
        Write-Output "Switched to new desktop $name"
    }
    catch {
        Write-Output "Warning: could not switch to a new desktop, streaming on the current desktop: $_"
        if ($new) {
            try {
                $new.Remove()
                Remove-Item -LiteralPath $desktopStateFile -Force -ErrorAction SilentlyContinue
            }
            catch {
                Write-Output "Warning: could not remove the new desktop: $_"
            }
        }
    }
}

# Returns to the original desktop and removes the temporary one, moving any windows still on it to the original.
# Does nothing when no temporary desktop was recorded, so it is safe to call whether or not the option is on.
function Exit-StreamDesktop {
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path -LiteralPath $desktopStateFile)) {
        return
    }
    try {
        $state = Read-DesktopState
        if ($state) {
            Import-Module $desktopModule -DisableNameChecking
            $temporary = @(Get-DesktopNumberByName $state.temp_name)
            if ($temporary.Count -eq 0) {
                Write-Output "Desktop $($state.temp_name) no longer exists"
            }
            else {
                $original = Find-OriginalDesktop $state
                $desktop = Get-Desktop -Index $temporary[0]
                if ($original) {
                    Switch-StreamDesktop $original
                }
                $desktop.Remove($original)
                Write-Output "Removed desktop $($state.temp_name)"
            }
        }
        Remove-Item -LiteralPath $desktopStateFile -Force
    }
    catch {
        Write-Output "Warning: could not remove the temporary desktop, the next stream will reuse it: $_"
    }
}
