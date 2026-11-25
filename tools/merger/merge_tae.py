"""TAE3 (64-bit, little endian) merge per animation id.

Layout (bb-modding knowledge\\anim-sound.md, "TAE layout"): animation table at q(0x58) with q(0x70) rows of (q id, q
header offset), id range index at q(0x60), first animation header at q(0x78), file size at i32 0x0c. An animation
header (0x30 bytes) holds q event headers, q event groups, q times, q anim-file block, then i32 event, group and time
counts and one more i32. Event header 0x18 bytes (q start time ptr, q end time ptr, q data ptr); event data 0x10
bytes (i32 type, i32, q argument ptr); event group 0x20 bytes (q count, q values ptr, q type data ptr, q 0), values
= i32 offsets of event headers.

An animation is compared by its content (types, times, argument bytes, groups, anim-file block), not by where its
bytes sit, so a TAE rewritten by an editor compares equal where its content is. Argument blocks have no size in the
file: an event type's size is the smallest room its blocks have in the game's file before the next piece anything
points at (the game packs them tightly), and zero bytes at a block's end are not compared. The
merged TAE is the game's file with each animation a mod changed appended as a copy (all its pieces, their pointers
moved), the animation table pointing at the copies; afterwards every animation is re-read and must equal the winning
mod's, else the merge is refused (MergeError)."""
from bisect import bisect_right
import struct

from common import MergeError

MAGIC = bytes.fromhex("54414520000000ff0c000100")


def _q(t, o):
    return struct.unpack_from("<q", t, o)[0]


def _i(t, o):
    return struct.unpack_from("<i", t, o)[0]


def anim_table(t):
    count, table = _q(t, 0x70), _q(t, 0x58)
    if count < 0 or table <= 0 or table + count * 16 > len(t):
        raise MergeError("TAE animation table outside the file")
    return [struct.unpack_from("<2q", t, table + k * 16) for k in range(count)]


class Layout:
    """Where things start in one TAE: `stops` every offset the file points at (sorted, the file end included),
    `pieces` the same without argument blocks. An argument pointer at a piece's start (an event type with no
    arguments points at the next event's data) has no room."""

    def __init__(self, t, base=None):
        pieces, args = {len(t), _q(t, 0x58), _q(t, 0x60)}, set()
        for _, ao in anim_table(t):
            eh, eg, times, fo, ec, gc, tc, _ = struct.unpack_from("<4q4i", t, ao)
            pieces.update((ao, eh, eg, times, fo))
            if _q(t, fo + 8):
                pieces.update((_q(t, fo + 8), _q(t, fo + 16)))
            for e in range(ec):
                h = eh + e * 24
                dat = _q(t, h + 16)
                pieces.update((h, _q(t, h), _q(t, h + 8), dat))
                args.add(_q(t, dat + 8))
            for g in range(gc):
                h = eg + g * 32
                pieces.update((h, _q(t, h + 8), _q(t, h + 16)))
        if base is not None:  # the game file inside a merged file: its pieces stay where they were
            pieces |= base.pieces
            args |= base.args
        self.size = len(t)
        self.pieces, self.args = pieces, args
        self.stops = sorted(x for x in pieces | args if 0 < x <= len(t))

    def room(self, at):
        """Bytes from `at` to the next place anything points at; 0 at a piece's start or outside the file."""
        if at in self.pieces or not 0 < at < self.size:
            return 0
        return self.stops[bisect_right(self.stops, at)] - at

    def next_stop(self, at):
        k = bisect_right(self.stops, at)
        return self.stops[k] if k < len(self.stops) else self.size




class Anim:
    """One animation of a TAE buffer: its content signature, and the pieces (spans, pointer and value places) a copy
    needs."""

    def __init__(self, t, ao, layout, sizes):
        try:
            self._read(t, ao, layout, sizes)
        except (struct.error, IndexError) as error:
            raise MergeError("TAE animation at 0x%X cannot be read: %s" % (ao, error))

    def _read(self, t, ao, layout, sizes):
        eh, eg, times, fo, ec, gc, tc, z = struct.unpack_from("<4q4i", t, ao)
        if ec < 0 or gc < 0 or tc < 0 or _q(t, fo) not in (0, 1):
            raise MergeError("unsupported TAE animation header at 0x%X" % ao)
        self.ao, self.counts = ao, (ec, gc, tc)
        # spans: (start, end) to copy; pointers: [(place of a q pointer, the offset it is moved as)], values: places
        # of i32 event header offsets; blanks: q pointer places that get a fresh empty name.
        spans, pointers, values, blanks = [], [], [], []

        def span(start, size):
            if size:
                if not 0 < start <= start + size <= len(t):
                    raise MergeError("TAE animation piece outside the file")
                spans.append((start, start + size))

        span(eh, ec * 24)
        span(eg, gc * 32)
        span(times, tc * 4)
        file_ptr = _q(t, fo + 8)
        span(fo, 48 if file_ptr else 16)
        if file_ptr:
            if file_ptr != fo + 16:
                raise MergeError("unsupported TAE anim-file block")
            pointers.append((fo + 8, fo + 16))
            name_at = _q(t, fo + 16)
            if not name_at:
                name = None
            elif tc and times <= name_at < times + tc * 4:
                name = ("times", name_at - times)  # some reference blocks point at the time list, not at a name
                pointers.append((fo + 16, name_at))
            elif name_at >= len(t):
                name = b""  # points at the file's end: no name (a copy points at an empty one)
                blanks.append(fo + 16)
            else:
                end = name_at
                limit = layout.next_stop(name_at)
                while end + 2 <= limit and t[end:end + 2] != b"\0\0":
                    end += 2
                name = t[name_at:end]
                span(name_at, min(end + 2, limit) - name_at)
                pointers.append((fo + 16, name_at))
            file_block = (_q(t, fo), 1, name, t[fo + 24:fo + 48])
        else:
            file_block = (_q(t, fo), 0)
        events = []
        for e in range(ec):
            h = eh + e * 24
            start_at, end_at, dat = _q(t, h), _q(t, h + 8), _q(t, h + 16)
            span(start_at, 4)
            span(end_at, 4)
            span(dat, 16)
            args_at = _q(t, dat + 8)
            room = layout.room(args_at)
            span(args_at, room)
            pointers.extend(((h, start_at), (h + 8, end_at), (h + 16, dat),
                             (dat + 8, args_at if room else dat + 16)))
            size = min(sizes.get(_i(t, dat), room), room)
            events.append((t[dat:dat + 8], t[start_at:start_at + 4], t[end_at:end_at + 4],
                           t[args_at:args_at + size].rstrip(b"\0")))
        groups = []
        for g in range(gc):
            h = eg + g * 32
            count, values_at, type_at, zero = struct.unpack_from("<4q", t, h)
            if count < 0 or zero:
                raise MergeError("unsupported TAE event group")
            span(values_at, count * 4)
            span(type_at, 16)
            if values_at:
                pointers.append((h + 8, values_at if count else h))
            if type_at:
                pointers.append((h + 16, type_at))
            members = []
            for k in range(count):
                p = values_at + k * 4
                value = _i(t, p)
                if not eh <= value < eh + ec * 24 or (value - eh) % 24:
                    raise MergeError("TAE event group points outside its animation")
                values.append(p)
                members.append((value - eh) // 24)
            groups.append((tuple(members), t[type_at:type_at + 16] if type_at else b""))
        self.header = (eh, eg, times, fo, z)
        self.spans, self.pointers, self.values, self.blanks = spans, pointers, values, blanks
        self.signature = (file_block, tuple(events), tuple(groups), t[times:times + tc * 4], z)

    def summary(self):
        return "%d events" % self.counts[0]


def check_tae(t):
    if t[:12] != MAGIC or len(t) < 0x80:
        raise MergeError("not a little-endian 64-bit TAE3")


def arg_gaps(t):
    """{event type: the smallest room any of its argument blocks has} of one TAE."""
    check_tae(t)
    layout = Layout(t)
    out = {}
    try:
        for _, ao in anim_table(t):
            eh, ec = _q(t, ao), _i(t, ao + 0x20)
            for e in range(ec):
                dat = _q(t, eh + e * 24 + 16)
                room = layout.room(_q(t, dat + 8))
                kind = _i(t, dat)
                out[kind] = min(out.get(kind, room), room)
    except (struct.error, IndexError) as error:
        raise MergeError("TAE cannot be read: %s" % error)
    return out


def arg_sizes(vanilla, mods):
    """Argument sizes per event type: the smallest room a type's blocks have in any of the files (the game packs
    them tightly; a tighter mod file lowers it), so every file reads the same number of bytes of a type."""
    sizes = {}
    for data in [vanilla] + list(mods):
        for kind, room in arg_gaps(data).items():
            sizes[kind] = min(sizes.get(kind, room), room)
    return sizes


def read_anims(t, sizes, base=None):
    """{id: Anim} of a TAE buffer. `base`: the Layout of the game file a merged file starts with (its replaced
    animations' pieces stay in place, no longer pointed at, and still end the blocks before them)."""
    check_tae(t)
    layout = Layout(t, base)
    out = {}
    for anim_id, ao in anim_table(t):
        if anim_id in out:
            raise MergeError("TAE lists animation %d twice" % anim_id)
        out[anim_id] = Anim(t, ao, layout, sizes)
    return out


def _append(out, blob, phase=0):
    """Appends `blob` so that its start has the given offset modulo 16; returns the start."""
    out.extend(bytes((phase - len(out)) % 16))
    start = len(out)
    out.extend(blob)
    return start


def copy_anim(out, src, anim):
    """Copies animation `anim` of buffer `src` into bytearray `out`; returns the new header offset."""
    intervals = []
    for start, end in sorted(anim.spans):
        if intervals and start <= intervals[-1][1]:
            intervals[-1][1] = max(intervals[-1][1], end)
        else:
            intervals.append([start, end])
    starts, moved = [], []
    for start, end in intervals:
        new = _append(out, src[start:end], start % 16)
        starts.append(start)
        moved.append((start, end, new))

    def where(addr):
        """The new offset of a source offset inside a copied interval (its end included: an empty block)."""
        k = bisect_right(starts, addr) - 1
        if k >= 0 and moved[k][0] <= addr <= moved[k][1]:
            return moved[k][2] + addr - moved[k][0]
        raise MergeError("TAE pointer outside the copied animation")

    for place, target in set(anim.pointers):
        struct.pack_into("<q", out, where(place), where(target))
    for place in anim.blanks:
        struct.pack_into("<q", out, where(place), _append(out, b"\0\0"))
    for p in set(anim.values):
        struct.pack_into("<i", out, where(p), where(_i(src, p)))
    eh, eg, times, fo, _ = anim.header
    ec, gc, tc = anim.counts
    head = bytearray(src[anim.ao:anim.ao + 48])
    for field, count, value in ((0, ec, eh), (8, gc, eg), (16, tc, times), (24, 1, fo)):
        struct.pack_into("<q", head, field, where(value) if count else 0)
    return _append(out, bytes(head))


def write_table(out, rows, base_rows):
    """Points the TAE at animation rows [(id, header offset)] sorted by id. The game's table is reused when the ids are
    the same; otherwise a new table and id range index are appended."""
    if [r[0] for r in rows] == [r[0] for r in base_rows]:
        table = _q(out, 0x58)
        for k, (_, ao) in enumerate(rows):
            struct.pack_into("<q", out, table + k * 16 + 8, ao)
    else:
        table = _append(out, b"".join(struct.pack("<2q", *row) for row in rows))
        groups = []
        for k, (anim_id, _) in enumerate(rows):
            if groups and groups[-1][1] + 1 == anim_id:
                groups[-1][1] = anim_id
            else:
                groups.append([anim_id, anim_id, table + k * 16])
        group_rows = _append(out, b"".join(struct.pack("<2iq", *g) for g in groups))
        group_header = _append(out, struct.pack("<2q", len(groups), group_rows))
        struct.pack_into("<i", out, 0x54, len(rows))
        struct.pack_into("<2q", out, 0x58, table, group_header)
        struct.pack_into("<q", out, 0x70, len(rows))
    if rows:
        struct.pack_into("<q", out, 0x78, rows[0][1])
    struct.pack_into("<i", out, 0x0c, len(out))


def merge_tae(label, vanilla, mods, report):
    """`vanilla`: the game's TAE bytes; `mods`: [(mod, TAE bytes)]. Returns (TAE bytes, animations taken)."""
    if vanilla is None:
        raise MergeError("a TAE the game does not have")
    sizes = arg_sizes(vanilla, [data for _, data in mods])
    base = read_anims(vanilla, sizes)
    for mod, data in mods:
        check_tae(data)
        if data[0x10:0x54] != vanilla[0x10:0x54] or data[0x68:0x70] != vanilla[0x68:0x70]:
            raise MergeError("%s changes the TAE's header, not only its animations" % mod)
    parsed = [(mod, data, read_anims(data, sizes)) for mod, data in mods]
    changes = {}  # id -> [(mod, data, Anim or None)]
    for mod, data, anims in parsed:
        for anim_id in sorted(set(anims) | set(base)):
            mine, theirs = anims.get(anim_id), base.get(anim_id)
            if (mine is None) != (theirs is None) or (mine and mine.signature != theirs.signature):
                changes.setdefault(anim_id, []).append((mod, data, mine))
    out = bytearray(vanilla)
    base_rows = anim_table(vanilla)
    rows = dict(base_rows)
    final_source = {}
    for anim_id in sorted(changes):
        values = changes[anim_id]
        distinct = []
        for _, _, anim in values:
            sig = anim.signature if anim else None
            if sig not in distinct:
                distinct.append(sig)
        if len(distinct) > 1:
            report.conflict("%s animation %d" % (label, anim_id),
                            [(m, a.summary() if a else "deletes it") for m, _, a in values], values[-1][0])
        mod, data, anim = values[-1]
        if anim is None:
            rows.pop(anim_id, None)
            continue
        rows[anim_id] = copy_anim(out, data, anim)
        final_source[anim_id] = anim.signature
    write_table(out, sorted(rows.items()), base_rows)
    result = bytes(out)
    check = read_anims(result, sizes, Layout(vanilla))
    if set(check) != set(rows):
        raise MergeError("merged TAE lost animations")
    for anim_id, anim in check.items():
        want = final_source.get(anim_id, base[anim_id].signature if anim_id in base else None)
        if anim.signature != want:
            raise MergeError("merged TAE animation %d does not read back as its source" % anim_id)
    return result, len(changes)
