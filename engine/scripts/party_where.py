# SPDX-License-Identifier: GPL-3.0-or-later
"""Where a hosting game is, in words (0.7.8): the table behind the launcher's 'Area' column and the
'Currently in: ...' line of the party lists (LAN discovery and the Nightwatch VPN lobby).

A host announces three small codes and nothing else about its place (no coordinates, no names):
  area   the map id 0xAABBCCDD of its world (0 unknown); only AA, the area, is turned into words
  phase  0 unknown, 1 day, 2 evening, 3 night, 4 Blood Moon   (the game's time-of-day flags 9800-9802)
  ng     0 = NG, 1..7 = NG+n, 255 unknown
  boss   a boss fight (fog) is on
The Nightwatch launcher can render the same line from the same codes: see docs/party/invasions.md.
"""

# AA (the map id's top byte) -> English name. Mirrors gpu/shim/party/party_split_notice.h AreaNameOf, plus the Chalice
# Dungeons (m29) as one name.
AREA_NAMES = {
    21: "Hunter's Dream",
    22: "Hemwick Charnel Lane",
    23: "Old Yharnam",
    24: "Cathedral Ward",
    25: "Cainhurst Castle",
    26: "Nightmare of Mensis",
    27: "Forbidden Woods",
    28: "Yahar'gul",
    29: "Chalice Dungeon",
    30: "Chalice Dungeon",
    32: "Byrgenwerth",
    33: "Nightmare Frontier",
    34: "Hunter's Nightmare",
    35: "Research Hall",
    36: "Fishing Hamlet",
}

PHASE_NAMES = {1: "day", 2: "evening", 3: "night", 4: "Blood Moon"}


def area_name(map_id: int) -> str:
    """The area's name for a map id, or '' when it is unknown / not announced."""
    if not isinstance(map_id, int) or map_id <= 0:
        return ""
    return AREA_NAMES.get((map_id >> 24) & 0xFF, "")


def phase_name(phase: int) -> str:
    return PHASE_NAMES.get(phase, "")


def ng_name(ng: int) -> str:
    """'NG', 'NG+1' ... 'NG+7'; '' when unknown (255)."""
    if not isinstance(ng, int) or ng < 0 or ng > 7:
        return ""
    return "NG" if ng == 0 else f"NG+{ng}"


def where_text(map_id: int = 0, phase: int = 0, ng: int = 255, boss: bool = False) -> str:
    """'Yahar'gul (Blood Moon), NG+1, boss fight' - the part after 'Currently in: '. '' when nothing is known."""
    area = area_name(map_id)
    parts = []
    if area:
        ph = phase_name(phase)
        parts.append(f"{area} ({ph})" if ph else area)
    elif phase_name(phase):
        parts.append(phase_name(phase)[0].upper() + phase_name(phase)[1:])
    if ng_name(ng):
        parts.append(ng_name(ng))
    if boss:
        parts.append("boss fight")
    return ", ".join(parts)


def currently_in(map_id: int = 0, phase: int = 0, ng: int = 255, boss: bool = False) -> str:
    text = where_text(map_id, phase, ng, boss)
    return f"Currently in: {text}" if text else ""
