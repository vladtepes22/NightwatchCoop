# Assembles a playable Nightwatch Co-op package in the Lantern Engine 0.9.34 layout:
#   <package>\Nightwatch Co-op.exe, Bloodborne.exe, bbport.ini, package.json (since beta 9.2 no
#             Lantern Engine.exe forwarder: beta 9.2 is a fresh download for everyone)
#   <package>\engine\out      this repo's bb-start, bb-probe, bb-shader-lab, ... and their DLLs
#   <package>\engine\patches  this repo's patches (with the Party: patches)
#   <package>\map-generator   the in-game map's generator (tools\map\build-generator.ps1, -MapGenerator)
#   <package>\seamless        the party save tools and their own Python (the launcher removes
#                             engine\scripts and engine\tools once the engine starts natively)
# Shaders, upscaler assets and the shader catalog come from the official Lantern Engine release
# this build is based on (-Base: its extracted BloodbornePC folder); the small start programs are
# built here (launcher\windows\BloodbornePlay).
#   tools\package-seamless.ps1 -Base work\downloads\lantern-0.9.37-beta.7\x\BloodbornePC
param(
    [Parameter(Mandatory)][string]$Base,
    [string]$Destination = '',
    [string]$Game = '',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Version = '',
    # tools\map\build-generator.ps1's output: the launcher makes the in-game map with it. Required
    # for a release; without it the package has no map.
    [string]$MapGenerator = '',
    [string]$RandomizerBuild = '',
    # A package to publish: no first-run settings with this PC's folders (the launcher finds its own).
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
# The release version: the repo's VERSION file unless -Version is given.
if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\VERSION') -Raw).Trim() }
# JSON without a byte-order mark (Windows PowerShell's -Encoding utf8 writes one).
function Write-Json($value, $path) { [IO.File]::WriteAllText($path, ($value | ConvertTo-Json), [Text.UTF8Encoding]::new($false)) }
$engineId = "nightwatch-coop-$Version"
$repo = Split-Path $PSScriptRoot -Parent
$engine = Join-Path $repo 'engine'
$native = Join-Path $MsysRoot 'clang64\bin'
$base = (Resolve-Path -LiteralPath $Base).Path
if (!$Destination) { $Destination = Join-Path $repo 'work\package\NightwatchCoop' }
$destinationRoot = [IO.Path]::GetFullPath($Destination)

$programs = 'bb-start.exe', 'bb-probe.exe', 'bb-shader-lab.exe', 'bb-engine-assets.exe', 'bb-gpu-capabilities.exe', 'bb-map-bake.exe'
if ($MapGenerator -and !(Test-Path -LiteralPath (Join-Path $MapGenerator 'generate_atlas_lite.exe'))) { throw "Not a built map generator: $MapGenerator" }
if ($Release -and !$MapGenerator) { throw "A release needs the map generator: tools\map\build-generator.ps1, then -MapGenerator." }
if ($MapGenerator) {
    # The frozen exe bundles its sources and the engine's game check: one built before they last
    # changed still has the old rules (beta 2 shipped one that refused other dumps of 1.09).
    $repoRoot = Split-Path $PSScriptRoot -Parent
    $generatorBuilt = (Get-Item -LiteralPath (Join-Path $MapGenerator 'generate_atlas_lite.exe')).LastWriteTimeUtc
    $generatorSources = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'tools\map\generator\src') -Recurse -File -Filter *.py) +
        (Get-Item (Join-Path $repoRoot 'tools\map\generator\generate_atlas.spec')) +
        (Get-Item (Join-Path $repoRoot 'engine\scripts\game_check.py')) +
        (Get-Item (Join-Path $repoRoot 'engine\scripts\prepare.py')) +
        (Get-Item (Join-Path $repoRoot 'engine\scripts\image_109_hashes.txt'))
    $stale = $generatorSources | Where-Object { $_.LastWriteTimeUtc -gt $generatorBuilt } | Select-Object -First 1
    if ($stale) { throw "The map generator at $MapGenerator is older than $($stale.Name): rebuild it with tools\map\build-generator.ps1." }
}
foreach ($file in $programs + 'gpu-cache.id', 'gpu-cache.legacy.id') {
    if (!(Test-Path -LiteralPath (Join-Path $engine "out\$file"))) { throw "Build the engine first (engine\build.sh): out\$file is missing." }
}
if (!(Test-Path -LiteralPath (Join-Path $base 'engine\out\bb-start.exe'))) { throw "Not a Lantern Engine 0.9.32+ package: $base" }
# The shader catalog holds shader states without their code, their specializations and pipeline keys:
# the start adds the game's own shader code back and compiles each one with this engine's compiler
# (bb-shader-lab --seed-cache), leaving out any whose specialization this compiler changed. So it does
# not depend on the compiler it was made with (gpu-cache.id), only on this engine reading its formats:
# bb-shader-lab --check-catalog checks each catalog folder (a GPU profile, states and specializations).
$keepCatalog = $true
$catalogs = @(Get-ChildItem -LiteralPath (Join-Path $base 'engine\shader-catalog') -Recurse -Filter profile.bin -File -ErrorAction SilentlyContinue)
$savedPath = $env:PATH
$env:PATH = "$native;$env:PATH"
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue' # the lab's GPU log on stderr is not an error (Windows PowerShell 5.1)
try {
    foreach ($catalogProfile in $catalogs) {
        $check = & (Join-Path $engine 'out\bb-shader-lab.exe') --check-catalog $catalogProfile.DirectoryName 2>$null | Where-Object { $_ -like 'Catalog check:*' }
        Write-Host "Shader catalog $($catalogProfile.Directory.Parent.Name)\$($catalogProfile.Directory.Name): $check"
        if ($LASTEXITCODE -ne 0) { $keepCatalog = $false }
    }
} finally { $env:PATH = $savedPath; $ErrorActionPreference = $savedPreference }
if (!$catalogs) { $keepCatalog = $false }
if (!$keepCatalog) { Write-Warning "The base's shader catalog is missing or not readable by this engine: the shader catalog is left out." }

if (Test-Path -LiteralPath $destinationRoot) {
    $marker = Join-Path $destinationRoot 'package.json'
    if (!(Test-Path -LiteralPath $marker) -or
        ((Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json).engine -notlike 'nightwatch-coop-*')) {
        throw "Destination exists and is not a Nightwatch Co-op package: $destinationRoot"
    }
}
New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
Write-Json @{engine=$engineId; incomplete=$true} (Join-Path $destinationRoot 'package.json')

# 1. The base package, without anything a player's start generated or the release's own identity.
robocopy $base $destinationRoot /E /XF package.json 'Lantern Engine.exe' 'Bloodborne Launcher.exe' Bloodborne.exe lantern.default.ini /XD user logs mods network engine-assets /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the base package failed (robocopy $LASTEXITCODE)." }
# Since Lantern 0.9.37 beta 2 the base ships its settings as engine\lantern.default.ini: they are
# this package's bbport.ini (the name the engine and launcher read; an update never replaces it).
$shippedSettings = Join-Path $base 'engine\lantern.default.ini'
if (Test-Path -LiteralPath $shippedSettings) {
    Copy-Item -LiteralPath $shippedSettings -Destination (Join-Path $destinationRoot 'bbport.ini') -Force
    # The Dream mirror and Rebirth at the Altar of Despair are on by default here (settings_rev 3).
    $ini = Join-Path $destinationRoot 'bbport.ini'
    $text = [IO.File]::ReadAllText($ini) -replace '(?m)^change_appearance=0', 'change_appearance=1' -replace '(?m)^rebirth=0', 'rebirth=1'
    [IO.File]::WriteAllText($ini, $text)
}
if (!$keepCatalog) { Remove-Item -LiteralPath (Join-Path $destinationRoot 'engine\shader-catalog') -Recurse -Force -ErrorAction SilentlyContinue }

# 2. This repo's engine programs; the MSYS2 DLLs they were linked with replace the base's.
$out = Join-Path $destinationRoot 'engine\out'
# NVIDIA's DLLs stay: DLSS (nvngx_dlss), DLSS frame generation (nvngx_dlssg + Streamline's sl.*) and Reflex.
Get-ChildItem -LiteralPath $out -File | Where-Object {
    $_.Extension -eq '.dll' -and $_.Name -notlike 'nvngx_*.dll' -and $_.Name -notlike 'sl.*.dll' -and $_.Name -ne 'NvLowLatencyVk.dll'
} | Remove-Item -Force -Confirm:$false
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
# Legacy CPU: bb-start reads the translation plan from engine\patches (start/cpu_compat.cpp).
Copy-Item -LiteralPath (Join-Path $engine 'tools\legacy-cpu-plan.json') -Destination (Join-Path $destinationRoot 'engine\patches') -Force
# Online play's first-time setup: the official OpenVPN installer; name and hash are pinned in the launcher.
$vpnSetup = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'launcher\windows\BloodborneLauncher\Core\OpenVpnSetup.cs')
$vpnMsi = [regex]::Match($vpnSetup, 'MsiName = "([^"]+)"').Groups[1].Value
$vpnHash = [regex]::Match($vpnSetup, 'MsiSha256 = "([^"]+)"').Groups[1].Value
$vpnSource = Join-Path $repoRoot "work\downloads\openvpn\$vpnMsi"
if ((Test-Path -LiteralPath $vpnSource) -and (Get-FileHash -LiteralPath $vpnSource -Algorithm SHA256).Hash -eq $vpnHash) {
    New-Item -ItemType Directory -Force -Path (Join-Path $destinationRoot 'openvpn') | Out-Null
    Copy-Item -LiteralPath $vpnSource -Destination (Join-Path $destinationRoot "openvpn\$vpnMsi")
} elseif ($Release) { throw "A release needs the OpenVPN installer with the pinned hash: work\downloads\openvpn\$vpnMsi" }
$licenses = Join-Path $destinationRoot 'engine\licenses'
New-Item -ItemType Directory -Force -Path (Join-Path $licenses 'msys2') | Out-Null
Copy-Item -Path (Join-Path $MsysRoot 'clang64\share\licenses\*') -Destination (Join-Path $licenses 'msys2') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $engine 'gpu\third_party\monocypher\LICENCE.md') -Destination (Join-Path $licenses 'Monocypher.md') -Force
# AMD's FSR SDK upscaler (Radeon cards) goes to engine\fsr_sdk, where the engine looks. engine/tools/fetch_fsr_sdk.sh
# fills engine\fsr_sdk (pinned SHA-256); AMD's license, which binary redistribution requires, travels with the DLLs.
$fsrSdk = Join-Path $engine 'fsr_sdk'
if (Test-Path -LiteralPath (Join-Path $fsrSdk 'amd_fidelityfx_upscaler_vk.dll')) {
    $fsrTarget = Join-Path $destinationRoot 'engine\fsr_sdk'
    New-Item -ItemType Directory -Force -Path $fsrTarget | Out-Null
    foreach ($file in 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll', 'LICENSE-AMD-FSR-SDK.md', 'SOURCE.txt') {
        Copy-Item -LiteralPath (Join-Path $fsrSdk $file) -Destination $fsrTarget -Force
    }
} elseif ($Release) { throw 'A release needs the AMD FSR SDK files: run engine/tools/fetch_fsr_sdk.sh' }
else { Write-Warning 'engine\fsr_sdk is missing (engine/tools/fetch_fsr_sdk.sh): FSR stays on FSR 3.1 in this package.' }

# 3. The party save tools and an embedded Python (python.org, signature checked) in seamless\.
$seamless = Join-Path $destinationRoot 'seamless'
$scripts = Join-Path $seamless 'scripts'
New-Item -ItemType Directory -Force -Path $scripts | Out-Null
foreach ($file in 'party_save_tool.py', 'party_session.py', 'party_saves.py', 'party_env.py', 'save_editor.py', 'saveedit_cli.py', 'mods.py', 'party_offline.py', 'party_offline_tool.py', 'net_doctor.py') {
    Copy-Item -LiteralPath (Join-Path $engine "scripts\$file") -Destination $scripts -Force
}
# The launcher's Save editor window runs saveedit_cli.py over the saveedit library (not its Tk window, ui.py).
$saveedit = Join-Path $scripts 'saveedit'
New-Item -ItemType Directory -Force -Path $saveedit | Out-Null
Get-ChildItem -LiteralPath (Join-Path $engine 'scripts\saveedit') -File | Where-Object { $_.Extension -in '.py', '.json' -and $_.Name -ne 'ui.py' } |
    Copy-Item -Destination $saveedit -Force
# saveedit\flagsdata.py derives the boss and lamp flag lists from the player's game through scripts\partydata
# (flag_tool, ids_tool); without them the Progress page lists no bosses or lamps.
$partydata = Join-Path $scripts 'partydata'
New-Item -ItemType Directory -Force -Path $partydata | Out-Null
foreach ($file in 'flag_tool.py', 'ids_tool.py') {
    Copy-Item -LiteralPath (Join-Path $engine "scripts\partydata\$file") -Destination $partydata -Force
}
# The connection doctor's launcher: next to scripts\ and python\ (it finds both relative to itself).
Copy-Item -LiteralPath (Join-Path $engine 'net_doctor.bat') -Destination $seamless -Force
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

# 4. The launcher (self-contained) and Bloodborne.exe (starts the game straight away).
# A release needs the Nightwatch VPN's host and pin (BloodborneLauncher.csproj); they are not in the repo.
if ($Release -and -not (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.nightwatch-vpn.props'))) {
    throw 'No %USERPROFILE%\.nightwatch-vpn.props: the launcher would ship with the Nightwatch VPN hidden.'
}
$published = Join-Path $repo 'work\published-launcher'
dotnet publish (Join-Path $repo 'launcher\windows\BloodborneLauncher\BloodborneLauncher.csproj') `
    -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true `
    -p:DebugType=none "-p:Version=$Version" -o $published --nologo -v q
if ($LASTEXITCODE) { throw 'Launcher publish failed.' }
Copy-Item -LiteralPath (Join-Path $published 'Nightwatch Co-op.exe') -Destination $destinationRoot -Force
$helpers = Join-Path $repo 'work\published-helpers'
New-Item -ItemType Directory -Force -Path $helpers | Out-Null
$path = $env:PATH
$env:PATH = "$native;$path"
try {
    foreach ($helper in @(@{ Dir = 'BloodbornePlay'; Rc = 'play.rc'; Exe = 'Bloodborne.exe' })) {
        $source = Join-Path $repo "launcher\windows\$($helper.Dir)"
        $res = Join-Path $helpers "$($helper.Dir).res"
        & (Join-Path $native 'windres.exe') -I $source (Join-Path $source $helper.Rc) -O coff -o $res
        if ($LASTEXITCODE) { throw "$($helper.Rc): windres failed." }
        & (Join-Path $native 'clang.exe') -O2 -municode -mwindows -o (Join-Path $helpers $helper.Exe) (Join-Path $source 'main.c') $res
        if ($LASTEXITCODE) { throw "$($helper.Exe): build failed." }
        Copy-Item -LiteralPath (Join-Path $helpers $helper.Exe) -Destination $destinationRoot -Force
    }
} finally {
    $env:PATH = $path
}
# The game window's icon until the launcher writes the chosen one (AppIcon.SyncGameWindow).
Copy-Item -LiteralPath (Join-Path $repo 'launcher\windows\BloodborneLauncher\app.ico') -Destination (Join-Path $destinationRoot 'engine\out\bloodborne.ico') -Force
Copy-Item -LiteralPath (Join-Path $repo 'README.md') -Destination (Join-Path $destinationRoot 'README.md') -Force
# The licence and Lantern Engine's NOTICE (its GPL-3.0 section 7 terms: the attribution ships with every copy).
foreach ($file in 'LICENSE', 'NOTICE') {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination (Join-Path $destinationRoot $file) -Force
}
# The in-game map's generator (run by the launcher on the player's PC; no maps are shipped).
$mapTarget = Join-Path $destinationRoot 'map-generator'
Remove-Item -LiteralPath $mapTarget -Recurse -Force -ErrorAction SilentlyContinue
if ($MapGenerator) { Copy-Item -Path $MapGenerator -Destination $mapTarget -Recurse }
# Local co-op tests on one PC: test packages only; a release's folder holds what players start.
if (!$Release) {
    Copy-Item -LiteralPath (Join-Path $engine 'docs\SEAMLESS_TESTING.md') -Destination (Join-Path $destinationRoot 'TESTING.md') -Force
    foreach ($file in 'Two-Launchers.cmd', 'two-launchers.ps1', 'Play-Two.cmd', 'play-two.ps1') {
        Copy-Item -LiteralPath (Join-Path $repo "tools\$file") -Destination $destinationRoot -Force
    }
}

# 5. First-run launcher settings and the package's identity.
$defaultsFile = Join-Path $destinationRoot 'seamless-defaults.json'
if ($Release) {
    Remove-Item -LiteralPath $defaultsFile -Force -ErrorAction SilentlyContinue
} else {
    $defaults = [ordered]@{ port_dir = $destinationRoot; party_mode = 'host'; party_max = '2'; hardware_offered = $true }
    if ($Game) { $defaults.game_dir = $Game }
    Write-Json $defaults $defaultsFile
}
$baseInfo = Get-Content -LiteralPath (Join-Path $base 'package.json') -Raw | ConvertFrom-Json
Write-Json ([ordered]@{
    version = $Version; engine = $engineId; source = (git -C $repo rev-parse HEAD)
    seamless = 'visy v0.7.9'
    runtime_sha256 = (Get-FileHash -LiteralPath (Join-Path $out 'bb-probe.exe')).Hash.ToLowerInvariant()
}) (Join-Path $destinationRoot 'package.json')
# Private tools ship compiled: no Python in their folder, and no copy of their sources anywhere in the package.
$privateFolder = Join-Path $destinationRoot 'randomizer'
if ($RandomizerBuild) {
    if (!(Test-Path -LiteralPath (Join-Path $RandomizerBuild 'randomizer.exe'))) { throw "No randomizer.exe in $RandomizerBuild" }
    Copy-Item -LiteralPath $RandomizerBuild -Destination $privateFolder -Recurse -Force
}
$leaks = @()
if (Test-Path -LiteralPath $privateFolder) {
    $leaks += Get-ChildItem -LiteralPath $privateFolder -Recurse -File | Where-Object { $_.Extension -in '.py', '.pyc', '.pdb', '.map' }
}
$privateSources = Join-Path $repo 'tools\randomizer'
if (Test-Path -LiteralPath $privateSources) {
    # Its scratch-archive-* folders hold one-off session scripts (none shipped); their names may match public files.
    $names = @(Get-ChildItem -LiteralPath $privateSources -Recurse -File -Filter *.py |
        Where-Object { $_.FullName.Substring($privateSources.Length) -notmatch '\\scratch-archive-' } | ForEach-Object { $_.BaseName })
    $leaks += Get-ChildItem -LiteralPath $destinationRoot -Recurse -File | Where-Object { $_.Extension -in '.py', '.pyc', '.pyd' -and $names -contains $_.BaseName }
}
if ($leaks) {
    Remove-Item -LiteralPath (Join-Path $destinationRoot 'package.json') -Force
    throw "Private source in the package: $(($leaks | Select-Object -First 5).FullName -join ', ')"
}
Write-Output "Package assembled: $destinationRoot"
