"""The event flags the editor offers by name: boss kills, lit lamps, ending flags.

Nothing of the game's data is stored here. The boss and lamp flag lists are derived from the player's own game
files at run time (derive(); names.Catalogue calls it and caches the result next to the item names): the flags
the party's category table (scripts/partydata/flag_tool.py category_rows) files under boss_defeated and
lamp_unlocked, in the main game and the DLC, chalice dungeons left out. Names come from the same files (lamp
warp-list texts and NPC/boss names). This file holds the rules and the area names (own strings):
  lamp object flags 1AAB7810 + 20n and +11 = the same lamp's object (set with the lamp-lit flag by the game's own
                         lighting; a save with the lit flag alone has the lamp in the travel list but dead at the
                         lamp itself, GitHub issue #28), see lamp_flags()
  lamp flag 7AAB02nn   = the n-th lamp of map AAB (m24_01 = AAB 241); its warp-list text is menu id 7000AABn
                         (4 lamps of Central Yharnam = 70002410..13: lamp counts per area agree, and
                         72102410 "1st Floor Sickroom" -> lamp 2412950 in docs/party/travel.md)
  boss kill flag 1AAB1800 -> the boss's name id: the name its health bar shows (the map's DisplayBossHealthBar
                         2003[11] of the entity its defeat event names), the key of its text in the NPC name list
"""
BOSS_BAR = (2003, 11)
# The 'ending seen' records the editor offers (our own labels).
ENDINGS_SEEN = {6600: "Yharnam Sunrise seen", 6601: "Honoring Wishes seen", 6602: "Childhood's Beginning seen",
                6603: "Any ending seen"}
ENDING_IDS = (21, 22, 23, 6600, 6601, 6602, 6603, 6604)  # our own list: ending A/B/C reached, the endings' unlocks, "ending reached"
AREAS = {210: "Hunter's Dream", 211: "Abandoned Old Workshop", 220: "Hemwick Charnel Lane", 230: "Old Yharnam",
         240: "Cathedral Ward", 241: "Central Yharnam", 242: "Upper Cathedral Ward", 250: "Cainhurst",
         260: "Nightmare of Mensis", 270: "Forbidden Woods", 280: "Yahar'gul", 320: "Byrgenwerth",
         330: "Nightmare Frontier", 340: "Hunter's Nightmare", 350: "Research Hall", 360: "Fishing Hamlet"}
ORDER = list(AREAS)


def group_of(fid):
    aab = fid // 10**4 % 1000
    return AREAS.get(aab) or AREAS.get(aab // 10 * 10, f"Map {aab}")


def _partydata():
    """flag_tool / ids_tool of scripts/partydata (ImportError when this copy ships without them)."""
    import sys
    from pathlib import Path
    d = str(Path(__file__).resolve().parents[1] / "partydata")
    if d not in sys.path:
        sys.path.insert(0, d)
    import flag_tool
    import ids_tool
    return flag_tool, ids_tool


def derive(game):
    """{"bosses": [[flag, NPC name id or 0]], "lamps": [flag]} from the game folder (the one holding event/
    and param/): boss kill flags 1AAB1700 / 1AAB1800 / 1AAB1850 filed under boss_defeated and lit-lamp flags 7AAB02nn
    under lamp_unlocked by the party's category table, areas below 40 without the chalice area 29. Raises when
    the files cannot be read."""
    from pathlib import Path
    ft, it = _partydata()
    game = Path(game)
    ev = game / "event"
    saved = dict(ft.MARKERS)
    ft.MARKERS[BOSS_BAR] = "boss_bar"  # 2003[11] DisplayBossHealthBar(on, entity, slot, name id): the boss's name
    try:
        allrec = ft.expand_all(ev, ft.load_emedf(ft.find_emedf(ev)))
    finally:
        ft.MARKERS.clear()
        ft.MARKERS.update(saved)
    rows = ft.category_rows(game, allrec)[0]
    bosses, lamps = set(), set()
    for lo, hi, cat, _note in rows:
        if hi - lo > 2000 or cat not in ("boss_defeated", "lamp_unlocked"):
            continue
        for f in range(lo, hi + 1):
            area = f // 10**5 % 100
            if area == 29 or area >= 40:
                continue
            # 1AAB1700: the mid-boss kills with a health bar (Cleric Beast, Darkbeast Paarl, Celestial Emissary);
            # Nightwatch lists them as 0.2.7's editor did (22 bosses, not 19).
            if cat == "boss_defeated" and 10**7 <= f < 2 * 10**7 and f % 1000 in (700, 800, 850):
                pass
            elif cat == "lamp_unlocked" and 7 * 10**7 <= f < 8 * 10**7 and f // 100 % 100 == 2:
                pass
            else:
                continue
            if ft.categorize(rows, f)[0] == cat:
                (bosses if cat == "boss_defeated" else lamps).add(f)
    entity, bars, bar_events = {}, {}, {}
    for stem, recs in allrec.items():
        for r in recs:
            for m in r["markers"]:
                if m["m"] in ("boss_defeat", "midboss_defeat") and m["v"] and r["event"] + r["slot"] in bosses:
                    entity[r["event"] + r["slot"]] = (stem, m["v"][0])
                elif m["m"] == "boss_bar" and len(m["v"]) >= 4 and m["v"][0] == 1 and m["v"][3] > 0:
                    bars.setdefault((stem, m["v"][1]), m["v"][3])
                    bar_events.setdefault((stem, r["event"]), m["v"][3])
    out = []
    for f in sorted(bosses):
        stem, ent = entity.get(f, (None, None))
        # The bar names the fight's entity; a boss whose defeat event names another entity than its bar
        # (m26's 2600803 and 2600802) is found by the bar's event, always the kill event + 3002 (1AAB4802 for 1AAB1800).
        name_id = bars.get((stem, ent)) or bar_events.get((stem, f + 3002)) or 0
        out.append([f, name_id])
    return {"bosses": out, "lamps": sorted(lamps)}


def derive_mod(game, event_dir, known=()):
    """[[flag, bar name id]] of the bosses the mod's own event files (a Nocturne `event` folder) define whose kill
    flag is not in `known` (the game's list [[flag, name id]]): the defeat markers (2003[12] / [53] / [15]) of its map
    events, the flag being the event's id, so a boss outside the 1AAB1700/1800/1850 pattern (the Lesser Demon,
    12301820) is found too. `game` is the game folder holding event/common.emedf. Raises when unreadable."""
    from pathlib import Path
    ft, _ = _partydata()
    saved = dict(ft.MARKERS)
    ft.MARKERS[BOSS_BAR] = "boss_bar"
    try:
        allrec = ft.expand_all(Path(event_dir), ft.load_emedf(ft.find_emedf(Path(game) / "event")))
    finally:
        ft.MARKERS.clear()
        ft.MARKERS.update(saved)
    seen = {f for f, _n in known}
    out = {}
    for stem, recs in allrec.items():
        if stem == "common":
            continue
        for r in recs:
            f = r["event"] + r["slot"]
            area = f // 10**5 % 100
            if f in seen or not 10**7 <= f < 2 * 10**7 or area == 29 or area >= 40:
                continue
            ents = [m["v"][0] for m in r["markers"] if m["m"] in ("boss_defeat", "midboss_defeat") and m["v"]]
            if ents:
                bar = next((m["v"][3] for m in r["markers"] if m["m"] == "boss_bar" and len(m["v"]) >= 4
                            and m["v"][1] == ents[0] and m["v"][3] > 0), 0)
                out[f] = bar or out.get(f, 0)
    return [[f, out[f]] for f in sorted(out)]


def bosses(cat=None):
    """[(flag, group, label)] with the flag id in brackets; the enabled Nocturne mod's own bosses
    (names.read_mod_bosses, Catalogue.mod_bosses) follow the game's."""
    out = []
    for f, chr_id in (cat.progress.get("bosses", ()) if cat is not None else ()):
        name = (cat.boss_name(chr_id) if chr_id else None) or "Boss"
        out.append((f, group_of(f), f"{name} [{f}]"))
    for f, name in (getattr(cat, "mod_bosses", None) or ()):
        out.append((f, group_of(f), f"{name} [{f}]"))
    return out


def lamp_flags(lit):
    """All the flags of the lamp whose lamp-lit flag is `lit` (7AAB02nn): the flag itself and its object's
    1AAB7810 + 20n and +11, which the game's own lighting sets with it (lampshare::LitFlags in the game)."""
    base = 10000000 + (lit // 10000 % 1000) * 10000 + 7800 + 20 * (lit % 100)
    return (lit, base + 10, base + 11)


def lamp_ids(cat=None):
    return tuple(cat.progress.get("lamps", ())) if cat is not None else ()


# Flags that go with a boss's kill flag. Ludwig's arena events test 9471 (his "dead" flag, docs/party/cutscenes_endings.md)
# and not the kill-event flag alone, so reviving or killing him in the editor sets or clears both.
BOSS_COMPANIONS = {13401800: (9471,)}


def flag_group(fid, cat=None):
    """All the flags edited together for one named boss, lamp or ending entry: a lamp is its lamp-lit flag and
    the lamp object's two flags, a boss with companions its kill flag and theirs, anything else itself.
    `cat`: the names.Catalogue whose derived lamp list says which flags are lamps."""
    if fid in lamp_ids(cat):
        return lamp_flags(fid)
    return (fid,) + BOSS_COMPANIONS.get(fid, ())


def lamps(cat=None):
    out = []
    for f in lamp_ids(cat):
        name = (cat.lamp_name(f) if cat is not None else None) or f"Lamp {f % 100 + 1}"
        out.append((f, group_of(f), f"{name} [{f}]"))
    return out


# The world's time of day: 9800 evening, 9801 night, 9802 Blood Moon, set together as the story moves on
# (0 day, 1 evening = 9800, 2 night = 9800 + 9801, 3 Blood Moon = all three; party_story.cpp kTodTable).
MOON_FLAGS = (9800, 9801, 9802)
MOON_PHASES = ("Day", "Evening", "Night", "Blood Moon")


def moon_phase(model):
    """0 day .. 3 Blood Moon: the latest phase whose flags are all set (party_story.cpp TodFromFlagValue);
    None when the save has no room for these flags."""
    bits = [model.flag(f) for f in MOON_FLAGS]
    if None in bits:
        return None
    phase = 0
    for i in range(1, 4):
        if all(bits[:i]):
            phase = i
    return phase


# The Progress tab's "Time of day" list: the same three flags one by one, under the group and names the launcher translates.
TIME_OF_DAY = {9800: "Evening reached", 9801: "Night", 9802: "Blood Moon"}


def time_of_day():
    return [(f, "Time of day", f"{label} [{f}]") for f, label in TIME_OF_DAY.items()]


def set_moon_phase(model, phase):
    if not isinstance(phase, int) or isinstance(phase, bool) or not 0 <= phase <= 3:
        raise ValueError("invalid moon phase")
    for i, f in enumerate(MOON_FLAGS):
        model.set_flag(f, i < phase)


def endings():
    """Only the 'ending seen' records (docs/party/cutscenes_endings.md). 21-23 start an ending's credits and
    NG+ on the next load and 6604 strips the key items on the next load, so the editor never offers them
    (Nightwatch; visy's editor offers all eight)."""
    return [(f, "Endings", label) for f, label in ENDINGS_SEEN.items()]
