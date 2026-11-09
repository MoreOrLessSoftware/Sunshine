# Shared helpers for the Nyrna plugin. Dot-source this file after setting $dataDir to the plugin's data directory.
#
# What's suspended is kept in suspended.json, and the menu's label for the command in menu.json (the manifest's
# status_file), both in the data directory.
#
# Nyrna and resume.ps1 run through scheduled tasks, as the signed-in user with full rights (see
# Register-Tasks). The notification's Resume button opens a sunshine-nyrna: link, which starts the resume task.

$appId = 'Sunshine.Nyrna'
$protocol = 'sunshine-nyrna'
$toastTag = 'suspended'
$toastGroup = 'nyrna'
$taskPath = '\Sunshine\'
$taskName = 'Nyrna Toggle'
$resumeTaskName = 'Nyrna Resume'

$exe = Join-Path $dataDir 'nyrna\nyrna.exe'
$stateFile = Join-Path $dataDir 'suspended.json'
$menuFile = Join-Path $dataDir 'menu.json'
$imageFile = Join-Path $dataDir 'suspended.png'

if (-not ('NyrnaNative' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class NyrnaNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public delegate bool EnumChildProc(IntPtr hwnd, IntPtr param);

    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hwnd, EnumChildProc proc, IntPtr param);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
}
"@
}
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
[NyrnaNative]::SetProcessDPIAware() | Out-Null

# --- What's in front

# Returns the foreground window's handle, process ID, title and a name for its app, or $null.
function Get-ForegroundApp {
    $hwnd = [NyrnaNative]::GetForegroundWindow()
    if ($hwnd -eq [IntPtr]::Zero) {
        return $null
    }

    [uint32]$procId = 0
    [NyrnaNative]::GetWindowThreadProcessId($hwnd, [ref]$procId) | Out-Null
    $process = Get-Process -Id $procId -ErrorAction SilentlyContinue

    # A UWP app's window is a frame owned by ApplicationFrameHost.exe, around a child window of the app's own
    if ($process -and $process.ProcessName -eq 'ApplicationFrameHost') {
        $script:uwpPid = 0
        $callback = [NyrnaNative+EnumChildProc] {
            param($child, $param)
            [uint32]$childPid = 0
            [NyrnaNative]::GetWindowThreadProcessId($child, [ref]$childPid) | Out-Null
            if ($childPid -ne 0 -and $childPid -ne $procId) {
                $script:uwpPid = $childPid
                return $false
            }
            return $true
        }
        [NyrnaNative]::EnumChildWindows($hwnd, $callback, [IntPtr]::Zero) | Out-Null
        if ($script:uwpPid -ne 0) {
            $procId = $script:uwpPid
            $process = Get-Process -Id $procId -ErrorAction SilentlyContinue
        }
    }
    if (-not $process) {
        return $null
    }

    $text = New-Object System.Text.StringBuilder 512
    [NyrnaNative]::GetWindowText($hwnd, $text, $text.Capacity) | Out-Null
    $title = $text.ToString()

    # The app's own name if it has one (a game's file description is usually its name), then the title
    $name = $null
    try {
        if ($process.Path) {
            $name = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($process.Path).FileDescription
        }
    }
    catch {
    }
    if (-not $name) { $name = $title }
    if (-not $name) { $name = $process.ProcessName }
    # Some describe themselves by their file name
    $name = $name.Trim() -replace '\.exe$', ''

    return [pscustomobject]@{
        Hwnd = $hwnd
        Pid = [int]$procId
        ProcessName = $process.ProcessName
        Title = $title
        Name = $name.Trim()
    }
}

# Whether every thread of a process is suspended, as Nyrna leaves it.
function Test-Suspended([int]$procId) {
    $process = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if (-not $process) {
        return $false
    }
    $threads = @($process.Threads)
    if ($threads.Count -eq 0) {
        return $false
    }
    $suspended = @($threads | Where-Object { $_.ThreadState -eq 'Wait' -and $_.WaitReason -eq 'Suspended' })
    return $suspended.Count -eq $threads.Count
}

# --- The window's picture

# Whether a picture is blank: a capture of a window the screen can't copy, such as an exclusive-fullscreen game,
# comes out all black (or all one color).
function Test-BlankImage([System.Drawing.Bitmap]$bitmap) {
    $first = $bitmap.GetPixel(0, 0)
    for ($y = 0; $y -lt 12; $y++) {
        for ($x = 0; $x -lt 12; $x++) {
            $pixel = $bitmap.GetPixel([int](($bitmap.Width - 1) * $x / 11), [int](($bitmap.Height - 1) * $y / 11))
            if ($pixel.ToArgb() -ne $first.ToArgb()) {
                return $false
            }
        }
    }
    return $true
}

# Saves a picture of a window to $imageFile, scaled to at most 720 pixels wide. Returns whether it did.
function Save-WindowImage([IntPtr]$hwnd) {
    Remove-Item $imageFile -Force -ErrorAction SilentlyContinue
    if ([NyrnaNative]::IsIconic($hwnd)) {
        return $false
    }
    $rect = New-Object NyrnaNative+RECT
    if (-not [NyrnaNative]::GetWindowRect($hwnd, [ref]$rect)) {
        return $false
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        return $false
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    try {
        # What's on screen first, then what the window draws for itself
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $graphics.Dispose()
        if (Test-BlankImage $bitmap) {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $hdc = $graphics.GetHdc()
            [NyrnaNative]::PrintWindow($hwnd, $hdc, 2) | Out-Null
            $graphics.ReleaseHdc($hdc)
            $graphics.Dispose()
            if (Test-BlankImage $bitmap) {
                return $false
            }
        }

        $scale = [Math]::Min(1.0, 720.0 / $width)
        $scaled = New-Object System.Drawing.Bitmap ([int]($width * $scale)), ([int]($height * $scale))
        $graphics = [System.Drawing.Graphics]::FromImage($scaled)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($bitmap, 0, 0, $scaled.Width, $scaled.Height)
        $graphics.Dispose()
        $scaled.Save($imageFile, [System.Drawing.Imaging.ImageFormat]::Png)
        $scaled.Dispose()
        return $true
    }
    catch {
        Write-Output "Couldn't capture the window: $($_.Exception.Message)"
        return $false
    }
    finally {
        $bitmap.Dispose()
    }
}

# --- Toggling

# Presses a hotkey such as "Pause" or "Ctrl+Alt+Insert": modifiers (Ctrl, Shift, Alt, Win) then a key name from
# System.Windows.Forms.Keys.
function Send-Hotkey([string]$hotkey) {
    $modifiers = @{ ctrl = 0x11; control = 0x11; shift = 0x10; alt = 0x12; win = 0x5B; windows = 0x5B }
    # Extended keys on a keyboard, which hotkeys may tell from their keypad twins
    $extended = @(0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0xA3, 0xA5)

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

    $send = {
        param([int]$key, [uint32]$flags)
        if ($extended -contains $key) {
            $flags = $flags -bor 0x1
        }
        $scan = [byte]([NyrnaNative]::MapVirtualKey([uint32]$key, 0) -band 0xFF)
        [NyrnaNative]::keybd_event([byte]$key, $scan, $flags, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 30
    }
    foreach ($key in $keys) {
        & $send $key 0
    }
    [array]::Reverse($keys)
    foreach ($key in $keys) {
        & $send $key 0x2
    }
}

# Has Nyrna toggle: the running Nyrna through its hotkey (a second one can't start while it runs, as they share
# their settings), otherwise the plugin's copy with --toggle.
function Invoke-NyrnaToggle([string]$hotkey, [bool]$minimize) {
    $running = @(Get-Process -Name 'nyrna' -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne $exe })
    if ($running.Count -gt 0) {
        if (-not $hotkey) {
            $hotkey = 'Pause'
        }
        Write-Output "$(Get-Date -Format o) Nyrna is already running ($($running[0].Path)); pressing its hotkey $hotkey"
        Send-Hotkey $hotkey
        return
    }

    if (-not (Test-Path $exe)) {
        throw 'Nyrna is not installed; install the plugin again.'
    }

    # Through Task Scheduler (see Register-ToggleTask), so Nyrna's own window can't take the foreground
    $task = Get-ScheduledTask -TaskPath $taskPath -TaskName $taskName -ErrorAction SilentlyContinue
    if (-not $task) {
        throw 'The scheduled task that runs Nyrna is missing; pick the Suspend command in Moonlight once to set it up again.'
    }
    $arguments = if ($minimize) { '--toggle' } else { '--toggle --no-minimize' }
    if ($task.Actions[0].Arguments -ne $arguments) {
        # The minimize option changed since the task was set up; only an administrator can change it
        Write-Output "$(Get-Date -Format o) The task runs Nyrna with $($task.Actions[0].Arguments), not $arguments"
    }
    Write-Output "$(Get-Date -Format o) Running $exe $($task.Actions[0].Arguments) through Task Scheduler"
    Start-ScheduledTask -TaskPath $taskPath -TaskName $taskName
    for ($i = 0; $i -lt 80; $i++) {
        Start-Sleep -Milliseconds 250
        if ((Get-ScheduledTask -TaskPath $taskPath -TaskName $taskName).State -ne 'Running') {
            break
        }
    }
    $info = Get-ScheduledTaskInfo -TaskPath $taskPath -TaskName $taskName
    Write-Output "$(Get-Date -Format o) Nyrna finished with result $($info.LastTaskResult)"
}

# Sets up a scheduled task that runs as this user with full rights, unless it's already set up that way.
function Register-Task([string]$name, [string]$execute, [string]$arguments, [string]$workingDir) {
    $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $existing = Get-ScheduledTask -TaskPath $taskPath -TaskName $name -ErrorAction SilentlyContinue
    if ($existing -and $existing.Actions[0].Execute -eq $execute -and $existing.Actions[0].Arguments -eq $arguments -and $existing.Principal.UserId -eq $user) {
        return
    }
    $action = New-ScheduledTaskAction -Execute $execute -Argument $arguments -WorkingDirectory $workingDir
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskPath $taskPath -TaskName $name -Action $action -Principal $principal -Settings $settings -Force | Out-Null
}

# Sets up the scheduled tasks; needs elevation. Started by Task Scheduler, Nyrna's window can't take the
# foreground: run from a script started by Sunshine, it would, and Nyrna would then find itself in front instead of
# the app. The resume task lets the notification's Resume button (a link, run without elevation) resume with full
# rights, so apps that run elevated can be resumed too.
function Register-Tasks([bool]$minimize) {
    $arguments = if ($minimize) { '--toggle' } else { '--toggle --no-minimize' }
    Register-Task $taskName $exe $arguments (Split-Path $exe)

    $resume = Join-Path $PSScriptRoot 'resume.ps1'
    Register-Task $resumeTaskName 'powershell.exe' "-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$resume`" -DataDir `"$dataDir`"" $PSScriptRoot
}

function Unregister-Tasks {
    foreach ($name in $taskName, $resumeTaskName) {
        Unregister-ScheduledTask -TaskPath $taskPath -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    }
}

# --- State

# What's suspended, or $null.
function Read-State {
    if (-not (Test-Path $stateFile)) {
        return $null
    }
    try {
        $state = Get-Content $stateFile -Raw | ConvertFrom-Json
        if ($state -and $state.pid) {
            return $state
        }
    }
    catch {
    }
    return $null
}

# Writes a file as UTF-8 without a byte order mark.
function Set-SharedFile([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

# The command's label while something is suspended, from the plugin's option. %APP% becomes its name.
function Get-ResumeLabel([string]$name) {
    $label = "$env:SUNSHINE_PLUGIN_OPT_RESUME_LABEL".Trim()
    if (-not $label) {
        $label = 'Resume (%APP%)'
    }
    return $label -replace '%APP%', $name.Replace('$', '$$')
}

function Set-Suspended($app, [string]$hotkey, [bool]$minimize, [bool]$hasImage) {
    $state = [ordered]@{ pid = $app.Pid; name = $app.Name; title = $app.Title; process = $app.ProcessName; hotkey = $hotkey; minimize = $minimize; image = $hasImage; time = (Get-Date -Format o) }
    Set-SharedFile $stateFile ($state | ConvertTo-Json)
    Set-SharedFile $menuFile (@{ label = (Get-ResumeLabel $app.Name); icon = 'material:play_circle' } | ConvertTo-Json)
}

# Forgets what was suspended.
function Clear-Suspended {
    Remove-Item $stateFile, $menuFile, $imageFile -Force -ErrorAction SilentlyContinue
    Remove-Toast
}

# --- The notification

# Registers the toast's app name and icon, and the sunshine-nyrna: link its Resume button opens, for this user.
function Register-Integration {
    $aumid = "HKCU:\Software\Classes\AppUserModelId\$appId"
    New-Item -Path $aumid -Force | Out-Null
    Set-ItemProperty -Path $aumid -Name DisplayName -Value 'Nyrna (Sunshine)'
    $icon = Join-Path $dataDir 'nyrna\data\flutter_assets\assets\icons\codes.merritt.Nyrna.png'
    if (Test-Path $icon) {
        Set-ItemProperty -Path $aumid -Name IconUri -Value $icon
    }

    $key = "HKCU:\Software\Classes\$protocol"
    New-Item -Path "$key\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path $key -Name '(default)' -Value 'URL:Sunshine Nyrna'
    Set-ItemProperty -Path $key -Name 'URL Protocol' -Value ''
    # A console program started from the link hangs before it runs (seen on Windows 11), so wscript.exe starts the
    # task through Task Scheduler's COM interface instead
    $link = Join-Path $PSScriptRoot 'resume-link.vbs'
    $command = "wscript.exe //B //Nologo `"$link`""
    Set-ItemProperty -Path "$key\shell\open\command" -Name '(default)' -Value $command
}

function Unregister-Integration {
    Remove-Item -Path "HKCU:\Software\Classes\$protocol" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Path "HKCU:\Software\Classes\AppUserModelId\$appId" -Recurse -Force -ErrorAction SilentlyContinue
}

function Initialize-Toasts {
    [Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
    [Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
}

# Shows a notification that stays until it's dismissed, with the picture and a Resume button.
function Show-Toast($app, [bool]$hasImage) {
    Initialize-Toasts
    $name = [System.Security.SecurityElement]::Escape($app.Name)
    $image = ''
    if ($hasImage) {
        $uri = [System.Uri]::new($imageFile).AbsoluteUri
        $image = "<image src=`"$uri`"/>"
    }
    $xml = @"
<toast scenario="reminder">
  <visual>
    <binding template="ToastGeneric">
      <text>$name is suspended</text>
      <text>Resume it here, or from Moonlight's menu.</text>
      $image
    </binding>
  </visual>
  <actions>
    <action content="Resume" activationType="protocol" arguments="${protocol}:resume"/>
    <action content="Dismiss" activationType="system" arguments="dismiss"/>
  </actions>
</toast>
"@
    $document = New-Object Windows.Data.Xml.Dom.XmlDocument
    $document.LoadXml($xml)
    $toast = [Windows.UI.Notifications.ToastNotification]::new($document)
    $toast.Tag = $toastTag
    $toast.Group = $toastGroup
    [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($appId).Show($toast)
}

function Remove-Toast {
    try {
        Initialize-Toasts
        [Windows.UI.Notifications.ToastNotificationManager]::History.Remove($toastTag, $toastGroup, $appId)
    }
    catch {
    }
}
