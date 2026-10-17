#!/usr/bin/env python3
"""Decode a Bloodborne 1.09 character save (userdataNNNN, save version 0x41) without the game.

  python -I tools/party/save_decode.py USERDATA [--flags ID,ID...] [--dump] [--inc gpu/shim/party/party_flags.inc]
                                       [--host-log H.log ...]

Saves are untrusted input: run with python -I. Prints the character (name, level, echoes, HP), the
key-item list and the progress flags the party cares about; --dump lists every set event flag with
its party category; --host-log compares the host's logged flag changes ("host flag N = V (cat)",
the last value of each) with this save and prints every difference. Written for the playtest 1
save audit (docs/party/playtest1_save_audit.md).

Layout (found 2026-10-14 on real saves, cross-checked against the flag store notes in
docs/party/event_flags.md):
  0x00 u32 save version (0x41); 0x04 u32 packed map id (area << 24 | block << 16); 0x08 u32 play time (ms)
  0x0c.. a table of u32 pairs {size, end offset}; the section of size 150001 is the event flag store:
          1200 groups x 125 bytes (the SprjEventFlagMan pool, group key = pool index) + 1 byte
          (SprjEventState +0x26 revive-wait).
  Group key of a flag id TAABZnnn (resolve_group_key 0x13bc710): ids < 10000 -> zone*5; global
  types 1/5/6/7 (AA=0, B=0) -> typeIdx{0,1,5,6,7} + zone*5; map types 1/5/6/7/9 -> zone*115 + 50 +
  23*typeIdx{1,5,6,7,9} + mapIdx (17 maps 21_00 .. 36_00; area 40 -> 11; areas 41-46 -> 17-22).
  Bit (nnn) = data[nnn >> 3] & (0x80 >> (nnn & 7)).
  Inventory entries {u32 handle (0xb... goods), u32 item id (category << 28 | id), u32 count, u32 index};
  the key-item list (goodsType 1) is a u32 count + 64 entries; character name UTF-16, stats before it.
"""
import argparse, re, struct, sys
from pathlib import Path

MAPS = [(21, 0), (21, 1), (22, 0), (23, 0), (24, 0), (24, 1), (24, 2), (25, 0), (26, 0), (27, 0), (28, 0),
        (29, 0), (32, 0), (33, 0), (34, 0), (35, 0), (36, 0)]
GLOBAL_TYPES = {0: 0, 1: 1, 5: 2, 6: 3, 7: 4}
MAP_TYPES = {1: 0, 5: 1, 6: 2, 7: 3, 9: 4}
POOL_BYTES = 1200 * 125
KEY_ITEMS = {4000: "Oedon Tomb Key", 4011: "Hunter Chief Emblem", 4103: "Blood Gem Workshop Tool",
             4114: "Sword Hunter Badge", 4312: "Old Hunter Bell"}


def group_key(fid):
    t, aa, b, z = fid // 10**7, (fid // 10**5) % 100, (fid // 10**4) % 10, (fid // 1000) % 10
    if fid < 10000:
        return z * 5
    if aa == 0 and b == 0 and t in GLOBAL_TYPES:
        return GLOBAL_TYPES[t] + z * 5
    if t not in MAP_TYPES:
        return None
    if (aa, b) in MAPS:
        mi = MAPS.index((aa, b))
    elif aa == 40:
        mi = 11
    elif 41 <= aa <= 46 and b == 0:
        mi = 17 + aa - 41
    else:
        return None
    return z * 115 + 50 + 23 * MAP_TYPES[t] + mi


def _base_ids():
    base = {}
    for t, ti in GLOBAL_TYPES.items():
        for z in range(10):
            base.setdefault(ti + z * 5, t * 10**7 + z * 1000)
    for t, ti in MAP_TYPES.items():
        for z in range(10):
            for mi, (a, b) in enumerate(MAPS):
                base.setdefault(z * 115 + 50 + 23 * ti + mi, t * 10**7 + a * 10**5 + b * 10**4 + z * 1000)
            for aa in range(41, 47):
                base.setdefault(z * 115 + 50 + 23 * ti + 17 + aa - 41, t * 10**7 + aa * 10**5 + z * 1000)
    return base


BASE_IDS = _base_ids()


class Save:
    def __init__(self, data):
        self.d = bytes(data)
        if len(self.d) < 0x200:
            raise ValueError("not a character save (too small)")
        self.version, self.map_id, self.play_ms = struct.unpack_from("<III", self.d, 0)
        self.pool = None
        for i in range(0x0c, 0x200, 4):
            size, end = struct.unpack_from("<II", self.d, i)
            if size == POOL_BYTES + 1 and size <= end <= len(self.d):
                self.pool = end - size
                break
        if self.pool is None:
            raise ValueError("no event flag section (size 150001) in the section table")

    @classmethod
    def read(cls, path):
        return cls(Path(path).read_bytes())

    def flag(self, fid):
        k = group_key(fid)
        if k is None:
            return None
        return bool(self.d[self.pool + k * 125 + (fid % 1000) // 8] & (0x80 >> (fid % 8)))

    def revive_wait(self):
        return self.d[self.pool + POOL_BYTES]

    def set_flags(self):
        out = []
        for k in range(1200):
            blk = self.d[self.pool + k * 125:self.pool + k * 125 + 125]
            for i, byte in enumerate(blk):
                for j in range(8):
                    if byte & (0x80 >> j):
                        out.append(BASE_IDS[k] + i * 8 + j)
        return sorted(out)

    def goods(self):
        """(offset, id, count) of every goods entry (inventory, key items, storage)."""
        out = []
        for i in range(0, len(self.d) - 12, 4):
            h, iid, n = struct.unpack_from("<III", self.d, i)
            if iid >> 28 == 4 and h >> 28 == 0xb and h & 0xfffffff == iid & 0xfffffff and 0 < n < 10000:
                out.append((i, iid & 0xfffffff, n))
        return out

    def character(self):
        m = re.search(rb"(?:[A-Za-z\xc0-\xff]\x00){4,16}\x00\x00", self.d[:0x40000])
        if not m:
            return {}
        n = m.start()
        name = m.group()[:-2].decode("utf-16le", "replace")
        hp, hp_max = struct.unpack_from("<II", self.d, n - 0x94)
        level, echoes = struct.unpack_from("<II", self.d, n - 0x18)
        return {"name": name, "hp": hp, "hp_max": hp_max, "level": level, "echoes": echoes}


def host_flags(logs):
    rx = re.compile(r"host flag (\d+) = ([01]) \(([a-z_]+)\)")
    last = {}
    for p in logs:
        for ln, line in enumerate(Path(p).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = rx.search(line)
            if m:
                last[int(m.group(1))] = (int(m.group(2)), m.group(3), Path(p).name, ln)
    return last


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("save")
    ap.add_argument("--flags", default="")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--inc", default=str(Path(__file__).resolve().parents[2] / "gpu/shim/party/party_flags.inc"))
    ap.add_argument("--host-log", action="append", default=[])
    a = ap.parse_args()
    s = Save.read(a.save)
    print(f"version {s.version:#x}, map {s.map_id >> 24}_{(s.map_id >> 16) & 0xff:02d}, play time "
          f"{s.play_ms / 3.6e6:.2f} h, flag store at {s.pool:#x}, revive-wait {s.revive_wait()}")
    print("character:", s.character())
    keys = [(i, n) for _, i, n in s.goods() if 4000 <= i < 5000]
    print("key items:", ", ".join(f"{i} {KEY_ITEMS.get(i, '')} x{n}".replace("  ", " ") for i, n in keys) or "none")
    for f in filter(None, a.flags.split(",")):
        print(f"flag {f}: {s.flag(int(f))}")
    rows = []
    if a.dump and Path(a.inc).is_file():
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import flag_tool as ft  # noqa: E402
        rows = ft.load_inc(a.inc)
    if a.dump:
        for f in s.set_flags():
            cat = ft.categorize(rows, f)[0] if rows else ""
            print(f, cat or "")
    if a.host_log:
        diff = 0
        last = host_flags(a.host_log)
        for fid, (v, cat, name, ln) in sorted(last.items()):
            if bool(v) != s.flag(fid):
                diff += 1
                print(f"differs: {fid} host {v} save {int(bool(s.flag(fid)))} ({cat}; {name}:{ln})")
        print(f"{len(last)} host flags compared, {diff} differ")


if __name__ == "__main__":
    main()
