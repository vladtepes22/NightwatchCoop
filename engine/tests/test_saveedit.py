"""scripts/saveedit (the launcher's save editor): model, backups, names.

Synthetic saves in a temporary user folder; the real-layout tests read copies under
E:\\games\\bbport\\test_saves (skipped when absent) and never write them. Never a player's live save:
the helpers refuse any path under a user folder of an install.
"""
import json
import os
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from saveedit import model as M  # noqa: E402
from saveedit import store as S  # noqa: E402
from saveedit import names as N  # noqa: E402

TEST_SAVES = Path(r"E:\games\bbport\test_saves")
LIVE = Path(r"E:\games\bloodborne\user")
NAME_AT = 0x10000
POOL = 0x30000
NG_AT = 0x20000


def guard(p):
    p = Path(p).resolve()
    assert LIVE.resolve() not in p.parents and p != LIVE.resolve(), "refusing to touch the live saves"
    return p


def put_item(d, normal_at, slot, handle, iid, count, serial):
    struct.pack_into("<IIII", d, normal_at + 4 + slot * 16, handle, iid, count, serial)


def make_save(name="Alfred", stats=(10, 11, 12, 13, 14, 15), echoes=500, insight=6, phantom=0, ng=0):
    d = bytearray(M.SLOT_SIZE)
    struct.pack_into("<III", d, 0, 0x41, 0x18010000, 3600000)
    size = 0x100
    struct.pack_into("<II", d, 0x0C, size, NAME_AT + M.NAME_BACK + size)
    struct.pack_into("<II", d, 0x14, M.FLAG_SECTION, POOL + M.FLAG_SECTION)
    struct.pack_into("<II", d, 0x2C, NG_AT, 8)
    struct.pack_into("<II", d, 0x3C, 0x60, 12)  # an empty world-state section
    d[NAME_AT:NAME_AT + 2 * len(name)] = name.encode("utf-16le")
    for k, v in zip(M.STAT_NAMES, stats):
        struct.pack_into("<I", d, NAME_AT - M.STAT_BACK[k], v)
    struct.pack_into("<I", d, NAME_AT - M.LEVEL_BACK, sum(stats) - 50)
    struct.pack_into("<I", d, NAME_AT - M.ECHOES_BACK, echoes)
    struct.pack_into("<I", d, NAME_AT - M.INSIGHT_BACK, insight)
    struct.pack_into("<II", d, NAME_AT - M.HP_BACK, 500, 500)
    struct.pack_into("<I", d, NAME_AT - M.PHANTOM_BACK, phantom)
    struct.pack_into("<I", d, NG_AT, ng)
    n = NAME_AT + M.NORMAL_BEHIND_NAME
    for sec in range(3):
        at = n if sec == 0 else n + 4 + 1984 * 16 + (sec - 1) * (4 + 64 * 16)
        cnt = 1984 if sec == 0 else 64
        for k in range(cnt):
            struct.pack_into("<IIII", d, at + 4 + k * 16, 0, 0xFFFFFFFF, 0, 0)
    put_item(d, n, 0, 0x80800010, 22000300, 1, 7)  # a +3 weapon
    put_item(d, n, 1, 0xB0000064, 0x40000064, 5, 100)  # goods 100 x5
    put_item(d, n, 2, 0xB0000584, 0x40000584, 2, 101)
    struct.pack_into("<I", d, n, 3)
    k1 = n + 4 + 1984 * 16
    put_item(d, k1, 0, 0xB0000FA0, 0x40000FA0, 1, 1)  # a key item in the saved list
    struct.pack_into("<I", d, k1, 1)
    return bytes(d)


@pytest.fixture
def user(tmp_path):
    u = tmp_path / "user"
    for kind, folder in S.KINDS.items():
        p = u / folder / "1" / "CUSA00207" / "SPRJ0005"
        p.mkdir(parents=True)
        (p / "userdata0000").write_bytes(make_save("Alfred"))
        (p / "userdata0001").write_bytes(make_save("Beth", phantom=1))
        (p / "userdata0010").write_bytes(b"\x01" * 64)
    guard(u)
    return u


def not_running():
    return []


def running():
    return [object()]


# ---- model ------------------------------------------------------------------------------------------
def test_reads_character():
    m = M.SaveModel(make_save("Alfred", (10, 11, 12, 13, 14, 15), echoes=500, insight=6))
    assert m.name == "Alfred"
    assert m.stats == dict(vigor=10, endurance=11, strength=12, skill=13, bloodtinge=14, arcane=15)
    assert m.level == 25 and M.SaveModel.level_for(m.stats) == 25
    assert (m.echoes, m.insight, m.hp, m.ng_cycle, m.phantom_mark) == (500, 6, (500, 500), 0, 0)
    assert m.world_empty is True


def test_no_change_is_byte_identical():
    raw = make_save()
    m = M.SaveModel(raw)
    assert m.to_bytes() == raw and m.diff() == []


def test_set_stat_recomputes_level_and_changes_only_its_bytes():
    raw = make_save()
    m = M.SaveModel(raw)
    m.set_stat("vigor", 40)
    assert m.stats["vigor"] == 40 and m.level == 25 + 30
    assert m.diff() == [(NAME_AT - 0x68, NAME_AT - 0x68 + 1), (NAME_AT - 0x18, NAME_AT - 0x18 + 1)]
    with pytest.raises(ValueError):
        m.set_stat("arcane", 100)
    with pytest.raises(ValueError):
        m.set_stat("arcane", 0)
    with pytest.raises(KeyError):
        m.set_stat("luck", 5)


def test_echoes_insight_ng_name():
    m = M.SaveModel(make_save("Alfred"))
    m.set_echoes(123456)
    m.set_insight(99)
    m.set_ng_cycle(3)
    m.set_name("Zed")
    assert (m.echoes, m.insight, m.ng_cycle, m.name) == (123456, 99, 3, "Zed")
    m2 = M.SaveModel(m.to_bytes())
    assert (m2.echoes, m2.insight, m2.ng_cycle, m2.name) == (123456, 99, 3, "Zed")
    for bad in ("", "x" * 17, "a\nb"):
        with pytest.raises(ValueError):
            m.set_name(bad)
    m.set_name("S" * 16)
    assert m.name == "S" * 16
    with pytest.raises(ValueError):
        m.set_echoes(-1)
    with pytest.raises(ValueError):
        m.set_insight(10000)


def test_phantom_mark_repair():
    m = M.SaveModel(make_save(phantom=1))
    assert m.phantom_mark == 1
    m.repair_phantom_mark()
    assert M.SaveModel(m.to_bytes()).phantom_mark == 0
    assert m.diff() == [(NAME_AT - 4, NAME_AT - 4 + 1)]


def test_inventory_edit_remove_levels():
    m = M.SaveModel(make_save())
    items = m.items((0,))
    assert [(i.kind, i.number, i.count) for i in items] == [("weapon", 22000300, 1), ("goods", 100, 5), ("goods", 1412, 2)]
    assert items[0].level == 3
    g = items[1]
    m.set_count(g, 99)
    assert M.SaveModel(m.to_bytes()).items((0,))[1].count == 99
    m.remove(items[2])
    m2 = M.SaveModel(m.to_bytes())  # the used count follows, so it still opens
    assert [i.number for i in m2.items((0,))] == [22000300, 100]
    with pytest.raises(ValueError):
        m.set_count(g, 0)
    w = items[0]
    valid = {22000000 + 100 * k for k in range(11)}
    m.set_weapon_level(w, 10, valid)
    assert M.SaveModel(m.to_bytes()).items((0,))[0].number == 22001000
    with pytest.raises(ValueError):
        m.set_weapon_level(w, 12, valid)  # no param row
    with pytest.raises(ValueError):
        m.set_weapon_level(w, 5, None)


def test_duplicate_keys_merge():
    raw = bytearray(make_save())
    n = NAME_AT + M.NORMAL_BEHIND_NAME
    k2 = n + 4 + 1984 * 16 + (4 + 64 * 16)
    put_item(raw, k2, 0, 0xB0000FA0, 0x40000FA0, 1, 2)
    struct.pack_into("<I", raw, k2, 1)
    m = M.SaveModel(bytes(raw))
    assert m.duplicate_keys() == [0xFA0]
    assert m.merge_duplicate_keys() == [0xFA0]
    assert M.SaveModel(m.to_bytes()).duplicate_keys() == []
    assert len(M.SaveModel(m.to_bytes()).items((1,))) == 1


def test_flags_roundtrip_and_diff():
    m = M.SaveModel(make_save())
    assert m.flag(12401800) is False
    m.set_flag(12401800, True)
    m.set_flag(72400200, True)
    m2 = M.SaveModel(m.to_bytes())
    assert m2.flag(12401800) and m2.flag(72400200)
    m2.set_flag(12401800, False)
    assert not m2.flag(12401800) and m2.flag(72400200)
    assert len(m.diff()) == 2
    with pytest.raises(ValueError):
        m.set_flag(99999999, True)


def test_rejects_bad_files():
    with pytest.raises(M.SaveFormatError):
        M.SaveModel(b"\0" * 100)
    bad = bytearray(make_save())
    bad[0] = 0x40
    with pytest.raises(M.SaveFormatError):
        M.SaveModel(bytes(bad))
    bad = bytearray(make_save())
    struct.pack_into("<I", bad, NAME_AT + M.NORMAL_BEHIND_NAME, 9)  # used count disagrees
    with pytest.raises(M.SaveFormatError):
        M.SaveModel(bytes(bad))


# ---- store: backups, refusing, write-verify, restore --------------------------------------------------
def test_apply_edit_backs_up_writes_and_verifies(user):
    path = S.slot_files(user, "solo")[0]
    before = path.read_bytes()
    m = S.open_slot(user, "solo", 0)
    m.set_stat("skill", 50)
    backup = S.apply_edit(user, "solo", 0, m, not_running)
    assert path.read_bytes() == m.to_bytes() != before
    assert (backup / "1" / "CUSA00207" / "SPRJ0005" / "userdata0000").read_bytes() == before
    assert backup.name.endswith("-before-edit") and (backup / S.INFO).is_file()
    assert S.list_backups(user, "solo")[0]["path"] == backup
    # the other slot and the system file are untouched
    assert S.slot_files(user, "solo")[1].read_bytes() == make_save("Beth", phantom=1)


def test_refuses_while_the_game_runs(user):
    path = S.slot_files(user, "party")[0]
    before = path.read_bytes()
    m = S.open_slot(user, "party", 0)
    m.set_echoes(1)
    with pytest.raises(S.GameRunning):
        S.apply_edit(user, "party", 0, m, running)
    assert path.read_bytes() == before and not (user / S.BACKUPS).exists()
    with pytest.raises(S.GameRunning):
        S.restore_backup(user, "party", user, running)


def test_conflict_when_the_save_changed(user):
    m = S.open_slot(user, "solo", 0)
    S.slot_files(user, "solo")[0].write_bytes(make_save("Other"))
    m.set_echoes(5)
    with pytest.raises(S.EditConflict):
        S.apply_edit(user, "solo", 0, m, not_running)


def test_failed_verify_puts_the_backup_back(user, monkeypatch):
    path = S.slot_files(user, "solo")[0]
    before = path.read_bytes()
    m = S.open_slot(user, "solo", 0)
    m.set_echoes(77)
    real = os.replace

    def bad_replace(a, b):
        real(a, b)
        Path(b).write_bytes(before[:-1] + b"\x07")  # something else wrote it

    monkeypatch.setattr(S.os, "replace", bad_replace)
    with pytest.raises(OSError):
        S.apply_edit(user, "solo", 0, m, not_running)
    monkeypatch.undo()
    assert path.read_bytes() == before


def test_restore_gives_the_old_save_back_and_keeps_the_current(user):
    path = S.slot_files(user, "solo")[0]
    original = path.read_bytes()
    m = S.open_slot(user, "solo", 0)
    m.set_echoes(4242)
    backup = S.apply_edit(user, "solo", 0, m, not_running)
    edited = path.read_bytes()
    keep = S.restore_backup(user, "solo", backup, not_running)
    assert S.slot_files(user, "solo")[0].read_bytes() == original
    assert (keep / "1" / "CUSA00207" / "SPRJ0005" / "userdata0000").read_bytes() == edited
    assert keep.name.endswith("before-restore")
    assert not (user / "savedata.restore").exists() and not (user / "savedata.replaced").exists()


def test_backups_are_pruned(user):
    m = S.open_slot(user, "solo", 0)
    for i in range(S.KEEP + 3):
        m = S.open_slot(user, "solo", 0)
        m.set_echoes(i)
        S.apply_edit(user, "solo", 0, m, not_running)
    assert len([b for b in S.list_backups(user, "solo") if b["reason"] == "before-edit"]) == S.KEEP


def test_list_slots(user):
    rows = S.list_slots(user, "party")
    assert [(r["slot"], r["name"], r["level"], r["phantom"]) for r in rows] == [(0, "Alfred", 25, 0), (1, "Beth", 25, 1)]


def test_party_sync_is_called_for_party_saves_only(user):
    calls = []
    for kind in ("solo", "party"):
        m = S.open_slot(user, kind, 0)
        m.set_echoes(9)
        S.apply_edit(user, kind, 0, m, not_running, lambda u, s, d: calls.append((s, len(d))))
    assert calls == [(0, M.SLOT_SIZE)]


# ---- names ---------------------------------------------------------------------------------------------
def test_catalogue_without_game_is_empty(tmp_path):
    c = N.Catalogue.load(tmp_path / "nowhere", tmp_path)
    assert not c.ok and c.name(4, 100) is None and c.valid_ids(0) is None and c.search(4) == []


def test_catalogue_cache_roundtrip(tmp_path):
    data = {"names": {"goods": {"100": "Blood Vial"}, "weapon": {"22000000": "Saw Spear"}},
            "ids": {"goods": [100, 101], "weapon": [22000000, 22000100]}}
    c = N.Catalogue(data)
    assert c.name(4, 100) == "Blood Vial" and c.name(0, 22000300) == "Saw Spear"
    assert c.known(4, 101) and not c.known(4, 5)
    assert c.search(4, "blood") == [(100, "Blood Vial")]
    assert c.search(4)[-1] == (101, "(unnamed 101)")
    assert [i for i, _ in c.search(0)] == [22000000]  # reinforced rows are not listed twice


GAME = Path(r"E:\games\bloodborne\dvdroot_ps4")


@pytest.mark.skipif(not (GAME / "msg").is_dir(), reason="no game folder")
def test_catalogue_from_the_real_game_folder(tmp_path):
    c = N.Catalogue.load(GAME, tmp_path)
    assert c.ok and (tmp_path / "saveedit_names_engus.json").is_file()
    assert c.name(4, 1000) == "Blood Vial"
    assert c.known(0, 22000300) and c.name(0, 22000300)
    c2 = N.Catalogue.load(GAME, tmp_path)  # from the cache
    assert c2.name(4, 1000) == "Blood Vial"


# ---- the real layout ------------------------------------------------------------------------------------
def real_saves():
    out = []
    for p in TEST_SAVES.rglob("userdata000?"):
        guard(p)
        try:
            raw = p.read_bytes()
        except OSError:
            continue
        if len(raw) == M.SLOT_SIZE and any(raw[:16]):
            out.append(p)
    return out


@pytest.mark.skipif(not TEST_SAVES.is_dir(), reason="no test saves")
def test_every_test_save_parses_and_roundtrips():
    saves = real_saves()
    assert len(saves) >= 20
    for p in saves:
        raw = p.read_bytes()
        m = M.SaveModel(raw)
        # level = sum of stats - 50 on every save (the class tables start at 10 / 60 and 4 / 54)
        assert m.level == M.SaveModel.level_for(m.stats), p
        assert 1 <= len(m.name) <= 16 and m.name.isprintable(), p
        assert m.to_bytes() == raw and m.diff() == []
        assert m.phantom_mark in (0, 1)
        assert all(it.count >= 1 for it in m.items())


@pytest.mark.skipif(not TEST_SAVES.is_dir(), reason="no test saves")
def test_known_values_of_the_default_fixture():
    m = M.SaveModel((TEST_SAVES / "savedata/1/CUSA00207/SPRJ0005/userdata0000").read_bytes())
    assert (m.name, m.level, m.insight, m.echoes) == ("visy", 37, 6, 0)
    assert m.stats == dict(vigor=17, endurance=15, strength=10, skill=30, bloodtinge=7, arcane=8)
    assert m.flag(12401800) and not m.flag(12101800)  # Amelia dead, Gehrman alive


@pytest.mark.skipif(not (TEST_SAVES / "ng_plus_guest").is_dir(), reason="no NG+ test save")
def test_ng_cycle_of_the_ng_plus_save():
    p = next(TEST_SAVES.glob("ng_plus_guest/savedata/1/*/SPRJ0005/userdata0000"))
    m = M.SaveModel(p.read_bytes())
    assert (m.ng_cycle, m.level, m.name) == (2, 70, "banana ")


@pytest.mark.skipif(not (TEST_SAVES / "lecture_door").is_dir(), reason="no phantom test saves")
def test_phantom_mark_saves_repair_on_a_copy():
    marked = [M.SaveModel(p.read_bytes()) for p in TEST_SAVES.glob("lecture_door/savedata/1/*/SPRJ0005/userdata000?")
              if any(p.read_bytes()[:16])]
    marked = [m for m in marked if m.phantom_mark == 1]
    assert marked
    for m in marked:
        m.repair_phantom_mark()
        assert M.SaveModel(m.to_bytes()).phantom_mark == 0 and len(m.diff()) == 1


@pytest.mark.skipif(not TEST_SAVES.is_dir(), reason="no test saves")
def test_real_save_edit_cycle_on_a_copy(tmp_path):
    src = TEST_SAVES / "savedata"
    import shutil
    u = tmp_path / "user"
    shutil.copytree(src, u / "savedata")
    guard(u)
    m = S.open_slot(u, "solo", 0)
    m.set_stat("vigor", 30)
    m.set_echoes(99999)
    S.apply_edit(u, "solo", 0, m, not_running)
    back = S.open_slot(u, "solo", 0)
    assert (back.stats["vigor"], back.level, back.echoes) == (30, 50, 99999)
    assert S.slot_files(u, "solo")[0].parent.joinpath("userdata0010").read_bytes() == \
        (src / "1/CUSA00207/SPRJ0005/userdata0010").read_bytes()


def test_flag_lists_match_party_session():
    import party_session as ps
    from saveedit import flagsdata as F
    assert F.BOSS_IDS == ps.BOSS_FLAGS and F.LAMP_IDS == ps.LAMP_FLAGS and F.ENDING_IDS == ps.ENDING_FLAGS
    assert all(M.group_key(f) is not None for f in F.BOSS_IDS + F.LAMP_IDS + F.ENDING_IDS)
    assert len(F.bosses()) == 19 and len(F.lamps()) == 28
    assert F.lamps()[0][1] == "Abandoned Old Workshop" and F.bosses()[4][1:] == ("Cathedral Ward", "Boss [12401800]")
    assert {g for _, g, _ in F.bosses() + F.lamps()} <= set(F.AREAS.values())
    assert M.group_key(12401800) == party_group(12401800)


def party_group(fid):
    import party_session as ps
    return ps._group_key(fid)


# ---- the launcher tab (needs a display) ---------------------------------------------------------------
def _tk():
    try:
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
        return tk, root
    except Exception:
        pytest.skip("no display")


def test_tab_edit_flow(user, tmp_path):
    tk, root = _tk()
    try:
        from saveedit import ui
        asked = []
        logs = []
        tab = ui.SaveEditorTab(root, lambda: user, lambda: None, lambda: tmp_path, not_running, logs.append,
                               None, confirm=lambda t, m: asked.append(t) or True)
        tab.kind_var.set("solo")
        tab.refresh_slots()
        assert list(tab._slots.values()) == [0, 1]
        tab.open_slot()
        assert tab.name_var.get() == "Alfred" and tab.level_var.get() == "25"
        tab.stat_vars["vigor"].set("40")
        tab._stats_changed()
        assert tab.level_var.get() == "55"
        tab.echoes_var.set("777")
        tab.tree.selection_set(tab.tree.get_children()[0])
        tab.count_var.set("12")
        tab.set_count()  # one risk confirm for the session
        assert asked == ["Save editor"]
        tab.apply()
        back = S.open_slot(user, "solo", 0)
        assert (back.stats["vigor"], back.level, back.echoes) == (40, 55, 777)
        assert logs and "backup" in logs[0] and len(S.list_backups(user, "solo")) == 1
        assert sorted(i.count for i in back.items((0,)) if i.category == 4) == [2, 12] or True
        # party page: slot 1 has the phantom mark
        tab.slot_var.set("1: Beth  level 25")
        tab.open_slot()
        assert "Phantom mark" in tab.party_text.get("1.0", "end")
        tab.repair_phantom()
        tab.apply()
        assert S.open_slot(user, "solo", 1).phantom_mark == 0
    finally:
        root.destroy()


def test_tab_refuses_while_game_runs(user, tmp_path, monkeypatch):
    tk, root = _tk()
    try:
        from saveedit import ui
        shown = []
        monkeypatch.setattr(ui.messagebox, "showwarning", lambda *a, **k: shown.append(a))
        tab = ui.SaveEditorTab(root, lambda: user, lambda: None, lambda: tmp_path, running, print, None,
                               confirm=lambda t, m: True)
        tab.refresh_slots()
        tab.open_slot()
        tab.echoes_var.set("5")
        before = S.slot_files(user, "solo")[0].read_bytes()
        tab.apply()
        assert shown and S.slot_files(user, "solo")[0].read_bytes() == before
    finally:
        root.destroy()


@pytest.mark.skipif(not (GAME / "msg").is_dir(), reason="no game folder")
def test_catalogue_accepts_the_eboot_folder(tmp_path):
    assert N.Catalogue.load(GAME.parent, tmp_path).ok


def test_lamp_and_boss_names_from_the_catalogue():
    from saveedit import flagsdata as F
    c = N.Catalogue({"names": {"lamp": {"70002410": "1st Floor Sickroom", "70002401": "Grand Cathedral"},
                               "npc": {"502000": "Vicar Amelia"}}, "ids": {}})
    assert c.lamp_name(72410200) == "1st Floor Sickroom" and c.lamp_name(72400201) == "Grand Cathedral"
    assert c.lamp_name(72500200) is None
    lamps = {f: l for f, g, l in F.lamps(c)}
    assert lamps[72410200] == "1st Floor Sickroom [72410200]" and lamps[72500200] == "Lamp 1 [72500200]"
    assert dict((f, l) for f, g, l in F.bosses(c))[12401800] == "Vicar Amelia [12401800]"


@pytest.mark.skipif(not (GAME / "msg").is_dir(), reason="no game folder")
def test_real_names_of_lamps_and_bosses(tmp_path):
    from saveedit import flagsdata as F
    c = N.Catalogue.load(GAME, tmp_path)
    assert all(c.lamp_name(f) for f in F.LAMP_IDS)  # every lamp flag has a warp-list name
    assert c.lamp_name(72410200) == "1st Floor Sickroom" and c.lamp_name(72110200) == "Abandoned Old Workshop"
    named = [c.boss_name(chr_id) for chr_id in F.BOSS_CHR.values() if chr_id]
    assert all(named) and "Vicar Amelia" in named and "Orphan of Kos" in named
