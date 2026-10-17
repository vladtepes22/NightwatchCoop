param(
    [string]$MsysRoot = 'C:\msys64',
    [int]$Jobs = 12,
    [string[]]$Targets = @('bloodborne-seamless', 'bb-engine-assets', 'bb-gpu-capabilities')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$engine = Join-Path $repo 'engine'
$toolchain = Join-Path $MsysRoot 'clang64\bin'
$cmake = Join-Path $toolchain 'cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CLANG64 CMake missing: $cmake" }
$env:PATH = "$toolchain;$env:PATH"
$env:PKG_CONFIG_PATH = Join-Path $MsysRoot 'clang64\lib\pkgconfig'
$env:CC = Join-Path $toolchain 'clang.exe'
$env:CXX = Join-Path $toolchain 'clang++.exe'
Push-Location $repo
try {
    git submodule update --init --recursive
    if ($LASTEXITCODE) { throw 'Submodule initialization failed' }
    $fsr = Join-Path $engine 'gpu\third_party\fsr-vulkan'
    foreach ($patch in Get-ChildItem -LiteralPath (Join-Path $engine 'gpu\patches\fsr-vulkan') -Filter '*.patch') {
        git -C $fsr apply --reverse --check $patch.FullName 2>$null
        if ($LASTEXITCODE) {
            git -C $fsr apply $patch.FullName
            if ($LASTEXITCODE) { throw "FSR patch failed: $($patch.Name)" }
        }
    }
    $build = Join-Path $engine 'out\gpu'
    & $cmake -S (Join-Path $engine 'gpu') -B $build -G Ninja `
        "-DCMAKE_C_COMPILER=$env:CC" "-DCMAKE_CXX_COMPILER=$env:CXX" `
        "-DCMAKE_MAKE_PROGRAM=$(Join-Path $toolchain 'ninja.exe')" `
        "-DCMAKE_PREFIX_PATH=$(Join-Path $MsysRoot 'clang64')" `
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_LTO=ON -DBB_PGO=off -DBB_NATIVE_RUNTIME=ON `
        '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -gline-tables-only -DNDEBUG' `
        '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -gline-tables-only -DNDEBUG'
    if ($LASTEXITCODE) { throw 'Engine configuration failed' }
    & $cmake --build $build --parallel $Jobs --target @Targets
    if ($LASTEXITCODE) { throw 'Engine build failed' }
} finally { Pop-Location }
