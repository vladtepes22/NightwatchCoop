"""scripts/save_editor.py (Export for save editor / Import from save editor) and tools/party/editor_check.py.

Synthetic saves in a temporary user folder only (never a player's save); one test also exports the
test fixture E:\\games\\bbport\\test_saves\\savedata when it is there (read only).
"""
import json
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "party"))
import editor_check as ec  # noqa: E402
import save_editor as se  # noqa: E402

FIXTURE = Path(r"E:\games\bbport\test_saves\savedata")
NAME_AT = 0x10000
POOL = 0x30000


def make_save(name="Alfred", play=3600000, guest=False, echoes=500):
    """A character save with the game's layout where the editor and the export look: the name, the
    "FACE" marker 469 + 34028 bytes after it, an empty inventory entry, the flag store, the world-state
    section (empty when guest: written in another player's world), the position, the blocks after it,
    and the game's unused buffer (0x5A) behind the data."""
    d = bytearray(b"\x5a" * se.SLOT_SIZE)
    d[:0x60000] = bytes(0x60000)
    struct.pack_into("<III", d, 0, 0x41, 0x18010000, play)
    d[NAME_AT + 1:NAME_AT + 1 + 2 * len(name)] = name.encode("utf-16le")  # the editor reads from name_at + 1
    struct.pack_into("<I", d, NAME_AT - 23, 37)
    struct.pack_into("<I", d, NAME_AT - 19, echoes)
    struct.pack_into("<IIII", d, NAME_AT + 469, 0, 0xFFFFFFFF, 0, 0)  # an empty inventory entry
    d[NAME_AT + 469 + 34028:NAME_AT + 469 + 34028 + 4] = b"FACE"
    a = POOL + 150001
    struct.pack_into("<II", d, 0x34, POOL, 150001)
    world = se.EMPTY_WORLD if guest else (
        struct.pack("<I", 0x24) + b" RHC\x00\x17\x09\x15" + bytes(0x1c) +
        struct.pack("<I", 0x2e) + b" JBO\x00\x17\x09\x15" + bytes(0x26) +
        struct.pack("<I", 0x14) + b"LCED\x00\x03\x02\x15" + b"\xff\xff\xff\xff" + bytes(8))
    d[a:a + len(world)] = world
    b = a + len(world)
    struct.pack_into("<fff", d, b, -196.1, -28.6, 63.4)
    c = b + 0x50
    struct.pack_into("<6I", d, 0x3c, a, len(world), b, len(world) + 0x50, c, 0x200)
    d[c:c + 0x200] = b"\x11" * 0x200
    struct.pack_into("<HHI", d, c + 0x200, 1, 0, 0x40)
    d[c + 0x208:c + 0x208 + 0x40 + 27] = b"\x22" * (0x40 + 27)
    return bytes(d)


def put(user, kind, slot, data):
    p = Path(user) / se.KINDS[kind] / "1" / "CUSA00207" / "SPRJ0005" / f"userdata000{slot}"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p


def test_editor_check_guest_save_and_own_save():
    own, guest = make_save(), make_save(guest=True)
    r = ec.parse(own)
    assert r["problems"] == [] and r["name"] == "Alfred" and r["level"] == 37
    assert [round(v, 1) for v in r["position"]] == [-196.1, -28.6, 63.4]
    r = ec.parse(guest)
    assert r["lced"] is None and len(r["problems"]) == 1 and "LCED" in r["problems"][0]
    assert r["position_at"] < NAME_AT + 0x1000  # read from the inventory, not the position


def test_export_import_round_trip_guest_party_save(tmp_path):
    user, out = tmp_path / "user", tmp_path / "export"
    live = put(user, "party", 1, make_save(guest=True))
    original = live.read_bytes()
    (user / "party_characters.txt").write_text(
        f"slot 1 used 1 play 3600000 name {se._fnv(se._name_bytes(original)):016x}\n", encoding="utf-8")
    (user / "party_rot.txt").write_text("slot 1 kinds 1 0 0 0 0\n", encoding="utf-8")
    rows = se.list_saves(user)
    assert [(r["kind"], r["slot"], r["guest_written"]) for r in rows] == [("party", 1, True)]
    assert "as a guest" in se.describe(rows[0])
    f = se.export_save(user, "party", 1, out)
    assert live.read_bytes() == original  # the live save is never changed
    man = json.loads((out / se.MANIFEST).read_text(encoding="utf-8"))
    assert man["kind"] == "party" and man["slot"] == 1 and man["changed"][0]["what"] == "world_state"
    assert (out / "party_files" / "party_rot.txt").is_file()
    exported = f.read_bytes()
    assert ec.parse(exported)["problems"] == []  # the editor reads the export
    assert [round(v, 1) for v in ec.parse(exported)["position"]] == [-196.1, -28.6, 63.4]
    # unchanged: the import gives the save back byte for byte
    plan = se.check_import(user, f)
    assert plan["data"] == original and plan["notes"] == ["the file was not changed since the export"]
    # an edit in the editor: Echoes and the name (the editor writes them at its offsets)
    d = bytearray(exported)
    struct.pack_into("<I", d, NAME_AT - 19, 99999)
    d[NAME_AT + 1:NAME_AT + 13] = "Djurax".encode("utf-16le")
    f.write_bytes(bytes(d))
    with pytest.raises(se.ImportNeedsConfirm):  # the name changes: asked first
        se.import_save(user, f)
    done = se.import_save(user, f, force=True)
    new = live.read_bytes()
    assert struct.unpack_from("<I", new, NAME_AT - 19)[0] == 99999 and se.world_empty(new)
    assert sum(x != y for x, y in zip(new, original)) == sum(x != y for x, y in zip(d, exported))
    assert (Path(done["backup"]) / "1" / "CUSA00207" / "SPRJ0005" / "userdata0001").read_bytes() == original
    line = (user / "party_characters.txt").read_text(encoding="utf-8").strip()
    assert line == f"slot 1 used 1 play 3600000 name {se._fnv(se._name_bytes(new)):016x}" and "Djurax" in \
        se.character(new)["name"]


def test_import_refusals(tmp_path):
    user, out = tmp_path / "user", tmp_path / "export"
    live = put(user, "solo", 0, make_save())
    f = se.export_save(user, "solo", 0, out)
    assert f.read_bytes() == live.read_bytes()  # an own-world save is exported as it is
    put(user, "solo", 0, make_save(play=3700000))  # the game saved after the export
    with pytest.raises(se.ImportNeedsConfirm, match="newer progress"):
        se.import_save(user, f)
    f.write_bytes(f.read_bytes()[:1000])  # an editor that broke the file
    with pytest.raises(se.SaveFormatError):
        se.check_import(user, f)
    lone = tmp_path / "lone" / "userdata0000"
    lone.parent.mkdir()
    lone.write_bytes(make_save())
    with pytest.raises(se.SaveFormatError, match=se.MANIFEST):
        se.check_import(user, lone)


@pytest.mark.skipif(not FIXTURE.is_dir(), reason="test fixture not on this PC")
def test_fixture_export_reads_in_the_editor(tmp_path):
    user = tmp_path / "user"
    src = next(FIXTURE.glob("*/*/SPRJ0005/userdata0000"))
    live = put(user, "party", 0, src.read_bytes())
    f = se.export_save(user, "party", 0, tmp_path / "export")
    assert ec.parse(f.read_bytes())["problems"] == [] and se.check_import(user, f)["data"] == live.read_bytes()
    # the same character as a guest-written save: the world state emptied as the game writes it there
    d = live.read_bytes()
    L = se.layout(d)
    guest = se._replace_world(d, se.EMPTY_WORLD, b"")
    assert ec.parse(guest)["problems"] and se.world_empty(guest) and L["an"] > 12
    put(user, "party", 0, guest)
    f = se.export_save(user, "party", 0, tmp_path / "export2")
    assert ec.parse(f.read_bytes())["problems"] == [] and se.check_import(user, f)["data"] == guest
