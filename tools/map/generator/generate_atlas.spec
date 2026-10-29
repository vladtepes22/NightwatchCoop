# Atlas Lite freeze; source algorithms remain the original generator scripts.
from pathlib import Path
from PyInstaller.utils.hooks import collect_all, collect_data_files, collect_submodules, copy_metadata

root = Path(SPECPATH)
# The engine's game check (game_identity.py): game_check.py and prepare.py, and beside them the
# 1.09 image's chunk hashes it reads (patched 1.09 eboots).
engine_scripts = root.parents[2] / "engine" / "scripts"
datas = [(str(engine_scripts / "image_109_hashes.txt"), ".")]
datas += collect_data_files("soulstruct", excludes=["**/*.dll", "**/*.exe", "**/*.pyi"])
datas += copy_metadata("soulstruct")
datas += copy_metadata("constrata")
binaries = []
hiddenimports = []
hiddenimports += ["scipy.linalg", "scipy.sparse"]
# scipy.spatial (generate_atlas_lite.py) loads compiled submodules of scipy.transform since scipy 1.16
# (_rigid_transform); a frozen build without them fails with "No module named ..." (NW-1883505).
hiddenimports += collect_submodules("scipy.spatial")

# zstandard backs Soulstruct's DCX reader; keep it until end-to-end extraction
# shows that the format path is not reached for the supported game inputs.
zstd_datas, zstd_binaries, zstd_hiddenimports = collect_all("zstandard")
datas += zstd_datas
binaries += zstd_binaries
hiddenimports += zstd_hiddenimports

a = Analysis(
    [str(root / "src" / "generate_atlas_lite.py")],
    pathex=[str(root / "src"), str(engine_scripts)],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        "matplotlib", "pandas", "torch", "IPython", "tkinter", "soulstruct.config",
        "scipy.fft", "scipy.integrate", "scipy.interpolate",
        "scipy.ndimage", "scipy.optimize", "scipy.stats",
    ],
    noarchive=False,
    optimize=0,
)

# PyInstaller's scipy hook places each delvewheel DLL both at package root and
# under the owning package's DLL directory. The self-test has shown that NumPy
# loads only numpy.libs and SciPy loads only scipy.libs. Retain those namespaced
# copies and discard only the never-loaded root duplicates.
duplicate_openblas_names = {
    "libscipy_openblas64_-ed4f167a5330424524f45258e7ca2c8d.dll",
    "libscipy_openblas-48c358d105077551cc9cc3ba79387ed5.dll",
}
unused_pillow_binary_names = {
    "_avif.cp312-win_amd64.pyd",
    "_webp.cp312-win_amd64.pyd",
    "_imagingcms.cp312-win_amd64.pyd",
    "_imagingmath.cp312-win_amd64.pyd",
    "_imagingtk.cp312-win_amd64.pyd",
}
a.binaries = [
    item
    for item in a.binaries
    if not (
        Path(item[0].replace("\\", "/")).parent == Path(".")
        and Path(item[0]).name in duplicate_openblas_names
    )
    and not (
        Path(item[0].replace("\\", "/")).parent == Path("PIL")
        and Path(item[0]).name in unused_pillow_binary_names
    )
]
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="generate_atlas_lite",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name="generate_atlas_lite",
)
