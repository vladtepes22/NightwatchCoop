"""Map event files, edited in place: an instruction is turned into a label, which has no arguments and does
nothing. No size, index or count changes, so skip counts, the party code's event and instruction keys, and
everything not named stay byte-identical. Offsets in the header are 64-bit; events 0x30 bytes, instructions
0x20, parameters 0x20, each relative to its table."""
import struct

LABEL_BANK = 1014
LABEL_IDS = tuple(range(10))  # the label ids vanilla uses
# Instructions whose first byte is a count of lines to skip (emedf: アクションスキップ).
SKIPS = frozenset(((1000, 1), (1000, 3), (1000, 5), (1000, 7), (1003, 1), (1003, 3), (1003, 5), (1003, 7), (1003, 9),
                   (1003, 11), (1003, 14)))
HEADER_OFFSETS =(0x18, 0x28, 0x38, 0x48, 0x58, 0x68, 0x78, 0x88)  # tables; 0x20 instruction count, 0x70 argument bytes, 0xc file size


class Emevd:
    def __init__(self, data):
        self.data = bytearray(data)
        self._q = lambda at: struct.unpack_from("<q", self.data, at)[0]
        self.count = self._q(0x10)
        self.events_at, self.instr_at, self.args_at, self.params_at = self._q(0x18), self._q(0x28), self._q(0x78), self._q(0x58)
        self.index = {}
        self.sources = {}  # {added event: (event it copies, body it was copied for, kind)}: see add_event
        self._noop_labels = {}  # {event: the label id neutralize turns its lines into}
        for i in range(self.count):
            self.index[self._q(self.events_at + i * 0x30)] = self.events_at + i * 0x30

    def event_ids(self):
        return sorted(self.index)

    def _instr(self, event, i):
        at = self.index[event]
        count, first = self._q(at + 8), self._q(at + 0x10)
        if not 0 <= i < count:
            raise IndexError("event %d has no instruction %d" % (event, i))
        return self.instr_at + first + i * 0x20

    def instructions(self, event):
        """[(bank, id, int words)] of one event."""
        at = self.index[event]
        count = self._q(at + 8)
        out = []
        for i in range(count):
            o = self._instr(event, i)
            bank, iid = struct.unpack_from("<ii", self.data, o)
            length, offset = self._q(o + 8), self._q(o + 0x10)
            a = bytes(self.data[self.args_at + offset:self.args_at + offset + length])
            out.append((bank, iid, struct.unpack_from("<%di" % (len(a) // 4), a, 0)))
        return out

    def params(self, event):
        """{instruction index: {argument offset: source offset}}"""
        at = self.index[event]
        count, first = self._q(at + 0x18), self._q(at + 0x20)
        out = {}
        for k in range(count):
            p = self.params_at + first + k * 0x20
            out.setdefault(self._q(p), {})[self._q(p + 8)] = self._q(p + 0x10)
        return out

    def param_entries(self, event):
        """[(instruction index, argument offset, source offset, byte length)], in file order: what add_event takes."""
        at = self.index[event]
        count, first = self._q(at + 0x18), self._q(at + 0x20)
        out = []
        for k in range(count):
            p = self.params_at + first + k * 0x20
            out.append((self._q(p), self._q(p + 8), self._q(p + 0x10), struct.unpack_from("<i", self.data, p + 0x18)[0]))
        return out

    def _free_label(self, event):
        """A label id the event's own lines never use, so no goto of the event lands on it; one id serves every line this
        Emevd neutralizes in the event (vanilla has only labels 0-9: m22 12204839, the Witch's three Mad One spawns,
        uses six and forces 6200 on six lines)."""
        if event in self._noop_labels:
            return self._noop_labels[event]
        used = {iid for bank, iid, _ in self.instructions(event) if bank == LABEL_BANK}
        for iid in reversed(LABEL_IDS):
            if iid not in used:
                self._noop_labels[event] = iid
                return iid
        raise ValueError("event %d has no free label" % event)

    def neutralize(self, event, i):
        """Instruction i does nothing from now on. Its parameter entries leave the event's list."""
        o = self._instr(event, i)
        struct.pack_into("<ii", self.data, o, LABEL_BANK, self._free_label(event))
        struct.pack_into("<q", self.data, o + 8, 0)
        at = self.index[event]
        count, first = self._q(at + 0x18), self._q(at + 0x20)
        kept = [bytes(self.data[self.params_at + first + k * 0x20:self.params_at + first + (k + 1) * 0x20])
                for k in range(count)
                if struct.unpack_from("<q", self.data, self.params_at + first + k * 0x20)[0] != i]
        for k, entry in enumerate(kept):
            self.data[self.params_at + first + k * 0x20:self.params_at + first + (k + 1) * 0x20] = entry
        struct.pack_into("<q", self.data, at + 0x18, len(kept))

    def set_word(self, event, i, k, value):
        """Argument word k of instruction i becomes value, in place (same size)."""
        o = self._instr(event, i)
        if not 0 <= k < self._q(o + 8) // 4:
            raise IndexError("instruction %d of event %d has no word %d" % (i, event, k))
        struct.pack_into("<i", self.data, self.args_at + self._q(o + 0x10) + k * 4, value)

    def restart(self, event):
        return self._q(self.index[event] + 0x28)

    def _insert(self, at, blob):
        """Bytes in at `at`; every table at or after it moves. The header's offsets are the only absolute ones."""
        self.data[at:at] = blob
        for field in HEADER_OFFSETS:
            value = self._q(field)
            if value >= at:
                struct.pack_into("<q", self.data, field, value + len(blob))
        struct.pack_into("<I", self.data, 0xc, len(self.data))
        self.events_at, self.instr_at, self.args_at, self.params_at = self._q(0x18), self._q(0x28), self._q(0x78), self._q(0x58)
        self.index = {self._q(self.events_at + i * 0x30): self.events_at + i * 0x30 for i in range(self.count)}

    def _store_instructions(self, records):
        """Adds whole instruction records at the end of the table; their offset from the table's start."""
        first = self._q(0x20) * 0x20
        self._insert(self.instr_at + first, b"".join(records))
        struct.pack_into("<q", self.data, 0x20, self._q(0x20) + len(records))
        return first

    def _store_args(self, words):
        """Arguments at the end of their table, padded to 8; their offset from the table's start."""
        blob = struct.pack("<%di" % len(words), *words)
        blob += b"\0" * (-len(blob) % 8)
        size = self._q(0x70)
        self._insert(self.args_at + size, blob)
        struct.pack_into("<q", self.data, 0x70, size + len(blob))
        return size

    def _record(self, bank, iid, words):
        offset = self._store_args(words) if words else 0
        return struct.pack("<iiqqq", bank, iid, len(words) * 4, offset, -1)

    def set_body(self, event, instructions):
        """The event now runs these (bank, id, words), with no parameters."""
        first = self._store_instructions([self._record(b, i, w) for b, i, w in instructions])
        at = self.index[event]
        struct.pack_into("<qq", self.data, at + 8, len(instructions), first)
        struct.pack_into("<q", self.data, at + 0x18, 0)

    def silence(self, event):
        """The event does nothing: one label, and it ends."""
        self.set_body(event, [(LABEL_BANK, LABEL_IDS[-1], [])])

    def add_event(self, event, restart, instructions, params=(), source=None):
        """A new event in id order. params: param_entries() of a template, so callers' arguments fill it in. source: for
        a copy of another event, (that event's id, the body it was copied for, kind), kept in `sources` (hostonly.py)."""
        if event in self.index:
            raise ValueError("event %d exists" % event)
        if source is not None:
            self.sources[event] = tuple(source)
        place = sum(1 for e in self.index if e < event)
        self._insert(self.events_at + place * 0x30, struct.pack("<6q", event, 0, 0, 0, 0, restart))
        self.count += 1
        struct.pack_into("<q", self.data, 0x10, self.count)
        self.index = {self._q(self.events_at + i * 0x30): self.events_at + i * 0x30 for i in range(self.count)}
        self.set_body(event, instructions)
        if params:
            for i, _, _, _ in params:
                if not 0 <= i < len(instructions):
                    raise ValueError("a parameter of event %d names instruction %d" % (event, i))
            total = self._q(0x50)
            self._insert(self.params_at + total * 0x20, b"".join(struct.pack("<qqqiI", *entry, 0) for entry in params))
            struct.pack_into("<q", self.data, 0x50, total + len(params))
            struct.pack_into("<qq", self.data, self.index[event] + 0x18, len(params), total * 0x20)

    def append_instruction(self, event, bank, iid, words):
        """One more instruction at the event's end. Earlier indices, so parameters and skips, stay as they are."""
        at = self.index[event]
        count, first = self._q(at + 8), self._q(at + 0x10)
        records = [bytes(self.data[self.instr_at + first + k * 0x20:self.instr_at + first + (k + 1) * 0x20]) for k in range(count)]
        records.append(self._record(bank, iid, words))
        moved = self._store_instructions(records)
        at = self.index[event]
        struct.pack_into("<qq", self.data, at + 8, count + 1, moved)

    def splice(self, event, i, instructions):
        """Instruction i becomes these (bank, id, words). Skips that jump over it grow to match; labels and gotos
        need nothing. An event with parameters is refused (their instruction indices would move)."""
        if self.params(event):
            raise ValueError("event %d has parameters" % event)
        body = self.instructions(event)
        if not 0 <= i < len(body):
            raise IndexError("event %d has no instruction %d" % (event, i))
        grow = len(instructions) - 1
        out = []
        for j, (bank, iid, words) in enumerate(body[:i]):
            words = list(words)
            if (bank, iid) in SKIPS and words and j < i <= j + (words[0] & 0xff):
                if (words[0] & 0xff) + grow > 0xff:
                    raise ValueError("skip too long")
                words[0] += grow
            out.append((bank, iid, words))
        out.extend((b, d, list(w)) for b, d, w in instructions)
        out.extend((b, d, list(w)) for b, d, w in body[i + 1:])
        self.set_body(event, out)

    def swap_ids(self, a, b):
        """Events a and b trade ids: each record takes the other's id and every argument word naming one names the
        other (calls, waits on its flag), so what runs, when, and who waits on it stay the same; only the ids move. Both
        records keep their places in the table (vanilla tables are not in id order: event 0, 50, then the rest)."""
        if a == b or a not in self.index or b not in self.index:
            raise ValueError("cannot swap events %d and %d" % (a, b))
        writes = {}  # by absolute place, so an argument block two instructions share is swapped once
        for event in self.event_ids():
            for i, (_, _, words) in enumerate(self.instructions(event)):
                o = self._instr(event, i)
                for k, word in enumerate(words):
                    if word in (a, b):
                        writes[self.args_at + self._q(o + 0x10) + k * 4] = b if word == a else a
        for at, value in writes.items():
            struct.pack_into("<i", self.data, at, value)
        struct.pack_into("<q", self.data, self.index[a], b)
        struct.pack_into("<q", self.data, self.index[b], a)
        self.index[a], self.index[b] = self.index[b], self.index[a]
        moved = {a: b, b: a}
        self.sources = {moved.get(e, e): s for e, s in self.sources.items()}

    def bytes(self):
        return bytes(self.data)
