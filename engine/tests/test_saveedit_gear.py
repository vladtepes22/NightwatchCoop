"""Save editor: adding items, gems and runes, gem effects, flag groups (scripts/saveedit gearops / dialogs / ui).

Synthetic saves and a synthetic Catalogue only (no game bytes). The real-game checks read copies of
E:\\games\\bbport\\test_saves and the player's game folder, and are skipped when absent.
"""
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tests"))
from saveedit import model as M, store as S, names as N, instances as I, gearops as G, flagsdata as F  # noqa: E402
import test_saveedit as T  # noqa: E402
from test_saveedit import make_save, user, not_running, _tk  # noqa: E402,F401

NONE = (I.NO_SLOT, 0)


def gem_save():
    """make_save() plus an instance table: the +3 cane with a socketed gem, the Memory Altar, a loose blood gem
    (radial) and a loose rune, all in the inventory the way the game keeps them."""
    d = bytearray(make_save())
    n = T.NAME_AT + M.NORMAL_BEHIND_NAME
    struct.pack_into("<15I", d, 0x54, 0x80800010, 22000300, M.GEAR_DURABILITY, 0, 1, 1, 0xC0800002, *NONE * 4)
    struct.pack_into("<15I", d, 0x54 + 60, 0x80800011, 1000, 0, 0, 0, *NONE * 5)
    struct.pack_into("<8I", d, 0x54 + 120, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF)
    gem = 0x54 + 152
    struct.pack_into("<10I", d, gem, 0xC0800001, 0x80000000 | 124001, 1, 1, 13105, *[0xFFFFFFFF] * 5)
    struct.pack_into("<10I", d, gem + 40, 0xC0800002, 0x80000000 | 124001, 1, 2, 13105, *[0xFFFFFFFF] * 5)
    struct.pack_into("<10I", d, gem + 80, 0xC0800003, 0x80000000 | 129001, 2, 63, 600, 601, *[0xFFFFFFFF] * 4)
    T.put_item(d, n, 3, 0xC0800001, 0x80000000 | 124001, 1, 8)
    T.put_item(d, n, 4, 0xC0800003, 0x80000000 | 129001, 1, 10)
    struct.pack_into("<I", d, n, 5)
    return bytes(d)


def catalogue():
    return N.Catalogue({
        "names": {"weapon": {22000000: "Test Cleaver", 7000000: "Saw Test"}, "goods": {1000: "Blood Vial", 1230: "Pebble"},
                  "armor": {40000: "Test Hat"},
                  "effect": {13105: "Add blood ATK +2.5", 13106: "Cut effect text", 28101: "ATK DOWN -0.3%", 600: "Rune effect",
                             700: "Caryll effect"},
                  "gem": {1000005: "Test Gemstone", 2000005: "Triangle Gemstone", 1100005: "Cursed Gemstone",
                          16000016: "Test Rune", 11000000: "Test Caryll"}},
        "ids": {"weapon": [22000300, 22001000, 7000000, 7000100], "goods": [1000, 1230], "armor": [40000, 41000]},
        "goods_sort": {1230: 110, 1000: 100},
        "effects": {13105: [0, 5, 5, 0], 13106: [0, 5, 5, 0], 28101: [1, 0, 0, 0], 600: [0, 216, 16, 1], 601: [0, 0, 0, 1],
                    700: [0, 0, 0, 1]},
        "gemgen": {124001: 0, 129001: 10000000, 127000: 0, 127002: 0, 100301: 10000000},
        "gemrows": {127000: [0, 1, 13105], 127002: [0, 2, 13105], 100301: [10000000, 1, 700]},
        "reach": [13105, 700, 28101],
        "progress": {"bosses": [[12401800, 0]], "lamps": [72410200, 73600202]}})  # made-up lists (the real ones come from the game)


# ---- adding items ------------------------------------------------------------------------------------------
def test_add_item_goods_weapon_armor_through_the_catalogue():
    c, m = catalogue(), M.SaveModel(make_save())
    G.add_item(m, c, 4, 1230, 5)
    entry = [i for i in m.items((0,)) if i.number == 1230][0]
    assert (entry.count, entry.serial) == (5, 110 << 12 | (entry.slot + 64))  # the menu index the game writes
    G.add_item(m, c, 0, 7000000)
    G.add_item(m, c, 1, 40000)
    back = M.SaveModel(m.to_bytes())
    assert [(i.kind, i.number) for i in back.items((0,))][-2:] == [("weapon", 7000000), ("armor", 40000)]
    assert back.stats == M.SaveModel(make_save()).stats and back.echoes == 500


def test_add_item_refuses_what_the_game_does_not_have():
    c, m = catalogue(), M.SaveModel(make_save())
    for call in ((4, 99999, 1), (4, 1230, 0), (4, 1230, 100), (0, 7000100, 1), (0, 1000, 1), (0, 1000000, 1), (3, 1, 1),
                 (0, 22000300, 1), (1, 41000, 1)):  # no goods row / count / a +1 weapon / placeholders / category / no name
        with pytest.raises(ValueError):
            G.add_item(m, c, *call)
    assert m.diff() == []
    with pytest.raises(ValueError):
        G.add_item(m, N.Catalogue(), 4, 1230, 1)  # no game folder: nothing is offered


def test_catalogue_rows_lists_only_named_game_rows():
    c = catalogue()
    assert G.catalogue_rows(c, 4, "pebble") == [(1230, "Pebble")]
    assert G.catalogue_rows(c, 0) == [(7000000, "Saw Test")]  # +0 rows only: levels are set on the item
    assert (0, 1000) in G.PLACEHOLDERS
    with pytest.raises(ValueError):
        G.catalogue_rows(c, 8)


# ---- gems -----------------------------------------------------------------------------------------------------
def test_gem_rows_name_effects_and_where():
    m, c = M.SaveModel(gem_save()), catalogue()
    rows = {r["handle"]: r for r in G.gem_rows(m, c)}
    assert rows[0xC0800001]["name"] == "Test Gemstone" and rows[0xC0800001]["type"] == 1
    assert rows[0xC0800001]["effects"] == [(13105, "Add blood ATK +2.5")] and rows[0xC0800001]["where"] == "inventory"
    assert rows[0xC0800003]["type"] == 2 and rows[0xC0800003]["name"] == "Test Rune"
    assert rows[0xC0800002]["where"] == "in Test Cleaver" and rows[0xC0800002]["name"] == "Triangle Gemstone"
    assert G.gem_rows(M.SaveModel(make_save()), c) == []  # no instance table: nothing listed


def test_set_gem_effects_writes_only_its_24_bytes_and_checks_the_pool():
    m, c = M.SaveModel(gem_save()), catalogue()
    before = m.to_bytes()
    G.set_gem_effects(m, c, 0xC0800001, [13105, 28101])
    changed = m.diff()
    assert len(changed) == 1 and changed[0][1] - changed[0][0] <= 24
    again = I.read(M.SaveModel(m.to_bytes()))
    assert again.gems[0xC0800001].effects == [13105, 28101]
    assert G.gem_label(again.gems[0xC0800001], c) == ("Cursed Gemstone", True)
    for handle, effects in ((0xC0800001, []), (0xC0800001, [13105] * 4), (0xC0800001, [600]), (0xC0800001, [99]),
                            (0xC0800001, ["13105"]), (0xC0800001, [13106]), (0xC0800003, [700]), (0xDEAD, [13105])):
        with pytest.raises(ValueError):
            G.set_gem_effects(M.SaveModel(before), c, handle, effects)


def test_add_blood_gem_and_rune_only_from_the_games_rows():
    m, c = M.SaveModel(gem_save()), catalogue()
    G.add_blood_gem(m, c, 2, [13105, 28101])
    G.add_rune(m, c, 700)
    rows = G.gem_rows(M.SaveModel(m.to_bytes()), c)
    new = [r for r in rows if r["handle"] > 0xC0800010]
    assert [(r["type"], r["shape"], [e for e, _ in r["effects"]]) for r in new] == [(1, 2, [13105, 28101]), (2, 1, [700])]
    assert new[1]["name"] == "Test Caryll"
    for bad in (lambda: G.add_blood_gem(m, c, 4, [13105]), lambda: G.add_blood_gem(m, c, 1, [13106]),
                lambda: G.add_blood_gem(m, c, 1, []), lambda: G.add_rune(m, c, 701), lambda: G.add_rune(m, c, "700")):
        with pytest.raises(ValueError):
            bad()
    assert [(e, n) for e, n, _ in G.rune_rows(c)] == [(700, "Test Caryll")]


def test_copy_gem_keeps_the_record_and_the_source():
    m = M.SaveModel(gem_save())
    G.copy_gem(m, 0xC0800003)
    t = I.read(M.SaveModel(m.to_bytes()))
    copy = [g for h, g in t.gems.items() if h > 0xC0800010][0]
    assert (copy.type, copy.shape, copy.effects, copy.gen) == (2, 63, [600, 601], 129001)
    assert t.gems[0xC0800003].effects == [600, 601]
    with pytest.raises(ValueError):
        G.copy_gem(m, 0xBEEF)


def test_apply_after_adding_gear_keeps_the_save_openable(user):
    """The whole path: add a weapon (the data behind the item table moves), Apply, reopen from disk."""
    path = S.slot_files(user, "solo")[0]
    path.write_bytes(gem_save())
    model = S.open_slot(user, "solo", 0)
    G.add_item(model, catalogue(), 0, 7000000)
    G.add_blood_gem(model, catalogue(), 1, [13105])
    backup = S.apply_edit(user, "solo", 0, model, not_running)
    assert backup.is_dir()
    back = S.open_slot(user, "solo", 0)
    assert back.shift == 52 + 32 and back.name == "Alfred"
    assert [i.number for i in back.items((0,)) if i.category == 0][-1] == 7000000
    again = back.to_bytes()
    assert M.SaveModel(again).to_bytes() == again  # decode -> encode is the identity
    assert path.read_bytes() == again


# ---- flags -----------------------------------------------------------------------------------------------------
def test_flag_groups_lamp_boss_companion():
    c = catalogue()
    assert F.flag_group(72410200, c) == F.lamp_flags(72410200) and len(F.flag_group(73600202, c)) == 3
    assert F.flag_group(72410200) == (72410200,)  # no catalogue: no lamp is known
    assert F.flag_group(12401800) == (12401800,)
    assert F.flag_group(13401800) == (13401800, 9471)
    assert F.flag_group(6600) == (6600,)
    m = M.SaveModel(make_save())
    for f in F.flag_group(13401800):
        assert M.group_key(f) is not None
        m.set_flag(f, True)
    assert all(M.SaveModel(m.to_bytes()).flag(f) for f in (13401800, 9471))


# ---- the tab -----------------------------------------------------------------------------------------------------
def test_tab_add_item_gems_and_apply(user, tmp_path, monkeypatch):
    tk, root = _tk()
    try:
        from saveedit import ui
        shown = []
        monkeypatch.setattr(ui.messagebox, "showerror", lambda *a, **k: shown.append(a))
        S.slot_files(user, "solo")[0].write_bytes(gem_save())
        tab = ui.SaveEditorTab(root, lambda: user, lambda: None, lambda: tmp_path, not_running, print, None,
                               confirm=lambda t, m: True)
        tab.refresh_slots()
        tab.open_slot()
        tab.catalogue = catalogue()
        tab._set_enabled(True)
        tab.fill_gems()
        assert len(tab.gem_tree.get_children()) == 3
        assert tab.add_item(4, 1230, 3) and tab.add_item(0, 7000000, 1)
        assert not tab.add_item(4, 99999, 1) and shown  # refused, shown, nothing staged
        assert tab.add_blood_gem(1, [13105, 28101]) and tab.add_rune(700)
        tab.gem_tree.selection_set("c0800001")
        assert tab.set_gem_effects(0xC0800001, [28101, 13105])
        tab.gem_tree.selection_set("c0800003")
        tab.copy_gem()
        assert len(tab.gem_tree.get_children()) == 3 + 3
        tab.apply()
        back = S.open_slot(user, "solo", 0)
        assert any(i.number == 7000000 for i in back.items((0,))) and any(i.number == 1230 for i in back.items((0,)))
        assert I.read(back).gems[0xC0800001].effects == [28101, 13105]
    finally:
        root.destroy()


def test_dialogs_build_and_add(user, tmp_path, monkeypatch):
    tk, root = _tk()
    try:
        from saveedit import ui, dialogs as D
        S.slot_files(user, "solo")[0].write_bytes(gem_save())
        tab = ui.SaveEditorTab(root, lambda: user, lambda: None, lambda: tmp_path, not_running, print, None,
                               confirm=lambda t, m: True)
        tab.refresh_slots()
        tab.open_slot()
        tab.catalogue = catalogue()
        d = D.AddItemDialog(tab)
        d.cat_var.set("Items")
        d.text_var.set("peb")
        d.fill()
        assert d.rows == [(1230, "Pebble")]
        d.lb.selection_set(0)
        d.count_var.set("4")
        d.add()
        assert any(i.number == 1230 and i.count == 4 for i in tab.model.items((0,)))
        g = D.GemDialog(tab, 0xC0800001)
        assert g.chosen() == [13105]
        g.vars[1].set("ATK DOWN -0.3%  [28101]")
        g.ok()
        assert I.read(tab.model).gems[0xC0800001].effects == [13105, 28101]
        a = D.GemDialog(tab)
        a.shape_var.set("Triangle")
        a.ok()
        r = D.RuneDialog(tab)
        r.lb.selection_set(0)
        r.add()
        types = sorted(g.type for g in I.read(tab.model).gems.values())
        assert types == [1, 1, 1, 2, 2]
        for w in (d, g, a, r):
            w.win.destroy()
    finally:
        root.destroy()


# ---- the player's own game and saves -------------------------------------------------------------------------------
GAME = Path(r"E:\games\bloodborne\dvdroot_ps4")
DEFAULT_SAVE = T.TEST_SAVES / "savedata" / "1" / "CUSA00207" / "SPRJ0005" / "userdata0000"


@pytest.mark.skipif(not ((GAME / "msg").is_dir() and DEFAULT_SAVE.is_file()), reason="no game folder or test save")
def test_real_save_adds_survive_a_reopen(tmp_path):
    c = N.Catalogue.load(GAME, tmp_path)
    assert c.ok
    raw = DEFAULT_SAVE.read_bytes()
    m = M.SaveModel(raw)
    before = len(m.items((0,)))
    goods = G.catalogue_rows(c, 4, "beast blood pellet")[0][0]
    G.add_item(m, c, 4, goods, 3)
    weapon = G.catalogue_rows(c, 0, "saw cleaver")[0][0]
    G.add_item(m, c, 0, weapon)
    for n, _ in G.catalogue_rows(c, 1, "gascoigne"):
        G.add_item(m, c, 1, n)
    effects = [e for e, _ in c.effects_for(0)][:2]
    G.add_blood_gem(m, c, 1, effects)
    rune = G.rune_rows(c)[0][0]
    G.add_rune(m, c, rune)
    out = m.to_bytes()
    back = M.SaveModel(out)
    assert len(out) == M.SLOT_SIZE and len(back.items((0,))) == before + 8
    assert back.name == M.SaveModel(raw).name and back.stats == M.SaveModel(raw).stats
    assert [i.iid for i in back.items((1,))] == [i.iid for i in M.SaveModel(raw).items((1,))]  # key lists untouched
    t = I.read(back)
    assert t is not None and {g.effects[0] for g in t.gems.values() if g.type == 1 and g.handle > 0xC0800100} >= {effects[0]}
    assert M.SaveModel(out).to_bytes() == out  # decode -> encode identical when unchanged


# ---- the summary of an Apply, the goods maximum -------------------------------------------------------------------
def test_summary_lists_what_changed_in_words():
    from saveedit import summary
    c, m = catalogue(), M.SaveModel(gem_save())
    assert summary.changes(m, c) == []
    m.set_stat("vigor", 30)
    m.set_echoes(9000)
    m.set_count([i for i in m.items((0,)) if i.number == 100][0], 9)
    m.set_weapon_level(m.items((0,))[0], 10, c.valid_ids(0))
    G.add_item(m, c, 4, 1230, 3)
    G.add_item(m, c, 0, 7000000)
    G.set_gem_effects(m, c, 0xC0800001, [13105, 28101])
    m.set_flag(12401800, True)
    m.set_flag(7, True)
    lines = summary.changes(m, c)
    text = "\n".join(lines)
    assert "Vigor: 10 -> 30" in text and "Blood Echoes: 500 -> 9000" in text
    assert "Test Cleaver +3 -> Test Cleaver +10" in text and "+ Pebble x3" in text and "+ Saw Test" in text
    assert "effects [13105] -> [13105, 28101]" in text
    assert "Set: Boss [12401800]" in text and "Event flag bits: 2 set, 0 cleared" in text
    assert len(lines) < 40


def test_summary_caps_a_long_list():
    from saveedit import summary
    m = M.SaveModel(make_save())
    for k in range(60):
        m.add_goods(2000 + k, 1)
    lines = summary.changes(m, catalogue())
    assert len(lines) == summary.MAX_LINES + 1 and lines[-1].startswith("... and ")


def test_goods_counts_stay_within_what_the_game_holds():
    c = catalogue()
    c._goods_max = {1230: 20, 1000: 20}
    m = M.SaveModel(make_save())
    with pytest.raises(ValueError, match="1 to 20"):
        G.add_item(m, c, 4, 1230, 21)
    G.add_item(m, c, 4, 1230, 20)
    item = [i for i in m.items((0,)) if i.number == 1230][0]
    with pytest.raises(ValueError):
        m.set_count(item, 21, c.goods_max(1230))
    m.set_count(item, 7, c.goods_max(1230))
    assert N.Catalogue().goods_max(1230) is None


@pytest.mark.skipif(not (GAME / "msg").is_dir(), reason="no game folder")
def test_real_game_goods_maximum(tmp_path):
    c = N.Catalogue.load(GAME, tmp_path)
    assert c.goods_max(1000) == 20 and c.goods_max(1230) == 20 and c.goods_max(1500) == 99


def test_guest_written_save_takes_counts_and_effects_but_not_new_gear(user, tmp_path, monkeypatch):
    import save_editor as se
    tk, root = _tk()
    try:
        from saveedit import ui
        shown = []
        monkeypatch.setattr(ui.messagebox, "showerror", lambda *a, **k: shown.append(a))
        d = bytearray(gem_save())
        struct.pack_into("<6I", d, 0x3C, 0x200, 12, 0x20C, 12, 0x20C, 16)  # a header save_editor.layout accepts
        path = S.slot_files(user, "party")[0]
        path.write_bytes(bytes(d))
        assert se.world_empty(bytes(d))
        tab = ui.SaveEditorTab(root, lambda: user, lambda: None, lambda: tmp_path, not_running, print, None,
                               confirm=lambda t, m: True)
        tab.kind_var.set("party")
        tab.refresh_slots()
        tab.open_slot()
        tab.catalogue = catalogue()
        assert tab.model.world_changes
        assert not tab.add_item(0, 7000000, 1) and not tab.add_rune(700) and len(shown) == 2
        assert tab.add_item(4, 1230, 2)  # goods only fill a free entry: nothing moves
        assert tab.set_gem_effects(0xC0800001, [13105, 28101])
        tab.apply()
        after = path.read_bytes()
        assert se.world_empty(after) and any(i.number == 1230 for i in M.SaveModel(after).items((0,)))
        assert I.read(M.SaveModel(after)).gems[0xC0800001].effects == [13105, 28101]
    finally:
        root.destroy()
