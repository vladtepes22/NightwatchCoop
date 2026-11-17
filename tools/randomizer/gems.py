"""Gem roulette (option gem_roulette, user 8 Nov 2026; PLAN 14): every blood gem a main map hands out, and half of its
small Blood Vial / Quicksilver Bullet pickups, become droplet blood gems with three effects the generator fixes. Each
gem is a new GemGenParam row with three type-1 (fixed) effects, so the game rolls nothing: the gem is the same for every
player of the world and is in the spoiler. Enemy gem drops list a few such gems of the enemy's tier by weight.

Facts this rests on (labels per project rule; game CUSA03173 1.09):
- DATA: a GemGenParam row's shape is its slotTypeRate column with a weight (A radial, B triangle, C waning, D circle,
  F droplet: vanilla's droplet pickups 125000, 126000-126002 weigh only F); an effect slot of type 1 gives that
  GemeffectParam row, type 2 draws one from a category. manifestRate 100 = the effect is always there.
- DATA: equipableSegmentWeapon marks blood gems, equipableSegmentBody Caryll and oath runes (1018 / 313 rows): runes
  are left alone.
- DATA: the menu name is GemGen.gemNameIdOffset + shape digit x 1e6 (droplet 6) + 1e5 when an effect is a curse + the
  first effect's gemNameIdOffset (engine docs save_layout.md, 0 misses on 77 gems; every droplet key used here has a
  name). So effect 1 names the gem.
- DATA: GemeffectParam categories are <kind><grade>00: grade 1-6 are value tables (Physical ATK UP rank 20: +4.5 %
  at grade 1, +22.5 % at grade 4, +28.1 % at grade 6), ranks 1-20 within. Vanilla's single-effect world gems use grade
  4 at ranks 1 (Old Yharnam) to 14 (Nightmare of Mensis): gems here use grade 4 and pick the rank by area tier.
- LIKELY: a main map adds no rank offset to a gem lot's id (engine items.md 10.1: the lot's id plus the area's offsets
  is the GemGen row). DATA for it: drop row 90150, rolled in four base maps, has no +1e6/+2e6 twin, and no DLC pickup
  row has one; LIVE for m27 (lot 2700180 gave gem 102001 itself).
- LIKELY: a droplet gem with three effects fits every weapon slot (one LIVE check planned)."""
from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
from gamedata import STEMS, map_variants
from seedcode import Stream
from shops import chain
from treasure import treasure_lots

GEM = 8  # ItemLotParam category: a GemGenParam row
GOODS = 4
SMALL_PICKUPS = (1000, 900)  # Blood Vial, Quicksilver Bullets: pickups of 1-2 may become gems (user, 8 Nov)
SMALL = 2
LOT_ITEMS = 8
GRADE = 4
EFFECTS = 3
TOP_RANK = 20
GEM_BASE = 380000  # free in vanilla (no GemGen row 300000-999999 but the 9xxxx drop rows); taken ids are skipped
DROPLET = "F"
SHAPE_DIGIT = 6  # the droplet's digit in the name key
CURSE_CHANCE = 15  # percent of gems that take a curse (option cursed_gems)
CURSE_BOOST = 3  # ranks the other two effects of a cursed gem gain
VARIANTS = 3  # pre-made gems per enemy drop entry
TRIES = 64
CHALICE_ROWS = 10000000  # LIKELY (chalice.py survey): a chalice dungeon NpcParam id is layer * 1e8 + 1e7 + base row
WEIGHT = {"A": 3, "B": 2, "C": 1}
# The kinds a gem may carry, by GemCategoryParam (excludeGroupId, isNegative): (family, element, what it does).
# Family A = main damage, B = strong utility or conditional damage, C = minor; curses are the isNegative kinds. Element:
# the damage a kind needs or adds (None: any weapon); one gem never mixes two elements. Every kind is DATA from its
# SpEffect row (bb.sqlite spe <id>, rank 20). Kinds left out: grade-4 categories with no category row or no effect text
# (cut content), and (400, 0) "Add blood ATK" (SpEffect 13420 sets only neutralAttackPower: what that does on a hunter's
# weapon is a GUESS, and no vanilla gem carries it).
KINDS = {
    (1700, 0): ("A", "phys", "Physical ATK UP %"),           # physicsAttackPowerRate
    (800, 0): ("A", "phys", "Add physical ATK"),              # physicsAttackPower
    (1300, 0): ("A", "blood", "Blood ATK UP %"),
    (1400, 0): ("A", "arcane", "Arcane ATK UP %"),
    (1500, 0): ("A", "fire", "Fire ATK UP %"),
    (1600, 0): ("A", "bolt", "Bolt ATK UP %"),
    (500, 0): ("A", "arcane", "Add arcane ATK"),              # weaponConvertAttribute 1 + magicAttackPower
    (600, 0): ("A", "fire", "Add fire ATK"),                  # converts to fire
    (700, 0): ("A", "bolt", "Add bolt ATK"),                  # converts to bolt
    (1800, 0): ("A", None, "ATK UP"),                         # every damage type
    (2400, 0): ("A", None, "ATK vs beasts UP"),               # antiDemonDamageCorrectRate
    (2600, 0): ("A", None, "ATK vs the kin UP"),              # antiWeakB_DamageCorrectRate
    (1100, 0): ("B", "phys", "Blunt ATK UP"),                 # blowAttackPowerRate: blunt hits only
    (1200, 0): ("B", "phys", "Thrust ATK UP"),
    (2000, 0): ("B", "phys", "STR scaling"),                  # changeStrengthPoint
    (2100, 0): ("B", "phys", "SKL scaling"),
    (2200, 0): ("B", "blood", "Bloodtinge scaling"),
    (2300, 0): ("B", "arcane", "Arcane scaling"),
    (3000, 0): ("B", None, "ATK vs open foes UP"),
    (3300, 0): ("B", "phys", "Charge ATKs UP"),               # physicsAttackPowerRate on charge attacks only
    (3900, 0): ("B", "phys", "Phys. UP at full HP"),
    (4000, 0): ("B", "phys", "Phys. UP near death"),
    (4200, 0): ("B", None, "ATK UP at full HP"),
    (4300, 0): ("B", None, "ATK UP near death"),
    (2800, 0): ("B", None, "Add slow poison"),
    (2900, 0): ("B", None, "Add rapid poison"),
    (3100, 0): ("B", None, "Reduces stamina costs"),
    (3400, 0): ("B", None, "HP continues to recover"),
    (3600, 0): ("B", None, "Boosts rally potential"),
    (3700, 0): ("C", None, "WPN durability UP"),
    (1800, 1): ("X", None, "ATK DOWN"),
    (2400, 1): ("X", None, "ATK vs beasts DOWN"),
    (2600, 1): ("X", None, "ATK vs the kin DOWN"),
    (3100, 1): ("X", None, "Increases stamina costs"),
    (3400, 1): ("X", None, "HP gradually depletes"),
    (3700, 1): ("X", None, "WPN durability DOWN"),
}
# DATA: Arcane scaling's grade-4 rank 20 row (SpEffect 32420) gives +65 against +22.5 for every other scaling kind
RANK_CAP = {(2300, 0): 19}
# Area tiers in the order a first playthrough reaches them; bands of effect rank (low, high). DATA for the scale:
# vanilla's world gems are grade 4 rank 1-7 in Central Yharnam / Cathedral Ward, 7-8 in the Forbidden Woods, 9 in
# Cainhurst, 12-14 in Yahar'gul and Mensis, 15 in the Research Hall, 16-17 in the Fishing Hamlet.
TIERS = {"m24_01": 1, "m21_00": 1, "m24_00": 2, "m23_00": 2, "m22_00": 3, "m27_00": 3, "m32_00": 4,
         "m25_00": 4, "m28_00": 5, "m21_01": 5, "m24_02": 5, "m33_00": 5, "m34_00": 6, "m35_00": 6,
         "m26_00": 7, "m36_00": 7}
BANDS = {1: (1, 4), 2: (3, 6), 3: (5, 8), 4: (7, 10), 5: (9, 12), 6: (11, 14), 7: (14, 17)}
DEFAULT_TIER = 4
# score = sum of family weight x rank; a gem outside (FLOOR x low, CAP x high) of its band is drawn again
FLOOR, CAP = 6, 7
GUARANTEED = ("phys", "fire", "bolt", "arcane")  # each world has one pickup gem led by each


def _tables(files):
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])

    def table(name):
        return Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3]), Param(bnd4_find(bnd, "\\%s.param" % name)[3]).first()
    return table


def census(files):
    """What the roulette draws from, read from the game and its mods: {'kinds': {key: {'category', 'effects': {rank: id},
    'names': {rank: name offset}}}, 'template': GemGen row bytes, 'gen_layout', 'blood': GemGen ids of blood gems,
    'taken': every GemGen id, 'lot_layout', 'lots'}."""
    table = _tables(files)
    gen_layout, gen = table("GemGenParam")
    effect_layout, effects = table("GemeffectParam")
    category_layout, categories = table("GemCategoryParam")
    lot_layout, lots = table("ItemLotParam")
    by_category = {}
    for effect_id, row in effects.items():
        if effect_layout.get(row, "sortCategory") == 0:
            by_category.setdefault(effect_layout.get(row, "categoryId"), {})[effect_layout.get(row, "effectRank")] = (
                effect_id, effect_layout.get(row, "gemNameIdOffset"))
    kinds = {}
    for cat in sorted(categories):
        row = categories[cat]
        key = (category_layout.get(row, "excludeGroupId"), category_layout.get(row, "isNegative"))
        ranks = by_category.get(cat, {})
        if (key not in KINDS or key in kinds or cat // 100 % 10 != GRADE or cat % 100
                or category_layout.get(row, "manifestRate") <= 0
                or not all(category_layout.get(row, "enableSlotType" + c) for c in "ABCDEF")  # droplet: every slot
                or any(r not in ranks for r in range(1, TOP_RANK + 1))):
            continue
        kinds[key] = {"category": cat, "effects": {r: ranks[r][0] for r in range(1, TOP_RANK + 1)},
                      "names": {r: ranks[r][1] for r in range(1, TOP_RANK + 1)}}

    def rates(row):
        return {c: gen_layout.get(row, "slotTypeRate" + c) for c in "ABCDEF"}
    blood = {i for i, row in gen.items() if gen_layout.get(row, "equipableSegmentWeapon")
             and not gen_layout.get(row, "equipableSegmentBody")}
    template = next((gen[i] for i in sorted(blood)
                     if gen_layout.get(gen[i], "gemNameIdOffset") == 0
                     and [c for c, v in rates(gen[i]).items() if v > 0] == [DROPLET]
                     and gen_layout.get(gen[i], "gemeffectGenParamType_0") == 1), None)
    if template is None:
        raise RuntimeError("no droplet blood gem row to copy (GemGenParam)")
    if not any(f == "A" for f, _, _ in (KINDS[k] for k in kinds)):
        raise RuntimeError("no gem effect kinds found (GemCategoryParam / GemeffectParam)")
    return {"kinds": kinds, "template": template, "gen_layout": gen_layout, "blood": blood, "taken": set(gen),
            "lot_layout": lot_layout, "lots": lots, "name_offset": gen_layout.get(template, "gemNameIdOffset")}


def pickup_stems(files, mapping=None):
    """{lot: map its pickup ends up in}: every treasure lot of the main maps, through the treasure shuffle's
    {spot: lot now there}."""
    where = {}
    for stem in STEMS:
        for variant in map_variants(files, stem):
            for _, lot in treasure_lots(dcx_read(files.read("map/mapstudio/%s.msb.dcx" % variant))[0]):
                where.setdefault(lot, stem)
    out = {}
    for spot, stem in where.items():
        lot = (mapping or {}).get(spot, spot)
        if lot not in out or TIERS.get(stem, DEFAULT_TIER) < TIERS.get(out[lot], DEFAULT_TIER):
            out[lot] = stem
    return out


def drop_stems(census, changes, slot_row, new_rows, drop_plan=None):
    """{lot: maps whose enemies drop it}, for the NpcParam rows the world ends with (clones, then the drop shuffle).
    Lots a chalice dungeon row also names are left out: the dungeons' gems stay theirs (chalice rows: CHALICE_ROWS)."""
    layout = census.npc_layout
    fields = [f for f in ("itemLotId_%d" % k for k in range(1, 9)) if f in layout.fields]
    made = dict(new_rows)
    drop_plan = drop_plan or {}
    out = {}
    for stem in STEMS:
        for s in census.slots[stem]:
            body = made[slot_row[(stem, s["part"])]] if s["part"] in changes.get(stem, {}) else census.npc_rows.get(s["npc"])
            if body is None:
                continue
            for f in fields:
                lot = layout.get(body, f)
                if lot > 0:
                    out.setdefault(drop_plan.get(lot, lot), set()).add(stem)
    elsewhere = {layout.get(row, f) for npc, row in census.npc_rows.items() if npc >= CHALICE_ROWS for f in fields}
    return {lot: stems for lot, stems in out.items() if lot not in elsewhere}


def tier_of(stems):
    return min(TIERS.get(s, DEFAULT_TIER) for s in stems)


def _weighted(stream, items, weights):
    total = sum(weights)
    pick = stream.below(total)
    for item, w in zip(items, weights):
        if pick < w:
            return item
        pick -= w
    return items[-1]


def draw(found, stream, tier, element=None, cursed=False):
    """One gem: {'kinds', 'ranks', 'effects', 'cursed', 'element', 'tier', 'name'}. Effect 1 is family A (of `element`
    when given), 2-3 by family weight; never two kinds of one exclude group, never two elements; ranks from the tier's
    band, drawn again while the score is outside the band's floor and cap (the nearest of TRIES is kept)."""
    kinds = found["kinds"]
    positive = sorted(k for k in kinds if KINDS[k][0] != "X")
    curses = sorted(k for k in kinds if KINDS[k][0] == "X")
    low, high = BANDS[tier]
    best = None
    for _ in range(TRIES):
        leads = [k for k in positive if KINDS[k][0] == "A" and (element is None or KINDS[k][1] == element)]
        chosen = [leads[stream.below(len(leads))]]
        for _slot in range(EFFECTS - 1):
            groups = {k[0] for k in chosen}
            have = {KINDS[k][1] for k in chosen} - {None}
            pool = [k for k in positive if k[0] not in groups and (KINDS[k][1] is None or not have or KINDS[k][1] in have)]
            chosen.append(_weighted(stream, pool, [WEIGHT[KINDS[k][0]] for k in pool]))
        ranks = [min(low + stream.below(high - low + 1), RANK_CAP.get(k, TOP_RANK)) for k in chosen]
        score = sum(WEIGHT[KINDS[k][0]] * r for k, r in zip(chosen, ranks))
        miss = max(FLOOR * low - score, score - CAP * high, 0)
        if best is None or miss < best[0]:
            best = (miss, chosen, ranks)
        if miss == 0:
            break
    _, chosen, ranks = best
    curse = False
    if cursed and curses and stream.percent(CURSE_CHANCE):
        at = 1 + stream.below(EFFECTS - 1)
        others = [k for i, k in enumerate(chosen) if i != at]
        pool = [k for k in curses if k[0] not in {o[0] for o in others}]
        if pool:
            chosen[at] = pool[stream.below(len(pool))]
            ranks = [low + stream.below(high - low + 1) if i == at else min(r + CURSE_BOOST, RANK_CAP.get(chosen[i], TOP_RANK))
                     for i, r in enumerate(ranks)]
            curse = True
    lead = next((KINDS[k][1] for k in chosen if KINDS[k][1]), None)
    return {"kinds": chosen, "ranks": ranks, "effects": [kinds[k]["effects"][r] for k, r in zip(chosen, ranks)],
            "cursed": curse, "element": lead, "tier": tier,
            "name": found["name_offset"] + SHAPE_DIGIT * 1000000 + (100000 if curse else 0)
            + kinds[chosen[0]]["names"][ranks[0]]}


def _entries(layout, row):
    return [(k, layout.get(row, "lotItemId%02d" % k), layout.get(row, "lotItemCategory%02d" % k),
             layout.get(row, "lotItemNum%02d" % k), layout.get(row, "lotItemBasePoint%02d" % k))
            for k in range(1, LOT_ITEMS + 1)]


def plan(found, seed, options_hash, pickups, drops, cursed=False, skip=frozenset()):
    """pickups: pickup_stems(); drops: drop_stems(); skip: lots another pass rewrote (shop trades).
    Returns {'gems': {gem id: gem}, 'lots': {lot row: {entry k: [(gem id, weight)]}}, 'where': {gem id: (kind, lot,
    stems)}}: a pickup entry holds one gem, a drop entry its weight split over up to VARIANTS gems."""
    layout, lots, blood = found["lot_layout"], found["lots"], found["blood"]
    gem_entries, small = {}, []
    for lot, stem in sorted(pickups.items()):
        if lot in skip:
            continue
        rows = chain(lots, lot)
        for r in rows:
            for k, item, cat, _, _ in _entries(layout, lots[r]):
                if cat == GEM and item in blood:
                    tier = TIERS.get(stem, DEFAULT_TIER)
                    gem_entries[(r, k)] = min(tier, gem_entries.get((r, k), tier))
        items = [e for e in _entries(layout, lots[lot]) if e[1] > 0] if len(rows) == 1 else []
        if len(items) == 1 and items[0][2] == GOODS and items[0][1] in SMALL_PICKUPS and 1 <= items[0][3] <= SMALL:
            small.append((lot, items[0][0]))
    # which half: by lot alone, so the treasure shuffle moves the same gems around
    for lot, k in Stream(seed, options_hash, "gems", "small").shuffled(small)[:len(small) // 2]:
        gem_entries.setdefault((lot, k), TIERS.get(pickups[lot], DEFAULT_TIER))
    pickup_keys = sorted(gem_entries)
    leads = {KINDS[k][1] for k in found["kinds"] if KINDS[k][0] == "A"}
    order = Stream(seed, options_hash, "gems", "guarantee").shuffled(pickup_keys)
    forced = dict(zip(order, [e for e in GUARANTEED if e in leads]))
    drop_tier = {}
    for lot, stems in sorted(drops.items()):
        for r in chain(lots, lot):
            if any(key[0] == r for key in pickup_keys):
                continue
            for k, item, cat, _, _ in _entries(layout, lots[r]):
                if cat == GEM and item in blood:
                    drop_tier[(r, k)] = min(tier_of(stems), drop_tier.get((r, k), 99))

    taken = set(found["taken"])
    next_id = [GEM_BASE]

    def new_id():
        while next_id[0] in taken:
            next_id[0] += 1
        taken.add(next_id[0])
        return next_id[0]
    out = {"gems": {}, "lots": {}, "where": {}}
    stem_of = {}
    for lot, stem in pickups.items():
        for r in chain(lots, lot):
            stem_of.setdefault(r, stem)
    for r, k in pickup_keys:
        gem = draw(found, Stream(seed, options_hash, "gems", "pickup", r, k), gem_entries[(r, k)], forced.get((r, k)), cursed)
        gem_id = new_id()
        out["gems"][gem_id] = gem
        out["where"][gem_id] = ("pickup", r, [stem_of.get(r, "")])
        out["lots"].setdefault(r, {})[k] = [(gem_id, layout.get(lots[r], "lotItemBasePoint%02d" % k))]
    lot_stems = {}
    for lot, stems in drops.items():
        for r in chain(lots, lot):
            lot_stems.setdefault(r, set()).update(stems)
    for r in sorted({r for r, _ in drop_tier}):
        row = lots[r]
        free = [k for k, item, _, _, points in _entries(layout, row) if item <= 0 and points == 0]
        for k in sorted(k for rr, k in drop_tier if rr == r):
            weight = layout.get(row, "lotItemBasePoint%02d" % k)
            n = max(1, min(VARIANTS, 1 + len(free), weight))
            shares = [weight // n + (1 if i < weight % n else 0) for i in range(n)]
            entry = []
            for i in range(n):
                gem = draw(found, Stream(seed, options_hash, "gems", "drop", r, k, i), drop_tier[(r, k)], None, cursed)
                gem_id = new_id()
                out["gems"][gem_id] = gem
                out["where"][gem_id] = ("drop", r, sorted(lot_stems.get(r, ())))
                entry.append((gem_id, shares[i]))
            out["lots"].setdefault(r, {})[k] = entry
            free = free[n - 1:]
    return out


def lot_rows(found, gem_plan, rows=None):
    """{lot row: new bytes}: each planned entry gives its first gem; a drop's further gems take free entries, copying
    the entry's count, flags and luck. rows: the lot rows to start from (default the input's)."""
    layout = found["lot_layout"]
    rows = rows or found["lots"]
    per_entry = ("lotItemCategory%02d", "lotItemNum%02d", "getItemFlagId%02d", "enableLuck%02d", "cumulateReset%02d")
    out = {}
    for r, entries in sorted(gem_plan["lots"].items()):
        row = bytearray(rows[r])
        free = [k for k, item, _, _, points in _entries(layout, row) if item <= 0 and points == 0]
        for k, gems in sorted(entries.items()):
            for i, (gem_id, weight) in enumerate(gems):
                at = k if i == 0 else free.pop(0)
                if i:
                    for f in per_entry:
                        layout.set(row, f % at, layout.get(row, f % k))
                layout.set(row, "lotItemId%02d" % at, gem_id)
                layout.set(row, "lotItemCategory%02d" % at, GEM)
                layout.set(row, "lotItemNum%02d" % at, 1)
                if len(gems) > 1:
                    layout.set(row, "lotItemBasePoint%02d" % at, weight)
                    cumulate = layout.get(row, "cumulateLotPoint%02d" % k)
                    layout.set(row, "cumulateLotPoint%02d" % at, cumulate * weight // max(1, sum(w for _, w in gems)))
        out[r] = bytes(row)
    return out


def gen_rows(found, gem_plan):
    """[(GemGen id, bytes)]: the template droplet row with the gem's three effects fixed."""
    layout = found["gen_layout"]
    out = []
    for gem_id, gem in sorted(gem_plan["gems"].items()):
        row = bytearray(found["template"])
        for i in range(6):
            effect = gem["effects"][i] if i < len(gem["effects"]) else -1
            layout.set(row, "gemeffectGenParamType_%d" % i, 1 if effect >= 0 else 0)
            layout.set(row, "gemeffectGenParam_%d" % i, effect)
            layout.set(row, "manifestRate_%d" % i, 100.0 if effect >= 0 else 0.0)
            layout.set(row, "negativizeRate_%d" % i, 0.0)
        out.append((gem_id, bytes(row)))
    return out


def apply(gen_param, lot_param, found, gem_plan):
    """Adds the gem rows and rewrites the lot rows in place (the first row of each id, the one the game finds)."""
    gen_param.insert_sorted(gen_rows(found, gem_plan))
    current = lot_param.first()
    new = lot_rows(found, gem_plan, current)
    done = set()
    for i, (row_id, body, name) in enumerate(lot_param.rows):
        if row_id in new and row_id not in done:
            lot_param.rows[i] = (row_id, new[row_id], name)
            done.add(row_id)
