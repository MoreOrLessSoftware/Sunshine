# Shared helper for running vdisplay.exe. Dot-source this file.
#
# vdisplay.exe is a GUI-subsystem program, so PowerShell neither waits for it nor sets $LASTEXITCODE
# when it is called with &. Start-Process -Wait cannot be used either: it also waits for child processes,
# and "vdisplay start" leaves its daemon running for the length of the stream.

$vdisplayExe = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'vdisplay\vdisplay.exe'

# Returns every SudoVDA virtual display adapter Windows knows about, including leftovers from earlier installs.
function Get-SudoVdaDevices {
    return @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\SudoMaker\SudoVDA' })
}

$sudoVdaKey = 'HKLM:\SOFTWARE\SudoMaker\SudoVDA'

# Returns the sdrBits value in the SudoVDA driver's registry settings, or $null when it is not set.
function Get-SudoVdaSdrBits {
    return (Get-ItemProperty -Path $sudoVdaKey -Name 'sdrBits' -ErrorAction SilentlyContinue).sdrBits
}

# Sets the SudoVDA driver's sdrBits registry value, or removes it for the 8-bit default when $Bits is $null.
# Requires administrator rights, and the driver only reads the value when it loads.
function Set-SudoVdaSdrBits([Nullable[int]]$Bits) {
    if ($null -ne $Bits) {
        if (-not (Test-Path $sudoVdaKey)) {
            $null = New-Item -Path $sudoVdaKey -Force
        }
        $null = New-ItemProperty -Path $sudoVdaKey -Name 'sdrBits' -PropertyType DWord -Value $Bits -Force
    }
    elseif (Test-Path $sudoVdaKey) {
        Remove-ItemProperty -Path $sudoVdaKey -Name 'sdrBits' -ErrorAction SilentlyContinue
    }
}

# Starts vdisplay with the given arguments and returns the process without waiting for it.
function Start-Vdisplay([string[]]$Arguments) {
    $process = Start-Process -FilePath $vdisplayExe -ArgumentList $Arguments -NoNewWindow -PassThru
    # Reading Handle keeps the process handle open so ExitCode is available after it exits.
    $null = $process.Handle
    return $process
}

# Runs vdisplay with the given arguments, waits for that process only, and returns its exit code.
function Invoke-Vdisplay([string[]]$Arguments) {
    $process = Start-Vdisplay $Arguments
    $process.WaitForExit()
    return $process.ExitCode
}

# The Display Manager plugin, which is bundled too, has the functions that turn displays on and make one primary.
$displayHelpers = Join-Path $PSScriptRoot '..\display-manager\display.ps1'

# Returns every connected display, on or off, to compare with after the virtual display arrives. Needs the Display
# Manager helpers in $displayHelpers.
function Get-DisplaysOn {
    . $displayHelpers
    return , @([DisplayManager]::GetTargets())
}

# Describes displays for the log, such as "BenQ EX3501R (on), SONY TV (off)".
function Format-Displays($Displays) {
    return (@($Displays) | ForEach-Object { "$(Get-DisplayLabel $_) ($(if ($_.Active) { 'on' } else { 'off' }))" }) -join ', '
}

# Puts the displays back the way they were before the virtual display arrived, keeping the virtual display primary:
# Windows applies the layout it saved for this set of displays, which can turn displays off or on. vdisplay puts back
# the layout from before the stream when the display is removed.
#
# Windows has no saved position for a display it has never shown together with this virtual display, so it can't be
# turned on by itself. Extend mode lets Windows place every connected display; the ones that were off before the stream
# are then turned off again, and Windows saves the result, so later streams from this client start that way.
function Restore-DisplaysOn($Before) {
    . $displayHelpers
    $wasOn = @($Before | Where-Object { $_.Active })
    $isOff = {
        param($now)
        @($wasOn | Where-Object { $was = $_; -not ($now | Where-Object { $_.Is($was) -and $_.Active }) })
    }
    $now = [DisplayManager]::GetTargets()

    # The virtual display is new: vdisplay adds it as a display that wasn't connected before the stream. Its connection
    # type can't be relied on, since the driver can report any.
    $virtual = @($now | Where-Object { $is = $_; $_.Active -and -not ($Before | Where-Object { $_.Is($is) }) }) | Select-Object -First 1
    $keep = New-Object 'System.Collections.Generic.List[DisplayManager+Target]'
    foreach ($target in $wasOn + @($virtual | Where-Object { $_ })) {
        $keep.Add($target)
    }
    $isExtra = {
        param($now)
        @($now | Where-Object { $is = $_; $_.Active -and -not ($keep | Where-Object { $_.Is($is) }) })
    }

    $turnedOff = & $isOff $now
    $extra = & $isExtra $now
    if ($turnedOff.Count -eq 0 -and $extra.Count -eq 0) {
        return
    }

    Write-Output "Displays before the stream: $(Format-Displays $Before)"
    Write-Output "Displays with the virtual display: $(Format-Displays $now)"
    if (-not $virtual) {
        # Never turn off a display without knowing which one Sunshine streams
        Write-Output "Couldn't tell which display is the virtual one, so the displays are left as Windows set them"
        return
    }
    Write-Output "Virtual display: $(Get-DisplayLabel $virtual)"

    try {
        if ($turnedOff.Count -gt 0) {
            Write-Output "Windows turned off $(($turnedOff | ForEach-Object { Get-DisplayLabel $_ }) -join ', ') with its saved layout for these displays; switching to extend mode to turn them back on"
            & (Join-Path $env:windir 'System32\DisplaySwitch.exe') /extend

            # DisplaySwitch returns before Windows has changed the layout
            $deadline = (Get-Date).AddSeconds(5)
            do {
                Start-Sleep -Milliseconds 250
                $now = [DisplayManager]::GetTargets()
                $stillOff = & $isOff $now
            } while ($stillOff.Count -gt 0 -and (Get-Date) -lt $deadline)
            if ($stillOff.Count -gt 0) {
                Write-Output "Windows kept $(($stillOff | ForEach-Object { Get-DisplayLabel $_ }) -join ', ') off"
            }

            # Extend mode can move the primary display, and Sunshine streams the primary one
            if ([DisplayManager]::MakePrimary($virtual)) {
                Write-Output 'Made the virtual display primary again'
            }
        }

        # Extend mode, or the saved layout, turned on displays that were off before the stream
        $extra = & $isExtra $now
        if ($extra.Count -gt 0 -and [DisplayManager]::KeepOnly($keep)) {
            Write-Output "Turned $(($extra | ForEach-Object { Get-DisplayLabel $_ }) -join ', ') off again, as before the stream"
            if ([DisplayManager]::MakePrimary($virtual)) {
                Write-Output 'Made the virtual display primary again'
            }
        }
        Write-Output "Displays now: $(Format-Displays ([DisplayManager]::GetTargets()))"
    }
    catch {
        # The stream still works on the virtual display, so don't fail it
        Write-Output "Couldn't put the other displays back: $($_.Exception.Message)"
    }
}
