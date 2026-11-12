# Downloads the portable build of Nyrna (https://nyrna.merritt.codes/) into the plugin's data directory.
# The portable build keeps its settings, and the window it suspended, next to nyrna.exe.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$version = '2.27.2'
$url = "https://github.com/Merrit/nyrna/releases/download/v$version/Nyrna-Windows-Portable.zip"
$sha256 = 'A3E609F3B80AB5B2D079F51F59FE6BB113A2003BAFFF3B32439A352000F9C7BC'

$installDir = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'nyrna'
$staging = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'nyrna-unpack'
$zip = Join-Path $env:SUNSHINE_PLUGIN_DATA_DIR 'Nyrna-Windows-Portable.zip'

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Write-Output "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

$actual = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
if ($actual -ne $sha256) {
    Remove-Item $zip -Force
    throw "Checksum mismatch for Nyrna-Windows-Portable.zip: expected $sha256, got $actual"
}

if (Test-Path $staging) {
    Remove-Item $staging -Recurse -Force
}
Expand-Archive -Path $zip -DestinationPath $staging
Remove-Item $zip -Force

if (-not (Test-Path (Join-Path $staging 'nyrna.exe'))) {
    Remove-Item $staging -Recurse -Force
    throw "nyrna.exe is missing from the download"
}

# Copy over an earlier install instead of replacing it, since a reinstall must keep the settings, and the window it
# suspended, that the portable build saves next to nyrna.exe. Exit codes from 8 up are failures.
& robocopy.exe $staging $installDir /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "Couldn't copy Nyrna to $installDir (robocopy exit code $LASTEXITCODE). Close Nyrna if it's running and try again."
}
Remove-Item $staging -Recurse -Force
Write-Output "Installed Nyrna $version in $installDir"
