#!/usr/bin/env python3
"""Packs a curated mod (Nocturne) for the launcher's download and writes its manifest.

    python tools/mod-release.py <mod folder> --version 1.3 --base-url https://<r2 host>/nocturne [--page <url>]
        [--manifest <old manifest.json>] [--out <dir>]

<mod folder> is the folder as it sits in the install's mods dir (it holds dvdroot_ps4/, or app0/, or bare game
folders). Writes <out>/nocturne-<version>.zip (the folder's content at the zip root) and <out>/manifest.json with
this version added on top of the old manifest's versions (pass --manifest to keep older versions downloadable:
a guest joining an older host fetches that one). Upload both; never overwrite an old zip, a new version gets a new
name.

The content hash identifies an install whatever its wrapper (a Nexus download, a hand copy): SHA-256 over the
sorted lines "<path>\\t<sha256 of the file>\\n", where <path> is lower-case, '/'-separated and relative to the
folder that holds dvdroot_ps4 (or to the mod folder for bare game folders), and readme/image/notes files
(.txt .md .jpg .png .ini .toml) and nocturne.json are left out. The launcher computes the same (Core/CuratedMods.cs).
"""
import argparse
import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path

LOOSE = {".txt", ".md", ".jpg", ".png", ".ini", ".toml"}
MARKER = "nocturne.json"


def content_root(folder: Path) -> Path:
    """The folder whose files keep their path: the one holding dvdroot_ps4, else the mod folder."""
    for base in [folder, folder / "app0", *sorted(p for p in folder.iterdir() if p.is_dir())]:
        if base.is_dir() and any(c.name.lower() == "dvdroot_ps4" and c.is_dir() for c in base.iterdir()):
            return base
    return folder


def counted(path: Path) -> bool:
    return path.suffix.lower() not in LOOSE and path.name.lower() != MARKER


def file_sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def content_hash(folder: Path) -> str:
    root = content_root(folder)
    lines = []
    for p in root.rglob("*"):
        if p.is_file() and counted(p):
            lines.append(f"{p.relative_to(root).as_posix().lower()}\t{file_sha(p)}\n")
    if not lines:
        raise SystemExit(f"no mod files under {folder}")
    return hashlib.sha256("".join(sorted(lines)).encode("utf-8")).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder", type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--base-url", required=True, help="public URL of the R2 folder (https), no trailing slash")
    ap.add_argument("--name", default="Nocturne")
    ap.add_argument("--page", default="", help="the mod's page (Nexus)")
    ap.add_argument("--manifest", type=Path, help="the current manifest.json, to keep its older versions")
    ap.add_argument("--out", type=Path, default=Path("."))
    a = ap.parse_args()
    if not a.base_url.startswith("https://"):
        raise SystemExit("--base-url must be https")
    folder = a.folder.resolve()
    key = a.name.lower()
    a.out.mkdir(parents=True, exist_ok=True)
    zip_name = f"{key}-{a.version}.zip"
    zip_path = a.out / zip_name
    if zip_path.exists():
        raise SystemExit(f"{zip_path} exists: a new version needs a new number")

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(folder.rglob("*")):
            if p.is_file() and p.name.lower() != MARKER:
                z.write(p, p.relative_to(folder).as_posix())
    entry = {
        "version": a.version,
        "url": f"{a.base_url.rstrip('/')}/{zip_name}",
        "sha256": file_sha(zip_path),
        "size": zip_path.stat().st_size,
        "content": content_hash(folder),
    }
    old = json.loads(a.manifest.read_text(encoding="utf-8")) if a.manifest else {}
    versions = [v for v in old.get("versions", []) if v.get("version") != a.version]
    manifest = {
        "schema": 1,
        "name": a.name,
        "page": a.page or old.get("page", ""),
        "latest": a.version,
        "versions": [entry] + versions,
    }
    (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{zip_path} ({entry['size'] / 1e6:.1f} MB)\n{a.out / 'manifest.json'}\ncontent {entry['content'][:16]}...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
