# Shared helpers for the Stadia Controller plugin: where Stadiacast lives, and starting and stopping the receiver.
#
# The receiver runs in the background, detached from the script that starts it. It serves its status on
# http://127.0.0.1:8008 (the plugin panel reads it through Sunshine) and stops cleanly, removing its virtual
# controllers and helper processes, when POST /_shutdown arrives from this machine.

$ErrorActionPreference = 'Stop'

$StadiacastPort = 8008
$DataDir = $env:SUNSHINE_PLUGIN_DATA_DIR
$BinDir = Join-Path $DataDir 'bin'
$StadiacastExe = Join-Path $BinDir 'stadiacast.exe'
$LogRoot = Join-Path $DataDir 'logs'
$PidFile = Join-Path $DataDir 'receiver.pid'
$TimeServiceFile = Join-Path $DataDir 'w32time-was-running'
$FirewallRule = 'Sunshine Stadia Controller (Stadiacast)'

function Test-ReceiverListening {
    <# Whether anything listens on the receiver's port. Calling a closed local port takes the full request timeout. #>
    $listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
    return @($listeners | Where-Object { $_.Port -eq $StadiacastPort }).Count -gt 0
}

function Get-ReceiverStatus {
    <# The receiver's /_status document, or $null when nothing answers on its port. #>
    if (-not (Test-ReceiverListening)) {
        return $null
    }
    try {
        return Invoke-RestMethod -Uri "http://127.0.0.1:$StadiacastPort/_status" -TimeoutSec 2 -UseBasicParsing
    }
    catch {
        return $null
    }
}

function Get-ReceiverProcess {
    <# The receiver process this plugin started, or $null. #>
    if (-not (Test-Path $PidFile)) {
        return $null
    }
    $id = [int](Get-Content $PidFile -Raw)
    $process = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($process -and $process.Path -and ($process.Path -ieq $StadiacastExe)) {
        return $process
    }
    return $null
}

function Get-DnsUpstream {
    <# The DNS option, or the first DNS server of the interface that holds this PC's default route. #>
    $configured = "$env:SUNSHINE_PLUGIN_OPT_DNS_UPSTREAM".Trim()
    if ($configured) {
        return $configured
    }
    $own = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object IPAddress)
    $routes = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue | Sort-Object RouteMetric, InterfaceMetric
    foreach ($route in $routes) {
        $servers = (Get-DnsClientServerAddress -InterfaceIndex $route.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).ServerAddresses
        foreach ($server in $servers) {
            if ($server -and -not $server.StartsWith('127.') -and ($own -notcontains $server)) {
                return $server
            }
        }
    }
    throw 'Could not find a DNS server to forward to; set the DNS upstream option to your router address.'
}

function Remove-OldLogs {
    param([int]$Keep)
    if (-not (Test-Path $LogRoot)) {
        return
    }
    Get-ChildItem $LogRoot -Directory | Sort-Object Name -Descending | Select-Object -Skip $Keep |
        ForEach-Object { Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}

function Suspend-WindowsTime {
    <# Stop the Windows Time service, which holds UDP 123, and remember to start it again. #>
    if ($env:SUNSHINE_PLUGIN_OPT_STOP_WINDOWS_TIME -ne 'true') {
        return
    }
    $service = Get-Service w32time -ErrorAction SilentlyContinue
    if ($service -and $service.Status -eq 'Running') {
        Write-Output 'Stopping the Windows Time service while the receiver runs (it holds UDP 123)'
        Stop-Service w32time -Force
        Set-Content -Path $TimeServiceFile -Value 'running'
    }
}

function Resume-WindowsTime {
    if (Test-Path $TimeServiceFile) {
        Write-Output 'Starting the Windows Time service again'
        Remove-Item $TimeServiceFile -Force
        Start-Service w32time -ErrorAction SilentlyContinue
    }
}

function Start-Receiver {
    <#
    Start the receiver in the background unless one is already running, then wait until it answers.
    Returns once the receiver is ready; throws with the receiver's own error when it exits early.
    #>
    if (-not (Test-Path $StadiacastExe)) {
        throw "Stadiacast is not installed ($StadiacastExe is missing); reinstall the plugin."
    }
    $status = Get-ReceiverStatus
    if ($status) {
        Write-Output "The receiver is already running (Stadiacast $($status.version))"
        return
    }

    $keep = [int]$env:SUNSHINE_PLUGIN_OPT_KEEP_LOGS
    if ($keep -lt 1) {
        $keep = 10
    }
    Remove-OldLogs -Keep ($keep - 1)
    $runDir = Join-Path $LogRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $upstream = Get-DnsUpstream
    $gamepad = "$env:SUNSHINE_PLUGIN_OPT_GAMEPAD"
    if (-not $gamepad) {
        $gamepad = 'xgpad'
    }
    $deadzone = 'on'
    if ($env:SUNSHINE_PLUGIN_OPT_DEADZONE -eq 'false') {
        $deadzone = 'off'
    }
    $arguments = @(
        '--preset', 'live',
        '--dns-upstream', $upstream,
        '--http-ports', "$StadiacastPort",
        '--gamepad', $gamepad,
        '--gamepad-deadzone', $deadzone,
        '--log-dir', "`"$runDir`""
    )

    Suspend-WindowsTime
    Write-Output "Starting the receiver: virtual controller $gamepad, DNS upstream $upstream, logs in $runDir"
    $out = Join-Path $runDir 'console.out'
    $err = Join-Path $runDir 'console.err'
    $process = Start-Process -FilePath $StadiacastExe -ArgumentList $arguments -WorkingDirectory $runDir `
        -WindowStyle Hidden -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
    Set-Content -Path $PidFile -Value $process.Id

    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
            Resume-WindowsTime
            $reason = (Get-Content $err -ErrorAction SilentlyContinue | Where-Object { $_ }) -join ' '
            if (-not $reason) {
                $reason = "exit code $($process.ExitCode)"
            }
            throw "The receiver stopped right away: $reason"
        }
        if (Get-ReceiverStatus) {
            Write-Output "The receiver is running (process $($process.Id)); wake the controller with a short press of its Stadia button"
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "The receiver did not answer on port $StadiacastPort within 20 seconds; see $runDir"
}

function Stop-Receiver {
    <# Ask the receiver to stop, then make sure it and its helper processes are gone. #>
    $process = Get-ReceiverProcess
    if (Get-ReceiverStatus) {
        Write-Output 'Stopping the receiver'
        try {
            Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$StadiacastPort/_shutdown" -TimeoutSec 5 -UseBasicParsing | Out-Null
        }
        catch {
            Write-Output "The receiver did not take the stop request: $($_.Exception.Message)"
        }
    }
    if ($process) {
        if (-not $process.WaitForExit(10000)) {
            Write-Output "The receiver did not stop on its own; ending process $($process.Id) and its helpers"
            & taskkill.exe /PID $process.Id /T /F | Out-Null
        }
    }
    elseif (-not (Get-ReceiverStatus)) {
        Write-Output 'The receiver is not running'
    }
    Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
    Resume-WindowsTime
}
