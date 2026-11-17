"""The spoiler in words, for the launcher's viewer: area and item names from the game's own English text (the base mods'
text when they ship it). Files are only peeked, so the spoiler never changes the input hash. Normal enemies have no name
in the game's text; they stay model numbers, and named ones (bosses, NPCs) get their name."""
from collections import Counter

from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read, fmg_read
from gamedata import STEMS, map_variants
from shops import BLOCK, SHOPS, chain, deal_price, entries, price
from treasure import treasure_lots

TEXT = "msg/engus/item.msgbnd.dcx"
MENU_TEXT = "msg/engus/menu.msgbnd.dcx"
GEM_NAMES, GEM_EFFECTS = 31, 35  # menu.msgbnd: blood gem names, effect texts (by GemeffectParam id)
NAME_FILES = {0: 11, 1: 12, 2: 13, 4: 10}  # ItemLotParam category -> item.msgbnd file: weapons, armour, accessories, goods
SHOP_TYPES = {0: 0, 1: 1, 2: 2, 3: 4}      # ShopLineupParam equipType -> the same categories
NPC_NAMES, PLACE_NAMES = 18, 19
HUMAN = "c0000"
GEM_GEN, GEM_TEXT = 8, "a random blood gem"  # DOC (paramdef enum): category 8 = GemGen, a gem made on the spot
LEVEL = 10000                               # weapon and armour ids carry their level or variant below this


def _texts(files):
    data = files.peek(TEXT)
    if data is None:
        return {}
    return {e[1]: fmg_read(e[3])["texts"] for e in bnd4_read(dcx_read(data)[0])["entries"]}


def _tables(files):
    bnd = bnd4_read(dcx_read(files.peek("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.peek("paramdef/paramdef.paramdefbnd.dcx"))[0])

    def table(name):
        return Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3]), Param(bnd4_find(bnd, "\\%s.param" % name)[3]).first()
    return table


def gem_texts(files, gem_plan):
    """{gem id: 'Name (effect; effect; effect)'} of the roulette's gems (gems.py), from the game's own text."""
    data = files.peek(MENU_TEXT)
    menu = {e[1]: fmg_read(e[3])["texts"] for e in bnd4_read(dcx_read(data)[0])["entries"]} if data else {}
    names, effects = menu.get(GEM_NAMES, {}), menu.get(GEM_EFFECTS, {})
    out = {}
    for gem_id, gem in (gem_plan or {}).get("gems", {}).items():
        name = (names.get(gem["name"]) or "Blood gem #%d" % gem_id).strip()
        said = [(effects.get(e) or "#%d" % e).strip() for e in gem["effects"]]
        out[gem_id] = "%s (%s)" % (name, "; ".join(said))
    return out


def area_name(texts, stem):
    """m24_01 -> place name 2410 (LIKELY: place ids are map * 100 + part * 10; every main map has one)."""
    number = int(stem[1:3]) * 100 + int(stem[4:6]) * 10
    return (texts.get(PLACE_NAMES, {}).get(number) or stem).strip()


def item_name(texts, category, item):
    names = texts.get(NAME_FILES.get(category), {})
    for key in (item, item - item % LEVEL):
        if names.get(key):
            return names[key].strip()
    return "#%d" % item


def lot_text(texts, layout, rows, lot, gem_names=None):
    """What a lot gives: 'Pebble x4'; a lot that draws one entry by weight (enemy drops) gives each chance,
    'Blood Vial x3 92%, Pebble 8%', with 'nothing' for the empty entry's weight. '(nothing)' for an empty lot."""
    row = rows.get(lot)
    if row is None:
        return "#%d" % lot
    entries, total = [], 0
    for k in range(1, 9):
        item = layout.get(row, "lotItemId%02d" % k)
        points = layout.get(row, "lotItemBasePoint%02d" % k)
        total += points
        if item > 0:
            category = layout.get(row, "lotItemCategory%02d" % k)
            name = (gem_names or {}).get(item, GEM_TEXT) if category == GEM_GEN else item_name(texts, category, item)
            count = layout.get(row, "lotItemNum%02d" % k)
            entries.append((name + (" x%d" % count if count > 1 else ""), points))
        elif points > 0:
            entries.append(("nothing", points))
    if not any(name != "nothing" for name, _ in entries):
        return "(nothing)"
    if len(entries) == 1 or total <= 0:
        return ", ".join(name for name, _ in entries)
    return ", ".join("%s %d%%" % (name, round(points * 100 / total)) for name, points in entries if points > 0)


def pickup_text(texts, layout, rows, lot, gem_names=None):
    """What one map pickup of lot gives: each row of its chain (lot, lot+1, ... while rows exist), 'Hunter Hat, Hunter Garb'."""
    found = chain(rows, lot)
    return ", ".join(lot_text(texts, layout, rows, r, gem_names) for r in found) if found else "#%d" % lot


def build(files, census, changes, bosses, lots, shop_plan, drop_plan, starter_plan, plan=None, shop_found=None, shop_deal=None,
          gem_plan=None, gem_rows=None):
    """plan: the planner's result, for each boss line's arena, mode and fighter. shop_plan: {(first block, index): lot row}
    of the shop trades, with shop_found its census (shops.census), for the items and prices; shop_deal: shops.deal.
    gem_plan: the gem roulette's plan (gems.plan), gem_rows: the lot rows it rewrote ({row: bytes}, gems.lot_rows)."""
    gem_rows = gem_rows or {}
    texts = _texts(files)
    table = _tables(files)
    out = {"areas": {stem: area_name(texts, stem) for stem in STEMS}}

    npc_names = texts.get(NPC_NAMES, {})
    models = {}
    for slots in census.slots.values():
        for s in slots:
            name = npc_names.get(s["npc"])
            if name and s["model"] != HUMAN:  # every human NPC shares one model
                models.setdefault(s["model"], Counter())[name.strip()] += 1
    out["models"] = {m: c.most_common(1)[0][0] for m, c in sorted(models.items())}

    out["enemies"] = {}
    for stem in STEMS:
        pairs = Counter()
        home = {s["part"]: s["model"] for s in census.slots[stem]}
        for part, change in changes[stem].items():
            pairs[(home[part], change[0])] += 1
        if pairs:
            out["enemies"][stem] = [[a, b, n] for (a, b), n in sorted(pairs.items(), key=lambda p: (-p[1], p[0]))]
    # [stem, from model, to model, arena key, mode, fighter key]: the viewer reads the first three
    arena_of = {s["entity"]: s["arena"] for slots in census.slots.values() for s in slots}
    plan = plan or {}
    out["bosses"] = [[stem, next(s["model"] for s in census.slots[stem] if s["part"] == part), changes[stem][part][0],
                      arena_of.get(entity), plan.get("arena_modes", {}).get(arena_of.get(entity)),
                      plan.get("boss_fighters", {}).get(entity)]
                     for entity, (stem, part) in sorted(bosses.items())]

    lot_layout, lot_rows = table("ItemLotParam")
    shop_layout, shop_rows = table("ShopLineupParam")
    # the lot rows as the world has them: a traded pickup row gives the item its shop row sold
    world_rows = dict(lot_rows)
    for (first, index), lot in sorted((shop_plan or {}).items()):
        row = bytearray(world_rows[lot])
        for k in range(1, 9):
            if lot_layout.get(row, "lotItemId%02d" % k) > 0:
                lot_layout.set(row, "lotItemId%02d" % k, shop_layout.get(shop_rows[first * BLOCK + index], "equipId"))
                break
        world_rows[lot] = bytes(row)
    world_rows.update(gem_rows)  # never a traded row (gems.plan skips them)
    gem_names = gem_texts(files, gem_plan) if gem_plan else {}
    traded = {lot for lot in (shop_plan or {}).values()}
    if lots or traded or gem_rows:
        where = {}
        for stem in STEMS:
            for variant in map_variants(files, stem):
                for _, lot in treasure_lots(dcx_read(files.peek("map/mapstudio/%s.msb.dcx" % variant))[0]):
                    where.setdefault(lot, stem)
        # every spot whose pickup changed: its lot moved (treasure) or a row of the lot now there was traded (shops)
        out["treasure"] = [[where[spot], pickup_text(texts, lot_layout, lot_rows, spot),
                            pickup_text(texts, lot_layout, world_rows, now, gem_names)]
                           for spot in sorted(where) for now in [(lots or {}).get(spot, spot)]
                           if spot != now or (traded | set(gem_rows)) & set(chain(lot_rows, now))]
    if drop_plan or gem_rows:
        droppers = {}
        layout = census.npc_layout
        for stem in STEMS:
            for s in census.slots[stem]:
                row = census.npc_rows.get(s["npc"])
                if row is not None:  # a moved lot is only ever a free slot's (drops.census); a gem lot may be anyone's
                    for k in range(1, 6):
                        droppers.setdefault(layout.get(row, "itemLotId_%d" % k), set()).add(s["model"])
        # each row of the lot's chain: an enemy drops lot+1 too (DATA: 3401730 is a unique item with its flag, 3401731 the
        # enemy's usual weighted drop)
        out["drops"] = [[sorted(droppers.get(lot, ())), pickup_text(texts, lot_layout, lot_rows, lot),
                         pickup_text(texts, lot_layout, world_rows, now, gem_names)]
                        for lot in sorted(set(drop_plan or {}) | set(droppers)) if lot > 0
                        for now in [(drop_plan or {}).get(lot, lot)] if lot != now or set(gem_rows) & set(chain(lot_rows, now))]
    if gem_plan:
        # [where, gem name and effects, cursed]: every gem the roulette made (pickups, then enemy drops)
        out["gems"] = [[", ".join(out["areas"].get(stem, stem) for stem in stems) + ("" if kind == "pickup" else " (enemy drop)"),
                        gem_names.get(gem_id, "#%d" % gem_id), int(gem_plan["gems"][gem_id]["cursed"])]
                       for gem_id, (kind, _, stems) in sorted(gem_plan["where"].items(), key=lambda w: (w[1][0] != "pickup", w[0]))]

    def sold(row_id):
        row = shop_rows.get(row_id)
        if row is None:
            return "#%d" % row_id
        return item_name(texts, SHOP_TYPES.get(shop_layout.get(row, "equipType"), -1), shop_layout.get(row, "equipId"))
    if shop_plan:
        # [shop 1-4, sold before, sold now, price before, price now, map the old item went to]: the viewer reads the first
        # three; prices are the shop's first copy (later copies of the Bath shop scale their own)
        numbers = {blocks[0]: n for n, blocks in enumerate(SHOPS, start=1)}
        out["shops"] = []
        for (first, index), lot in sorted(shop_plan.items()):
            row = shop_rows[first * BLOCK + index]
            kind, own = shop_layout.get(row, "equipType"), shop_layout.get(row, "value")
            new = entries(lot_layout, lot_rows[lot])[0][1]
            spot = (shop_found or {}).get("spots", {}).get(lot, ("", lot))
            out["shops"].append([numbers[first], sold(first * BLOCK + index), item_name(texts, SHOP_TYPES[kind], new), own,
                                 price(shop_found, first, kind, new, own) if shop_found else own, spot[0]])
    if shop_deal and shop_found:
        numbers = {blocks[0]: n for n, blocks in enumerate(SHOPS, start=1)}
        out.setdefault("shops", [])
        for (first, index), origin in sorted(shop_deal.items()):
            item, _, kind = shop_found["deck_items"][origin]
            own = shop_found["deck_items"][(first, index)][1]
            if item == shop_found["deck_items"][(first, index)][0]:
                continue  # the same item again (another shop's row of it)
            out["shops"].append([numbers[first], sold(first * BLOCK + index), item_name(texts, SHOP_TYPES[kind], item),
                                 own, deal_price(shop_found, first, origin, own), ""])
    if starter_plan:
        out["starters"] = [[sold(row), item_name(texts, 0, weapon)] for row, weapon in sorted(starter_plan.items())]
    return out
