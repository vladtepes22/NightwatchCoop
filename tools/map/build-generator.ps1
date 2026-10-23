# Builds the package's map-generator folder: Hunter's Atlas's map generator (tools\map\generator,
# GPL-2.0-or-later) frozen with PyInstaller, the map's parchment texture, and the notices and source
# the frozen libraries need beside them. package-seamless.ps1 -MapGenerator <this output> ships it;
# the launcher runs it once on each player's PC (Core\MapPrep.cs). No game file goes in.
#   tools\map\build-generator.ps1 -Python <venv>\Scripts\python.exe
# The Python is a CPython 3.12 venv with generator\requirements-frozen.txt and
# generator\requirements-build.txt installed (pip install -r each).
param(
    [Parameter(Mandatory)][string]$Python,
    [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here -Parent) -Parent
if (!$Destination) { $Destination = Join-Path $repo 'work\map-generator' }
$work = Join-Path $repo 'work\map-generator-build'

# 1. The frozen generator (generate_atlas.spec: one folder, generate_atlas_lite.exe).
Remove-Item -LiteralPath $work, $Destination -Recurse -Force -ErrorAction SilentlyContinue
& $Python -m PyInstaller --noconfirm --distpath (Join-Path $work 'dist') --workpath (Join-Path $work 'build') (Join-Path $here 'generator\generate_atlas.spec')
if ($LASTEXITCODE) { throw "PyInstaller failed ($LASTEXITCODE)" }
Copy-Item -Path (Join-Path $work 'dist\generate_atlas_lite') -Destination $Destination -Recurse
$exe = Join-Path $Destination 'generate_atlas_lite.exe'
& $exe --self-test-imports | Out-Null
if ($LASTEXITCODE) { throw "The frozen generator's import self-test failed ($LASTEXITCODE)" }

# 2. The parchment the map is drawn on (Hunter's Atlas's art), as the engine reads it.
& $Python (Join-Path $here 'parchment-bbra.py') (Join-Path $here 'art\parchment.png') (Join-Path $Destination 'parchment.bbra')
if ($LASTEXITCODE) { throw "parchment-bbra.py failed ($LASTEXITCODE)" }

# 3. Notices: every bundled library's licence, and Soulstruct's source (GPL-3.0-or-later; its exact
#    PyPI source distribution, checked against the receipt's hash).
Copy-Item -Path (Join-Path $here 'generator\licenses') -Destination (Join-Path $Destination 'licenses') -Recurse
Copy-Item -LiteralPath (Join-Path $here 'generator\dependency-notices.json') -Destination (Join-Path $Destination 'licenses')
$receipt = Get-Content -LiteralPath (Join-Path $here 'generator\licenses\soulstruct-license-receipt.json') -Raw | ConvertFrom-Json
$sdist = Join-Path $Destination "licenses\soulstruct-$($receipt.version).tar.gz"
Invoke-WebRequest -Uri $receipt.pypi_source_distribution_url -OutFile $sdist -UseBasicParsing
if ((Get-FileHash -LiteralPath $sdist).Hash.ToLowerInvariant() -ne $receipt.source_distribution_sha256) {
    throw "Soulstruct's source distribution does not match its receipt"
}
[IO.File]::WriteAllText((Join-Path $Destination 'README.txt'),
    "The map generator: Hunter's Atlas by MikeTat1988 (github.com/MikeTat1988/HuntersAtlas),`r`n" +
    "GPL-2.0-or-later, run once by the launcher to make the in-game map from your own game files.`r`n" +
    "Its source is in Nightwatch Co-op's repository under tools\map\generator (built with`r`n" +
    "tools\map\build-generator.ps1); the bundled libraries' licences are in licenses\, Soulstruct's`r`n" +
    "source beside them. No game file is included.`r`n", [Text.UTF8Encoding]::new($false))
Remove-Item -LiteralPath $work -Recurse -Force
Write-Output "Map generator built: $Destination"
