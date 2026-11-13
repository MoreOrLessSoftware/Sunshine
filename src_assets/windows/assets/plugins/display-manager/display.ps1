# Shared helpers for arranging the physical displays. Dot-source this file.
#
# Displays are turned on and off, and made primary, with the Windows display configuration functions
# (QueryDisplayConfig and SetDisplayConfig). Resolutions and refresh rates are listed and set with
# EnumDisplaySettingsEx and ChangeDisplaySettingsEx, which name refresh rates in whole hertz the way the Windows
# settings do: 59 is 59.94 Hz and 60 is 60 Hz. The display layout from before the stream is saved in the data folder,
# so the undo script can put it back.

$displaySource = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class DisplayManager {
    const uint AllPaths = 1;
    const uint OnlyActivePaths = 2;
    const int ErrorInsufficientBuffer = 122;
    const int PathSize = 72;
    const int ModeSize = 64;
    const uint PathActive = 1;
    const uint ModeIdxInvalid = 0xFFFFFFFF;
    const uint ModeInfoSource = 1;
    const int GetSourceName = 1;
    const int GetTargetName = 2;
    const uint SdcTopologySupplied = 0x10;
    const uint SdcUseDatabaseCurrent = 0xF;
    const uint SdcUseSuppliedDisplayConfig = 0x20;
    const uint SdcApply = 0x80;
    const uint SdcSaveToDatabase = 0x200;
    const uint SdcAllowChanges = 0x400;
    const uint SdcAllowPathOrderChanges = 0x2000;
    const int EnumCurrentSettings = -1;
    const uint DmPelsWidth = 0x80000;
    const uint DmPelsHeight = 0x100000;
    const uint DmDisplayFrequency = 0x400000;

    [StructLayout(LayoutKind.Sequential)]
    struct Luid {
        public uint Low;
        public int High;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Header {
        public int Type;
        public int Size;
        public Luid Adapter;
        public uint Id;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TargetName {
        public Header Header;
        public uint Flags;
        public int OutputTechnology;
        public ushort ManufactureId;
        public ushort ProductCodeId;
        public uint ConnectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string FriendlyName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DevicePath;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SourceName {
        public Header Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string GdiName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DevMode {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        public ushort SpecVersion;
        public ushort DriverVersion;
        public ushort Size;
        public ushort DriverExtra;
        public uint Fields;
        public int PositionX;
        public int PositionY;
        public uint DisplayOrientation;
        public uint DisplayFixedOutput;
        public short Color;
        public short Duplex;
        public short YResolution;
        public short TTOption;
        public short Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
        public ushort LogPixels;
        public uint BitsPerPel;
        public uint PelsWidth;
        public uint PelsHeight;
        public uint DisplayFlags;
        public uint DisplayFrequency;
        public uint IcmMethod;
        public uint IcmIntent;
        public uint MediaType;
        public uint DitherType;
        public uint Reserved1;
        public uint Reserved2;
        public uint PanningWidth;
        public uint PanningHeight;
    }

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);

    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint pathCount, [Out] byte[] paths, ref uint modeCount, [Out] byte[] modes, IntPtr topology);

    [DllImport("user32.dll")]
    static extern int SetDisplayConfig(uint pathCount, byte[] paths, uint modeCount, byte[] modes, uint flags);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")]
    static extern int GetInfo(ref TargetName info);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")]
    static extern int GetInfo(ref SourceName info);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsEx(string device, int mode, ref DevMode devMode, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int ChangeDisplaySettingsEx(string device, ref DevMode devMode, IntPtr window, uint flags, IntPtr parameter);

    // A display layout as Windows reports it: the paths from sources to displays, and their modes.
    public class Config {
        public byte[] Paths;
        public uint PathCount;
        public byte[] Modes;
        public uint ModeCount;
    }

    // A connected display. The adapter and target ID identify it until the next restart; the device path identifies
    // it across restarts.
    public class Target {
        public uint AdapterLow;
        public int AdapterHigh;
        public uint TargetId;
        public string FriendlyName;
        public string DevicePath;
        public int OutputTechnology;
        public bool Active;
        public bool Primary;
        public uint SourceId;
        public int X;
        public int Y;

        public bool Is(Target other) {
            return AdapterLow == other.AdapterLow && AdapterHigh == other.AdapterHigh && TargetId == other.TargetId;
        }
    }

    // A resolution and refresh rate the display supports.
    public class Mode {
        public uint Width;
        public uint Height;
        public uint Frequency;
    }

    static void Check(int result, string what) {
        if (result != 0) {
            throw new Exception(string.Format("{0} failed: {1} ({2})", what, new Win32Exception(result).Message, result));
        }
    }

    static uint U32(byte[] data, int offset) {
        return BitConverter.ToUInt32(data, offset);
    }

    static int I32(byte[] data, int offset) {
        return BitConverter.ToInt32(data, offset);
    }

    static void Put(byte[] data, int offset, uint value) {
        BitConverter.GetBytes(value).CopyTo(data, offset);
    }

    static void Put(byte[] data, int offset, int value) {
        BitConverter.GetBytes(value).CopyTo(data, offset);
    }

    static Header MakeHeader<T>(int type, uint adapterLow, int adapterHigh, uint id) {
        Header header = new Header();
        header.Type = type;
        header.Size = Marshal.SizeOf(typeof(T));
        header.Adapter.Low = adapterLow;
        header.Adapter.High = adapterHigh;
        header.Id = id;
        return header;
    }

    static Config Query(uint flags) {
        while (true) {
            uint pathCount;
            uint modeCount;
            Check(GetDisplayConfigBufferSizes(flags, out pathCount, out modeCount), "GetDisplayConfigBufferSizes");
            Config config = new Config();
            config.Paths = new byte[pathCount * PathSize];
            config.Modes = new byte[modeCount * ModeSize];
            int result = QueryDisplayConfig(flags, ref pathCount, config.Paths, ref modeCount, config.Modes, IntPtr.Zero);
            if (result == ErrorInsufficientBuffer) {
                continue;  // The display layout changed between the two calls.
            }
            Check(result, "QueryDisplayConfig");
            config.PathCount = pathCount;
            config.ModeCount = modeCount;
            return config;
        }
    }

    // Returns the layout of the active displays, to put back later with Restore.
    public static Config GetActiveConfig() {
        return Query(OnlyActivePaths);
    }

    // Puts back a layout from GetActiveConfig, and saves it as the layout Windows uses for these displays.
    // Falls back to the layout Windows last saved if the displays changed since.
    public static void Restore(Config config) {
        int result = SetDisplayConfig(config.PathCount, config.Paths, config.ModeCount, config.Modes, SdcApply | SdcUseSuppliedDisplayConfig | SdcAllowChanges | SdcSaveToDatabase);
        if (result != 0) {
            Check(SetDisplayConfig(0, null, 0, null, SdcApply | SdcUseDatabaseCurrent), "SetDisplayConfig");
            throw new Exception(string.Format("Couldn't put back the saved display layout ({0}), so Windows' own layout was used", new Win32Exception(result).Message));
        }
    }

    static Target ReadTarget(Config config, uint index) {
        int path = (int)index * PathSize;
        Target target = new Target();
        target.AdapterLow = U32(config.Paths, path + 20);
        target.AdapterHigh = I32(config.Paths, path + 24);
        target.TargetId = U32(config.Paths, path + 28);
        target.OutputTechnology = I32(config.Paths, path + 36);
        target.Active = (U32(config.Paths, path + 68) & PathActive) != 0;
        if (target.Active) {
            target.SourceId = U32(config.Paths, path + 8);
            uint modeIdx = U32(config.Paths, path + 12);
            if (modeIdx < config.ModeCount && U32(config.Modes, (int)modeIdx * ModeSize) == ModeInfoSource) {
                target.X = I32(config.Modes, (int)modeIdx * ModeSize + 28);
                target.Y = I32(config.Modes, (int)modeIdx * ModeSize + 32);
                target.Primary = target.X == 0 && target.Y == 0;
            }
        }
        TargetName name = new TargetName();
        name.Header = MakeHeader<TargetName>(GetTargetName, target.AdapterLow, target.AdapterHigh, target.TargetId);
        if (GetInfo(ref name) == 0) {
            target.FriendlyName = name.FriendlyName;
            target.DevicePath = name.DevicePath;
        }
        return target;
    }

    // Returns every connected display, whether it is on or off.
    public static List<Target> GetTargets() {
        Config config = Query(AllPaths);
        List<Target> targets = new List<Target>();
        for (uint i = 0; i < config.PathCount; i++) {
            bool available = U32(config.Paths, (int)i * PathSize + 60) != 0;
            bool active = (U32(config.Paths, (int)i * PathSize + 68) & PathActive) != 0;
            if (!available && !active) {
                continue;
            }
            Target target = ReadTarget(config, i);
            int known = targets.FindIndex(t => t.Is(target));
            if (known < 0) {
                targets.Add(target);
            }
            else if (target.Active && !targets[known].Active) {
                targets[known] = target;  // Each display is listed once for each source that could show it.
            }
        }
        return targets;
    }

    // Turns on exactly these displays, keeping each one that is already on on its source.
    public static void SetActiveTargets(List<Target> wanted) {
        Config config = Query(AllPaths);
        List<int> chosen = new List<int>();
        foreach (Target want in wanted) {
            int pick = -1;
            for (uint i = 0; i < config.PathCount && pick < 0; i++) {
                Target path = ReadTarget(config, i);
                if (path.Is(want) && path.Active) {
                    pick = (int)i;
                }
            }
            for (uint i = 0; i < config.PathCount && pick < 0; i++) {
                int offset = (int)i * PathSize;
                Target path = ReadTarget(config, i);
                if (!path.Is(want) || U32(config.Paths, offset + 60) == 0) {
                    continue;
                }
                // A source can only show one picture, so use one no other chosen display on this adapter uses.
                uint source = U32(config.Paths, offset + 8);
                bool used = chosen.Exists(c => U32(config.Paths, c * PathSize + 8) == source && U32(config.Paths, c * PathSize) == U32(config.Paths, offset) && I32(config.Paths, c * PathSize + 4) == I32(config.Paths, offset + 4));
                if (!used) {
                    pick = (int)i;
                }
            }
            if (pick < 0) {
                throw new Exception("No free output to turn on " + want.FriendlyName);
            }
            chosen.Add(pick);
        }

        byte[] paths = new byte[chosen.Count * PathSize];
        for (int i = 0; i < chosen.Count; i++) {
            Array.Copy(config.Paths, chosen[i] * PathSize, paths, i * PathSize, PathSize);
            Put(paths, i * PathSize + 12, ModeIdxInvalid);
            Put(paths, i * PathSize + 32, ModeIdxInvalid);
            Put(paths, i * PathSize + 68, PathActive);
        }
        Check(SetDisplayConfig((uint)chosen.Count, paths, 0, null, SdcApply | SdcTopologySupplied | SdcAllowPathOrderChanges), "SetDisplayConfig");
    }

    // Makes a display that is on the primary display, by moving the desktop so that it starts at (0,0).
    // Returns false when it already is.
    public static bool MakePrimary(Target target) {
        Config config = Query(OnlyActivePaths);
        int dx = 0;
        int dy = 0;
        bool found = false;
        for (uint i = 0; i < config.PathCount; i++) {
            Target path = ReadTarget(config, i);
            if (path.Is(target) && path.Active) {
                dx = path.X;
                dy = path.Y;
                found = true;
            }
        }
        if (!found) {
            throw new Exception(target.FriendlyName + " is not on");
        }
        if (dx == 0 && dy == 0) {
            return false;
        }
        for (uint i = 0; i < config.ModeCount; i++) {
            int mode = (int)i * ModeSize;
            if (U32(config.Modes, mode) == ModeInfoSource) {
                Put(config.Modes, mode + 28, I32(config.Modes, mode + 28) - dx);
                Put(config.Modes, mode + 32, I32(config.Modes, mode + 32) - dy);
            }
        }
        Check(SetDisplayConfig(config.PathCount, config.Paths, config.ModeCount, config.Modes, SdcApply | SdcUseSuppliedDisplayConfig | SdcAllowChanges), "SetDisplayConfig");
        return true;
    }

    // Returns the name, such as \\.\DISPLAY1, that the mode functions know a display that is on by.
    public static string GetDeviceName(Target target) {
        Config config = Query(OnlyActivePaths);
        for (uint i = 0; i < config.PathCount; i++) {
            Target path = ReadTarget(config, i);
            if (path.Is(target)) {
                SourceName name = new SourceName();
                name.Header = MakeHeader<SourceName>(GetSourceName, U32(config.Paths, (int)i * PathSize), I32(config.Paths, (int)i * PathSize + 4), U32(config.Paths, (int)i * PathSize + 8));
                Check(GetInfo(ref name), "DisplayConfigGetDeviceInfo");
                return name.GdiName;
            }
        }
        throw new Exception(target.FriendlyName + " is not on");
    }

    static DevMode NewDevMode() {
        DevMode mode = new DevMode();
        mode.Size = (ushort)Marshal.SizeOf(typeof(DevMode));
        return mode;
    }

    public static Mode GetCurrentMode(string device) {
        DevMode current = NewDevMode();
        if (!EnumDisplaySettingsEx(device, EnumCurrentSettings, ref current, 0)) {
            throw new Exception("Couldn't read the current mode of " + device);
        }
        Mode mode = new Mode();
        mode.Width = current.PelsWidth;
        mode.Height = current.PelsHeight;
        mode.Frequency = current.DisplayFrequency;
        return mode;
    }

    // Returns the resolutions and refresh rates the display supports at its current color depth.
    public static List<Mode> GetModes(string device) {
        DevMode current = NewDevMode();
        EnumDisplaySettingsEx(device, EnumCurrentSettings, ref current, 0);
        List<Mode> modes = new List<Mode>();
        DevMode entry = NewDevMode();
        for (int i = 0; EnumDisplaySettingsEx(device, i, ref entry, 0); i++) {
            if (current.BitsPerPel != 0 && entry.BitsPerPel != current.BitsPerPel) {
                continue;
            }
            Mode mode = new Mode();
            mode.Width = entry.PelsWidth;
            mode.Height = entry.PelsHeight;
            mode.Frequency = entry.DisplayFrequency;
            modes.Add(mode);
            entry = NewDevMode();
        }
        return modes;
    }

    // Sets the resolution and refresh rate of a display until it changes again.
    public static void SetMode(string device, uint width, uint height, uint frequency) {
        DevMode mode = NewDevMode();
        if (!EnumDisplaySettingsEx(device, EnumCurrentSettings, ref mode, 0)) {
            throw new Exception("Couldn't read the current mode of " + device);
        }
        mode.PelsWidth = width;
        mode.PelsHeight = height;
        mode.DisplayFrequency = frequency;
        mode.Fields = DmPelsWidth | DmPelsHeight | DmDisplayFrequency;
        int result = ChangeDisplaySettingsEx(device, ref mode, IntPtr.Zero, 0, IntPtr.Zero);
        if (result != 0) {
            throw new Exception(string.Format("ChangeDisplaySettingsEx failed with result {0}", result));
        }
    }
}
'@

if (-not ('DisplayManager' -as [type])) {
    Add-Type -TypeDefinition $displaySource
}

$savedLayoutFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'saved-layout.json'

# Output technologies of displays that only exist in software, such as the Virtual Display plugin's.
$virtualOutputTechnologies = @(17)  # DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL

# Returns the connected physical displays, whether they are on or off.
function Get-PhysicalDisplays {
    return @([DisplayManager]::GetTargets() | Where-Object { $virtualOutputTechnologies -notcontains $_.OutputTechnology })
}

# Names a kind of display connection, or returns $null for one without a common name.
function Get-ConnectionName([int]$technology) {
    switch ($technology) {
        0 { 'VGA' }
        4 { 'DVI' }
        5 { 'HDMI' }
        10 { 'DisplayPort' }
        11 { 'built-in DisplayPort' }
        16 { 'USB or wireless' }
        -2147483648 { 'built-in' }
        default { $null }
    }
}

# Picks the refresh rate to use from those a display supports at the client's resolution: the client's own rate when
# the display has it, otherwise the lowest one above it, otherwise the highest one below it. Rates are whole numbers
# the way Windows lists them, where a rate just under a whole number (59.94) is listed as the number below it (59).
function Select-RefreshRate([int[]]$Available, [decimal]$ClientFps) {
    $wanted = [int][Math]::Floor($ClientFps + [decimal]0.01)
    if ($Available -contains $wanted) {
        return $wanted
    }
    $higher = @($Available | Where-Object { $_ -gt $wanted } | Sort-Object)
    if ($higher.Count -gt 0) {
        return $higher[0]
    }
    $lower = @($Available | Where-Object { $_ -lt $wanted } | Sort-Object -Descending)
    if ($lower.Count -gt 0) {
        return $lower[0]
    }
    return $null
}

# Saves the current layout, unless a layout saved by a stream that didn't end cleanly is still there, since that one
# is the layout from before any stream.
function Save-DisplayLayout {
    if (Test-Path -LiteralPath $savedLayoutFile) {
        Write-Output 'Keeping the display layout saved by an earlier stream'
        return
    }
    $config = [DisplayManager]::GetActiveConfig()
    [ordered]@{
        paths      = [Convert]::ToBase64String($config.Paths)
        path_count = $config.PathCount
        modes      = [Convert]::ToBase64String($config.Modes)
        mode_count = $config.ModeCount
    } | ConvertTo-Json | Set-Content -LiteralPath $savedLayoutFile -Encoding UTF8
}

# Puts back the layout from before the stream, if one was saved.
function Restore-DisplayLayout {
    if (-not (Test-Path -LiteralPath $savedLayoutFile)) {
        return
    }
    $saved = Get-Content -LiteralPath $savedLayoutFile -Raw | ConvertFrom-Json
    $config = New-Object DisplayManager+Config
    $config.Paths = [Convert]::FromBase64String($saved.paths)
    $config.PathCount = [uint32]$saved.path_count
    $config.Modes = [Convert]::FromBase64String($saved.modes)
    $config.ModeCount = [uint32]$saved.mode_count
    try {
        [DisplayManager]::Restore($config)
        Write-Output 'Put back the display layout from before the stream'
    }
    finally {
        Remove-Item -LiteralPath $savedLayoutFile -Force
    }
}

# Returns a display's name, such as "BenQ EX3501R", or one made from its connection for a display without a name,
# such as a laptop's own screen.
function Get-DisplayLabel($display) {
    if ($display.FriendlyName) {
        return $display.FriendlyName
    }
    $connection = Get-ConnectionName $display.OutputTechnology
    if ($connection -like 'built-in*') {
        return 'Built-in display'
    }
    if ($connection) {
        return "$connection display"
    }
    return "Display $($display.TargetId)"
}

# Turns on the chosen display (or keeps the primary one), turns off the others when asked, makes it the primary
# display, and sets the client's resolution and the closest refresh rate when the display supports that resolution.
function Set-StreamDisplay {
    $width = [uint32]$env:SUNSHINE_CLIENT_WIDTH
    $height = [uint32]$env:SUNSHINE_CLIENT_HEIGHT
    $fps = if ($env:SUNSHINE_CLIENT_FPS_X100) { [decimal]$env:SUNSHINE_CLIENT_FPS_X100 / 100 } else { [decimal]$env:SUNSHINE_CLIENT_FPS }

    $displays = Get-PhysicalDisplays
    $active = @($displays | Where-Object Active)
    $chosen = $null
    if ($env:SUNSHINE_PLUGIN_OPT_DISPLAY) {
        $chosen = $displays | Where-Object { $_.DevicePath -eq $env:SUNSHINE_PLUGIN_OPT_DISPLAY } | Select-Object -First 1
        if (-not $chosen) {
            Write-Output 'Warning: the chosen display is not connected, using the primary display'
        }
    }
    if (-not $chosen) {
        $chosen = $active | Where-Object Primary | Select-Object -First 1
    }
    if (-not $chosen) {
        throw 'No physical display is on'
    }
    $name = Get-DisplayLabel $chosen

    Save-DisplayLayout

    $exclusive = $env:SUNSHINE_PLUGIN_OPT_EXCLUSIVE -eq 'true'
    if ($exclusive -and ($active.Count -ne 1 -or -not $chosen.Active)) {
        Write-Output "Turning on only $name"
        $wanted = New-Object 'System.Collections.Generic.List[DisplayManager+Target]'
        $wanted.Add($chosen)
        [DisplayManager]::SetActiveTargets($wanted)
    }
    elseif (-not $chosen.Active) {
        Write-Output "Turning on $name"
        $wanted = New-Object 'System.Collections.Generic.List[DisplayManager+Target]'
        foreach ($display in $active) { $wanted.Add($display) }
        $wanted.Add($chosen)
        [DisplayManager]::SetActiveTargets($wanted)
    }

    if ([DisplayManager]::MakePrimary($chosen)) {
        Write-Output "Made $name the primary display"
    }

    if (-not $width -or -not $height) {
        Write-Output 'The client did not send a resolution; leaving the resolution as it is'
        return
    }
    $device = [DisplayManager]::GetDeviceName($chosen)
    $rates = @([DisplayManager]::GetModes($device) | Where-Object { $_.Width -eq $width -and $_.Height -eq $height } |
        ForEach-Object { [int]$_.Frequency } | Sort-Object -Unique)
    if ($rates.Count -eq 0) {
        Write-Output "$name does not support ${width}x${height}; leaving its resolution and refresh rate as they are"
        return
    }
    $rate = Select-RefreshRate $rates $fps
    $current = [DisplayManager]::GetCurrentMode($device)
    if ($current.Width -eq $width -and $current.Height -eq $height -and $current.Frequency -eq $rate) {
        Write-Output "$name is already at ${width}x${height} $rate Hz"
        return
    }
    Write-Output "Setting $name to ${width}x${height} $rate Hz for a $fps fps client (available: $($rates -join ', ') Hz)"
    [DisplayManager]::SetMode($device, $width, $height, [uint32]$rate)
}
