"""Shop shuffle (user, 7 Nov: "all, except Blood Vial and Quicksilver Bullets have to be randomized").

Trades: weapon and armour rows (buy-once and unlimited alike) and buy-once goods rows trade items with unique world
pickups of the same type. The pickup now gives the item the shop sold, and the row sells the pickup's item at a price
that fits it. A row keeps its place, its unlock condition (qwcId), its bought-once flag and its stock; only the item and
the price change. Unlimited goods rows trade only when they sell a chalice ritual material and the chalice materials
option shuffles those too. An armour row that resells a piece some lot gives (a set found whole, then sold piece by
piece) never trades, so no piece lies alone in a pickup while its set's lot still gives it; it is dealt instead.

Deals (`deal`): every weapon and armour row no trade took, and the unlimited goods rows (user, 6 Nov: "shop shuffled
but not the goods"), are dealt out again across all the shops, echo and insight alike, each type among its own rows. A
row keeps its place, its unlock condition and its stock; it sells another shop row's item at a price for this shop's
currency and tier (`deal_price`). No shop sells one item from two rows after a deal, where a swap can avoid it. Blood
Vials and Quicksilver Bullets, required goods and chalice ritual materials stay where they are. (Buy-once goods rows are
nearly all key items: of each shop's nine, only the Sinister Resonant Bell trades; checked 6 Nov.)

Blood stone floor (`STONES`, user 9 Nov: "anything after gascoigne at still +0 is just unplayable"): the Insight shop's
four blood stone rows leave the deal and sell their stone, unlimited, for a fixed insight price once a boss arena is
cleared (its defeat flag, so with boss swaps whoever stands there). The Bath Messengers' stone rows are dealt like any
consumable, so a seed can still sell Twin Shards early; the deal counts the pinned stones as sold in the Insight shop, so
it never sells one twice. `apply_stones` runs whenever an item shuffle (treasure, shops, drops) can move stones.

Words used here (DATA, vanilla tables):
- buy-once row: sellQuantity 1 with a bought flag (eventFlag > 0). Unlimited rows: sellQuantity -1 (every weapon row).
- unique world pickup: one ItemLotParam row reached by a main-map treasure (the placed lot or a row of its chain: a
  pickup gives lot, lot+1, ... while rows exist) that holds exactly one item, count 1, of a type a shop sells (weapon,
  armour, goods), whose item is in no other ItemLotParam row and no ShopLineupParam row, placed in one map only once,
  not reached by any enemy's lot, nor by a lot an event awards, and not pinned by the treasure rules (required goods,
  an event knows its flag, no item row). Boss drops and quest rewards are lots events award or lots an NPC drops, so
  they never count; key items are required goods, so they never trade either way.
- the price that fits (per shop copy, so each tier keeps its own scale): the item's own value (its EquipParam
  sellValue) times the median price/sellValue of that copy's buy-once rows of the same type (all its rows of that type
  when no buy-once row has a value), rounded to two digits, kept inside that copy's lowest and highest price for the
  type. A unique pickup is sold in no shop, so no vanilla shop price exists for it. Without a value the row keeps its price.
"""
from collections import Counter
from fractions import Fraction

from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
from gamedata import STEMS, map_variants
from arenas import BY_KEY
from seedcode import Stream
from treasure import _event_values, lot_pins, required_goods, treasure_lots

# Each shop is written once per block with the same items at the same indexes; other blocks (the free first weapons,
# and three rows no shop is known to use) are not shops to trade.
SHOPS = ((10, 11, 12, 13, 14), (20, 21, 22, 23, 24), (51, 52, 53, 54), (61, 62, 63, 64))
BLOCK = 10000
WEAPON, ARMOUR, GOODS = 0, 1, 3  # ShopLineupParam's equipType
TYPES = {0: 0, 1: 1, 3: 4}  # ShopLineupParam equipType -> ItemLotParam category: weapon, armour, goods
VALUES = {0: "EquipParamWeapon", 1: "EquipParamProtector", 3: "EquipParamGoods"}
LOT_ITEMS = 8
STAPLES = frozenset((1000, 900))  # Blood Vial, Quicksilver Bullets: never dealt, so every run can buy them from the start
INSIGHT = 20  # the Insight shop's first block (DATA 9 Nov: t210698.esd in m21 opens rows 200000-200099)
# goods -> (insight price, arena whose defeat flag shows the row) (user 9 Nov); the stones' vanilla Insight rows ask 1/2/20/60
STONES = {3000: (1, "cleric_beast"), 3010: (2, "shadows"), 3020: (5, "one_reborn"), 3030: (10, "wet_nurse")}
NPC_LOTS = tuple("itemLotId_%d" % k for k in range(1, 7)) + ("humanityLotId",)
ITEM_LOTS = ("vagrantItemLotId", "vagrantBonusEneDropItemLotId", "vagrantItemEneDropItemLotId")


def _reader(files):
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])

    def table(name):
        return Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3]), Param(bnd4_find(bnd, "\\%s.param" % name)[3]).first()
    return table


def _tables(files):
    return _reader(files)("ShopLineupParam")


def stone_rows(layout, rows):
    """{(INSIGHT, index): stone}: the Insight shop's rows that sell a blood stone (`STONES`), by its first block."""
    return {(INSIGHT, rid % BLOCK): layout.get(row, "equipId") for rid, row in sorted(rows.items())
            if rid // BLOCK == INSIGHT and layout.get(row, "equipType") == GOODS and layout.get(row, "equipId") in STONES}


def apply_stones(param, layout, stones):
    """Rewrites every copy of the Insight shop's stone rows: unlimited, `STONES`' price, shown by the arena's defeat flag."""
    blocks = next(b for b in SHOPS if b[0] == INSIGHT)
    wanted = {block * BLOCK + index: STONES[item] for (_, index), item in stones.items() for block in blocks}
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in wanted:
            price, arena = wanted[row_id]
            row = bytearray(body)
            layout.set(row, "value", price)
            layout.set(row, "qwcId", BY_KEY[arena].flags[0])
            layout.set(row, "sellQuantity", -1)
            param.rows[i] = (row_id, bytes(row), name)


def chain(lots, lot):
    """The rows one pickup of lot gives: lot, lot+1, ... while rows exist."""
    out = []
    while lot in lots:
        out.append(lot)
        lot += 1
    return out


def entries(layout, row):
    """[(category, item, count)] of a lot row's items."""
    out = []
    for k in range(1, LOT_ITEMS + 1):
        item = layout.get(row, "lotItemId%02d" % k)
        if item > 0:
            out.append((layout.get(row, "lotItemCategory%02d" % k), item, layout.get(row, "lotItemNum%02d" % k)))
    return out


def _round(value):
    """Two significant digits, at least 1: 376 -> 380, 7518 -> 7500, 3.4 -> 3."""
    if value < 1:
        return 1
    step = 10 ** max(0, len(str(int(value))) - 2)
    return int(value / step + Fraction(1, 2)) * step


def _fits(table, layout, rows):
    """{block: {equip type: (price, sellValue) of the median row, lowest price, highest price}} from vanilla rows."""
    worth = {kind: table(name) for kind, name in VALUES.items()}
    out = {}
    for block in sorted({b for blocks in SHOPS for b in blocks}):
        mine = [r for rid, r in sorted(rows.items()) if rid // BLOCK == block]
        out[block] = {}
        for kind, (value_layout, values) in worth.items():
            typed = [r for r in mine if layout.get(r, "equipType") == kind]
            if not typed:
                continue
            prices = [layout.get(r, "value") for r in typed]

            def pairs(group):
                found = []
                for r in group:
                    item, price = layout.get(r, "equipId"), layout.get(r, "value")
                    sell = value_layout.get(values[item], "sellValue") if item in values else -1
                    if sell > 0 and price > 0:
                        found.append((Fraction(price, sell), price, sell))
                return sorted(found)
            ratios = pairs([r for r in typed if layout.get(r, "sellQuantity") == 1]) or pairs(typed)
            if ratios:
                _, price, sell = ratios[(len(ratios) - 1) // 2]
                out[block][kind] = (price, sell, min(prices), max(prices))
    return out


def price(found, block, kind, item, own):
    """What a row of block (a shop copy) asks for item of equip type kind; own: the row's vanilla price."""
    fit = found["fits"].get(block, {}).get(kind)
    sell = found["values"].get(kind, {}).get(item, -1)
    if fit is None or sell <= 0:
        return own
    median_price, median_sell, lowest, highest = fit
    return max(lowest, min(highest, _round(Fraction(sell * median_price, median_sell))))


def _deal_ratios(layout, rows, goods_values, keep):
    """{block: median price / sellValue of its unlimited goods rows}: each shop copy's currency and tier for consumables
    (an insight shop asks about 1/200 of an echo shop's number). keep: goods that are not dealt, left out."""
    out = {}
    for block in sorted({b for blocks in SHOPS for b in blocks}):
        ratios = sorted(Fraction(layout.get(r, "value"), goods_values[i]) for rid, r in rows.items()
                        if rid // BLOCK == block and layout.get(r, "equipType") == GOODS and layout.get(r, "sellQuantity") == -1
                        and (i := layout.get(r, "equipId")) not in keep and goods_values.get(i, -1) > 0 and layout.get(r, "value") > 0)
        if ratios:
            out[block] = ratios[(len(ratios) - 1) // 2]
    return out


def census(files, join=frozenset(), materials=frozenset()):
    """{'rows': {equip type: [(first block, index)]}, 'pinned': {(first block, index): reason}, 'items': {(first block,
    index): item}, 'pickups': {equip type: [lot row]}, 'pickup_items': {lot row: item}, 'spots': {lot row: (map, placed
    lot)}, 'resold': armour rows that resell a piece some lot gives (never traded), 'fits', 'values', 'deck': [(first
    block, index)] of the dealt consumable rows, 'deck_items': {(first block,
    index): (item, price in the first copy, equip type)} of every row a deal may move, 'ratios': _deal_ratios}. join:
    goods whose unlimited rows trade too
    (chalice materials, when shuffled); materials: the chalice ritual materials, never dealt."""
    table = _reader(files)
    layout, rows = table("ShopLineupParam")
    awarded, named, _ = _event_values(files)
    required = required_goods(files)
    gates = {layout.get(r, "qwcId") for r in rows.values()}
    goods_layout, goods_rows = table(VALUES[GOODS])
    goods_values = {i: goods_layout.get(r, "sellValue") for i, r in goods_rows.items()}
    keep = STAPLES | set(materials) | set(join) | set(required)
    tradable, pinned, items, deck, deck_items = {}, {}, {}, [], {}
    stones = stone_rows(layout, rows)
    shape = lambda r: (layout.get(r, "equipType"), layout.get(r, "equipId"), layout.get(r, "eventFlag"), layout.get(r, "sellQuantity"))
    for blocks in SHOPS:
        first = {rid % BLOCK: r for rid, r in rows.items() if rid // BLOCK == blocks[0]}
        for index, row in sorted(first.items()):
            kind, item, flag = layout.get(row, "equipType"), layout.get(row, "equipId"), layout.get(row, "eventFlag")
            once = layout.get(row, "sellQuantity") == 1 and flag > 0
            copies = [rows.get(b * BLOCK + index) for b in blocks]
            if (blocks[0], index) in stones:
                continue  # the blood stone floor: apply_stones sets it
            if (kind == GOODS and layout.get(row, "sellQuantity") == -1 and item not in keep and flag <= 0
                    and goods_values.get(item, -1) > 0 and all(c is not None and shape(c) == shape(row) for c in copies)):
                deck.append((blocks[0], index))
                deck_items[(blocks[0], index)] = (item, layout.get(row, "value"), GOODS)
                continue
            if kind not in TYPES or not (once or kind != GOODS or item in join):
                continue  # an unlimited goods row that is not dealt (a staple, required goods, a material) stays
            reason = None
            if any(c is None or shape(c) != shape(row) for c in copies):
                reason = "its copies differ"
            elif kind == GOODS and item in required:
                reason = "required goods %d" % item
            elif flag > 0 and (flag in named or flag in gates):
                reason = "something reads its flag"
            if reason:
                pinned[(blocks[0], index)] = reason
            else:
                tradable.setdefault(kind, []).append((blocks[0], index))
                items[(blocks[0], index)] = item
                if kind != GOODS:
                    deck_items[(blocks[0], index)] = (item, layout.get(row, "value"), kind)

    lot_layout, lots = table("ItemLotParam")
    sold = {(TYPES.get(layout.get(r, "equipType")), layout.get(r, "equipId")) for r in rows.values()}
    held = Counter((c, i) for r in lots.values() for c, i, _ in entries(lot_layout, r))
    # an armour row that resells a piece some lot gives (a set found whole in the world and then sold piece by piece,
    # vanilla's and a mod's alike: DATA Gascoigne's, Bone Ash, Nocturne's Dark set) never trades: its piece would lie
    # alone in a pickup while the set's lot still gives it (user 24 Nov); it is dealt within its shop instead
    given = {(c, i) for lot, r in lots.items() if lot > 0 for c, i, _ in entries(lot_layout, r)}
    resold = sorted(k for k in tradable.get(ARMOUR, []) if (TYPES[ARMOUR], items[k]) in given)
    # rows something other than a map pickup reaches: enemies' lots, item rows' lots, lots events award
    reached = set()
    npc_layout, npcs = table("NpcParam")
    others = {npc_layout.get(r, f) for r in npcs.values() for f in NPC_LOTS}
    for name in VALUES.values():
        value_layout, values = table(name)
        others |= {value_layout.get(r, f) for r in values.values() for f in ITEM_LOTS if f in value_layout.fields}
    for lot in others | awarded:
        if lot > 0:
            reached.update(chain(lots, lot))
    placed, twice = {}, set()
    for stem in STEMS:
        seen = set()
        for variant in map_variants(files, stem):
            count = Counter(lot for _, lot in treasure_lots(dcx_read(files.read("map/mapstudio/%s.msb.dcx" % variant))[0]))
            twice.update(lot for lot, n in count.items() if n > 1)
            seen.update(count)
        for lot in seen:
            if lot in placed:
                twice.add(lot)
            placed.setdefault(lot, stem)
    why = lot_pins(files)
    covers = {}
    for lot in sorted(placed):
        for r in chain(lots, lot):
            covers.setdefault(r, []).append(lot)
    category = {c: k for k, c in TYPES.items()}
    pickups, pickup_items, spots = {}, {}, {}
    for r, by in sorted(covers.items()):
        found = entries(lot_layout, lots[r])
        if len(found) != 1 or r in reached:
            continue
        cat, item, count = found[0]
        # a weapon some shop sells still counts: every weapon row trades or is dealt, so the shop's copy moves too (else
        # only 2 weapon pickups would trade, DATA 7 Nov; 18 with this)
        if cat not in category or count != 1 or held[(cat, item)] != 1 or ((cat, item) in sold and cat != TYPES[WEAPON]):
            continue
        if any(lot in twice or why(lot) for lot in by) or len({placed[lot] for lot in by}) != 1:
            continue
        pickups.setdefault(category[cat], []).append(r)
        pickup_items[r] = item
        spots[r] = (placed[by[0]], by[0])
    values = {}
    for kind, name in VALUES.items():
        value_layout, value_rows = table(name)
        values[kind] = {i: value_layout.get(value_rows[i], "sellValue") for i in
                        {pickup_items[r] for r in pickups.get(kind, ())} | {items[k] for k in tradable.get(kind, ())}
                        if i in value_rows}
    return {"rows": tradable, "pinned": pinned, "items": items, "pickups": pickups, "pickup_items": pickup_items,
            "spots": spots, "fits": _fits(table, layout, rows), "values": values, "deck": deck, "deck_items": deck_items,
            "ratios": _deal_ratios(layout, rows, goods_values, keep), "stones": stones, "resold": resold}


def plan(found, seed, opts_hash):
    """{(first block, index): lot row}: each shop row and the pickup it trades with. Per type, the rows that trade (all
    of them but the resold armour when pickups are enough) and their pickups are drawn from the seed."""
    out = {}
    for kind in sorted(found["rows"]):
        rows = [k for k in found["rows"][kind] if k not in set(found["resold"])]
        picks = found["pickups"].get(kind, [])
        n = min(len(rows), len(picks))
        trading = Stream(seed, opts_hash, "shops/rows/%d" % kind).shuffled(rows)[:n]
        partners = Stream(seed, opts_hash, "shops/pickups/%d" % kind).shuffled(picks)[:n]
        out.update(zip(trading, partners))
    return dict(sorted(out.items()))


def apply(param, layout, found, trades):
    """Rewrites the shop rows in place: every copy of a trading row sells its pickup's item at the price that fits."""
    wanted = {}
    for (first, index), lot in trades.items():
        for blocks in SHOPS:
            if blocks[0] == first:
                for block in blocks:
                    wanted[block * BLOCK + index] = found["pickup_items"][lot]
    done = set()
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in wanted and row_id not in done:
            done.add(row_id)
            row = bytearray(body)
            kind, item = layout.get(row, "equipType"), wanted[row_id]
            layout.set(row, "value", price(found, row_id // BLOCK, kind, item, layout.get(row, "value")))
            layout.set(row, "equipId", item)
            param.rows[i] = (row_id, bytes(row), name)


def apply_lots(param, layout, found, trades):
    """Rewrites the traded pickup rows in place: each now gives the item its shop row sold (same type, count 1)."""
    given = {lot: found["items"][spot] for spot, lot in trades.items()}
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in given:
            row = bytearray(body)
            for k in range(1, LOT_ITEMS + 1):
                if layout.get(row, "lotItemId%02d" % k) > 0:
                    layout.set(row, "lotItemId%02d" % k, given.pop(row_id))
                    break
            param.rows[i] = (row_id, bytes(row), name)


def deal(found, seed, opts_hash, trades=()):
    """{(first block, index): (first block, index) of the row whose item it now sells}: the consumable rows across all
    shops, and the weapon and armour rows no trade takes within their shop, each type among its own rows, each a
    permutation drawn from the seed and then spread (`_spread`) so that no shop sells one item twice."""
    trades = dict(trades)
    groups = [("shops/goods", found["deck"], found["deck"])]
    # weapons and armour are dealt within each shop: shops sell some of the same weapons (DATA: none twice in one shop),
    # so a deal across shops would put two rows of one weapon into the Bath Messengers' list, which has most weapon rows
    for kind, first in ((kind, blocks[0]) for kind in (WEAPON, ARMOUR) for blocks in SHOPS):
        rows = [k for k in found["rows"].get(kind, []) if k[0] == first]
        slots = [k for k in rows if k not in trades]
        # a weapon a pickup brought in that the shops also sell: its shop row's item goes out instead as one of the items
        # the trades sent into the world (still found there, sold here once), so no item is sold by two rows for it
        incoming = Counter(found["pickup_items"][trades[k]] for k in rows if k in trades)
        spare = [k for k in rows if k in trades and found["deck_items"][k][0] not in incoming]
        origins = []
        for k in slots:
            if incoming[found["deck_items"][k][0]] > 0 and spare:
                incoming[found["deck_items"][k][0]] -= 1
                k = spare.pop(0)
            origins.append(k)
        groups.append(("shops/deal/%d/%d" % (kind, first), slots, origins))
    taken = Counter((first, found["pickup_items"][lot]) for (first, _), lot in trades.items())
    taken.update((first, item) for (first, _), item in found["stones"].items())  # the Insight shop already sells them
    out = {}
    for name, slots, origins in groups:
        if slots:
            dealt = _moved(dict(zip(slots, Stream(seed, opts_hash, name).shuffled(origins))), found["deck_items"])
            out.update(_spread(dealt, found["deck_items"], taken))
    return out


def _moved(dealt, deck_items):
    """dealt with rows that would sell their own item again swapped with another row of the deal where neither then does
    (rows in order, the first such row): every row changes where its deal allows (user 7 Nov: all rows randomized)."""
    dealt = dict(dealt)
    item = lambda origin: deck_items[origin][0]
    for slot in sorted(dealt):
        if item(dealt[slot]) != item(slot):
            continue
        for other in sorted(dealt):
            if item(dealt[other]) != item(slot) and item(dealt[slot]) != item(other):
                dealt[slot], dealt[other] = dealt[other], dealt[slot]
                break
    return dealt


def _spread(dealt, deck_items, taken):
    """dealt with duplicates swapped away: while a shop gets one item from two rows, that row swaps origins with a row of
    another shop where neither shop then sells its new item twice (rows in order, the first such row). taken: (shop,
    item) counts already sold there by trades; updated with this deal."""
    dealt = dict(dealt)
    item = lambda slot: deck_items[dealt[slot]][0]
    count = taken
    count.update((slot[0], item(slot)) for slot in dealt)
    for slot in sorted(dealt):
        if count[(slot[0], item(slot))] < 2:
            continue
        mine = item(slot)
        for other in sorted(dealt):
            theirs = item(other)
            if other[0] != slot[0] and theirs != mine and not count[(slot[0], theirs)] and not count[(other[0], mine)]:
                count.subtract({(slot[0], mine): 1, (other[0], theirs): 1})
                dealt[slot], dealt[other] = dealt[other], dealt[slot]
                count.update({(slot[0], theirs): 1, (other[0], mine): 1})
                break
    return dealt


def deal_price(found, block, origin, own=None):
    """What a row of block (a shop copy) asks for the item of row origin. Consumables: origin's first-copy price carried
    from origin's currency and tier into block's (the median price / sellValue of each copy's consumables), two digits,
    at least 1. Weapons and armour: the price that fits (`price`), else own (the row's own price)."""
    item, own_origin, kind = found["deck_items"][origin]
    if kind != GOODS:
        return price(found, block, kind, item, own_origin if own is None else own)
    own = own_origin
    to, frm = found["ratios"].get(block), found["ratios"].get(origin[0])
    if to is None or frm is None:
        return own
    return _round(Fraction(own) * to / frm)


def apply_deal(param, layout, found, dealt):
    """Rewrites the dealt consumable rows in place: every copy of a row sells its new item at deal_price."""
    wanted = {}
    for (first, index), origin in dealt.items():
        for blocks in SHOPS:
            if blocks[0] == first:
                for block in blocks:
                    wanted[block * BLOCK + index] = origin
    for i, (row_id, body, name) in enumerate(param.rows):
        if row_id in wanted:
            origin = wanted.pop(row_id)
            row = bytearray(body)
            layout.set(row, "equipId", found["deck_items"][origin][0])
            layout.set(row, "value", deal_price(found, row_id // BLOCK, origin, layout.get(row, "value")))
            param.rows[i] = (row_id, bytes(row), name)


def traded_lots(found, trades):
    """{lot row: item it now gives}, for the spoiler."""
    return {lot: found["items"][spot] for spot, lot in trades.items()}
