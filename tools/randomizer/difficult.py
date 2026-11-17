"""Difficult enemies (option difficult_enemies; One Reborn's bell maidens with bell_maidens 2). User's rule, 4 Nov 2026:
a regular enemy is difficult when its base HP (NpcParam hp of its placed row) is at least `ratio` times the median base HP
of the regular enemies of its own area; bosses, NPCs and scripted-pose enemies never count.

Regular = a slot the census leaves free (kept None). Area = the stem (planner.area_number is one number per stem, so it
is the same partition). Scripted pose = a free slot that still has `needs` (an MSB start animation or an event forced
animation): it counts in the median but is never difficult, and it is outside the pool anyway. Invisible models (chrbnd
under 4 KB, lead decision 4 Nov, D11) never count. Pure: everything comes from the census."""
import statistics

INVISIBLE = 4 << 10


def _hp(census, npc):
    return census.npc_layout.get(census.npc_rows[npc], "hp")


def area_median(census, stem):
    """The median base HP (NpcParam hp of the placed row) of the regular enemies of one area (stem), None without any."""
    free = [s for s in census.slots[stem] if s["kept"] is None]
    return statistics.median(_hp(census, s["npc"]) for s in free) if free else None


def rows(census, ratio=3):
    """Every difficult slot, for the report: [{stem, part, entity, model, think, npc, hp, median, ratio}], map order."""
    out = []
    for stem in sorted(census.slots):
        free = [s for s in census.slots[stem] if s["kept"] is None]
        if not free:
            continue
        median = area_median(census, stem)
        for s in free:
            hp = _hp(census, s["npc"])
            if s["needs"] or census.sizes.get(s["model"], 0) < INVISIBLE or hp < ratio * median:
                continue
            out.append({"stem": stem, "part": s["part"], "entity": s["entity"], "model": s["model"], "think": s["think"],
                        "npc": s["npc"], "hp": hp, "median": median, "ratio": round(hp / median, 2)})
    return out


def candidates(census, ratio=3):
    """{model: [(think, npc)]}: the difficult rows that are also pool candidates (sorted, like census.candidates)."""
    out = {}
    for r in rows(census, ratio):
        pair = (r["think"], r["npc"])
        if pair in census.candidates.get(r["model"], ()):
            out.setdefault(r["model"], set()).add(pair)
    return {m: sorted(v) for m, v in sorted(out.items())}


list = rows  # the name DESIGN_R2 uses (nothing in this module calls the builtin)
