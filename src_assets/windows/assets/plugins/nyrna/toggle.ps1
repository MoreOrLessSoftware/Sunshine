# Suspends the window in the foreground, or resumes the one Nyrna suspended last, wherever it is.
# Run by the plugin's "Nyrna Suspend" client command; its output goes to the plugin's log.
#
# A second Nyrna can't run while one is already running (such as Nyrna installed with its tray icon): they share
# their settings, and the running one keeps them open. So when Nyrna is running, this presses its hotkey and lets
# it do the toggling; otherwise it runs the plugin's copy with --toggle.

$ErrorActionPreference = 'Stop'

# Nyrna exits once it has toggled; one still running after this long is stuck
$timeoutSeconds = 20

$exe = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'nyrna\nyrna.exe'

# Presses a hotkey such as "Pause" or "Ctrl+Alt+P": modifiers (Ctrl, Shift, Alt, Win) then a key name from
# System.Windows.Forms.Keys.
function Send-Hotkey([string]$hotkey) {
    Add-Type -AssemblyName System.Windows.Forms
    if (-not ('NyrnaKeys' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NyrnaKeys
{
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    [DllImport("user32.dll")]
    public static extern uint MapVirtualKey(uint code, uint mapType);
}
"@
    }

    # Keys that are extended keys on a keyboard (Insert, Delete, Home, End, Page Up/Down, the arrows, the
    # right-hand Ctrl and Alt, Win, Menu, Num Lock and Num /), which hotkeys may tell from their keypad twins
    $extended = @(0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0xA3, 0xA5)

    $modifiers = @{ ctrl = 0x11; control = 0x11; shift = 0x10; alt = 0x12; win = 0x5B; windows = 0x5B }
    $keys = @()
    foreach ($part in ($hotkey -split '\+') | ForEach-Object { $_.Trim() } | Where-Object { $_ }) {
        if ($modifiers.ContainsKey($part.ToLower())) {
            $keys += $modifiers[$part.ToLower()]
        }
        else {
            try {
                $keys += [int][Enum]::Parse([System.Windows.Forms.Keys], $part, $true)
            }
            catch {
                throw "Unknown key '$part' in the hotkey '$hotkey'"
            }
        }
    }
    if ($keys.Count -eq 0) {
        throw 'The hotkey is empty; set it in the plugin options.'
    }

    $keyUp = 0x2
    $send = {
        param([int]$key, [uint32]$flags)
        if ($extended -contains $key) {
            $flags = $flags -bor 0x1
        }
        $scan = [byte]([NyrnaKeys]::MapVirtualKey([uint32]$key, 0) -band 0xFF)
        [NyrnaKeys]::keybd_event([byte]$key, $scan, $flags, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 30
    }
    foreach ($key in $keys) {
        & $send $key 0
    }
    [array]::Reverse($keys)
    foreach ($key in $keys) {
        & $send $key $keyUp
    }
}

$running = @(Get-Process -Name 'nyrna' -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne $exe })
if ($running.Count -gt 0) {
    $hotkey = if ($env:SUNSHINE_PLUGIN_OPT_HOTKEY) { $env:SUNSHINE_PLUGIN_OPT_HOTKEY } else { 'Pause' }
    Write-Output "$(Get-Date -Format o) Nyrna is already running ($($running[0].Path)); pressing its hotkey $hotkey"
    Send-Hotkey $hotkey
    exit 0
}

if (-not (Test-Path $exe)) {
    throw 'Nyrna is not installed; install the plugin again.'
}

$arguments = @('--toggle')
if ($env:SUNSHINE_PLUGIN_OPT_MINIMIZE -eq 'false') {
    $arguments += '--no-minimize'
}

Write-Output "$(Get-Date -Format o) Running $exe $($arguments -join ' ')"
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory (Split-Path $exe) -PassThru
if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    throw "Nyrna didn't finish within $timeoutSeconds seconds"
}
Write-Output "$(Get-Date -Format o) Nyrna exited with code $($process.ExitCode)"
exit $process.ExitCode
