"""Drop shuffle: NpcParam rows trade drop lots. A lot's contents and flag never change, so everything keyed by lot still
holds. Only lots no row outside the normal enemies uses may move, so bosses, NPCs and chalice enemies keep theirs."""
from gamedata import STEMS
from seedcode import Stream
from treasure import lot_pins

LOT_FIELDS = tuple("itemLotId_%d" % k for k in range(1, 6))


def census(files, game, chalice=frozenset()):
    """{'movable': sorted lots, 'pinned': {lot: reason}}. game: the generator's Census. chalice: goods whose lots stay
    (the chalice ritual materials while chalice_materials is off)."""
    layout = game.npc_layout
    free = {s["npc"] for stem in STEMS for s in game.slots[stem] if s["kept"] is None}

    def lots_of(rows):
        return {lot for r in rows for lot in (layout.get(game.npc_rows[r], name) for name in LOT_FIELDS) if lot > 0}

    why = lot_pins(files, chalice)
    others = lots_of(r for r in game.npc_rows if r not in free)
    movable, pinned = [], {}
    for lot in sorted(lots_of(free)):
        reason = "another kind of enemy uses it" if lot in others else why(lot)
        if reason:
            pinned[lot] = reason
        else:
            movable.append(lot)
    return {"movable": movable, "pinned": pinned}


def plan(found, seed, opts_hash):
    """{lot: lot now dropped in its place}."""
    movable = found["movable"]
    return dict(zip(movable, Stream(seed, opts_hash, "drops").shuffled(movable)))


def apply(param, layout, mapping):
    """Rewrites every row in place, new clones included."""
    for i, (row_id, body, name) in enumerate(param.rows):
        row = bytearray(body)
        for field in LOT_FIELDS:
            lot = layout.get(row, field)
            layout.set(row, field, mapping.get(lot, lot))
        param.rows[i] = (row_id, bytes(row), name)
