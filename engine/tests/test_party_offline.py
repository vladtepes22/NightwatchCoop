r"""scripts/party_offline.py: a party character copied to single-player, cleaned.

Synthetic saves (tests/test_saveedit.make_save) plus the real fixtures of docs/party/test_saves.md under
E:\games\bbport\test_saves (skipped when absent; read only). Never the live saves.
"""
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tests"))
import party_offline as PO  # noqa: E402
from saveedit import model as M  # noqa: E402
from test_saveedit import make_save, LIVE  # noqa: E402

TEST_SAVES = Path(r"E:\games\bbport\test_saves")
PATH = "1/CUSA00207/SPRJ0005"
needs_fixtures = pytest.mark.skipif(not TEST_SAVES.is_dir(), reason="test saves absent")


def slot_bytes(m, slot):
    return [PO._group(m, slot, z)[1] for z in range(10)]


def synth_pool(m):
    """The synthetic save has an all-zero flag pool; give every chalice slot the never-used state."""
    for k in range(PO.SLOTS):
        for z in range(10):
            at, _ = PO._group(m, k, z)
            m.d[at:at + 125] = PO._pristine(z)


def party_save(phantom=1, rec_map=0x1D0A5A00, selected=1):
    """A synthetic guest save: phantom mark, the host's dungeon in slot 0, an own dungeon in slot 1."""
    m = M.SaveModel(make_save(phantom=phantom))
    synth_pool(m)
    if rec_map:
        at, _ = PO._group(m, 0, 0)
        m.d[at:at + 4] = struct.pack(">I", rec_map)
        m.d[at + 4:at + 12] = bytes.fromhex("458bd79900000001")
        at5, _ = PO._group(m, 0, 5)
        m.d[at5 + 3] = 0x55  # a case flag of the host's dungeon (94005300 + n)
    at1, _ = PO._group(m, 1, 0)
    m.d[at1:at1 + 4] = struct.pack(">I", 0x1D0B6B00)
    m.set_flag(9020 + selected)
    return m.to_bytes()


def test_clears_phantom_mark_and_network_slot():
    src = party_save()
    out, rep = PO.clean_for_offline(src)
    m, m0 = M.SaveModel(out), M.SaveModel(src)
    assert m0.phantom_mark == 1 and m.phantom_mark == 0
    assert PO.slot_record(m0, 0) == 0x1D0A5A00 and PO.slot_record(m, 0) is None
    assert slot_bytes(m, 0) == [PO._pristine(z) for z in range(10)]
    assert len(rep["changed"]) == 2 and not rep["warnings"]
    assert "0x1d0a5a00" in rep["changed"][1]


def test_only_the_phantom_word_and_slot_zero_differ_and_the_rest_is_kept():
    src = party_save()
    out, _ = PO.clean_for_offline(src)
    a, b = M.SaveModel(src), M.SaveModel(out)
    assert slot_bytes(a, 1) == slot_bytes(b, 1)
    assert PO.slot_record(b, 1) == 0x1D0B6B00 and b.flag(9021)
    assert (a.name, a.level, a.echoes, a.play_ms, a.ng_cycle) == (b.name, b.level, b.echoes, b.play_ms, b.ng_cycle)
    lo = b.name_at - M.PHANTOM_BACK
    allowed = set(range(lo, lo + 4))
    for z in range(10):
        at = PO._group(b, 0, z)[0]
        allowed |= set(range(at, at + 125))
    assert all(i in allowed for i in range(M.SLOT_SIZE) if src[i] != out[i])


def test_clean_is_idempotent_and_a_plain_save_is_unchanged():
    out, _ = PO.clean_for_offline(party_save())
    again, rep = PO.clean_for_offline(out)
    assert again == out and rep["changed"] == []
    plain = party_save(phantom=0, rec_map=0)
    same, rep = PO.clean_for_offline(plain)
    assert same == plain and rep["changed"] == []
    assert "Nothing party-only" in PO.describe(rep)


def test_input_is_not_modified_and_bad_files_are_refused():
    src = party_save()
    keep = bytes(src)
    PO.clean_for_offline(src)
    assert src == keep
    with pytest.raises(M.SaveFormatError):
        PO.clean_for_offline(b"\0" * 100)
    with pytest.raises(M.SaveFormatError):
        PO.clean_for_offline(bytes(M.SLOT_SIZE))  # an empty slot


def test_warns_when_saved_inside_a_dungeon_not_in_the_own_list():
    m = M.SaveModel(party_save(selected=0))
    struct.pack_into("<I", m.d, 4, 0x1D000000)
    out, rep = PO.clean_for_offline(m.to_bytes())
    assert len(rep["warnings"]) == 1 and "WARNING" in PO.describe(rep)
    m2 = M.SaveModel(party_save(selected=1))
    struct.pack_into("<I", m2.d, 4, 0x1D000000)
    assert PO.clean_for_offline(m2.to_bytes())[1]["warnings"] == []


def make_user(tmp_path, party_slots=(0,), solo_slots=()):
    user = tmp_path / "user"
    for s in party_slots:
        p = user / PO.PARTY / PATH / f"userdata{s:04d}"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(party_save())
        (p.parent.parent / "sce_sys").mkdir(exist_ok=True)
        (p.parent.parent / "sce_sys" / "param.sfo").write_bytes(b"sfo")
    for s in solo_slots:
        p = user / PO.SOLO / PATH / f"userdata{s:04d}"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(make_save(name="Solo"))
    return user


def test_copy_into_a_free_slot_backs_up_and_leaves_the_party_save(tmp_path):
    user = make_user(tmp_path, solo_slots=(0,))
    before = (user / PO.PARTY / PATH / "userdata0000").read_bytes()
    res = PO.copy_party_to_offline(user, 0, 1)
    assert (user / PO.PARTY / PATH / "userdata0000").read_bytes() == before
    out = (user / PO.SOLO / PATH / "userdata0001").read_bytes()
    assert M.SaveModel(out).phantom_mark == 0 and PO.slot_record(M.SaveModel(out), 0) is None
    assert (user / PO.SOLO / PATH / "userdata0000").read_bytes() == make_save(name="Solo")
    assert res["backup"] and (res["backup"] / PATH / "userdata0000").is_file()
    assert not list((user / PO.SOLO).rglob("*.party-copy"))


def test_copy_refuses_to_replace_without_replace_and_keeps_a_backup(tmp_path):
    user = make_user(tmp_path, solo_slots=(0,))
    with pytest.raises(PO.CopyError):
        PO.copy_party_to_offline(user, 0, 0)
    assert (user / PO.SOLO / PATH / "userdata0000").read_bytes() == make_save(name="Solo")
    res = PO.copy_party_to_offline(user, 0, 0, replace=True)
    assert (res["backup"] / PATH / "userdata0000").read_bytes() == make_save(name="Solo")
    assert M.SaveModel((user / PO.SOLO / PATH / "userdata0000").read_bytes()).phantom_mark == 0


def test_copy_with_no_single_player_save_and_refusals(tmp_path):
    user = make_user(tmp_path)
    with pytest.raises(PO.CopyError):
        PO.copy_party_to_offline(user, 0, running=lambda: ["bbport"])
    with pytest.raises(PO.CopyError):
        PO.copy_party_to_offline(user, 3)
    assert not (user / PO.SOLO).exists()
    res = PO.copy_party_to_offline(user, 0)
    assert res["backup"] is None
    assert (user / PO.SOLO / PATH / "userdata0000").is_file()
    assert (user / PO.SOLO / "1/CUSA00207" / "sce_sys" / "param.sfo").is_file()


@needs_fixtures
def test_real_fixtures_every_slot_cleans_and_keeps_the_rest():
    files = sorted(TEST_SAVES.glob("*/savedata/**/userdata000[0-9]"))
    assert files
    marks = hosts = 0
    for p in files:
        assert LIVE.resolve() not in p.resolve().parents
        d = p.read_bytes()
        if not any(d[:16]):
            continue
        out, rep = PO.clean_for_offline(d)
        a, b = M.SaveModel(d), M.SaveModel(out)
        marks += a.phantom_mark
        hosts += PO.slot_record(a, 0) is not None
        assert b.phantom_mark == 0 and PO.slot_record(b, 0) is None
        assert slot_bytes(b, 0) == [PO._pristine(z) for z in range(10)]
        for k in range(1, PO.SLOTS):
            assert slot_bytes(a, k) == slot_bytes(b, k)
        assert (a.name, a.level, a.echoes, a.play_ms, a.ng_cycle, a.world_empty) == \
               (b.name, b.level, b.echoes, b.play_ms, b.ng_cycle, b.world_empty)
        assert [(i.iid, i.count) for i in a.items()] == [(i.iid, i.count) for i in b.items()]
        assert PO.clean_for_offline(out)[0] == out
    assert marks >= 1 and hosts >= 1  # the fixtures hold saves with a guest mark and a joined dungeon
    d = next(TEST_SAVES.glob("cleric_beast_lamp/savedata/**/userdata0000")).read_bytes()
    assert PO.clean_for_offline(d)[0] == d  # a save that never was in a party passes through unchanged


@needs_fixtures
def test_real_guest_save_with_joined_dungeon_keeps_its_own_dungeon():
    d = next(TEST_SAVES.glob("chalice_guest_brought/savedata/**/userdata0000")).read_bytes()
    a = M.SaveModel(d)
    assert PO.slot_record(a, 0) == 0x1D0A5A00 and PO.slot_record(a, 1) == 0x1D0A5A00
    out, rep = PO.clean_for_offline(d)
    b = M.SaveModel(out)
    assert PO.slot_record(b, 0) is None and PO.slot_record(b, 1) == 0x1D0A5A00
    assert rep["changed"] and not rep["warnings"]
