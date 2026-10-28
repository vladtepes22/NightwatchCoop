"""SaveModel: read and edit the fields of one Bloodborne character save (userdata000N, 0x140000 bytes).

Pure Python, no GUI. Every offset here was pinned on the test saves (docs/party/save_layout.md has the
evidence); anything not pinned there is not in this file. Edits patch single fields inside a copy of the
file's bytes and never rebuild the file, so diff() against the original lists exactly the edited ranges.

Layout facts (name = the character name's UTF-16 field, 17 characters of 2 bytes, the last a 0):
  name        = end - size - 0x10C91 of the section table's first pair (header +0x0C {size, end})
  name - 0x68 vigor, -0x60 endurance, -0x50 strength, -0x48 skill, -0x40 bloodtinge, -0x38 arcane (u32;
              -0x58 is the unused third slot, 0)           level = sum(6 stats) - 50  (u32 at name - 0x18)
  name - 0x94 HP, -0x90 max HP (u32)  -0x14 Blood Echoes  -0x24 Insight  -0x04 remembered chr type
              (1 = summoned guest phantom mark)
  header +0x2C {offset, 8}: u32 NG cycle, u32 ending type at that offset
  inventory   = three lists: the normal one (u32 used count + 1984 x 16-byte entries) 0x1C4 bytes behind the
              name, then the saved key list and the summoned key list (u32 count + 64 entries each)
              entry {u32 handle, u32 item id (category << 28 | id), u32 count, u32 serial}; empty is
              {0, 0xFFFFFFFF, 0, 0}; weapons: id = base + 100 * reinforcement level
  event flags = the 150001-byte section (group key and bit: scripts/party_session._group_key)
"""
import re
import struct

SLOT_SIZE = 0x140000
SAVE_VERSION = 0x41
NAME_BACK = 0x10C91
NAME_CHARS = 16
POOL_BYTES = 1200 * 125
FLAG_SECTION = POOL_BYTES + 1
NORMAL_ENTRIES = 1984
KEY_ENTRIES = 64
ENTRY = 16
NORMAL_BEHIND_NAME = 0x1C4
EMPTY_ID = 0xFFFFFFFF
STAT_NAMES = ("vigor", "endurance", "strength", "skill", "bloodtinge", "arcane")
STAT_BACK = {"vigor": 0x68, "endurance": 0x60, "strength": 0x50, "skill": 0x48, "bloodtinge": 0x40, "arcane": 0x38}
LEVEL_BASE = 50
LEVEL_BACK, ECHOES_BACK, INSIGHT_BACK, HP_BACK, PHANTOM_BACK = 0x18, 0x14, 0x24, 0x94, 4
CATEGORIES = {0: "weapon", 1: "armor", 2: "accessory", 4: "goods", 8: "gem"}

_GLOBAL_TYPES = {0: 0, 1: 1, 5: 2, 6: 3, 7: 4}
_MAP_TYPES = {1: 0, 5: 1, 6: 2, 7: 3, 9: 4}
_MAPS = [(21, 0), (21, 1), (22, 0), (23, 0), (24, 0), (24, 1), (24, 2), (25, 0), (26, 0), (27, 0), (28, 0),
         (29, 0), (32, 0), (33, 0), (34, 0), (35, 0), (36, 0)]


class SaveFormatError(ValueError):
    pass


def group_key(fid):
    """The flag store group of a flag id (same rule as tools/party/save_decode.py), None if unmapped."""
    t, aa, b, z = fid // 10**7, (fid // 10**5) % 100, (fid // 10**4) % 10, (fid // 1000) % 10
    if fid < 10000:
        return z * 5
    if aa == 0 and b == 0 and t in _GLOBAL_TYPES:
        return _GLOBAL_TYPES[t] + z * 5
    if t not in _MAP_TYPES:
        return None
    if (aa, b) in _MAPS:
        mi = _MAPS.index((aa, b))
    elif aa == 40:
        mi = 11
    elif 41 <= aa <= 46 and b == 0:
        mi = 17 + aa - 41
    else:
        return None
    return z * 115 + 50 + 23 * _MAP_TYPES[t] + mi


class Item:
    """One inventory entry. slot: its index in its list; section: 0 normal, 1 saved keys, 2 summoned keys."""

    def __init__(self, section, slot, handle, iid, count, serial):
        self.section, self.slot, self.handle, self.iid, self.count, self.serial = section, slot, handle, iid, count, serial

    @property
    def category(self):
        return self.iid >> 28

    @property
    def kind(self):
        return CATEGORIES.get(self.category, f"cat{self.category}")

    @property
    def number(self):
        return self.iid & 0x0FFFFFFF

    @property
    def level(self):
        """Weapon reinforcement level (id = base + 100 * level), None for other items."""
        return (self.number % 10000) // 100 if self.category == 0 and self.number >= 1000000 else None

    def __repr__(self):
        return f"Item({self.kind} {self.number} x{self.count} slot {self.section}:{self.slot})"


class SaveModel:
    def __init__(self, data):
        if len(data) != SLOT_SIZE:
            raise SaveFormatError(f"{len(data)} bytes, expected {SLOT_SIZE}")
        self.orig = bytes(data)
        self.d = bytearray(data)
        if self.u32(0) != SAVE_VERSION:
            raise SaveFormatError(f"save version {self.u32(0):#x}, expected {SAVE_VERSION:#x}")
        size, end = struct.unpack_from("<II", self.d, 0x0C)
        self.name_at = end - size - NAME_BACK
        if not (0x100 <= self.name_at <= 0x40000):
            raise SaveFormatError("the character name is not where this save version keeps it")
        self.pool = self._find_pool()
        self.normal_at = self.name_at + NORMAL_BEHIND_NAME
        self._check_inventory()

    # --- raw helpers -------------------------------------------------------------------------
    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def _put(self, o, v):
        struct.pack_into("<I", self.d, o, v & 0xFFFFFFFF)

    def _find_pool(self):
        for i in range(0x0C, 0x200, 4):
            size, end = struct.unpack_from("<II", self.d, i)
            if size == FLAG_SECTION and size <= end <= SLOT_SIZE:
                return end - size
        raise SaveFormatError("no event flag section in the section table")

    def _check_inventory(self):
        n = self.u32(self.normal_at)
        used = sum(1 for k in range(NORMAL_ENTRIES) if not self._empty(self.normal_at + 4 + k * ENTRY))
        if n != used or self.normal_at + 4 + NORMAL_ENTRIES * ENTRY + 2 * (4 + KEY_ENTRIES * ENTRY) > SLOT_SIZE:
            raise SaveFormatError("the inventory is not where this save version keeps it")

    def _empty(self, o):
        return self.u32(o) == 0 and self.u32(o + 4) == EMPTY_ID

    @property
    def empty(self):
        """True for a slot without a character (all zero header)."""
        return not any(self.d[:16])

    # --- character -----------------------------------------------------------------------------
    @property
    def name(self):
        raw = bytes(self.d[self.name_at:self.name_at + 2 * (NAME_CHARS + 1)])
        end = next((i for i in range(0, len(raw), 2) if raw[i:i + 2] == b"\0\0"), len(raw))
        return raw[:end].decode("utf-16le", "replace")

    def set_name(self, name):
        name = name.rstrip("\0")
        raw = name.encode("utf-16le")
        if not name or len(raw) > 2 * NAME_CHARS or re.search(r"[\x00-\x1f]", name):
            raise ValueError(f"the name needs 1 to {NAME_CHARS} characters")
        self.d[self.name_at:self.name_at + 2 * (NAME_CHARS + 1)] = raw + bytes(2 * (NAME_CHARS + 1) - len(raw))

    def _field(self, back):
        return self.u32(self.name_at - back)

    @property
    def stats(self):
        return {k: self._field(STAT_BACK[k]) for k in STAT_NAMES}

    @property
    def level(self):
        return self._field(LEVEL_BACK)

    @staticmethod
    def level_for(stats):
        return sum(stats.values()) - LEVEL_BASE

    def set_stat(self, stat, value):
        """Set one stat (1..99) and the level that follows from the stats (the level is not editable)."""
        if stat not in STAT_BACK:
            raise KeyError(stat)
        if not 1 <= int(value) <= 99:
            raise ValueError("a stat is 1 to 99")
        self._put(self.name_at - STAT_BACK[stat], int(value))
        self._put(self.name_at - LEVEL_BACK, self.level_for(self.stats))

    @property
    def hp(self):
        return self._field(HP_BACK), self._field(HP_BACK - 4)

    @property
    def echoes(self):
        return self._field(ECHOES_BACK)

    def set_echoes(self, v):
        if not 0 <= int(v) <= 0x7FFFFFFF:
            raise ValueError("Blood Echoes 0 to 2147483647")
        self._put(self.name_at - ECHOES_BACK, int(v))

    @property
    def insight(self):
        return self._field(INSIGHT_BACK)

    def set_insight(self, v):
        if not 0 <= int(v) <= 9999:
            raise ValueError("Insight 0 to 9999")
        self._put(self.name_at - INSIGHT_BACK, int(v))

    @property
    def play_ms(self):
        return self.u32(8)

    # --- NG cycle -------------------------------------------------------------------------------
    def _ng_at(self):
        off, size = struct.unpack_from("<II", self.d, 0x2C)
        if size != 8 or not 0x54 <= off <= SLOT_SIZE - 8:
            raise SaveFormatError("no NG cycle block")
        return off

    @property
    def ng_cycle(self):
        return self.u32(self._ng_at())

    def set_ng_cycle(self, v):
        if not 0 <= int(v) <= 99:
            raise ValueError("NG cycle 0 to 99")
        self._put(self._ng_at(), int(v))

    # --- party -----------------------------------------------------------------------------------
    @property
    def phantom_mark(self):
        """The remembered chr type: 1 = this save was last written as a summoned guest (own-world soft lock)."""
        return self._field(PHANTOM_BACK)

    def repair_phantom_mark(self):
        self._put(self.name_at - PHANTOM_BACK, 0)

    @property
    def world_empty(self):
        """True: written in another player's world (three empty world-state blocks, no LCED)."""
        a, an = struct.unpack_from("<II", self.d, 0x3C)
        return an == 12 and not any(self.d[a:a + 12]) if 0x54 <= a <= SLOT_SIZE - 12 else False

    # --- inventory --------------------------------------------------------------------------------
    def _section_at(self, section):
        if section == 0:
            return self.normal_at, NORMAL_ENTRIES
        base = self.normal_at + 4 + NORMAL_ENTRIES * ENTRY
        return base + (section - 1) * (4 + KEY_ENTRIES * ENTRY), KEY_ENTRIES

    def items(self, sections=(0, 1, 2)):
        out = []
        for sec in sections:
            at, n = self._section_at(sec)
            for k in range(n):
                o = at + 4 + k * ENTRY
                if not self._empty(o):
                    h, iid, c, s = struct.unpack_from("<IIII", self.d, o)
                    out.append(Item(sec, k, h, iid, c, s))
        return out

    def _item_off(self, item):
        at, n = self._section_at(item.section)
        return at + 4 + item.slot * ENTRY

    def _recount(self, section):
        at, n = self._section_at(section)
        self._put(at, sum(1 for k in range(n) if not self._empty(at + 4 + k * ENTRY)))

    def set_count(self, item, count, maximum=None):
        count = int(count)
        if count < 1 or count > (maximum or 0x7FFFFFFF):
            raise ValueError(f"the count is 1 to {maximum or 0x7FFFFFFF}")
        o = self._item_off(item)
        if self.u32(o + 4) != item.iid:
            raise ValueError("the item list changed")
        self._put(o + 8, count)
        item.count = count

    def remove(self, item):
        o = self._item_off(item)
        if self.u32(o + 4) != item.iid:
            raise ValueError("the item list changed")
        self.d[o:o + ENTRY] = struct.pack("<IIII", 0, EMPTY_ID, 0, 0)
        self._recount(item.section)

    def set_weapon_level(self, item, level, valid_ids=None):
        """Weapon reinforcement: id = base + 100 * level. valid_ids (the game's EquipParamWeapon ids) is
        required: a level without a param row would crash the game."""
        if item.level is None:
            raise ValueError("this weapon has no reinforcement levels")
        if valid_ids is None:
            raise ValueError("the game's weapon list is needed to change a level")
        base = item.number - (item.number % 10000) // 100 * 100
        new = base + 100 * int(level)
        if new not in valid_ids:
            raise ValueError(f"this weapon has no level {level}")
        o = self._item_off(item)
        self._put(o + 4, (0 << 28) | new)
        item.iid = new

    def duplicate_keys(self):
        """Goods ids that have a stack in both key lists, or twice in one."""
        seen, dup = {}, set()
        for it in self.items((1, 2)):
            seen[it.number] = seen.get(it.number, 0) + 1
        return sorted(i for i, n in seen.items() if n > 1)

    def merge_duplicate_keys(self):
        """Keep one stack per key item: the saved list's (else the first); the others are emptied."""
        done = []
        for number in self.duplicate_keys():
            stacks = sorted((i for i in self.items((1, 2)) if i.number == number), key=lambda i: i.section)
            for extra in stacks[1:]:
                self.remove(extra)
            done.append(number)
        return done

    # --- event flags -----------------------------------------------------------------------------
    def flag(self, fid):
        k = group_key(fid)
        if k is None:
            return None
        return bool(self.d[self.pool + k * 125 + (fid % 1000) // 8] & (0x80 >> (fid % 8)))

    def set_flag(self, fid, on=True):
        k = group_key(fid)
        if k is None:
            raise ValueError(f"flag {fid} has no place in the save")
        o = self.pool + k * 125 + (fid % 1000) // 8
        bit = 0x80 >> (fid % 8)
        self.d[o] = (self.d[o] | bit) if on else (self.d[o] & ~bit & 0xFF)

    # --- output ------------------------------------------------------------------------------------
    def to_bytes(self):
        return bytes(self.d)

    def diff(self):
        """[(start, end)] byte ranges that differ from the file as opened."""
        out, start = [], None
        o, d = self.orig, self.d
        for i in range(SLOT_SIZE):
            if o[i] != d[i]:
                if start is None:
                    start = i
            elif start is not None:
                out.append((start, i))
                start = None
        if start is not None:
            out.append((start, SLOT_SIZE))
        return out
