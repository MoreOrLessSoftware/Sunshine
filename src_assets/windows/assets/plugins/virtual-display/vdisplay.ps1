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
