# Shared helpers for Windows' Auto Color Management (wide color) on the virtual display. Dot-source this file.
#
# SudoVDA's 10-bit SDR mode only changes the depth of the display mode. Windows keeps composing the desktop in 8 bits,
# which is what Sunshine captures, unless Auto Color Management is on for that monitor. Windows remembers the setting
# per monitor, and each client can have its own, so it is checked at the start of every stream.
# Problems here only log a warning, since the stream still works with 8-bit source frames.

$colorSource = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class VdisplayColor {
    public const int ModeSdr = 0;
    public const int ModeWideColor = 1;
    public const int ModeHdr = 2;

    const uint OnlyActivePaths = 2;
    const int ErrorInsufficientBuffer = 122;
    const int GetTargetName = 2;
    const int GetAdvancedColorInfo2 = 15;
    const int SetWideColorState = 17;
    const uint WideColorSupportedBit = 1u << 6;

    [StructLayout(LayoutKind.Sequential)]
    public struct Luid {
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

    [StructLayout(LayoutKind.Explicit, Size = 72)]
    struct PathInfo {
        [FieldOffset(20)] public Luid TargetAdapter;
        [FieldOffset(28)] public uint TargetId;
    }

    [StructLayout(LayoutKind.Explicit, Size = 64)]
    struct ModeInfo {
        [FieldOffset(0)] public int InfoType;
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

    [StructLayout(LayoutKind.Sequential)]
    struct ColorInfo2 {
        public Header Header;
        public uint Flags;
        public int ColorEncoding;
        public uint BitsPerColor;
        public int ActiveMode;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct WideColorState {
        public Header Header;
        public uint Value;
    }

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);

    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint pathCount, [Out] PathInfo[] paths, ref uint modeCount, [Out] ModeInfo[] modes, IntPtr topology);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")]
    static extern int GetInfo(ref TargetName info);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")]
    static extern int GetInfo(ref ColorInfo2 info);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigSetDeviceInfo")]
    static extern int SetInfo(ref WideColorState info);

    public class Target {
        public Luid Adapter;
        public uint Id;
        public string DevicePath;
        public bool WideColorSupported;
        public int Mode;
    }

    static Header MakeHeader<T>(int type, Luid adapter, uint id) {
        Header header = new Header();
        header.Type = type;
        header.Size = Marshal.SizeOf(typeof(T));
        header.Adapter = adapter;
        header.Id = id;
        return header;
    }

    static void Check(int result) {
        if (result != 0) {
            throw new Win32Exception(result);
        }
    }

    // Returns every active display target and its monitor device path.
    public static List<Target> GetTargets() {
        PathInfo[] paths;
        uint pathCount;
        while (true) {
            uint modeCount;
            Check(GetDisplayConfigBufferSizes(OnlyActivePaths, out pathCount, out modeCount));
            paths = new PathInfo[pathCount];
            ModeInfo[] modes = new ModeInfo[modeCount];
            int result = QueryDisplayConfig(OnlyActivePaths, ref pathCount, paths, ref modeCount, modes, IntPtr.Zero);
            if (result == ErrorInsufficientBuffer) {
                continue;  // The display layout changed between the two calls.
            }
            Check(result);
            break;
        }

        List<Target> targets = new List<Target>();
        for (uint i = 0; i < pathCount; i++) {
            TargetName name = new TargetName();
            name.Header = MakeHeader<TargetName>(GetTargetName, paths[i].TargetAdapter, paths[i].TargetId);
            Check(GetInfo(ref name));
            Target target = new Target();
            target.Adapter = paths[i].TargetAdapter;
            target.Id = paths[i].TargetId;
            target.DevicePath = name.DevicePath;
            targets.Add(target);
        }
        return targets;
    }

    // Updates a target's wide color support and active color mode. Needs a Windows version with this query.
    public static void ReadColor(Target target) {
        ColorInfo2 info = new ColorInfo2();
        info.Header = MakeHeader<ColorInfo2>(GetAdvancedColorInfo2, target.Adapter, target.Id);
        Check(GetInfo(ref info));
        target.WideColorSupported = (info.Flags & WideColorSupportedBit) != 0;
        target.Mode = info.ActiveMode;
    }

    // Turns Auto Color Management on for a target.
    public static void EnableWideColor(Target target) {
        WideColorState state = new WideColorState();
        state.Header = MakeHeader<WideColorState>(SetWideColorState, target.Adapter, target.Id);
        state.Value = 1;
        Check(SetInfo(ref state));
    }
}
'@

# Compiles the display configuration helper. It is only done when it will be used, as compiling takes a moment.
function Import-VdisplayColor {
    if (-not ('VdisplayColor' -as [type])) {
        Add-Type -TypeDefinition $colorSource
    }
}

# Turns on Auto Color Management for the active SudoVDA monitors, unless they are in HDR mode, and waits for it.
function Enable-VirtualDisplayWideColor {
    $ErrorActionPreference = 'Stop'
    try {
        Import-VdisplayColor
        $targets = @([VdisplayColor]::GetTargets() | Where-Object { $_.DevicePath -like '*#SMKD1CE#*' })
        if ($targets.Count -eq 0) {
            Write-Output 'Warning: could not find the virtual display to turn on Auto Color Management'
            return
        }

        foreach ($target in $targets) {
            [VdisplayColor]::ReadColor($target)
            if ($target.Mode -eq [VdisplayColor]::ModeHdr) {
                continue
            }
            if ($target.Mode -eq [VdisplayColor]::ModeWideColor) {
                Write-Output 'Auto Color Management is already on for the virtual display'
                continue
            }
            if (-not $target.WideColorSupported) {
                Write-Output 'Warning: the virtual display does not support Auto Color Management, so the stream will use 8-bit source frames'
                continue
            }

            [VdisplayColor]::EnableWideColor($target)
            for ($attempt = 0; $attempt -lt 30 -and $target.Mode -ne [VdisplayColor]::ModeWideColor; $attempt++) {
                Start-Sleep -Milliseconds 100
                [VdisplayColor]::ReadColor($target)
            }
            if ($target.Mode -eq [VdisplayColor]::ModeWideColor) {
                Write-Output 'Turned on Auto Color Management for the virtual display'
            }
            else {
                Write-Output 'Warning: Auto Color Management did not turn on, so the stream may use 8-bit source frames'
            }
        }
    }
    catch {
        Write-Output "Warning: could not turn on Auto Color Management, so the stream may use 8-bit source frames: $_"
    }
}
