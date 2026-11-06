"""The engine's C++ table derivation (gpu/shim/gamedata, built as party-tables-cli) against the Python oracle
(scripts/partydata/oracle.py), on the player's own game folder (BB_GAME_DIR, else the agent clone's game/);
skipped without a game folder or without the built CLI (cmake --build out/gpu --target party-tables-cli).
No game data is stored here."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "out" / "gpu" / "party-tables-cli.exe"
MINGW = r"C:\msys64\mingw64\bin"


def game_dir():
    for c in (os.environ.get("BB_GAME_DIR"), str(ROOT.parent / "game")):
        if c and (Path(c) / "dvdroot_ps4" / "param").is_dir():
            return Path(c)
    return None


def run_cli(game, out, *extra):
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ.get("PATH", ""))
    return subprocess.run([str(CLI), str(game), str(out), *extra], capture_output=True, text=True, env=env)


# Sections the engine derives that the Python oracle has no counterpart for (their reference was the hand-typed
# tables of party_worldact.cpp, compared once on 1.09 when they moved).
CPP_ONLY = {"bossGates", "gateRules", "bossEntities", "shopGates", "startWeapons", "bossArenas", "firstFightSpots", "stateIds", "runtimeIds"}


def body(path):
    """The cache text without its key line and without the C++-only sections."""
    out, skip = [], False
    for l in Path(path).read_text(encoding="utf-8").split("\n"):
        if l.startswith("key "):
            continue
        if l.startswith("["):
            skip = l[1:l.index("]")] in CPP_ONLY
        if not skip:
            out.append(l)
    return out


@unittest.skipUnless(game_dir() and CLI.exists(), "no game folder or no out/gpu/party-tables-cli.exe")
class CppVersusPython(unittest.TestCase):
    def test_same_tables_and_cache_reuse(self):
        with tempfile.TemporaryDirectory() as d:
            cpp, py = Path(d) / "cpp.txt", Path(d) / "py.txt"
            r = run_cli(game_dir(), cpp, "--force")
            self.assertEqual(r.returncode, 0, r.stderr)
            subprocess.run([sys.executable, "-I", "-X", "utf8", str(ROOT / "scripts" / "partydata" / "oracle.py"),
                            str(game_dir()), str(py)], check=True, capture_output=True)
            self.assertEqual(body(cpp), body(py))
            mtime = cpp.stat().st_mtime_ns
            r = run_cli(game_dir(), cpp)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("cache current", r.stdout)
            self.assertEqual(cpp.stat().st_mtime_ns, mtime)

    def test_no_game_data_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            r = run_cli(Path(d) / "nogame", Path(d) / "out.txt")
            self.assertEqual(r.returncode, 1)
            self.assertFalse((Path(d) / "out.txt").exists())


if __name__ == "__main__":
    unittest.main()


def _patch_param_flag(game, view, new_flag):
    """A mod view of `game`: the param file with one ItemLotParam row's pickup flag changed, everything else a
    junction to the original folders (synthetic edit; nothing is committed)."""
    import struct
    import zlib
    sys.path.insert(0, str(ROOT / "scripts" / "partydata"))
    import flag_tool as ft
    src = Path(game) / "dvdroot_ps4"
    (view / "dvdroot_ps4" / "param" / "gameparam").mkdir(parents=True)
    (view / "dvdroot_ps4" / "paramdef").mkdir(parents=True)
    pbnd, dbnd = src / "param/gameparam/gameparam.parambnd.dcx", src / "paramdef/paramdef.paramdefbnd.dcx"
    data = bytearray(ft.read_maybe_dcx(pbnd))
    ents = ft.bnd4(bytes(data))
    name = next(k for k in ents if k.replace("/", "\\").endswith("\\ItemLotParam.param"))
    count, = struct.unpack_from("<I", data, 0x0c)
    esz, = struct.unpack_from("<Q", data, 0x20)
    ent_off = None
    for i in range(count):
        o = 0x40 + i * esz
        off, = struct.unpack_from("<I", data, o + 0x18)
        csz, = struct.unpack_from("<q", data, o + 8)
        if bytes(data[off:off + csz]) == ents[name]:
            ent_off = off
    defs = ft.bnd4(ft.read_maybe_dcx(dbnd))
    dn = next(k for k in defs if k.replace("/", "\\").endswith("\\ItemLotParam.paramdef"))
    lay = {f[0]: f for f in ft.paramdef_layout(defs[dn])}
    foff = lay["getItemFlagId"][2]
    pb = bytes(ents[name])
    n, = struct.unpack_from("<H", pb, 0x0a)
    changed = None
    for i in range(n):
        rid, = struct.unpack_from("<i", pb, 0x40 + i * 0x18)
        doff, = struct.unpack_from("<q", pb, 0x40 + i * 0x18 + 8)
        cur, = struct.unpack_from("<i", pb, doff + foff)
        if cur > 0 and rid < 100000000 and not 5000 <= cur <= 8999:
            struct.pack_into("<i", data, ent_off + doff + foff, new_flag)
            changed = rid
            break
    orig = pbnd.read_bytes()
    z = zlib.compress(bytes(data))
    out = bytearray(orig[:0x4c])
    struct.pack_into(">II", out, 0x1c, len(data), len(z))
    (view / "dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx").write_bytes(bytes(out) + z)
    (view / "dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx").write_bytes(dbnd.read_bytes())
    for sub in ("event", "map", "remo"):
        subprocess.run(["cmd", "/c", "mklink", "/J", str(view / "dvdroot_ps4" / sub), str(src / sub)], check=True, capture_output=True)
    return changed


@unittest.skipUnless(game_dir() and CLI.exists() and os.name == "nt", "no game folder, no CLI, or not Windows")
class ModOverlay(unittest.TestCase):
    def test_tables_follow_a_param_edit(self):
        with tempfile.TemporaryDirectory() as d:
            view = Path(d) / "view"
            lot = _patch_param_flag(game_dir(), view, 54000999)
            self.assertIsNotNone(lot)
            vanilla, mod, py = Path(d) / "v.txt", Path(d) / "m.txt", Path(d) / "p.txt"
            self.assertEqual(run_cli(game_dir(), vanilla, "--force").returncode, 0)
            r = run_cli(view, mod, "--force")
            self.assertEqual(r.returncode, 0, r.stderr)
            subprocess.run([sys.executable, "-I", "-X", "utf8", str(ROOT / "scripts" / "partydata" / "oracle.py"), str(view), str(py)],
                           check=True, capture_output=True)
            self.assertEqual(body(mod), body(py))  # the C++ tables equal the oracle's on the modded files
            self.assertNotEqual(body(mod), body(vanilla))
            self.assertIn(f"{lot}\t54000999", "\n".join(body(mod)))
            self.assertNotIn(f"{lot}\t54000999", "\n".join(body(vanilla)))
