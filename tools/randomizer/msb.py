"""Map files. Edits patch bytes in place and fix what they shift; nothing else in the file is rebuilt,
so anything this module does not touch stays byte-identical.

Layout: sections chained from 0x10 (int version, int count, q name offset, q entry offsets[count - 1],
q next section). Section offsets are absolute, a record's own fields are relative to the record.
Model record: q name, int type (2 enemy), int per-type id, q sib path, int instance count, strings.
Part record: q name at +8, int type at +0x14 (2 enemy), int model index at +0x1c, q base data at +0xb0,
q type data at +0xb8. Base data +0 = entity id. Enemy type data +8 = think id, +0xc = NPC id."""
import struct

MODELS, PARTS = "MODEL_PARAM_ST", "PARTS_PARAM_ST"
EVENTS = "EVENT_PARAM_ST"
ENEMY = 2
# Words that hold a part index, as (offset in the type data, count): by event type and by part type (DATA).
EVENT_PART_REFS = {4: ((0x8, 1),), 5: ((0x50, 32),), 7: ((0x4, 1),), 16: ((0x10, 32),)}
PART_PART_REFS = {1: ((0x8, 1),), 9: ((0x8, 1),), 2: ((0x1c, 1),), 10: ((0x1c, 1),), 11: ((0x0, 1),)}


def _utf16z(data, offset):
    end = offset
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[offset:end].decode("utf-16-le")


def _i32(data, at):
    return struct.unpack_from("<i", data, at)[0]


def _i64(data, at):
    return struct.unpack_from("<q", data, at)[0]


def sections(data):
    """{name: (section offset, name offset, [offsets of all entries, last = next section])}, in file order."""
    out, at = {}, 0x10
    while True:
        count = _i32(data, at + 4)
        name_at = _i64(data, at + 8)
        table = list(struct.unpack_from("<%dq" % count, data, at + 16))
        out[_utf16z(data, name_at)] = (at, name_at, table)
        at = table[-1]
        if at == 0:
            return out


class Msb:
    def __init__(self, data):
        self.data = bytearray(data)

    def _entries(self, section):
        return sections(self.data)[section][2][:-1]

    def model_names(self):
        return [_utf16z(self.data, at + _i64(self.data, at)) for at in self._entries(MODELS)]

    def enemies(self):
        """Every enemy part: dict with name, model, entity, think, npc, talk id, start animation and its offset."""
        names = self.model_names()
        out = []
        for at in self._entries(PARTS):
            if _i32(self.data, at + 0x14) != ENEMY:
                continue
            base, kind = _i64(self.data, at + 0xb0), _i64(self.data, at + 0xb8)
            out.append({
                "part": _utf16z(self.data, at + _i64(self.data, at + 8)),
                "model": names[_i32(self.data, at + 0x1c)],
                "entity": _i32(self.data, at + base),
                "think": _i32(self.data, at + kind + 8),
                "npc": _i32(self.data, at + kind + 0xc),
                "talk": _i32(self.data, at + kind + 0x10),  # DATA: the talk script id (t<id>.esd), 0 = none
                "anim": _i32(self.data, at + kind + 0x18),
                "at": at,
            })
        return out

    def part_names(self):
        return [_utf16z(self.data, at + _i64(self.data, at + 8)) for at in self._entries(PARTS)]

    def add_model(self, name):
        """A new enemy model record after the last enemy model, copied from it; returns its index."""
        table = self._entries(MODELS)
        kinds = [_i32(self.data, at + 8) for at in table]
        last = max(i for i, kind in enumerate(kinds) if kind == ENEMY)
        old_name = _utf16z(self.data, table[last] + _i64(self.data, table[last]))
        if name in self.model_names():
            raise ValueError("model %s is already listed" % name)
        if len(old_name) != len(name):
            raise ValueError("model names must have the same length")
        secs = sections(self.data)
        record = bytearray(bytes(self.data[table[last]:table[last + 1] if last + 1 < len(table) else secs[MODELS][2][-1]])
                           .replace(old_name.encode("utf-16-le"), name.encode("utf-16-le")))
        struct.pack_into("<i", record, 0xc, max(_i32(self.data, at + 0xc) for at, kind in zip(table, kinds) if kind == ENEMY) + 1)
        struct.pack_into("<i", record, 0x18, 0)

        index = last + 1
        model_at, model_name_at, model_table = secs[MODELS]
        insert_at = model_table[index]  # the next record, or the next section when it is the last
        slot_at = model_at + 16 + index * 8
        grow = len(record)

        def moved(offset):
            return offset + (8 if offset >= slot_at else 0) + (grow if offset >= insert_at else 0)

        new = bytearray(bytes(self.data[:slot_at]) + b"\0" * 8 + bytes(self.data[slot_at:insert_at]) + bytes(record) + bytes(self.data[insert_at:]))
        struct.pack_into("<i", new, model_at + 4, len(model_table) + 1)
        struct.pack_into("<q", new, model_at + 8, moved(model_name_at))
        fixed = [moved(o) for o in model_table[:index]] + [insert_at + 8] + [moved(o) for o in model_table[index:]]
        struct.pack_into("<%dq" % len(fixed), new, model_at + 16, *fixed)
        for section, (at, name_at, offsets) in secs.items():
            if section == MODELS:
                continue
            at2 = moved(at)
            struct.pack_into("<q", new, at2 + 8, moved(name_at))
            fixed = [moved(o) if o else 0 for o in offsets]
            struct.pack_into("<%dq" % len(fixed), new, at2 + 16, *fixed)
        self.data = new
        for at in self._entries(PARTS):
            if _i32(self.data, at + 0x1c) >= index:
                struct.pack_into("<i", self.data, at + 0x1c, _i32(self.data, at + 0x1c) + 1)
        return index

    def swap_enemies(self, changes):
        """{part name: (model, think, npc)}. Keeps entity, position and order; keeps instance counts right.
        Adds the models the file does not list yet, in sorted order."""
        for name in sorted({c[0] for c in changes.values()} - set(self.model_names())):
            self.add_model(name)
        names = self.model_names()
        index = {name: i for i, name in enumerate(names)}
        models = self._entries(MODELS)
        done = set()
        for part in self.enemies():
            change = changes.get(part["part"])
            if change is None:
                continue
            at, kind = part["at"], _i64(self.data, part["at"] + 0xb8)
            old, new = index[part["model"]], index[change[0]]
            struct.pack_into("<i", self.data, at + 0x1c, new)
            struct.pack_into("<ii", self.data, at + kind + 8, change[1], change[2])
            struct.pack_into("<i", self.data, models[old] + 0x18, _i32(self.data, models[old] + 0x18) - 1)
            struct.pack_into("<i", self.data, models[new] + 0x18, _i32(self.data, models[new] + 0x18) + 1)
            done.add(part["part"])
        return done

    def clear_start_animations(self, parts):
        """The named enemy parts start with no animation (-1: standing). Returns the parts found."""
        done = set()
        for part in self.enemies():
            if part["part"] in parts:
                kind = _i64(self.data, part["at"] + 0xb8)
                struct.pack_into("<i", self.data, part["at"] + kind + 0x18, -1)
                done.add(part["part"])
        return done

    def _part_index_words(self):
        """Absolute offsets of every int that holds a part's index in the parts list (DATA, all 24 main-map files,
        cross-checked by resolving names with an outside reader): event base +0 (any event), treasure +8, generator
        spawn parts +0x50 (32), object action +4, group tour +0x10 (32); object and dummy object +8, enemy and dummy
        enemy +0x1c (collision), connect collision +0 (collision). Regions hold none."""
        secs = sections(self.data)
        out = []
        for at in secs[EVENTS][2][:-1]:
            base, kind, etype = _i64(self.data, at + 0x18), _i64(self.data, at + 0x20), _i32(self.data, at + 0xc)
            if base:
                out.append(at + base)
            for offset, count in EVENT_PART_REFS.get(etype, ()):
                out.extend(at + kind + offset + 4 * k for k in range(count))
        for at in secs[PARTS][2][:-1]:
            kind, ptype = _i64(self.data, at + 0xb8), _i32(self.data, at + 0x14)
            for offset, count in PART_PART_REFS.get(ptype, ()):
                out.extend(at + kind + offset + 4 * k for k in range(count))
        return out

    def part_record(self, name):
        """(index in the parts list, record offset, record bytes) of a part by name."""
        table = sections(self.data)[PARTS][2]
        for i, at in enumerate(table[:-1]):
            if _utf16z(self.data, at + _i64(self.data, at + 8)) == name:
                end = table[i + 1] if i + 2 < len(table) else None
                if end is None:
                    raise ValueError("%s is the last part; its record has no known end" % name)
                return i, at, bytes(self.data[at:end])
        raise KeyError(name)

    def free_part_name(self, model):
        """model_NNNN with the lowest instance number no part of this file uses."""
        used = set(self.part_names())
        return next("%s_%04d" % (model, n) for n in range(10000) if "%s_%04d" % (model, n) not in used)

    def add_enemy(self, template, name, entity, model, think, npc, position, rotation=None, start_anim=-1, talk=0):
        """A new enemy part: a copy of this file's part `template` (so its collision, groups, lighting and other map
        data are this map's) with a new name (`model_NNNN`), entity id, model, think and NPC rows, talk script (0 =
        none; never the template's, which would give the copy the template's dialogue), position and rotation
        (degrees). It goes after the last enemy part, so the parts stay grouped by type like every vanilla
        file; every part index at or after it moves up by one. Adds the model if missing. Returns its part index."""
        if name in self.part_names():
            raise ValueError("part %s exists" % name)
        if len(name) != len(template) or not name.startswith(model + "_"):
            raise ValueError("part name must be model_NNNN like the template")
        if model not in self.model_names():
            self.add_model(model)
        secs = sections(self.data)
        part_at, part_name_at, table = secs[PARTS]
        types = [_i32(self.data, at + 0x14) for at in table[:-1]]
        p = max(i for i, t in enumerate(types) if t == ENEMY) + 1
        for word in self._part_index_words():
            if _i32(self.data, word) >= p:
                struct.pack_into("<i", self.data, word, _i32(self.data, word) + 1)
        _, _, record = self.part_record(template)
        record = bytearray(record)
        if _i32(record, 0x14) != ENEMY:
            raise ValueError("template %s is no enemy" % template)
        struct.pack_into("<%ds" % (2 * len(name)), record, _i64(record, 8), name.encode("utf-16-le"))
        struct.pack_into("<i", record, 0x10, int(name.rsplit("_", 1)[1]))
        struct.pack_into("<i", record, 0x18, types.count(ENEMY))
        names = self.model_names()
        struct.pack_into("<i", record, 0x1c, names.index(model))
        struct.pack_into("<3f", record, 0x28, *position)
        if rotation is not None:
            struct.pack_into("<3f", record, 0x34, *rotation)
        base, kind = _i64(record, 0xb0), _i64(record, 0xb8)
        struct.pack_into("<i", record, base, entity)
        struct.pack_into("<iii", record, kind + 8, think, npc, talk)
        struct.pack_into("<i", record, kind + 0x18, start_anim)

        insert_at = table[p]
        slot_at = part_at + 16 + p * 8
        grow = len(record)

        def moved(offset):
            return offset + (8 if offset >= slot_at else 0) + (grow if offset >= insert_at else 0)

        new = bytearray(bytes(self.data[:slot_at]) + b"\0" * 8 + bytes(self.data[slot_at:insert_at]) + bytes(record) + bytes(self.data[insert_at:]))
        struct.pack_into("<i", new, part_at + 4, len(table) + 1)
        struct.pack_into("<q", new, part_at + 8, moved(part_name_at))
        fixed = [moved(o) for o in table[:p]] + [insert_at + 8] + [moved(o) if o else 0 for o in table[p:]]
        struct.pack_into("<%dq" % len(fixed), new, part_at + 16, *fixed)
        self.data = new
        models = self._entries(MODELS)
        at = models[names.index(model)]
        struct.pack_into("<i", self.data, at + 0x18, _i32(self.data, at + 0x18) + 1)
        return p

    def bytes(self):
        return bytes(self.data)
