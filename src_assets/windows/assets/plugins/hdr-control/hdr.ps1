# Shared helpers for HDR color profiles on the primary display. Dot-source this file.
#
# Windows keeps a list of the color profiles linked to each display in HDR (advanced color) mode, and uses the last
# one as the display's HDR profile. The scripts make the chosen profile the HDR profile with the mscms display
# association functions (Windows 10 2004 and later), which take the adapter and source ID of the display's path.
# The display's own list is read from the registry, since Windows doesn't report it in either mode.

$hdrSource = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class HdrControl {
    public const int ModeSdr = 0;
    public const int ModeWideColor = 1;
    public const int ModeHdr = 2;
    public const int ScopeSystemWide = 0;
    public const int ScopeCurrentUser = 1;

    const uint OnlyActivePaths = 2;
    const int ErrorInsufficientBuffer = 122;
    const int ModeInfoSource = 1;
    const int GetTargetName = 2;
    const int GetAdvancedColorInfo = 9;
    const int GetAdvancedColorInfo2 = 15;
    const int SetAdvancedColorState = 10;
    const int SetHdrState = 16;
    const uint AdvancedColorSupportedBit = 1u;
    const uint AdvancedColorEnabledBit = 1u << 1;
    const uint HdrSupportedBit = 1u << 4;
    const uint HdrUserEnabledBit = 1u << 5;
    const int ProfileTypeIcc = 0;
    const int ProfileSubtypeHdr = 8;  // CPST_EXTENDED_DISPLAY_COLOR_MODE
    const int FileNotFound = unchecked((int)0x80070002);

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
        [FieldOffset(0)] public Luid SourceAdapter;
        [FieldOffset(8)] public uint SourceId;
        [FieldOffset(12)] public uint SourceModeIdx;
        [FieldOffset(20)] public Luid TargetAdapter;
        [FieldOffset(28)] public uint TargetId;
    }

    [StructLayout(LayoutKind.Explicit, Size = 64)]
    struct ModeInfo {
        [FieldOffset(0)] public int InfoType;
        [FieldOffset(28)] public int X;
        [FieldOffset(32)] public int Y;
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
    struct ColorInfo {
        public Header Header;
        public uint Flags;
        public int ColorEncoding;
        public uint BitsPerColor;
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
    struct SetState {
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
    static extern int GetInfo(ref ColorInfo info);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo")]
    static extern int GetInfo(ref ColorInfo2 info);

    [DllImport("user32.dll", EntryPoint = "DisplayConfigSetDeviceInfo")]
    static extern int SetInfo(ref SetState info);

    [DllImport("mscms.dll")]
    static extern int ColorProfileGetDisplayUserScope(Luid adapter, uint sourceId, out int scope);

    [DllImport("mscms.dll")]
    static extern int ColorProfileGetDisplayDefault(int scope, Luid adapter, uint sourceId, int type, int subtype, out IntPtr name);

    [DllImport("mscms.dll", CharSet = CharSet.Unicode)]
    static extern int ColorProfileAddDisplayAssociation(int scope, string name, Luid adapter, uint sourceId, bool setAsDefault, bool advancedColor);

    [DllImport("mscms.dll", CharSet = CharSet.Unicode)]
    static extern int ColorProfileRemoveDisplayAssociation(int scope, string name, Luid adapter, uint sourceId, bool advancedColor);

    [DllImport("mscms.dll", CharSet = CharSet.Unicode)]
    static extern int ColorProfileSetDisplayDefaultAssociation(int scope, string name, int type, int subtype, Luid adapter, uint sourceId);

    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);

    public class Display {
        public Luid SourceAdapter;
        public uint SourceId;
        public Luid TargetAdapter;
        public uint TargetId;
        public bool Primary;
        public string FriendlyName;
        public string DevicePath;
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

    static void CheckHresult(int result, string what) {
        if (result != 0) {
            throw new Exception(string.Format("{0} failed: 0x{1:X8} {2}", what, result, new Win32Exception(result).Message));
        }
    }

    // Returns every active display. The primary display's desktop starts at (0,0).
    public static List<Display> GetDisplays() {
        PathInfo[] paths;
        ModeInfo[] modes;
        uint pathCount;
        uint modeCount;
        while (true) {
            Check(GetDisplayConfigBufferSizes(OnlyActivePaths, out pathCount, out modeCount));
            paths = new PathInfo[pathCount];
            modes = new ModeInfo[modeCount];
            int result = QueryDisplayConfig(OnlyActivePaths, ref pathCount, paths, ref modeCount, modes, IntPtr.Zero);
            if (result == ErrorInsufficientBuffer) {
                continue;  // The display layout changed between the two calls.
            }
            Check(result);
            break;
        }

        List<Display> displays = new List<Display>();
        for (uint i = 0; i < pathCount; i++) {
            Display display = new Display();
            display.SourceAdapter = paths[i].SourceAdapter;
            display.SourceId = paths[i].SourceId;
            display.TargetAdapter = paths[i].TargetAdapter;
            display.TargetId = paths[i].TargetId;
            uint modeIdx = paths[i].SourceModeIdx;
            display.Primary = modeIdx < modeCount && modes[modeIdx].InfoType == ModeInfoSource && modes[modeIdx].X == 0 && modes[modeIdx].Y == 0;
            TargetName name = new TargetName();
            name.Header = MakeHeader<TargetName>(GetTargetName, display.TargetAdapter, display.TargetId);
            Check(GetInfo(ref name));
            display.FriendlyName = name.FriendlyName;
            display.DevicePath = name.DevicePath;
            displays.Add(display);
        }
        return displays;
    }

    // Returns the display's color mode: ModeSdr, ModeWideColor or ModeHdr.
    public static int GetMode(Display display) {
        ColorInfo2 info2 = new ColorInfo2();
        info2.Header = MakeHeader<ColorInfo2>(GetAdvancedColorInfo2, display.TargetAdapter, display.TargetId);
        if (GetInfo(ref info2) == 0) {
            return info2.ActiveMode;
        }
        // Before Windows 11 24H2, advanced color on a display means HDR.
        ColorInfo info = new ColorInfo();
        info.Header = MakeHeader<ColorInfo>(GetAdvancedColorInfo, display.TargetAdapter, display.TargetId);
        Check(GetInfo(ref info));
        return (info.Flags & 2) != 0 ? ModeHdr : ModeSdr;
    }

    // Returns whether the display supports HDR.
    public static bool IsHdrSupported(Display display) {
        ColorInfo2 info2 = new ColorInfo2();
        info2.Header = MakeHeader<ColorInfo2>(GetAdvancedColorInfo2, display.TargetAdapter, display.TargetId);
        if (GetInfo(ref info2) == 0) {
            return (info2.Flags & HdrSupportedBit) != 0;
        }
        ColorInfo info = new ColorInfo();
        info.Header = MakeHeader<ColorInfo>(GetAdvancedColorInfo, display.TargetAdapter, display.TargetId);
        Check(GetInfo(ref info));
        return (info.Flags & AdvancedColorSupportedBit) != 0;
    }

    // Returns whether HDR is turned on for the display, as the Use HDR switch in Settings shows it.
    public static bool IsHdrOn(Display display) {
        ColorInfo2 info2 = new ColorInfo2();
        info2.Header = MakeHeader<ColorInfo2>(GetAdvancedColorInfo2, display.TargetAdapter, display.TargetId);
        if (GetInfo(ref info2) == 0) {
            return (info2.Flags & HdrUserEnabledBit) != 0;
        }
        ColorInfo info = new ColorInfo();
        info.Header = MakeHeader<ColorInfo>(GetAdvancedColorInfo, display.TargetAdapter, display.TargetId);
        Check(GetInfo(ref info));
        return (info.Flags & AdvancedColorEnabledBit) != 0;
    }

    // Turns HDR on or off for the display, as Sunshine's display device configuration does.
    public static void SetHdr(Display display, bool on) {
        ColorInfo2 probe = new ColorInfo2();
        probe.Header = MakeHeader<ColorInfo2>(GetAdvancedColorInfo2, display.TargetAdapter, display.TargetId);
        // Windows 11 24H2 has a switch for HDR alone; before it, HDR is advanced color.
        int type = GetInfo(ref probe) == 0 ? SetHdrState : SetAdvancedColorState;
        SetState state = new SetState();
        state.Header = MakeHeader<SetState>(type, display.TargetAdapter, display.TargetId);
        state.Value = on ? 1u : 0u;
        Check(SetInfo(ref state));
    }

    // Returns whether the display's profiles are the signed-in user's own or shared by every user.
    public static int GetScope(Display display) {
        int scope;
        CheckHresult(ColorProfileGetDisplayUserScope(display.SourceAdapter, display.SourceId, out scope), "ColorProfileGetDisplayUserScope");
        return scope;
    }

    // Returns the file name of the display's HDR profile, or null when it has none.
    public static string GetHdrProfile(Display display, int scope) {
        IntPtr name;
        int result = ColorProfileGetDisplayDefault(scope, display.SourceAdapter, display.SourceId, ProfileTypeIcc, ProfileSubtypeHdr, out name);
        if (result == FileNotFound) {
            return null;
        }
        CheckHresult(result, "ColorProfileGetDisplayDefault");
        if (name == IntPtr.Zero) {
            return null;
        }
        string profile = Marshal.PtrToStringUni(name);
        LocalFree(name);
        return profile;
    }

    // Links a profile to the display in HDR mode and makes it the display's HDR profile.
    public static void UseHdrProfile(Display display, int scope, string profile) {
        CheckHresult(ColorProfileAddDisplayAssociation(scope, profile, display.SourceAdapter, display.SourceId, true, true), "ColorProfileAddDisplayAssociation");
    }

    // Makes a profile already linked to the display its HDR profile again.
    public static void SetHdrProfile(Display display, int scope, string profile) {
        CheckHresult(ColorProfileSetDisplayDefaultAssociation(scope, profile, ProfileTypeIcc, ProfileSubtypeHdr, display.SourceAdapter, display.SourceId), "ColorProfileSetDisplayDefaultAssociation");
    }

    // Unlinks a profile from the display in HDR mode.
    public static void UnlinkHdrProfile(Display display, int scope, string profile) {
        CheckHresult(ColorProfileRemoveDisplayAssociation(scope, profile, display.SourceAdapter, display.SourceId, true), "ColorProfileRemoveDisplayAssociation");
    }
}
'@

$colorDir = Join-Path $env:windir 'System32\spool\drivers\color'
$stateFile = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'state.json'

# Compiles the display helper. It is only done when it will be used, as compiling takes a moment.
function Import-HdrControl {
    if (-not ('HdrControl' -as [type])) {
        Add-Type -TypeDefinition $hdrSource
    }
}

# Returns the primary display, or $null when there is none.
function Get-PrimaryDisplay {
    Import-HdrControl
    [HdrControl]::GetDisplays() | Where-Object { $_.Primary } | Select-Object -First 1
}

# Returns the display with a device path, or $null when it isn't active.
function Get-DisplayByPath([string]$devicePath) {
    Import-HdrControl
    [HdrControl]::GetDisplays() | Where-Object { $_.DevicePath -eq $devicePath } | Select-Object -First 1
}

# Returns the profiles linked to a display in HDR mode, in the order Windows keeps them; the last is the HDR profile.
function Get-LinkedHdrProfiles($display, [int]$scope) {
    # \\?\DISPLAY#BNQ7F5E#5&eb70393&e&UID265#{e6f07b5f-...} is the device DISPLAY\BNQ7F5E\5&eb70393&e&UID265
    $parts = $display.DevicePath -replace '^\\\\\?\\', '' -split '#'
    if ($parts.Count -lt 3) {
        return @()
    }
    $instance = $parts[0..2] -join '\'
    $driver = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Enum\$instance" -Name Driver -ErrorAction SilentlyContinue).Driver
    if (-not $driver) {
        return @()
    }
    if ($scope -eq [HdrControl]::ScopeCurrentUser) {
        $key = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\ICM\ProfileAssociations\Display\$driver"
    }
    else {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$driver"
    }
    $value = (Get-ItemProperty -LiteralPath $key -Name ICMProfileAC -ErrorAction SilentlyContinue).ICMProfileAC
    @($value | Where-Object { $_ })
}

function Read-BigEndianUInt32([byte[]]$bytes, [int]$offset) {
    ([uint32]$bytes[$offset] -shl 24) -bor ([uint32]$bytes[$offset + 1] -shl 16) -bor ([uint32]$bytes[$offset + 2] -shl 8) -bor [uint32]$bytes[$offset + 3]
}

# Reads an ICC profile. Returns its description and the peak luminance of its MHC2 tag, which marks a profile made
# for HDR, or $null when the file isn't such a profile.
function Read-HdrProfile([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 132 -or [Text.Encoding]::ASCII.GetString($bytes, 36, 4) -ne 'acsp') {
        return $null
    }
    $tags = @{}
    $count = Read-BigEndianUInt32 $bytes 128
    for ($i = 0; $i -lt $count -and 144 + 12 * $i -le $bytes.Length; $i++) {
        $entry = 132 + 12 * $i
        $tags[[Text.Encoding]::ASCII.GetString($bytes, $entry, 4)] = [int](Read-BigEndianUInt32 $bytes ($entry + 4))
    }
    if (-not $tags.ContainsKey('MHC2') -or $tags['MHC2'] + 20 -gt $bytes.Length) {
        return $null
    }

    $description = $null
    if ($tags.ContainsKey('desc')) {
        $offset = $tags['desc']
        $type = [Text.Encoding]::ASCII.GetString($bytes, $offset, 4)
        if ($type -eq 'desc') {
            $length = [int](Read-BigEndianUInt32 $bytes ($offset + 8))
            $description = [Text.Encoding]::ASCII.GetString($bytes, $offset + 12, [Math]::Max(0, $length - 1))
        }
        elseif ($type -eq 'mluc') {
            $length = [int](Read-BigEndianUInt32 $bytes ($offset + 20))
            $start = [int](Read-BigEndianUInt32 $bytes ($offset + 24))
            $description = [Text.Encoding]::BigEndianUnicode.GetString($bytes, $offset + $start, $length)
        }
    }
    [pscustomobject]@{
        Description = ($description -replace "`0", '').Trim()
        PeakNits = [Math]::Round((Read-BigEndianUInt32 $bytes ($tags['MHC2'] + 16)) / 65536)
    }
}

# Returns every installed profile made for HDR, keyed by file name.
function Get-InstalledHdrProfiles {
    $profiles = [ordered]@{}
    Get-ChildItem -LiteralPath $colorDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in '.icc', '.icm' } |
        Sort-Object Name |
        ForEach-Object {
            try {
                $hdrProfile = Read-HdrProfile $_.FullName
                if ($hdrProfile) {
                    $profiles[$_.Name] = $hdrProfile
                }
            }
            catch {
                # A profile that can't be read isn't offered.
            }
        }
    $profiles
}

# Reads what the plugin changed this session: an array with an entry per display, holding its device path, its HDR
# profile before the plugin changed it (`previous`, valid once `profile_saved`), the profiles the plugin linked to it
# (`added`), and whether HDR was on before the plugin changed it (`hdr_was`, null when it didn't).
function Read-HdrState {
    if (-not (Test-Path -LiteralPath $stateFile)) {
        return @()
    }
    try {
        # Windows PowerShell's ConvertFrom-Json passes an array on as one object, so assign it before wrapping it.
        $parsed = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        $entries = @($parsed)
    }
    catch {
        Write-Output "Warning: ignoring an unreadable $stateFile"
        return @()
    }
    foreach ($entry in $entries) {
        # Version 1.0 always saved the previous profile and never changed HDR.
        if ($entry.PSObject.Properties.Name -notcontains 'profile_saved') {
            $entry | Add-Member -NotePropertyName profile_saved -NotePropertyValue $true
        }
        if ($entry.PSObject.Properties.Name -notcontains 'hdr_was') {
            $entry | Add-Member -NotePropertyName hdr_was -NotePropertyValue $null
        }
    }
    $entries
}

function Write-HdrState($entries) {
    ConvertTo-Json -InputObject @($entries) -Depth 4 | Set-Content -LiteralPath $stateFile -Encoding UTF8
}

# Returns whether the stream wants HDR on ($true) or off ($false), or $null to leave it as it is.
function Get-WantedHdr {
    switch ($env:SUNSHINE_PLUGIN_OPT_HDR) {
        'on' { return $true }
        'off' { return $false }
        'client' { return $env:SUNSHINE_CLIENT_HDR -eq 'true' }
    }
    $null
}

# Turns HDR on or off for the primary display as the stream wants, then makes the profile chosen for the stream its
# HDR profile once it is in HDR mode. Records what it changes the first time, so the undo script can put it back.
# Problems only log a warning, since the stream still works with the display as it is.
function Set-StreamHdr {
    try {
        $display = Get-PrimaryDisplay
        if (-not $display) {
            Write-Output 'Warning: there is no primary display; leaving the display as it is'
            return
        }

        $entries = @(Read-HdrState)
        $entry = $entries | Where-Object { $_.device -eq $display.DevicePath } | Select-Object -First 1
        if (-not $entry) {
            $entry = [pscustomobject]@{ device = $display.DevicePath; profile_saved = $false; previous = $null; added = @(); hdr_was = $null }
            $entries += $entry
        }

        $wantHdr = Get-WantedHdr
        if ($null -ne $wantHdr) {
            $state = if ($wantHdr) { 'on' } else { 'off' }
            if (-not [HdrControl]::IsHdrSupported($display)) {
                Write-Output "Warning: $($display.FriendlyName) doesn't support HDR; leaving it as it is"
            }
            elseif ([HdrControl]::IsHdrOn($display) -eq $wantHdr) {
                Write-Output "HDR is already $state for $($display.FriendlyName)"
            }
            else {
                if ($null -eq $entry.hdr_was) {
                    $entry.hdr_was = -not $wantHdr
                }
                [HdrControl]::SetHdr($display, $wantHdr)
                Write-HdrState $entries
                Write-Output "Turned HDR $state for $($display.FriendlyName)"
            }
        }

        $profileName = $env:SUNSHINE_PLUGIN_OPT_PROFILE
        if (-not $profileName) {
            Write-Output 'No HDR profile chosen; leaving the HDR profile as it is'
            return
        }
        $streamHdr = if ($null -ne $wantHdr) { $wantHdr } else { $env:SUNSHINE_CLIENT_HDR -eq 'true' }
        if (-not $streamHdr) {
            Write-Output "The stream for $env:SUNSHINE_CLIENT_NAME isn't in HDR; leaving the HDR profile as it is"
            return
        }
        if (-not (Test-Path -LiteralPath (Join-Path $colorDir $profileName))) {
            Write-Output "Warning: the HDR profile $profileName isn't installed; leaving the HDR profile as it is"
            return
        }

        # Windows may still be turning on HDR for the stream.
        $deadline = [DateTime]::UtcNow.AddSeconds([double]$env:SUNSHINE_PLUGIN_OPT_WAIT_SECONDS)
        while ([HdrControl]::GetMode($display) -ne [HdrControl]::ModeHdr) {
            if ([DateTime]::UtcNow -ge $deadline) {
                Write-Output "Warning: $($display.FriendlyName) isn't in HDR mode; leaving its HDR profile as it is"
                return
            }
            Start-Sleep -Milliseconds 250
            $display = Get-DisplayByPath $entry.device
            if (-not $display) {
                Write-Output 'Warning: the primary display went away; leaving the display as it is'
                return
            }
        }

        $scope = [HdrControl]::GetScope($display)
        if ($scope -ne [HdrControl]::ScopeCurrentUser) {
            Write-Output "Warning: $($display.FriendlyName) uses color profiles shared by every user, which only an administrator can change. Turn on 'Use my settings for this device' for it in Color Management to let the plugin change them."
            return
        }

        $current = [HdrControl]::GetHdrProfile($display, $scope)
        if ($current -eq $profileName) {
            Write-Output "$profileName is already the HDR profile of $($display.FriendlyName)"
            return
        }

        $linked = Get-LinkedHdrProfiles $display $scope
        if (-not $entry.profile_saved) {
            $entry.previous = $current
            $entry.profile_saved = $true
        }
        [HdrControl]::UseHdrProfile($display, $scope, $profileName)
        if ($linked -notcontains $profileName -and @($entry.added) -notcontains $profileName) {
            $entry.added = @($entry.added) + $profileName
        }
        Write-HdrState $entries
        Write-Output "Made $profileName the HDR profile of $($display.FriendlyName) (was $(if ($current) { $current } else { 'none' }))"
    }
    catch {
        Write-Output "Warning: could not set up HDR for the stream: $_"
    }
}

# Puts back the HDR profiles and HDR modes the plugin changed, and unlinks the profiles it linked, then forgets them.
function Restore-StreamHdr {
    $entries = @(Read-HdrState)
    if ($entries.Count -eq 0) {
        return
    }
    if ($env:SUNSHINE_PLUGIN_OPT_RESTORE -eq 'true') {
        foreach ($entry in $entries) {
            try {
                $display = Get-DisplayByPath $entry.device
                if (-not $display) {
                    Write-Output "Warning: $($entry.device) isn't active, so its HDR settings weren't put back"
                    continue
                }
                if ($entry.profile_saved) {
                    $scope = [HdrControl]::GetScope($display)
                    if ($entry.previous) {
                        [HdrControl]::SetHdrProfile($display, $scope, $entry.previous)
                        Write-Output "Put back $($entry.previous) as the HDR profile of $($display.FriendlyName)"
                    }
                    foreach ($added in @($entry.added)) {
                        if ($added -and $added -ne $entry.previous) {
                            [HdrControl]::UnlinkHdrProfile($display, $scope, $added)
                            Write-Output "Unlinked $added from $($display.FriendlyName)"
                        }
                    }
                }
                if ($null -ne $entry.hdr_was -and [HdrControl]::IsHdrOn($display) -ne [bool]$entry.hdr_was) {
                    [HdrControl]::SetHdr($display, [bool]$entry.hdr_was)
                    Write-Output "Turned HDR back $(if ($entry.hdr_was) { 'on' } else { 'off' }) for $($display.FriendlyName)"
                }
            }
            catch {
                Write-Output "Warning: could not put back the HDR settings of $($entry.device): $_"
            }
        }
    }
    else {
        Write-Output "Keeping the stream's HDR settings"
    }
    Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
}
