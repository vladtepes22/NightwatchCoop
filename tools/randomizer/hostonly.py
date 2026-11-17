"""Co-op: copied phase events keep the party's host-only rule.

The party code takes no guest's event state for a fixed list of boss phase events (engine
party_bossfog_rules.h, HostOnlyBossEvent): there a guest's copy of the event runs on the guest's own boss, whose HP can
disagree with the host's, and the host would play the phase as done. The rule is keyed on the event id alone. A boss
that moves brings its phase events as copies under new ids (boss_events.move_phases, helpers.travel), which the rule
does not know: the host takes a guest's state for them (co-op breaker, 5 Nov: the Blood-starved Beast's phase copy in
the Cleric Beast's arena was applied on the host, while vanilla's 12414707 there is dropped).

`cover` gives such copies listed ids the map already has: a listed event of this map that the generator emptied
(boss_events silenced it, helpers parked it) trades ids with a copy (Emevd.swap_ids: a pure relabelling, so what runs and
who waits on what stay the same; the emptied event goes on under the copy's old id). Never an id of another map, never a
listed event that still runs. A copy no listed id can hold is reported (`uncovered`): parity with vanilla needs no more
where the map lists none (m25, m26, m35 list nothing: vanilla lets a guest's state through there too). The uncovered
copies go into the world's PARTY_FILE (party_ids), which a host's engine adds to the list: those of a map that lists ids,
and copies of listed events in any map."""
import arenas
from emevd import LABEL_BANK, LABEL_IDS

# Mirror of the engine's list (HostOnlyBossEvent kIds); tests/test_hostonly.py checks it equals the header.
HOST_ONLY = frozenset((
    12104807, 12204807, 12304707, 12304807, 12304808, 12404807, 12414707, 12424980, 12704806,
    12704812, 12804870, 12804871, 12904028, 12904735, 12904868, 12904884, 12904889, 12906766,
    12906767, 12906863, 12906869, 12906870, 13204807, 13304807, 13304808, 13404820, 13404821,
    13404822, 13404823, 13404824, 13404825, 13604820))
# The arena whose boss each main-map listed event drives (DATA, vanilla: the bodies it names; the test re-derives it).
OWNER = {12104807: "gehrman", 12204807: "witch", 12304707: "paarl", 12304807: "blood_starved_beast",
         12304808: "blood_starved_beast", 12404807: "amelia", 12414707: "cleric_beast", 12424980: "ebrietas",
         12704806: "shadows", 12704812: "shadows", 12804870: "one_reborn", 12804871: "one_reborn", 13204807: "rom",
         13304807: "amygdala", 13304808: "amygdala", 13404820: "ludwig", 13404821: "ludwig", 13404822: "ludwig",
         13404823: "ludwig", 13404824: "ludwig", 13404825: "ludwig", 13604820: "orphan"}
# A host's mods name more host-only events in this file at the mod folder's root, beside dvdroot_ps4 (bb-start and the
# engine's scripts/mods.py merge the enabled mods' lists into out/party_host_only.txt; the party host reads it): one
# decimal event id per line, '#' comments, at most PARTY_MAX ids over all mods (more, and none are used). Outside
# dvdroot_ps4 it is no game file and counts in no party or mods hash.
PARTY_FILE = "party_host_only.txt"
PARTY_MAX = 4096
HP_RATIO = (4, 2)
AI_SENDS = frozenset(((2004, 17), (2004, 20), (2003, 18)))  # AI command, replan, forced animation
IF_FLAG = (3, 0)


def emptied(events, event):
    """The event does nothing any more: silenced (one label, boss_events) or parked (one wait on a flag, helpers.park)."""
    if events.params(event):
        return False
    body = events.instructions(event)
    if len(body) != 1:
        return False
    bank, iid, words = body[0]
    return (bank == LABEL_BANK and iid in LABEL_IDS and not words) or ((bank, iid) == IF_FLAG and len(words) == 2
                                                                       and words[0] == 256)


def rank(events, event):
    """How much a copy needs a listed id, or None: 0 a copy of a listed event (it was host-only at home), 1 a phase copy
    (boss_events.move_phases) that tests HP and sends the AI something, 2 any other phase copy. Part templates, the
    events of a body's own moves and the helpers' events whose source is not listed are not phases: vanilla keeps none
    of them host-only, so they go without (parity)."""
    source, _, kind = events.sources[event]
    if source in HOST_ONLY:
        return 0
    if kind != "phase":
        return None
    body = events.instructions(event)
    if any((b, i) == HP_RATIO for b, i, _ in body) and any((b, i) in AI_SENDS for b, i, _ in body):
        return 1
    return 2


def _arena_key(body):
    arena = arenas.arena_of(body)
    return arena.key if arena is not None else None


def _top_slot(events, event):
    """The highest slot any run of the event takes (0: none above slot 0)."""
    return max([w[0] for e in events.event_ids() for b, d, w in events.instructions(e)
                if (b, d) == (2000, 0) and len(w) > 1 and w[1] == event] or [0])


def _swap_safe(events, a, b):
    """Events a and b may trade ids: neither one's runs in slots above 0 then own the flag (new id + slot,
    helpers.slot_flags) of an event of this file (found 8 Nov by tests/test_carried_bodies, seed 1: the emptied Shadows
    template 12704812, run in slots 0-2, took a copy's id 12704869, and its runs' flags 12704870/871 were the ids of a
    travelling One Reborn's copies)."""
    for event, new in ((a, b), (b, a)):
        top = _top_slot(events, event)
        if any(new + k in events.index and new + k not in (a, b) for k in range(1, top + 1)):
            return False
    return True


def cover(events):
    """Gives this map's copies that need it (rank) the listed ids of this map's emptied events: an arena's own first,
    then any left in the map, copies in rank order. Edits events in place. Returns {"covered": [[copy id before, listed
    id, source event, body, rank]], "uncovered": [[copy id, source event, body, rank]], "listed": n, "free": n}; empty
    ({}) when the map has neither a listed id nor a copy that needs one."""
    listed = [e for e in sorted(HOST_ONLY) if e in events.index]
    free = [e for e in listed if emptied(events, e)]
    needs = sorted((r, e) for e in events.sources if not emptied(events, e) for r in (rank(events, e),) if r is not None)
    if not needs and not listed:
        return {}
    taken, plan = set(), {}
    for own in (True, False):
        for r, copy in needs:
            if copy in plan:
                continue
            body = events.sources[copy][1]
            pick = [e for e in free if e not in taken and (not own or OWNER.get(e) == _arena_key(body))
                    and _swap_safe(events, copy, e)]
            if pick:
                plan[copy] = pick[0]
                taken.add(pick[0])
    report = {"listed": len(listed), "free": len(free), "covered": [], "uncovered": []}
    for r, copy in needs:
        source, body, _ = events.sources[copy]
        if copy in plan:
            events.swap_ids(copy, plan[copy])
            report["covered"].append([copy, plan[copy], source, body, r])
        else:
            report["uncovered"].append([copy, source, body, r])
    return report


def party_ids(reports):
    """The ids for PARTY_FILE (reports: {stem: cover's report}), sorted: every copy `cover` left uncovered that keeps
    vanilla's rule for its boss or for its arena. A copy of a listed event (rank 0) in any map: host-only at the boss's
    home, so host-only wherever it runs (the file, unlike a swap, needs no listed id of the map). Any other phase copy
    only in a map that lists ids; where the map lists none (m25, m26, m35) neither the boss's home nor the arena keeps
    it host-only in vanilla, so it goes without."""
    ids = sorted({u[0] for report in reports.values() if report
                  for u in report.get("uncovered", ()) if report.get("listed") or u[3] == 0})
    if len(ids) > PARTY_MAX:  # never near (about 15 a world): more would make the host drop the whole list
        raise RuntimeError("%d host-only copies, more than the party list holds (%d)" % (len(ids), PARTY_MAX))
    return ids


def party_text(ids):
    """PARTY_FILE's text for the ids (LF, ASCII), or None for no ids: then the world has no file."""
    if not ids:
        return None
    return "".join(["# Boss phase events this world copies under new ids: only the party host decides them\n"]
                   + ["%d\n" % i for i in ids])
