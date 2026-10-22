"""tools/party/save_decode.py on a synthetic character save (no game data, no real save)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "party"))
import save_decode as sd  # noqa: E402


def make_save(flags=(), goods=(), name="Hunter", level=22, echoes=3993):
    d = bytearray(0x60000)
    struct.pack_into("<III", d, 0, 0x41, 0x18000000, 6660776)
    pool = 0x1000
    struct.pack_into("<II", d, 0x0c, 0x54, 0x100)  # an unrelated section first
    struct.pack_into("<II", d, 0x34, sd.POOL_BYTES + 1, pool + sd.POOL_BYTES + 1)
    for f in flags:
        k = sd.group_key(f)
        d[pool + k * 125 + (f % 1000) // 8] |= 0x80 >> (f % 8)
    at = 0x40000
    for gid, n in goods:
        struct.pack_into("<IIII", d, at, 0xb0000000 | gid, 0x40000000 | gid, n, 0)
        at += 16
    n_at = 0x30000
    d[n_at:n_at + 2 * len(name) + 2] = name.encode("utf-16le") + b"\0\0"
    struct.pack_into("<II", d, n_at - 0x94, 512, 616)
    struct.pack_into("<II", d, n_at - 0x18, level, echoes)
    return bytes(d), pool


def test_group_keys_cover_the_pool_once():
    assert len(sd.BASE_IDS) == 1200
    assert sorted(sd.BASE_IDS) == list(range(1200))
    for k, base in sd.BASE_IDS.items():
        assert sd.group_key(base) == k
    assert sd.group_key(2400) == 10  # global zone 2
    assert sd.group_key(60009036) == 3 + 9 * 5  # the C3 ledger: type-6 global zone 9
    assert sd.group_key(12411800) == 1 * 115 + 50 + 0 + 5  # m24_01 type 1 zone 1


def test_flags_items_character():
    want = [2400, 9457, 12411700, 12411800, 60009025, 60009036, 72300200]
    data, pool = make_save(want, goods=[(1000, 14), (4000, 1)])
    s = sd.Save(data)
    assert s.pool == pool and s.version == 0x41 and s.revive_wait() == 0
    assert s.set_flags() == want
    assert s.flag(12411800) and not s.flag(12301800)
    assert (4000, 1) in [(i, n) for _, i, n in s.goods()]
    c = s.character()
    assert c == {"name": "Hunter", "hp": 512, "hp_max": 616, "level": 22, "echoes": 3993}


def test_host_log_compare(tmp_path):
    log = tmp_path / "host.log"
    log.write_text("Party progress: host flag 12411800 = 1 (boss_defeated) seq 1\n"
                   "Party progress: host flag 72100100 = 1 (npc_quest) seq 2\n"
                   "Party progress: host flag 72100100 = 0 (npc_quest) seq 3\n", encoding="utf-8")
    last = sd.host_flags([log])
    assert last[12411800][0] == 1 and last[72100100][0] == 0 and last[72100100][3] == 3


def world_save(blocks=(0x24, 0x2e, 0x14)):
    """A 0x140000-byte character save with the writer's layout around the world-state section (header
    +0x34..+0x50): the flag store, the CHR / OBJ / DECL blocks of the given sizes (0: empty), the position."""
    d = bytearray(0x140000)
    struct.pack_into("<III", d, 0, 0x41, 0x15000000, 1000)
    pool = 0x1000
    a = pool + sd.POOL_BYTES + 1
    struct.pack_into("<II", d, 0x34, pool, sd.POOL_BYTES + 1)
    at = a
    for magic, n in zip(sd.Save.WORLD_MAGIC, blocks):
        struct.pack_into("<I", d, at, n)
        at += 4
        if n:
            d[at:at + 4] = magic
            at += n
    an = at - a
    struct.pack_into("<6I", d, 0x3c, a, an, at, an + 0x50, at + 0x50, 0x200)
    return bytes(d)


def test_world_state():
    s = sd.Save(world_save())
    assert s.world()["blocks"] == [0x24, 0x2e, 0x14] and s.world_empty() is False
    empty = sd.Save(world_save((0, 0, 0)))  # written in the host's world
    assert empty.world()["size"] == 12 and empty.world_empty() is True
    broken = bytearray(world_save())
    broken[0x40] ^= 1  # the header no longer lines up
    assert sd.Save(bytes(broken)).world() is None and sd.Save(bytes(broken)).world_empty() is None
