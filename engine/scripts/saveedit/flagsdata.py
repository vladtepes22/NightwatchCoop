"""The event flags the editor offers by name: boss kills, lit lamps, ending flags.

The ids are the lists scripts/party_session.py checks after a party session (tests/test_saveedit.py keeps
them equal). Names come from the player's own game files at run time (names.Catalogue: lamp warp-list
texts and NPC/boss names); this file holds only ids and the area of each (own strings):
  lamp flag 7AAB02nn   = the n-th lamp of map AAB (m24_01 = AAB 241); its warp-list text is menu id 7000AABn
                         (4 lamps of Central Yharnam = 70002410..13: lamp counts per area agree, and
                         72102410 "1st Floor Sickroom" -> lamp 2412950 in docs/party/travel.md)
  boss kill flag 1AAB1800 -> the chr id of the boss, from docs/party/campaign_soak.md and event_flags.md
"""
BOSS_IDS = (12101800, 12101850, 12201800, 12301800, 12401800, 12411800, 12421800, 12501800, 12601800,
            12601850, 12701800, 12801800, 13201800, 13301800, 13401800, 13401850, 13501800, 13501850,
            13601800)
LAMP_IDS = (72110200, 72200200, 72200201, 72300200, 72300201, 72300202, 72400200, 72400201, 72410200,
            72410201, 72410202, 72410203, 72420200, 72420201, 72420202, 72500200, 72500201, 72500202,
            72600200, 72600201, 72600202, 72600203, 72700200, 72700201, 72800200, 72800201, 72800202,
            72800203)
ENDING_IDS = (21, 22, 23, 6600, 6601, 6602, 6603, 6604)
ENDINGS_SEEN = {6600: "Yharnam Sunrise seen", 6601: "Honoring Wishes seen", 6602: "Childhood's Beginning seen",
                6603: "Any ending seen"}
# boss kill flag -> chr id in the game's NPC name list (None: no evidence which boss it is)
BOSS_CHR = {12101800: 804000, 12101850: 540000, 12201800: 210000, 12301800: 209000, 12401800: 502000,
            12411800: 271000, 12421800: 251000, 12501800: 232000, 12601800: 551000, 12601850: 899000,
            12701800: 212010, 12801800: 507000, 13201800: 510000, 13301800: 512000, 13401800: 451000,
            13401850: 403000, 13501800: 452000, 13501850: None, 13601800: 453000}
AREAS = {210: "Hunter's Dream", 211: "Abandoned Old Workshop", 220: "Hemwick Charnel Lane", 230: "Old Yharnam",
         240: "Cathedral Ward", 241: "Central Yharnam", 242: "Upper Cathedral Ward", 250: "Cainhurst",
         260: "Nightmare of Mensis", 270: "Forbidden Woods", 280: "Yahar'gul", 320: "Byrgenwerth",
         330: "Nightmare Frontier", 340: "Hunter's Nightmare", 350: "Research Hall", 360: "Fishing Hamlet"}
ORDER = list(AREAS)


def group_of(fid):
    aab = fid // 10**4 % 1000
    return AREAS.get(aab) or AREAS.get(aab // 10 * 10, f"Map {aab}")


def bosses(cat=None):
    """[(flag, group, label)] with the flag id in brackets."""
    out = []
    for f in BOSS_IDS:
        chr_id = BOSS_CHR.get(f)
        name = (cat.boss_name(chr_id) if cat is not None and chr_id else None) or "Boss"
        out.append((f, group_of(f), f"{name} [{f}]"))
    return out


def lamps(cat=None):
    out = []
    for f in LAMP_IDS:
        name = (cat.lamp_name(f) if cat is not None else None) or f"Lamp {f % 100 + 1}"
        out.append((f, group_of(f), f"{name} [{f}]"))
    return out


def endings():
    """Only the 'ending seen' records (docs/party/cutscenes_endings.md). 21-23 start an ending's credits and
    NG+ on the next load and 6604 strips the key items on the next load, so they are never offered."""
    return [(f, "Endings", label) for f, label in ENDINGS_SEEN.items()]
