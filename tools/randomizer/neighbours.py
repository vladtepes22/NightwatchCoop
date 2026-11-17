"""Maps the game holds in memory together, seen in headless lamp routes' file logs (LIVE, 1 Nov 2026:
37 lamps over all 16 main maps, new-game progress). Each entry was opened from one lamp.
The budget for such a group may not pass the largest group vanilla already holds."""
LOADED_TOGETHER = (
    ("m21_01", "m24_00"),
    ("m23_00", "m24_00", "m28_00"),
    ("m24_00", "m24_01"),
    ("m24_00", "m24_02"),
    ("m24_00", "m27_00"),
    ("m24_01", "m27_00"),
    ("m27_00", "m32_00"),
    ("m35_00", "m36_00"),
)

# Maps the player goes between without the Hunter's Dream, read once from the vanilla game files (DATA, 4 Nov 2026;
# the generator never reads these files for it, so the world input hash is unchanged):
# - WALKED: MSB parts of type 11 (connect collision: type data +0 collision part index, +4 area and block bytes, 255 =
#   any block) in every variant of the main maps' map/mapstudio/*.msb.dcx, a pair when either map names the other.
#   m25_00 also names m30, which is no main map.
# - WARPED: 2003[49] (warp to a lamp) with a fixed lamp id in a main map's own event/*.emevd.dcx, the lamp in another main
#   map (lamp id // 100000 = area, // 10000 % 10 = block): m22_00 12200131 -> 2502959, m24_00 12405263 -> 3402959,
#   m28_00 12800430/431 -> 3202959/3202958, m32_00 13200310 -> 2602959, 13200311 -> 3302959, 13201803 -> 2802958.
#   The lamp warps fed by event parameters (lamps, common 7100/7200/7300, m21_00 12107000/12107200) all lead to or from
#   the Hunter's Dream, which is a hub and so left out. No 2003[14] (map move) and no cutscene that moves to another
#   map is used in the main maps.
WALKED = (
    ("m21_01", "m24_00"), ("m22_00", "m24_00"), ("m23_00", "m24_00"), ("m23_00", "m28_00"), ("m24_00", "m24_01"),
    ("m24_00", "m24_02"), ("m24_00", "m27_00"), ("m24_00", "m28_00"), ("m24_01", "m27_00"), ("m25_00", "m28_00"),
    ("m27_00", "m32_00"), ("m34_00", "m35_00"), ("m35_00", "m36_00"),
)
WARPED = (
    ("m22_00", "m25_00"), ("m24_00", "m34_00"), ("m28_00", "m32_00"), ("m26_00", "m32_00"), ("m32_00", "m33_00"),
)


def load_groups():
    return [list(group) for group in LOADED_TOGETHER]


def connected_pairs():
    """Every pair of maps that load together or that the player goes between, as sorted (a, b) with a < b."""
    pairs = set()
    for group in LOADED_TOGETHER:
        pairs.update((a, b) for a in group for b in group if a < b)
    pairs.update(tuple(sorted(p)) for p in WALKED + WARPED)
    return sorted(pairs)


def connections():
    """{stem: set of connected stems} (connected_pairs, both ways)."""
    out = {}
    for a, b in connected_pairs():
        out.setdefault(a, set()).add(b)
        out.setdefault(b, set()).add(a)
    return out


def group_limits(census, groups):
    """(models, bytes) of the largest vanilla group."""
    most_models, most_bytes = 0, 0
    for group in groups:
        models = set()
        for stem in group:
            models.update(slot["model"] for slot in census.slots[stem])
        most_models = max(most_models, len(models))
        most_bytes = max(most_bytes, sum(census.sizes[m] for m in models))
    return most_models, most_bytes
