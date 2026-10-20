# Assembles a playable Awaken Seamless package in the Lantern Engine 0.9.34 layout:
#   <package>\Lantern Engine.exe, Bloodborne.exe, Bloodborne Launcher.exe, bbport.ini, package.json
#   <package>\engine\out      this repo's bb-start, bb-probe, bb-shader-lab, ... and their DLLs
#   <package>\engine\patches  this repo's patches (with the Party: patches)
#   <package>\seamless        the party save tools and their own Python (the launcher removes
#                             engine\scripts and engine\tools once the engine starts natively)
# Shaders, upscaler assets, the shader catalog and the two small start programs come from the
# official Lantern Engine release this build is based on (-Base: its extracted BloodbornePC folder).
#   tools\package-seamless.ps1 -Base work\downloads\lantern-0.9.34-beta.9.5\x\BloodbornePC
param(
    [Parameter(Mandatory)][string]$Base,
    [string]$Destination = '',
    [string]$Game = '',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Version = '0.2.0-beta.4',
    # A package to publish: no first-run settings with this PC's folders (the launcher finds its own).
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
# JSON without a byte-order mark (Windows PowerShell's -Encoding utf8 writes one).
function Write-Json($value, $path) { [IO.File]::WriteAllText($path, ($value | ConvertTo-Json), [Text.UTF8Encoding]::new($false)) }
$engineId = "awaken-seamless-$Version"
$repo = Split-Path $PSScriptRoot -Parent
$engine = Join-Path $repo 'engine'
$native = Join-Path $MsysRoot 'clang64\bin'
$base = (Resolve-Path -LiteralPath $Base).Path
if (!$Destination) { $Destination = Join-Path $repo 'work\package\AwakenSeamless' }
$destinationRoot = [IO.Path]::GetFullPath($Destination)

$programs = 'bb-start.exe', 'bb-probe.exe', 'bb-shader-lab.exe', 'bb-engine-assets.exe', 'bb-gpu-capabilities.exe'
foreach ($file in $programs + 'gpu-cache.id', 'gpu-cache.legacy.id') {
    if (!(Test-Path -LiteralPath (Join-Path $engine "out\$file"))) { throw "Build the engine first (engine\build.sh): out\$file is missing." }
}
if (!(Test-Path -LiteralPath (Join-Path $base 'engine\out\bb-start.exe'))) { throw "Not a Lantern Engine 0.9.32+ package: $base" }
# The shader catalog is only valid for the shader compiler it was made with.
$ourCache = (Get-Content -LiteralPath (Join-Path $engine 'out\gpu-cache.id') -Raw).Trim()
$baseCache = (Get-Content -LiteralPath (Join-Path $base 'engine\out\gpu-cache.id') -Raw).Trim()
$keepCatalog = $ourCache -eq $baseCache
if (!$keepCatalog) { Write-Warning "Shader compiler differs from the base ($ourCache vs $baseCache): the shader catalog is left out." }

if (Test-Path -LiteralPath $destinationRoot) {
    $marker = Join-Path $destinationRoot 'package.json'
    if (!(Test-Path -LiteralPath $marker) -or
        ((Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json).engine -notlike 'awaken-seamless-*')) {
        throw "Destination exists and is not an Awaken Seamless package: $destinationRoot"
    }
}
New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
Write-Json @{engine=$engineId; incomplete=$true} (Join-Path $destinationRoot 'package.json')

# 1. The base package, without anything a player's start generated or the release's own identity.
robocopy $base $destinationRoot /E /XF package.json 'Lantern Engine.exe' /XD user logs mods network engine-assets /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the base package failed (robocopy $LASTEXITCODE)." }
if (!$keepCatalog) { Remove-Item -LiteralPath (Join-Path $destinationRoot 'engine\shader-catalog') -Recurse -Force -ErrorAction SilentlyContinue }

# 2. This repo's engine programs; the MSYS2 DLLs they were linked with replace the base's.
$out = Join-Path $destinationRoot 'engine\out'
Get-ChildItem -LiteralPath $out -File | Where-Object { $_.Extension -eq '.dll' -and $_.Name -ne 'nvngx_dlss.dll' } |
    Remove-Item -Force -Confirm:$false
foreach ($file in $programs + 'gpu-cache.id', 'gpu-cache.legacy.id') {
    Copy-Item -LiteralPath (Join-Path $engine "out\$file") -Destination $out -Force
}
$queue = [Collections.Generic.Queue[string]]::new()
foreach ($file in $programs) { $queue.Enqueue((Join-Path $out $file)) }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($queue.Count) {
    $binary = $queue.Dequeue()
    if (!$seen.Add($binary)) { continue }
    $imports = & (Join-Path $native 'llvm-objdump.exe') -p $binary
    if ($LASTEXITCODE) { throw "Cannot inspect $binary" }
    foreach ($line in $imports) {
        if ($line -match '^Export Table:') { break }
        if ($line -notmatch 'DLL Name:\s*(\S+)') { continue }
        $dll = $Matches[1]
        if ($dll -ieq [IO.Path]::GetFileName($binary) -or $dll -eq 'vulkan-1.dll' -or $dll -like 'api-ms-*' -or
            $dll -like 'ext-ms-*' -or (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$dll"))) { continue }
        $target = Join-Path $out $dll
        if (!(Test-Path -LiteralPath $target)) {
            $source = Join-Path $native $dll
            if (!(Test-Path -LiteralPath $source)) { throw "Missing runtime dependency: $dll ($binary)" }
            Copy-Item -LiteralPath $source -Destination $target
        }
        $queue.Enqueue($target)
    }
}
foreach ($folder in 'patches', 'data') {
    Copy-Item -Path (Join-Path $engine "$folder\*") -Destination (Join-Path $destinationRoot "engine\$folder") -Recurse -Force
}
$licenses = Join-Path $destinationRoot 'engine\licenses'
New-Item -ItemType Directory -Force -Path (Join-Path $licenses 'msys2') | Out-Null
Copy-Item -Path (Join-Path $MsysRoot 'clang64\share\licenses\*') -Destination (Join-Path $licenses 'msys2') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $engine 'gpu\third_party\monocypher\LICENCE.md') -Destination (Join-Path $licenses 'Monocypher.md') -Force

# 3. The party save tools and an embedded Python (python.org, signature checked) in seamless\.
$seamless = Join-Path $destinationRoot 'seamless'
$scripts = Join-Path $seamless 'scripts'
New-Item -ItemType Directory -Force -Path $scripts | Out-Null
foreach ($file in 'party_save_tool.py', 'party_session.py', 'party_saves.py', 'party_env.py') {
    Copy-Item -LiteralPath (Join-Path $engine "scripts\$file") -Destination $scripts -Force
}
$pythonVersion = '3.12.10'
$downloads = Join-Path $repo 'work\downloads'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null
$archive = Join-Path $downloads "python-$pythonVersion-embed-amd64.zip"
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip" -OutFile $archive
}
$python = Join-Path $seamless 'python'
New-Item -ItemType Directory -Force -Path $python | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $python -Force
Set-Content -LiteralPath (Join-Path $python 'python312._pth') -Encoding ascii -Value @('python312.zip', '.', '..\scripts')
if ((Get-AuthenticodeSignature -FilePath (Join-Path $python 'python.exe')).Status -ne 'Valid') {
    throw 'The embedded Python signature is not valid.'
}

# 4. The launcher (self-contained); Bloodborne.exe and Bloodborne Launcher.exe are the base's.
$published = Join-Path $repo 'work\published-launcher'
dotnet publish (Join-Path $repo 'launcher\windows\BloodborneLauncher\BloodborneLauncher.csproj') `
    -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true `
    -p:DebugType=none "-p:Version=$Version" -o $published --nologo -v q
if ($LASTEXITCODE) { throw 'Launcher publish failed.' }
Copy-Item -LiteralPath (Join-Path $published 'Lantern Engine.exe') -Destination $destinationRoot -Force
Copy-Item -LiteralPath (Join-Path $engine 'docs\SEAMLESS_TESTING.md') -Destination (Join-Path $destinationRoot 'TESTING.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination (Join-Path $destinationRoot 'README.md') -Force
# Local co-op tests on one PC (README: Test co-op alone on one PC).
foreach ($file in 'Two-Launchers.cmd', 'two-launchers.ps1', 'Play-Two.cmd', 'play-two.ps1') {
    Copy-Item -LiteralPath (Join-Path $repo "tools\$file") -Destination $destinationRoot -Force
}

# 5. First-run launcher settings and the package's identity.
$defaultsFile = Join-Path $destinationRoot 'seamless-defaults.json'
if ($Release) {
    Remove-Item -LiteralPath $defaultsFile -Force -ErrorAction SilentlyContinue
} else {
    $defaults = [ordered]@{ port_dir = $destinationRoot; party_mode = 'host'; party_max = '2'; check_updates = $false; hardware_offered = $true }
    if ($Game) { $defaults.game_dir = $Game }
    Write-Json $defaults $defaultsFile
}
$baseInfo = Get-Content -LiteralPath (Join-Path $base 'package.json') -Raw | ConvertFrom-Json
Write-Json ([ordered]@{
    version = $Version; engine = $engineId; source = (git -C $repo rev-parse HEAD)
    base = "Lantern Engine $($baseInfo.version) ($($baseInfo.engine))"; seamless = 'visy v0.6.9'
    runtime_sha256 = (Get-FileHash -LiteralPath (Join-Path $out 'bb-probe.exe')).Hash.ToLowerInvariant()
}) (Join-Path $destinationRoot 'package.json')
Write-Output "Package assembled: $destinationRoot"
