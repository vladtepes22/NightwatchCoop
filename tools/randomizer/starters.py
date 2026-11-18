"""Starting weapons: the five free weapons the Hunter's Dream offers before the first hunt (three melee, two guns) are
drawn again from the weapons the game really hands out. Each row sells a copy of the drawn weapon (new ids, same name,
look and stats) that needs no stats; the weapon itself keeps its requirements wherever else it is found or sold.
The shop rows keep their place, their price (free) and their bought-once flag; only the weapon in them changes.
How many are offered is set apart (starter_melee 1-5, starter_guns 1-3): fewer hides rows, more adds rows that always
sell a drawn copy, with or without the shuffle."""
import json

from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
from seedcode import Stream

MELEE_ROWS = (2000, 2001, 2002, 2003, 2004)  # Saw Cleaver, Hunter Axe, Threaded Cane in vanilla; 2003, 2004 added
GUN_ROWS = (2010, 2011, 2012)                # Hunter Pistol, Hunter Blunderbuss; 2012 added
VANILLA = {"melee": 3, "guns": 2}            # rows the game has (DATA: ids 2003, 2004, 2012 are free)
# Taking one melee weapon closes the melee messenger once any of 72101000-02 is set, guns once 72101010 or 72101011 is
# (m21_00 events 12101020, 12101021, DATA). An added row sets the first row's flag of its list, so it closes the same
# messenger and no event changes: still one melee and one gun.
ADDED_FLAG_OF = {"melee": 2000, "guns": 2010}
WEAPON = 0                       # ShopLineupParam equipType and ItemLotParam category for weapons
LEVEL = 10000                    # a weapon's id is its base id plus its reinforcement level
# A secondary (left-hand) weapon has a left-hand model point and no right-hand one (DATA, 6 Nov: true for all 90 weapons
# the game and Nocturne v2.0.0 give out). A fixed motion list missed Rosmarinus, the Piercing Rifle and Fist of Gratia /
# Vacuous Iron, and would miss every mod weapon.
LEFT_HAND = "dmypolyId_Slot0LeftFormA"
NOT_A_MAIN_WEAPON = frozenset((20, 25, 42))  # wepmotionCategory: shields, torches, bare fists (not offered as starters)
REQUIREMENTS = ("properStrength", "properAgility", "properMagic", "properFaith")
WEAPON_NUMBER = 1000000          # id = weapon number * 1000000 + variant * 10000 + level * 100 (Uncanny +1, Lost +2)
COPY_VARIANTS = range(50, 90)    # vanilla variants are 0-29 and 90 (DATA), Nocturne's Forgotten Greatsword 30-32
ORIGINS = ("originEquipWep",) + tuple("originEquipWep%d" % k for k in range(1, 16))
TEXTS = (11, 21, 25)             # item.msgbnd file ids: weapon name, description, lore (DATA)


def _tables(files):
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])

    def table(name):
        return Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3]), Param(bnd4_find(bnd, "\\%s.param" % name)[3]).first()
    return table


def census(files):
    """{'melee': [weapon ids], 'guns': [weapon ids]}: base weapons the game gives out, sorted."""
    table = _tables(files)
    weapon_layout, weapons = table("EquipParamWeapon")
    lot_layout, lots = table("ItemLotParam")
    shop_layout, shops = table("ShopLineupParam")
    given = set()
    for row in lots.values():
        for k in range(1, 9):
            item = lot_layout.get(row, "lotItemId%02d" % k)
            if item > 0 and lot_layout.get(row, "lotItemCategory%02d" % k) == WEAPON:
                given.add(item // LEVEL * LEVEL)
    for row in shops.values():
        if shop_layout.get(row, "equipType") == WEAPON:
            given.add(shop_layout.get(row, "equipId") // LEVEL * LEVEL)
    melee, guns = [], []
    for weapon in sorted(given):
        row = weapons.get(weapon)
        if row is None:
            continue
        if weapon_layout.get(row, "wepmotionCategory") in NOT_A_MAIN_WEAPON:
            continue
        (guns if weapon_layout.get(row, LEFT_HAND) >= 0 else melee).append(weapon)
    free_rows = {r: shop_layout.get(shops[r], "equipId") // LEVEL * LEVEL for r in MELEE_ROWS + GUN_ROWS if r in shops}
    return {"melee": melee, "guns": guns, "rows": free_rows}


def _offered(melee, guns):
    return {"melee": MELEE_ROWS[:melee], "guns": GUN_ROWS[:guns]}


def hidden_rows(melee, guns):
    """The game's free rows no longer offered."""
    offered = _offered(melee, guns)
    return {r for kind, rows in (("melee", MELEE_ROWS), ("guns", GUN_ROWS)) for r in rows[:VANILLA[kind]] if r not in offered[kind]}


def added_rows(melee, guns):
    """{new row id: row whose body and flag it copies}."""
    offered = _offered(melee, guns)
    return {r: ADDED_FLAG_OF[kind] for kind in offered for r in offered[kind][VANILLA[kind]:]}


def plan(found, seed, opts_hash, melee=3, guns=2, shuffle=True):
    """{shop row id: weapon id}: distinct weapons, melee for the melee rows, firearms for the gun rows. With shuffle, every
    offered row is drawn; without, only the added rows, from weapons the game's own free rows do not already sell."""
    out = {}
    offered = _offered(melee, guns)
    for kind, pool in (("melee", found["melee"]), ("guns", found["guns"])):
        rows = offered[kind] if shuffle else offered[kind][VANILLA[kind]:]
        if not shuffle:
            pool = [w for w in pool if w not in found["rows"].values()]
        for row, weapon in zip(rows, Stream(seed, opts_hash, "starters", kind).shuffled(pool)):
            out[row] = weapon
    return out


def add_rows(param, added):
    """Adds the new free rows, each a copy of its list's first row (free, sold once, the same bought flag)."""
    bodies = param.first()
    param.insert_sorted([(row, bodies[source]) for row, source in sorted(added.items())])


def _levels(ids, base):
    """The ids of one weapon's upgrade levels (base + 100 * N, DATA): same weapon number and variant as the base."""
    return sorted(i for i in ids if i // LEVEL * LEVEL == base)


def copy_ids(weapon_ids, gen_ids, drawn):
    """{drawn weapon: base id of its starter copy}. The copy keeps the weapon number (id // 1000000) and takes the first
    variant (the two digits under it) from COPY_VARIANTS whose every upgrade level is free in both tables."""
    taken = set(weapon_ids) | set(gen_ids)
    out = {}
    for base in sorted(set(drawn)):
        offsets = [i - base for i in _levels(weapon_ids, base)]
        number = base // WEAPON_NUMBER * WEAPON_NUMBER
        for variant in COPY_VARIANTS:
            copy = number + variant * LEVEL
            if all(copy + o not in taken for o in offsets):
                out[base] = copy
                taken.update(copy + o for o in offsets)
                break
        else:
            raise ValueError("no free id for a starter copy of weapon %d" % base)
    return out


def hide_rows(param, rows):
    """Removes these shop rows: a shop lists the rows of its id range that exist (vanilla ranges have gaps)."""
    param.rows = [r for r in param.rows if r[0] not in rows]


def apply_shop(param, layout, mapping, copies):
    """The free rows sell the starter copies of the drawn weapons."""
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in mapping:
            row = bytearray(body)
            layout.set(row, "equipType", WEAPON)
            layout.set(row, "equipId", copies[mapping[row_id]])
            param.rows[i] = (row_id, bytes(row), name)


def apply_weapons(param, layout, copies):
    """Adds each copy's rows, one per upgrade level of the drawn weapon, without strength, skill, bloodtinge or arcane
    requirements. The drawn weapon's own rows, wherever else it is found or sold, stay as they were. Origin links into
    the weapon's own levels (each level names the one below it, DATA) point to the copy's levels."""
    rows = param.first()
    new = []
    for base, copy in copies.items():
        for row_id in _levels(rows, base):
            row = bytearray(rows[row_id])
            for field in REQUIREMENTS:
                layout.set(row, field, 0)
            for field in ORIGINS:
                linked = layout.get(row, field)
                if linked > 0 and linked // LEVEL * LEVEL == base:
                    layout.set(row, field, linked - base + copy)
            new.append((row_id - base + copy, bytes(row)))
    param.insert_sorted(new)
    return [i for i, _ in sorted(new)]


def apply_gems(param, layout, copies):
    """Gem slots: each weapon level has a WeaponGenParam row under its own id (DATA). Extra rows under small ids (14000
    -> 6000900) name a level in wepParamId; they are other generation variants and are not copied."""
    rows = param.first()
    new = []
    for base, copy in copies.items():
        for row_id in _levels(rows, base):
            row = bytearray(rows[row_id])
            weapon = layout.get(row, "wepParamId")
            if weapon // LEVEL * LEVEL == base:
                layout.set(row, "wepParamId", weapon - base + copy)
            new.append((row_id - base + copy, bytes(row)))
    param.insert_sorted(new)


def copy_text(fmg, copies):
    """Weapon name, description and lore of each drawn level, under the copy's id (`fmg` from containers.fmg_read)."""
    texts = fmg["texts"]
    for base, copy in copies.items():
        for text_id in _levels([i for i, t in texts.items() if t is not None], base):
            texts[text_id - base + copy] = texts[text_id]


# Nightwatch 0.3.0 `weapon_clip_groups` (docs/modding/weapon_clip_groups.md): a mod gives a weapon its own attack clip
# group in nightwatch/weapon_clip_groups.json (key: the weapon's base id, covering its +0..+9999 ids), used only when the
# mod's nightwatch.json lists the feature. Nocturne v3.0.0 does so for Reverie (162), Yorukage (163) and Vesper Blades
# (154), Uncanny and Lost too (DATA). A starter copy has new ids, so without a row of its own it would play its category's
# vanilla moves (Chikage, Blades of Mercy, Beast Claw): the world ships a row per such copy, same group as its origin.
CLIP_FEATURE = "weapon_clip_groups"
NIGHTWATCH_MANIFEST = "nightwatch.json"
CLIP_TABLE = "nightwatch/weapon_clip_groups.json"
CLIP_REQUIRES = "0.3.0"          # the first Nightwatch with the feature


def _json(data):
    try:
        out = json.loads(data.decode("utf-8-sig"))
    except (UnicodeDecodeError, ValueError):
        return None
    return out if isinstance(out, dict) else None


def _clip_rows(table):
    """The table's {weapon: clip group}, or None when the engine would drop it whole (wrong version, a bad row, a weapon
    listed twice: features.h ParseWeaponClipGroups)."""
    if table is None or table.get("version") != 1 or not isinstance(table.get("rows"), list):
        return None
    out = {}
    for row in table["rows"]:
        weapon = row.get("weapon") if isinstance(row, dict) else None
        group = row.get("clip_group") if isinstance(row, dict) else None
        if (type(weapon) is not int or type(group) is not int or not 10000 <= weapon <= 2147480000 or weapon % LEVEL
                or not 1 <= group <= 999 or weapon in out):
            return None
        out[weapon] = group
    return out


def clip_groups(files):
    """{weapon base id: clip group} of the base mods' weapon_clip_groups tables, merged as the engine does (load order, a
    later mod's row wins per weapon). A mod counts only when its nightwatch.json lists the feature; files read count in
    the input hash (GameFiles.read_beside)."""
    out = {}
    for index in range(1, len(files.mod_names) + 1):
        raw = files.read_beside(index, NIGHTWATCH_MANIFEST)
        manifest = _json(raw) if raw is not None else None
        features = manifest.get("features") if manifest else None
        if not isinstance(features, list) or CLIP_FEATURE not in features:
            continue
        raw = files.read_beside(index, CLIP_TABLE)
        rows = _clip_rows(_json(raw)) if raw is not None else None
        out.update(rows or {})
    return out


def copy_clip_groups(groups, copies):
    """[{"weapon": copy base id, "clip_group": its origin's group}] for the copies of weapons with a clip group, by id."""
    return [{"weapon": copy, "clip_group": groups[base]} for base, copy in sorted(copies.items(), key=lambda c: c[1])
            if base in groups]


def clip_files(rows, name):
    """{root-relative path: bytes}: the world's own nightwatch.json and weapon_clip_groups table for these rows; none
    without rows (a world on mods without clip groups needs no Nightwatch feature and keeps its files as before)."""
    if not rows:
        return {}
    manifest = {"id": "randomizer-world", "name": name, "requires": CLIP_REQUIRES, "features": [CLIP_FEATURE]}
    table = {"version": 1, "rows": rows}
    dump = lambda o: (json.dumps(o, indent=2) + "\n").encode("utf-8")
    return {NIGHTWATCH_MANIFEST: dump(manifest), CLIP_TABLE: dump(table)}


# Starting armour (option starter_armor, user 7 Nov: "you spawn with randomized armor", Forgotten armour included).
# DATA: CharaInitParam holds the ten origins twice, 2000-2009 and 3000-3009 (same stats). 2000-2009 wear the head piece
# 230000 (a hood, "Black Hood" per the user) with Foreign Garb 231000, Sullied Bandage 232000 and Foreign Trousers 233000; 3000-3009 the same
# without the head piece. LIVE (user 7 Nov, b9): a new character made with only 3000-3009 changed still woke in the vanilla
# set with the hood, so the game builds it from 2000-2009; both sets are dressed. Armour has no stat requirements.
ORIGIN_ROWS = (*range(2000, 2010), *range(3000, 3010))
PROTECTOR = 1                    # ShopLineupParam equipType and ItemLotParam category for armour
ARMOR_SLOTS = ("equip_Helm", "equip_Armer", "equip_Gaunt", "equip_Leg")  # EquipParamProtector protectorCategory 0-3


def armor_census(files):
    """[[head ids], [chest ids], [arms ids], [legs ids]]: armour the game gives out (item lots and shop rows, so a mod's
    armour too, e.g. Nocturne's Forgotten set), sorted."""
    table = _tables(files)
    armor_layout, armor = table("EquipParamProtector")
    lot_layout, lots = table("ItemLotParam")
    shop_layout, shops = table("ShopLineupParam")
    given = set()
    for row in lots.values():
        for k in range(1, 9):
            item = lot_layout.get(row, "lotItemId%02d" % k)
            if item > 0 and lot_layout.get(row, "lotItemCategory%02d" % k) == PROTECTOR:
                given.add(item)
    for row in shops.values():
        if shop_layout.get(row, "equipType") == PROTECTOR:
            given.add(shop_layout.get(row, "equipId"))
    out = [[] for _ in ARMOR_SLOTS]
    for item in sorted(given):
        if item in armor:
            slot = armor_layout.get(armor[item], "protectorCategory")
            if 0 <= slot < len(ARMOR_SLOTS):
                out[slot].append(item)
    return out


def armor_plan(found, seed, opts_hash):
    """[head, chest, arms, legs]: one piece drawn for each slot, the same for every origin (None: the slot stays)."""
    return [Stream(seed, opts_hash, "starter-armor", slot).shuffled(pool)[0] if pool else None
            for slot, pool in zip(ARMOR_SLOTS, found)]


def apply_armor(param, layout, outfit):
    """The origin rows start wearing `outfit` (armor_plan)."""
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in ORIGIN_ROWS:
            row = bytearray(body)
            for field, item in zip(ARMOR_SLOTS, outfit):
                if item is not None:
                    layout.set(row, field, item)
            param.rows[i] = (row_id, bytes(row), name)
