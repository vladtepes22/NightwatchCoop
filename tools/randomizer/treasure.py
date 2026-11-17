"""Treasure shuffle: map events trade lot ids. A lot's contents and pickup flag never change, so everything keyed
by lot still holds. A lot stays where it is when it gives a required item or when map events know about it."""
import struct

from containers import Layout, Param, bnd4_find, bnd4_read, dcx_read
from gamedata import STEMS, _load_events, map_variants
from msb import sections, _i32, _i64
from seedcode import Stream

TREASURE = 4
LOT_WORDS = (0x10, 0x18, 0x20, 0x28)  # within an event's type data
GOODS = 4  # ItemLotParam's category for goods
PROTECTOR = 1
ACCESSORY = 2
KEY_GOODS_TYPE = 1  # keys, workshop tools, badges, messenger items
QUEST_GOODS = frozenset((130, 703, 4306))
AWARD = frozenset(((2003, 4), (2003, 36)))
HAS_ITEM = (3, 4)  # first word's high byte is the item category, second word the item
GOODS_CONDITION = 3
DOOR_ASKS_GOODS = 1  # ObjActParam.spQualifiedType


def treasure_lots(data):
    """[(offset of the word, lot)] for every lot in a map's treasure events."""
    out = []
    for at in sections(data)["EVENT_PARAM_ST"][2][:-1]:
        if _i32(data, at + 0xc) == TREASURE:
            base = at + _i64(data, at + 0x20)
            for word in LOT_WORDS:
                lot = _i32(data, base + word)
                if lot > 0:
                    out.append((base + word, lot))
    return out


def trade_lots(data, mapping):
    """The map with its treasure lots traded; nothing else changes."""
    new = bytearray(data)
    for at, lot in treasure_lots(data):
        struct.pack_into("<i", new, at, mapping.get(lot, lot))
    return bytes(new)


def _event_values(files):
    """(lots any event awards, every number any event names, callers' arguments included, goods any event asks for)."""
    awarded, named, asked = set(), set(), set()
    for stem in list(STEMS) + [None]:
        events = _load_events(files, stem + "_00_00" if stem else "common")
        for instructions, _ in events.values():
            for bank, iid, words in instructions:
                named.update(words)
                if (bank, iid) in AWARD and words:
                    awarded.add(words[0])
                if (bank, iid) == HAS_ITEM and len(words) > 1 and words[0] >> 8 == GOODS_CONDITION and words[1] > 0:
                    asked.add(words[1])
    return awarded, named, asked


def required_goods(files):
    """The goods that stay where the game has them: key goods, what doors and events ask for, the hand-picked rest."""
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
    goods_layout = Layout(bnd4_find(defs, "\\EquipParamGoods.paramdef")[3])
    goods = Param(bnd4_find(bnd, "\\EquipParamGoods.param")[3]).first()
    act_layout = Layout(bnd4_find(defs, "\\ObjActParam.paramdef")[3])
    out = set(QUEST_GOODS) | _event_values(files)[2]
    out.update(i for i, row in goods.items() if goods_layout.get(row, "goodsType") == KEY_GOODS_TYPE)
    for row in Param(bnd4_find(bnd, "\\ObjActParam.param")[3]).first().values():
        for kind, item in (("spQualifiedType", "spQualifiedId"), ("spQualifiedType2", "spQualifiedId2")):
            if act_layout.get(row, kind) == DOOR_ASKS_GOODS:
                out.add(act_layout.get(row, item))
    return out


def chalice_materials(files):
    """The goods some chalice ritual asks for (RitualRequiredMatParam): Ritual Blood, Tomb Mold, Arcane Haze ..."""
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
    layout = Layout(bnd4_find(defs, "\\RitualRequiredMatParam.paramdef")[3])
    rows = Param(bnd4_find(bnd, "\\RitualRequiredMatParam.param")[3]).first()
    return frozenset(item for row in rows.values() for k in range(1, 6)
                     for item in [layout.get(row, "materialItemId_%d" % k)] if item > 0)


def lot_pins(files, chalice=frozenset()):
    """A function: lot -> why it must stay where it is, or None. chalice: goods whose lots stay too (chalice_materials)."""
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
    lot_layout = Layout(bnd4_find(defs, "\\ItemLotParam.paramdef")[3])
    lots = Param(bnd4_find(bnd, "\\ItemLotParam.param")[3]).first()
    required = required_goods(files)
    awarded, named, _ = _event_values(files)
    # an item whose row does not exist shows as ?GoodsName? and gives nothing: dummy lots stay where they are
    present = {cat: set(Param(bnd4_find(bnd, "\\%s.param" % table)[3]).first())
               for cat, table in ((GOODS, "EquipParamGoods"), (PROTECTOR, "EquipParamProtector"), (ACCESSORY, "EquipParamAccessory"))}

    def why(lot):
        reason = "no such lot" if lot not in lots else None
        row_id = lot
        while reason is None and row_id in lots:  # an award also rolls the rows right after it
            row = lots[row_id]
            if row_id in awarded:
                reason = "an event awards it"
            elif lot_layout.get(row, "getItemFlagId") > 0 and lot_layout.get(row, "getItemFlagId") in named:
                reason = "an event knows its flag"
            for k in range(1, 9):
                item = lot_layout.get(row, "lotItemId%02d" % k)
                category = lot_layout.get(row, "lotItemCategory%02d" % k)
                if item > 0 and category == GOODS and item in required:
                    reason = "required goods %d" % item
                elif item > 0 and category == GOODS and item in chalice:
                    reason = "chalice material %d" % item
                elif item > 0 and category in present and item not in present[category]:
                    reason = "no row for item %d" % item
            row_id += 1
        return reason
    return why


def lot_kinds(files):
    """{lot: category of its first item}: weapon, armour, accessory or goods, for trades within one kind."""
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
    layout = Layout(bnd4_find(defs, "\\ItemLotParam.paramdef")[3])
    out = {}
    for lot, row in Param(bnd4_find(bnd, "\\ItemLotParam.param")[3]).first().items():
        for k in range(1, 9):
            if layout.get(row, "lotItemId%02d" % k) > 0:
                out[lot] = layout.get(row, "lotItemCategory%02d" % k)
                break
    return out


def census(files, chalice=frozenset()):
    """{'movable': sorted lots, 'pinned': {lot: reason}, 'kinds': {lot: kind}}. Lots reach the map's every variant; all are counted."""
    why = lot_pins(files, chalice)
    found = set()
    for stem in STEMS:
        for variant in map_variants(files, stem):
            found.update(lot for _, lot in treasure_lots(dcx_read(files.read("map/mapstudio/%s.msb.dcx" % variant))[0]))
    movable, pinned = [], {}
    for lot in sorted(found):
        reason = why(lot)
        if reason:
            pinned[lot] = reason
        else:
            movable.append(lot)
    kinds = lot_kinds(files)
    return {"movable": movable, "pinned": pinned, "kinds": {lot: kinds.get(lot, -1) for lot in movable}}


def plan(treasure, seed, opts_hash, by_type=False):
    """{lot: lot now in its place}, a shuffle of the movable lots; with by_type only among lots of one kind."""
    movable = treasure["movable"]
    if not by_type:
        return dict(zip(movable, Stream(seed, opts_hash, "treasure").shuffled(movable)))
    out = {}
    for kind in sorted(set(treasure["kinds"].values())):
        group = [lot for lot in movable if treasure["kinds"][lot] == kind]
        out.update(zip(group, Stream(seed, opts_hash, "treasure", kind).shuffled(group)))
    return out
