# Functions any plugin script can use. Sunshine passes this file's path in SUNSHINE_PLUGIN_HELPERS; dot-source it:
#
#   . $env:SUNSHINE_PLUGIN_HELPERS

# Loads a C# type, such as one declaring Windows API functions with DllImport. Compiling it with Add-Type takes a
# quarter of a second or more on every run, so it's compiled once into the plugin's data folder, named after its source
# so a changed one is compiled again, and loaded from there afterwards. It's compiled in memory instead when that
# can't be done, for example when the script can't write to the data folder. Pass -DataDir from a script that Sunshine
# didn't start, such as one a scheduled task runs.
function Import-CompiledType([string]$name, [string]$source, [string]$dataDir = $env:SUNSHINE_PLUGIN_DATA_DIR) {
    if ($name -as [type]) {
        return
    }
    $partial = $null
    try {
        $hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($source))).Replace('-', '').Substring(0, 16)
        $dll = Join-Path $dataDir "$name-$hash.dll"
        if (-not (Test-Path -LiteralPath $dll)) {
            # Written under another name first, so a script running at the same time never loads half a file
            $partial = "$dll.$PID.tmp"
            Add-Type -TypeDefinition $source -OutputAssembly $partial -OutputType Library
            Move-Item -LiteralPath $partial -Destination $dll -Force
            $partial = $null
            # Remove the ones compiled from earlier sources; one a running script still has loaded stays until next time
            Get-ChildItem -LiteralPath $dataDir -Filter "$name-*.dll" | Where-Object { $_.FullName -ne $dll } |
                Remove-Item -Force -ErrorAction SilentlyContinue
        }
        Add-Type -Path $dll
    }
    catch {
        if ($partial) {
            Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        }
        if (-not ($name -as [type])) {
            Add-Type -TypeDefinition $source
        }
    }
}
