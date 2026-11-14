# Zips a package folder for a release, with the folder itself as the zip's top entry.
#   tools\package-zip.ps1 -Package B:\work\package-b2\NightwatchCoop -Zip work\release\NightwatchCoop-v0.3.2-beta.2-win64.zip
# Entry names use '/' as the zip format requires. Compress-Archive and ZipFile.CreateFromDirectory under
# Windows PowerShell 5.1 write '\', which Linux archive tools (file managers, bsdtar) keep as part of the file
# name: the package then extracts flat and the launcher finds no engine (0.3.2 beta 1 under Proton).
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Zip
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$root = (Resolve-Path -LiteralPath $Package).Path.TrimEnd('\')
$top = Split-Path $root -Leaf
$Zip = [IO.Path]::GetFullPath($Zip)
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force -Confirm:$false }

$stream = [IO.File]::Open($Zip, [IO.FileMode]::CreateNew)
$archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File -Force) {
        $name = $top + '/' + $file.FullName.Substring($root.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $archive.Dispose()
    $stream.Dispose()
}
"zip: {0} ({1:N0} MB)" -f (Split-Path $Zip -Leaf), ((Get-Item -LiteralPath $Zip).Length / 1MB)
