"""Chalice Dungeon bosses as fighters of the main-game boss pool (option chalice_bosses), and the chalice rows of Rom,
Amygdala and Ebrietas (options hard_rom, hard_amygdala, hard_ebrietas). Lead decisions 4 Nov (D12, D13).

The chalice maps themselves are never read for a world and never written: the rows below are survey data (boss round 2
survey 3, 4 Nov 2026: every m29 MSB variant and event file). Only what a world needs is read, so the input hash grows only
when an option uses it: the chalice AI archive (when a chalice script is merged), and a chalice model's chrbnd head and
anibnd (when its fighter is asked for).

Survey facts (DATA unless marked):
- A chalice NpcParam id is L * 100,000,000 + 10,000,000 + base row (LIKELY, strong: L = 0..3 is layer 1..4 of a dungeon).
  Rows of one boss differ only in spEffectID6 (layer strength 7300/7310/7320) and drops; the same id is used at every depth.
- The deepest row: Defiled (6401 Cursed and Defiled Root, 6409 Defiled) where the boss has one, else the deepest ritual
  level, then the highest layer, then HP.
- Every battle script below exists in script/m29_00_00_00.luabnd.dcx (and only there for five of them); logic 10000 is in
  aicommon (resident); Bloodletting's headless variant 509010 is not used here.
- The hard rows use the main game's own battle script (present in the boss's own map) and the same model and TAE; against
  the main row they differ only in a few defence and perception fields (Rom def_phys 100->102; Amygdala def_phys 100->118,
  maxUndurationAng 0->30; Ebrietas def_phys 120->124, think eye/nose distances). With slot scaling HP and spEffectID6 still
  come from the arena (user, 4 Nov), so the difference in play is small."""
from containers import bnd4_read, dcx_read, dcx_size
from gamedata import animations_of

ARCHIVE = "script/m29_00_00_00.luabnd.dcx"
ARCHIVE_STEM = "m29_00"  # ailua.Archive(stem + "_00_00") names its index files m29_00_00_00.luagnl / .luainfo

# key, name, model, think, deepest npc row, health-bar name (NpcName id the chalice passes to its bar)
BOSSES = (
    ("bloodletting_beast", "Bloodletting Beast", "c5090", 509000, 310509006, 509000),
    ("pthumerian_descendant", "Pthumerian Descendant", "c3050", 305000, 310305006, 305000),
    ("pthumerian_elder", "Pthumerian Elder", "c3050", 305010, 310305016, 305002),
    ("abhorrent_beast", "Abhorrent Beast", "c5040", 504000, 310504006, 504010),
    ("loran_darkbeast", "Loran Darkbeast", "c5080", 508090, 310508006, 508010),
    ("yharnam", "Yharnam, Pthumerian Queen", "c3060", 306001, 210306016, 306001),
    ("watchdog", "Watchdog of the Old Lords", "c5010", 501000, 310501006, 501000),
)
KEY_PREFIX = "chalice:"

# main-game entity -> (model, deepest think, deepest npc row), with the option that turns it on
HARD = {
    3200800: ("c5100", 510090, 310510096, "hard_rom"),
    3300800: ("c5120", 512090, 310512096, "hard_amygdala"),
    2420800: ("c2510", 251090, 310251096, "hard_ebrietas"),
}


def extend_census(files, census, models):
    """Sizes (chrbnd head) and animations (anibnd TAE table) of models the main maps don't place."""
    for model in models:
        if model not in census.sizes:
            head = files.head("chr/%s.chrbnd.dcx" % model, 0x24)
            census.sizes[model] = dcx_size(head) if head else 0
        if model not in census.animations:
            census.animations[model] = animations_of(files, model)


def _check_row(census, model, think, npc):
    if think not in census.think_rows or npc not in census.npc_rows:
        raise ValueError("chalice row %d/%d of %s is missing from the params" % (think, npc, model))
    variation = census.npc_layout.get(census.npc_rows[npc], "behaviorVariationId")
    if variation // 10 != int(model[1:]):  # DATA: behaviorVariationId = model number * 10 for every row here
        raise ValueError("row %d is not a %s row" % (npc, model))


def fighters(files, census):
    """The 7 chalice bosses of the pool as fighter dicts (arenas.py contract). Call only when they may fight (chalice_bosses, or second stages filling the pool): it reads
    their models' chrbnd heads and anibnds into the world's inputs."""
    out = []
    extend_census(files, census, sorted({b[2] for b in BOSSES}))
    for key, name, model, think, npc, bar in BOSSES:
        _check_row(census, model, think, npc)
        out.append({"key": KEY_PREFIX + key, "name": name, "model": model, "think": think, "npc": npc, "origin": None,
                    "home": None, "extras": (), "extra_models": (), "bar_name": bar})
    return out


def hard_rows(files, census):
    """{main-game entity: (think, npc)} of the deep rows of Rom, Amygdala and Ebrietas; same model as the main body."""
    out = {}
    by_entity = {s["entity"]: s for slots in census.slots.values() for s in slots}
    for entity, (model, think, npc, _) in sorted(HARD.items()):
        main = by_entity.get(entity)
        if main is None or main["model"] != model:
            raise ValueError("%d is not the %s of the main game" % (entity, model))
        _check_row(census, model, think, npc)
        out[entity] = (think, npc)
    return out


def hard_options():
    """{main-game entity: seedcode option name}."""
    return {entity: v[3] for entity, v in HARD.items()}


def archive(census):
    """The chalice maps' AI archive, read once and only when a script has to come from it (it then counts as an input)."""
    bnd = getattr(census, "chalice_archive", None)
    if bnd is None:
        bnd = bnd4_read(dcx_read(census.files.read(ARCHIVE))[0])
        census.chalice_archive = bnd
        census.chalice_script_files = {e[2].replace("/", "\\").rsplit("\\", 1)[1] for e in bnd["entries"]}
    return bnd
