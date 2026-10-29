"""Thin entry point around the unchanged Atlas generator source."""

from __future__ import annotations

import ctypes
import json
import sys
import tempfile
from pathlib import Path

if __name__ == "__main__":
    # The frozen exe is also parallel_maps.py's worker processes: they run the pool's task, not main().
    import multiprocessing

    multiprocessing.freeze_support()


def _loaded_scientific_modules() -> list[str]:
    """Return loaded native-module paths from this process (Windows only)."""
    if sys.platform != "win32":
        raise RuntimeError("--self-test-imports is a Windows build diagnostic")
    from ctypes import wintypes

    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    enum_modules = psapi.EnumProcessModules
    enum_modules.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.HMODULE),
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
    ]
    enum_modules.restype = wintypes.BOOL
    module_name = psapi.GetModuleFileNameExW
    module_name.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    module_name.restype = wintypes.DWORD
    process = kernel32.GetCurrentProcess()
    modules = (wintypes.HMODULE * 4096)()
    needed = wintypes.DWORD()
    if not enum_modules(process, modules, ctypes.sizeof(modules), ctypes.byref(needed)):
        raise ctypes.WinError(ctypes.get_last_error())
    count = min(needed.value // ctypes.sizeof(wintypes.HMODULE), len(modules))
    paths: list[str] = []
    for handle in modules[:count]:
        buffer = ctypes.create_unicode_buffer(32768)
        if module_name(process, handle, buffer, len(buffer)):
            value = buffer.value
            lowered = Path(value).name.casefold()
            if any(token in lowered for token in ("openblas", "geos", "_ckdtree", "_multiarray", "_ufunc", "_imaging")):
                paths.append(value)
    return sorted(set(paths), key=str.casefold)


def _self_test_imports() -> int:
    # Mirror the explicit imports performed in generate_atlas.main(), without
    # requiring a game folder or invoking any reader/generation function.
    import pipeline_context as ctx

    with tempfile.TemporaryDirectory(prefix="atlas-lite-import-check-") as temporary:
        root = Path(temporary)
        ctx.configure(root / "absent-game-input", root / "stage")
        import soulstruct_local_config

        sys.modules["soulstruct.config"] = soulstruct_local_config
        import generate_navigation_maps
        import probe_markers
        import update_verified_markers
        import generate_floorplans_v12
        import probe_gates
        import extract_verified_gates
        import probe_location_names
        import extract_subarea_names
        import generate_map_v13
        import extract_native_map_font
        import extract_atlas_features
        import game_identity
        # The engine's game check reads the 1.09 chunk hashes beside it (patched eboots).
        if not Path(game_identity.game_check.__file__).with_name("image_109_hashes.txt").is_file():
            raise RuntimeError("image_109_hashes.txt is not bundled beside game_check")
        import numpy
        from PIL import Image, ImageDraw
        import scipy
        import scipy.spatial
        import scipy.spatial.transform  # compiled _rigid_transform since scipy 1.16 (NW-1883505)
        import shapely

        probe = Image.new("1", (8, 8))
        ImageDraw.Draw(probe).polygon([(1, 1), (6, 1), (4, 6)], fill=1)
        if int(numpy.asarray(probe, dtype=numpy.uint8).sum()) == 0:
            raise RuntimeError("Pillow polygon rasterization self-test failed")

        module_files = {
            name: getattr(sys.modules.get(name), "__file__", None)
            for name in (
                "numpy",
                "scipy",
                "scipy.spatial",
                "scipy.spatial._ckdtree",
                "shapely",
                "generate_map_v13",
                "generate_floorplans_v12",
            )
        }
        print(
            json.dumps(
                {
                    "self_test_imports": "pass",
                    "module_files": module_files,
                "loaded_optional_modules": sorted(
                    name
                    for name in sys.modules
                    if name.startswith(("lxml", "zstandard", "fontTools"))
                ),
                    "loaded_native_modules": _loaded_scientific_modules(),
                },
                sort_keys=True,
            ),
            flush=True,
        )
    return 0


if "--self-test-imports" in sys.argv[1:]:
    raise SystemExit(_self_test_imports())

from generate_atlas import main

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        import traceback

        print(json.dumps({"stage": "error", "message": str(error)}), flush=True)
        traceback.print_exc(file=sys.stderr)
        raise SystemExit(1)
