# Downloads the portable build of Nyrna (https://nyrna.merritt.codes/) into the plugin's data directory.
# The portable build keeps its settings, and the window it suspended, next to nyrna.exe.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$version = '2.27.2'
$url = "https://github.com/Merrit/nyrna/releases/download/v$version/Nyrna-Windows-Portable.zip"
$sha256 = 'A3E609F3B80AB5B2D079F51F59FE6BB113A2003BAFFF3B32439A352000F9C7BC'

$installDir = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'nyrna'
$zip = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'Nyrna-Windows-Portable.zip'

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Write-Output "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

$actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
if ($actual -ne $sha256) {
    Remove-Item $zip -Force
    throw "Checksum mismatch for Nyrna-Windows-Portable.zip: expected $sha256, got $actual"
}

if (Test-Path $installDir) {
    Remove-Item $installDir -Recurse -Force
}
Expand-Archive -Path $zip -DestinationPath $installDir
Remove-Item $zip -Force

$exe = Join-Path $installDir 'nyrna.exe'
if (-not (Test-Path $exe)) {
    throw "nyrna.exe is missing from the download"
}
Write-Output "Installed Nyrna $version in $installDir"
