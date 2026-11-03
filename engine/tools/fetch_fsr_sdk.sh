#!/usr/bin/env bash
# Downloads AMD's FidelityFX SDK (FSR) Vulkan upscaler for Radeon cards: the signed loader and
# upscaler DLLs from github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag v2.4.0), under AMD's
# license (docs/license.md there: binary redistribution, no reverse engineering); they are not
# part of this repository. bbport loads them from <exe>/../fsr_sdk (vk_fsr_sdk.cpp) or
# BB_FSR_SDK_DIR, so the default output is engine/fsr_sdk. Every file is checked against the
# pinned SHA-256 below. Usage: fetch_fsr_sdk.sh [output dir]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
tag=v2.4.0
repo="GPUOpen-LibrariesAndSDKs/FidelityFX-SDK"
base="https://raw.githubusercontent.com/$repo/$tag"
bin="Kits/FidelityFX/signedbin"
loader_sha=38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9
upscaler_sha=4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
license_sha=410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe
dest=${1:-fsr_sdk}
mkdir -p "$dest"
sha() { sha256sum -- "$1" | cut -d' ' -f1; }
fetch() { # <url> <name> <sha256>
    local name=$2
    if [[ -s $dest/$name && $(sha "$dest/$name") == "$3" ]]; then echo "FSR SDK: $dest/$name present"; return; fi
    curl -fsSL --retry 3 -o "$dest/$name.part" "$1"
    if [[ $(sha "$dest/$name.part") != "$3" ]]; then
        rm -f "$dest/$name.part"
        echo "FSR SDK: $name does not match the pinned SHA-256" >&2
        exit 1
    fi
    mv "$dest/$name.part" "$dest/$name"
    echo "FSR SDK: downloaded $dest/$name"
}
fetch "$base/$bin/amd_fidelityfx_loader_vk.dll" amd_fidelityfx_loader_vk.dll "$loader_sha"
fetch "$base/$bin/amd_fidelityfx_upscaler_vk.dll" amd_fidelityfx_upscaler_vk.dll "$upscaler_sha"
fetch "$base/docs/license.md" LICENSE-AMD-FSR-SDK.md "$license_sha"
cat > "$dest/SOURCE.txt" <<TXT
AMD FidelityFX SDK (FSR) $tag, Vulkan upscaler, unmodified signed binaries.
Source: https://github.com/$repo/tree/$tag/$bin
License: LICENSE-AMD-FSR-SDK.md (https://github.com/$repo/blob/$tag/docs/license.md)
amd_fidelityfx_loader_vk.dll   SHA-256 $loader_sha
amd_fidelityfx_upscaler_vk.dll SHA-256 $upscaler_sha
TXT
