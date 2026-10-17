param(
    [string]$Destination = 'E:\games\bloodborne seamless\AwakenSeamless',
    [string]$Game = 'E:\games\bloodborne seamless\gamedir\CUSA03173',
    [string]$RuntimeAssets = 'E:\games\bloodborne seamless\BloodbornePC\engine',
    [string]$MsysRoot = 'C:\msys64',
    [string]$Version = '0.1.0-beta.4'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$engine = Join-Path $repo 'engine'
$destinationRoot = [IO.Path]::GetFullPath($Destination)
$native = Join-Path $MsysRoot 'clang64\bin'
if (!(Test-Path -LiteralPath (Join-Path $engine 'out\bb-probe.exe'))) { throw 'Build the native engine first.' }
if (!(Test-Path -LiteralPath (Join-Path $Game 'eboot.bin'))) { throw 'Game dump not found.' }
if (Test-Path -LiteralPath $destinationRoot) {
    $marker = Join-Path $destinationRoot 'package.json'
    if (!(Test-Path -LiteralPath $marker) -or
        ((Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json).engine -notlike 'awaken-seamless-*')) {
        throw "Destination is not an existing seamless package: $destinationRoot"
    }
}
New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
@{engine='awaken-seamless-0.1.0';incomplete=$true} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $destinationRoot 'package.json') -Encoding utf8NoBOM
$out = Join-Path $destinationRoot 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($file in 'bb-probe.exe','bb-engine-assets.exe','bb-gpu-capabilities.exe') {
    Copy-Item -LiteralPath (Join-Path $engine "out\$file") -Destination $out -Force
}
foreach ($folder in 'scripts','patches','docs') {
    Copy-Item -LiteralPath (Join-Path $engine $folder) -Destination $destinationRoot -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $engine 'run.bat') -Destination $destinationRoot -Force
Copy-Item -LiteralPath (Join-Path $engine 'LICENSE') -Destination $destinationRoot -Force
Copy-Item -LiteralPath (Join-Path $engine 'docs\SEAMLESS_TESTING.md') -Destination (Join-Path $destinationRoot 'TESTING.md') -Force
$sourceLicenses = Join-Path $destinationRoot 'licenses\source'
New-Item -ItemType Directory -Force -Path $sourceLicenses | Out-Null
Copy-Item -LiteralPath (Join-Path $engine 'gpu\third_party\monocypher\LICENCE.md') -Destination (Join-Path $sourceLicenses 'Monocypher.md') -Force
Copy-Item -LiteralPath (Join-Path $engine 'licenses\GPL-2.0.txt') -Destination $sourceLicenses -Force
foreach ($folder in 'fsr4_shaders','fsr4vk','licenses') {
    $source = Join-Path $RuntimeAssets $folder
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $destinationRoot -Recurse -Force }
}
foreach ($file in 'out\nvngx_dlss.dll','gamecontrollerdb.txt') {
    $source = Join-Path $RuntimeAssets $file
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $destinationRoot $file) -Force }
}

# Walk imports from the built binaries. Windows and the display driver's Vulkan loader stay system-owned.
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $out -File | Where-Object Extension -in '.exe','.dll' | ForEach-Object { $queue.Enqueue($_.FullName) }
$provider = Join-Path $destinationRoot 'fsr4vk\amd_fidelityfx_upscaler_vk.dll'
if (Test-Path -LiteralPath $provider) { $queue.Enqueue($provider) }
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
        if ($dll -ieq [IO.Path]::GetFileName($binary)) { continue }
        if ($dll -eq 'vulkan-1.dll' -or $dll -like 'api-ms-*' -or $dll -like 'ext-ms-*' -or
            (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$dll"))) { continue }
        $target = Join-Path $out $dll
        if (!(Test-Path -LiteralPath $target)) {
            $source = Join-Path $native $dll
            if (!(Test-Path -LiteralPath $source)) { throw "Missing runtime dependency: $dll ($binary)" }
            Copy-Item -LiteralPath $source -Destination $target
        }
        $queue.Enqueue($target)
    }
}
$licenseDestination = Join-Path $destinationRoot 'licenses\msys2'
New-Item -ItemType Directory -Force -Path $licenseDestination | Out-Null
Copy-Item -Path (Join-Path $MsysRoot 'clang64\share\licenses\*') -Destination $licenseDestination -Recurse -Force

$pythonVersion = '3.12.10'
$archive = Join-Path $repo "work\python-$pythonVersion-embed-amd64.zip"
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip" -OutFile $archive
}
$pythonDestination = Join-Path $destinationRoot 'tools\python'
New-Item -ItemType Directory -Force -Path $pythonDestination | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $pythonDestination -Force
Set-Content -LiteralPath (Join-Path $pythonDestination 'python312._pth') -Encoding ascii -Value @('python312.zip','.','..\..\scripts')
$signature = Get-AuthenticodeSignature -FilePath (Join-Path $pythonDestination 'python.exe')
if ($signature.Status -ne 'Valid') { throw 'The embedded Python signature is not valid.' }

dotnet publish (Join-Path $repo 'launcher\windows\BloodborneLauncher\BloodborneLauncher.csproj') `
    -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true `
    -p:DebugType=none "-p:Version=$Version" -o (Join-Path $repo 'work\published-launcher') --nologo
if ($LASTEXITCODE) { throw 'Launcher publish failed.' }
Copy-Item -LiteralPath (Join-Path $repo 'work\published-launcher\Bloodborne Launcher.exe') -Destination $destinationRoot -Force
$artifacts = 'D:\project-artifacts\BloodborneSeamless\user-tests'
New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
$defaults = @{
    game_dir=$Game; port_dir=$destinationRoot; party_mode='host'; party_max='2';
    check_updates=$false; artifact_dir=$artifacts; hardware_offered=$true
}
$defaults | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationRoot 'seamless-defaults.json') -Encoding utf8NoBOM
$metadata = @{
    version=$Version; engine='awaken-seamless-0.1.0';
    source=(git -C $repo rev-parse HEAD); runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $out 'bb-probe.exe')).Hash.ToLowerInvariant();
    source_url='https://github.com/Artificial-Sweetener/BloodBorneButPc/tree/seamless-integration';
    base='Awakenchan f3dcca13da2c2c81d2b9904dd549277e876e19e6'; seamless='visy ae113015554722e7e150e93c30060784694136ab'
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationRoot 'package.json') -Encoding utf8NoBOM
$ini = Join-Path $destinationRoot 'bbport.ini'
if (!(Test-Path -LiteralPath $ini)) {
    Set-Content -LiteralPath $ini -Encoding ascii -Value @('upscaler=fsr3','preset=0','output_res=1920x1080','live_resolution=auto','fullscreen=0','show_fps=1','party_hud=1','party_name_tags=1','party_toasts=1')
}
Write-Output "Package assembled: $destinationRoot"
