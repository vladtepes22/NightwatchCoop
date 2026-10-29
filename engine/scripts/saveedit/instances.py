"""The save's item instance table: weapon sockets and the effects of every blood gem and rune.

Layout (checked on two party saves, level 10 and 90): the table starts at 0x54 and ends before the name
area. Records, by the top byte of their handle:
  0x80 weapon, 0x90 armor: 60 bytes {u32 handle, u32 item id, u32 durability, u32, u32,
                            5 x {u32 slot shape (0x80000000 = no slot), u32 gem handle (0 = empty)}}
  0xC0 gem or rune:        40 bytes {u32 handle, u32 0x80000000 | GemGen id, u32 type (1 gem, 2 rune),
                            u32 shape (1 radial, 2 triangle, 4 waning, 8 circle, 63 droplet/rune),
                            6 x u32 GemeffectParam id (0xFFFFFFFF empty)}
  empty:                   8 bytes {0, 0xFFFFFFFF}
Gems in the inventory point here by handle (inventory entry handle 0xC08000nn); socketed gems are only in
their weapon's slots. Weapon 1000 is the Memory Altar: its slots hold the equipped runes and the oath.
Reading stops (table None, gems read-only) when any record or handle does not resolve.
"""
import struct

START = 0x54
WEAPON, ARMOR, GEM = 0x80, 0x90, 0xC0
SIZES = {WEAPON: 60, ARMOR: 60, GEM: 40}
EMPTY = 0xFFFFFFFF
NO_SLOT = 0x80000000
MAX_EFFECTS = 6


class Gem:
    def __init__(self, at, handle, gen, kind, shape, effects):
        self.at, self.handle, self.gen, self.type, self.shape, self.effects = at, handle, gen, kind, shape, effects


class Table:
    """weapons: {handle: [(shape, gem handle) per slot]}, weapon_ids: {handle: weapon id}, gems: {handle: Gem}."""

    def __init__(self, d, end):
        self.weapons, self.weapon_ids, self.gems = {}, {}, {}
        o = START
        while o + 8 <= end:
            h, iid = struct.unpack_from("<II", d, o)
            if h == 0 and iid == EMPTY:
                o += 8
                continue
            top = h >> 24
            if top not in SIZES or o + SIZES[top] > end:
                break
            if top == GEM:
                v = struct.unpack_from("<10I", d, o)
                self.gems[h] = Gem(o, h, iid & 0x0FFFFFFF, v[2], v[3], [e for e in v[4:] if e != EMPTY])
            elif top == WEAPON:
                v = struct.unpack_from("<15I", d, o)
                self.weapons[h] = [(v[5 + 2 * i], v[6 + 2 * i]) for i in range(5) if v[5 + 2 * i] != NO_SLOT]
                self.weapon_ids[h] = iid & 0x0FFFFFFF
            o += SIZES[top]

    def socketed(self):
        """{gem handle: weapon handle} of every gem sitting in a weapon slot."""
        return {g: w for w, slots in self.weapons.items() for _, g in slots if g}


def read(model):
    """The instance table of a SaveModel, or None when it does not check out against the inventory."""
    t = Table(model.d, model.name_at)
    socketed = t.socketed()
    if any(g not in t.gems for g in socketed):
        return None
    if any(i.handle not in t.gems for i in model.items((0,)) if i.category == 8):
        return None
    return t


def set_effects(model, gem, effects):
    """Write a gem's effect ids (1 to 6, the rest emptied); the caller validates the ids."""
    if not 1 <= len(effects) <= MAX_EFFECTS:
        raise ValueError("a gem has 1 to 6 effects")
    if struct.unpack_from("<I", model.d, gem.at)[0] != gem.handle:
        raise ValueError("the gem list changed")
    struct.pack_into("<6I", model.d, gem.at + 16, *(list(effects) + [EMPTY] * (MAX_EFFECTS - len(effects))))
    gem.effects = list(effects)
