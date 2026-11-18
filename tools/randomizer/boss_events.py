"""Boss arena events that follow a swapped boss, body by body (arenas.py names the bodies of every fight).

A boss's phase change lives in its arena's map events: at an HP ratio the event sends the boss's AI a command, or forces
an animation. A swap keeps the arena's events, so the old boss's phase is sent to the new boss and the new boss's own
phase never fires. `move_phases` moves the events: the old ones go quiet, the new boss's are copied in under new ids,
with its entity and flags rewritten for this body.

Always, on a swapped arena: the boss's per-part events (templates run once per body part: part breaks, staggers) and the
events that drive its own moves follow it the same way; the arena opens as on a second attempt (fog wall up, the boss
waiting, no first-meeting scene or entry move made for the old boss); every health bar of a body shows its new boss's
name; and every scene in an event naming a body is inert, since a scene shows the old boss.

`arena_events` does all of it for one map, plus what each kind of fight needs (lead decisions 4 Nov, DESIGN_R2 D1-D5, D9):
- Gascoigne, Ludwig, the Orphan: one boss each, whose second body is the helpers module's.
- GROUP (Witch, Shadows): every body is named by its own boss; the Witch's think switch of body 1 becomes a label.
  normal (mix off) = the boss on body 1 alone: the others leave, dead at load, and the defeat waits on body 1 alone.
- Celestial Emissary: the big form 2420811 grows at the fight's start (started flag instead of the HP test, no AI
  message, no damage link); normal = the small ones, their generators and the tentacles leave; mix = the small ones
  stay, spawned once (generators off). Only 2420811 shows a bar.
- Living Failures: respawns stopped. normal = the boss on 3500851, immortal, its damage still linked to the pool
  3500850 (end and bar unchanged), the other three leave; mix = all four at the start, links off, the bar on 3500851
  and, once spawned, bars on 3500852/853 (slots 1, 2: vanilla shows three at most), the defeat event also fires when
  all four are dead, and a new event kills the pool then too.
- Co-op doping (lead decision 5 Nov): vanilla dopes no Emissary body and of the Failures only the pool; a body of these
  arenas that holds a boss takes the doping calls of its arena's `dope_as` (arenas.py), next to them; the Failures
  that only exist once spawned are doped by a copy of the pool's doping path at the end of the spawn event.
- One Reborn: the boss on 2800800 keeps its immortality and damage link to the pool 2800803; 2800801/802 leave.
- The first-meeting flag of every swapped arena is also set as the first instruction of the map's event 0.
A body that leaves the fight is disabled at load and every plain enable of it becomes a label."""
import struct

import arenas
import helpers
from emevd import LABEL_BANK

ARENA_FLAGS = {e: a.flags for a in arenas.ARENAS for e in a.bodies + a.extras + ((a.pool,) if a.pool else ())}
CALL = (2000, 0)
HP_RATIO = (4, 2)
AI_COMMAND = (2004, 17)
REPLAN = (2004, 20)
FORCED = (2003, 18)
SCENE_BANK = 2002
AI_SWITCH = (2004, 1)
SET_FLAG = (2003, 2)
FOG_BUTTON = (3, 24)  # the action prompt on a fog wall
GOTO_IF_FLAG = (1003, 101)
HEALTH_BAR = (2003, 11)  # [shown, entity, bar slot, name text id]
ENABLE = (2004, 5)       # [entity, 1 on / 0 off]
IMMORTAL = (2004, 12)    # [entity, 1]
KILL = (2004, 4)         # [entity, 0]
DAMAGE_LINK = (2004, 33)
GENERATOR = (2003, 3)    # [generator, 1 on / 0 off]
IF_DEAD = (4, 0)         # [condition group, entity, 1 dead]
GROUP_WAIT = (0, 0)      # [group | state << 8 | tested group << 16]
IF_FLAG = (3, 0)         # [group | state << 8, flag]; group 0 waits in place
END_IF_FLAG = (1003, 2)  # [256, flag]: the event ends when the flag is on
WAIT_SECONDS = (1001, 0)
# the event a GROUP fight runs once per body with the other bodies as parameters (DATA: m22 event 0 lines 46-47,
# m27 event 0 lines 131-133)
GROUP_TEMPLATE = {"witch": 12204812, "shadows": 12704812}
BAR_SLOTS = 3           # boss bar slots vanilla uses (0-2, DATA: every 2003[11] of the game's event files)
# instructions whose words name event flags ranges or bit fields: every flag in them is taken
FLAG_FIELDS = frozenset(((3, 12), (3, 20), (2003, 31), (2003, 32), (2003, 41), (2003, 42)))
# The Hunter's Dream fights start as story steps (Gehrman on a talk, the Moon Presence after Gehrman and the cords), not
# as a walk to a boss, so they keep their first start (arenas.py story_start).
STORY_STARTS = frozenset(e for a in arenas.ARENAS if a.story_start for e in a.bodies)
# Events of a fight's own machinery, never phases of one body (lead decision 4 Nov, D1/D2/D3): the handovers,
# the Emissary's growth, the Witch's and the Shadows' group events.
NOT_PHASES = frozenset((12414807, 13404824, 13404825, 12424790, 12204839, 12704806, 13604820))
# Events that drive one body's own moves on its AI's messages (DATA m34: Ludwig's teleport on message 100, body 2's
# replan on message 300). They follow the body like its part events.
# DATA 8 Nov (survey of plain events on a pool boss's own AI messages): the Moon Presence's Doom Gaze keeps the player
# alive (m21 12104870: immortal on her message 10 until SpEffect 5570, then 5572 cleared; without it the gaze kills);
# Gehrman and Maria clear their own SpEffect 5526 (+20 % attack, -20 % defence) on message 20 (12104808, 13504822);
# Laurence turns his hit mask bit 10 on at message 400 (13404875). Arena-side ones stay home (music, lock camera,
# the Witch's Mad Ones, the Shadows' snake balls).
BODY_EVENTS = {3400800: (13404840,), 3400801: (13404841,), 2100810: (12104870,), 2100800: (12104808,),
               3500800: (13504822,), 3400850: (13404875,)}
# An event part (2004[22] [boss, part id | group << 16, part HP, part damage correction, body damage correction, ...])
# scales the damage its body group takes. DATA 8 Nov (survey of every pool boss's part templates): only Amygdala's run
# arguments go below 1.0: m33 13304830 runs 3308 (tail, group 3), 3309/3310 (legs, groups 11/12) at 0.2 (0.3 once
# broken). LIVE 8 Nov (headless, Saw Cleaver R1s standing 1.4-2 m in front of her in the Cleric Beast's arena): with them
# most hits took 1 HP (100 HP from 12 swings), without them every hit took 30-36 (334 from 10): the player report "took
# no damage". A moved boss's carried part runs take no correction under 1.0 (user, 8 Nov: "so you can damage her
# again"); the arm parts, their breaks and the head part stay as at home.
PART_CREATE = (2004, 22)
PART_CORRECTIONS = (12, 16)  # byte offsets of the part and body damage corrections in 2004[22]
ONE = struct.unpack("<i", struct.pack("<f", 1.0))[0]
# Phase events with no HP test, which phase_events cannot see (DATA m24_01: beast Gascoigne's music box).
BODY_PHASES = {2410811: (12414809,)}
# A second-attempt start that keeps these lines of the first-meeting event (DATA m27: the three Shadows warp to their
# places, the intro SFX goes).
RETRY_KEEP = {12701802: (2, 3, 4, 5)}


def _excluded(entity):
    return NOT_PHASES | helpers.OWNED.get(entity, frozenset())


def _old_map_id(word, origin):
    """A seven-digit id of the boss's old map other than the boss (a region, an object, a part there)."""
    return 1000000 <= word < 10000000 and word // 10000 == origin // 10000 and word != origin


def _callers(events):
    out = {}
    for event in events.event_ids():
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) == CALL and len(words) >= 2:
                out.setdefault(words[1], []).append((event, i, words))
    return out


def phase_events(events, entity):
    """Ids of the plain phase events of one boss body: called once, no parameters, an HP ratio and something sent to the
    body. A fight's own machinery (NOT_PHASES) and the helpers module's events (helpers.OWNED) are never phases."""
    callers = _callers(events)
    skip = _excluded(entity)
    out = []
    for event in events.event_ids():
        if event in skip or len(callers.get(event, ())) != 1 or events.params(event):
            continue
        hp, acts = False, False
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == HP_RATIO and len(words) > 1 and words[1] == entity:
                hp = True
            if (bank, iid) in (AI_COMMAND, FORCED) and words and words[0] == entity:
                acts = True
        if hp and acts:
            out.append(event)
    return out


def bodies(events, entity):
    """[(event id, restart kind, instructions)] of a boss's phase events, read from its own map."""
    return [(e, events.restart(e), events.instructions(e)) for e in phase_events(events, entity)]


def _renames(origin, arena):
    """The boss and its fight flags, old body to new (a global flag the old fight's events end on: the new defeat)."""
    old, new = arenas.arena_of(origin), arenas.arena_of(arena)
    out = {origin: arena, old.flags[0]: new.flags[0], old.flags[1]: new.flags[1]}
    if old.ends_on is not None:  # lead decision 4 Nov (D1): Ludwig's copies end on this arena's defeat, not on 9471
        out[old.ends_on] = new.flags[0]
    return out


def _old_map_flag(word, origin):
    """An event flag of the boss's old map (its arena's flag block, like 1230xxxx for m23_00)."""
    return 10000000 <= word < 100000000 and word // 10000 == arenas.arena_of(origin).flags[0] // 10000


def _rewrite(body, origin, arena, moved=None):
    """The instructions for the new body. moved: {old event id: its copy's id} of the events that came along, so a
    copy waiting on a sibling waits on the sibling's copy. The boss and its fight flags are renamed; what only made sense
    in the old map (its regions, its other bodies, its other flags, such as the flags with which Rom calls her spiders)
    becomes a label."""
    new = {**_renames(origin, arena), **(moved or {})}
    used = {iid for bank, iid, _ in body if bank == LABEL_BANK}
    free = [i for i in reversed(range(10)) if i not in used]
    out = []
    for bank, iid, words in body:
        if any(_old_map_id(w, origin) or (_old_map_flag(w, origin) and w not in new) for w in words):
            if not free:
                raise ValueError("no free label in a moved event")
            out.append((LABEL_BANK, free.pop(0), []))
        else:
            out.append((bank, iid, [new.get(w, w) for w in words]))
    return out


def _taken(events):
    """Every id this file uses or names: events, words, and the flags of flag ranges and bit fields."""
    taken = set(events.event_ids()) | helpers.slot_flags(events)  # a run's own flag in slot n is its event id + n
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            taken.update(words)
            flags = [w for w in words if 10000000 <= w < 100000000]
            if (bank, iid) in FLAG_FIELDS and flags:
                taken.update(range(flags[0], flags[0] + 32))
            for a, b in zip(flags, flags[1:]):
                if a < b <= a + 1000:
                    taken.update(range(a, b + 1))
    return taken


def _free_ids(events, arena, count, slots=None):
    """New event ids in the thousand block of the arena's fight-started flag (X4890-X4999 first, then X4800-X4889, then
    the rest of the block) that no event here uses, no word here names and no flag range here covers. slots: per id, the
    highest slot its runs will take (a template copied with its runs): ids X+1..X+slot, those runs' own flags
    (helpers.slot_flags), must be free too and are given to nothing else."""
    block = arenas.arena_of(arena).flags[1] // 1000 * 1000
    taken = _taken(events)
    order = list(range(block + 890, block + 1000)) + list(range(block + 800, block + 890)) + list(range(block + 1, block + 800))
    slots = list(slots) if slots is not None else [0] * count
    out = []
    for top in slots:
        pick = next((c for c in order if all(c + k not in taken for k in range(top + 1))), None)
        if pick is None:
            raise ValueError("no %d free event ids near %d" % (count, block))
        out.append(pick)
        taken.update(range(pick, pick + top + 1))
    return out


def _new_events(events, arena, bodies_):
    """Adds each instruction list as a new event (run once from event 0). Returns their ids."""
    ids = _free_ids(events, arena, len(bodies_))
    for event, body in zip(ids, bodies_):
        events.add_event(event, 0, body)
        events.append_instruction(0, CALL[0], CALL[1], [0, event, 0])
    return ids


def _copy_plain(events, arena, origin, source, plain, kind):
    """Copies plain events (no parameters, run once) of the origin body for the arena body; started from event 0. kind:
    "phase" or "body", recorded with each copy (Emevd.sources, read by hostonly.cover)."""
    if not plain:
        return []
    moved = dict(zip(plain, _free_ids(events, arena, len(plain))))
    for event in plain:
        events.add_event(moved[event], source.restart(event), _rewrite(source.instructions(event), origin, arena, moved),
                         source=(event, arena, kind))
        events.append_instruction(0, CALL[0], CALL[1], [0, moved[event], 0])
    return [moved[e] for e in plain]


def _source(origin_events, origin):
    return origin_events(origin) if callable(origin_events) else origin_events


def move_phases(events, swaps, origin_events):
    """swaps: {body: entity the new boss came from, None for a boss with no home events (chalice)}.
    origin_events(entity) -> the Emevd of its own map. Returns [(body, old events silenced, new
    event ids)]."""
    report = []
    for arena in sorted(swaps):
        origin = swaps[arena]
        # the helpers' copies (helpers.travel ran first: a travelling Orphan's handover, the One Reborn's AI phases)
        # test this body's HP too, but they are the new boss's own and stay
        old = [e for e in phase_events(events, arena) if events.sources.get(e, (0, 0, ""))[2] != "helper"]
        old += list(BODY_PHASES.get(arena, ()))
        for event in old:
            events.silence(event)
        added = []
        if origin is not None:
            source = _source(origin_events, origin)
            added = _copy_plain(events, arena, origin, source,
                                phase_events(source, origin) + list(BODY_PHASES.get(origin, ())), "phase")
        report.append((arena, old, added))
    return report


def _names_something(word):
    """A seven-digit entity, region or object, or an eight-digit event flag."""
    return 1000000 <= word < 100000000


def part_events(events, entity):
    """Ids of a boss's per-part events: templates (they take parameters) whose body names the boss and whose runs pass
    only part, effect and animation numbers. A template run with other entities or flags (Rom's 13204050 is run
    for her 30 spiders) belongs to the arena and stays."""
    callers = _callers(events)
    skip = _excluded(entity)
    return [e for e in events.event_ids() if e not in skip and events.params(e) and callers.get(e)
            and any(entity in words for _, _, words in events.instructions(e))
            and not any(_names_something(w) for _, _, run in callers[e] for w in run[2:])]


def _float(word):
    return struct.unpack("<f", struct.pack("<i", word))[0]


def _correction_args(body, params):
    """Indexes into a template run's arguments (the words after the event id) that feed an event part's damage
    corrections (PART_CORRECTIONS of a 2004[22] line)."""
    return sorted({src // 4 for i, dst, src, _ in params
                   if i < len(body) and tuple(body[i][:2]) == PART_CREATE and dst in PART_CORRECTIONS})


def move_parts(events, swaps, origin_events):
    """The body's per-part events and the events of its own moves go quiet; the new boss's are copied in (body,
    parameters, every run of them with its arguments), with the boss and its fight flags renamed for this body.
    swaps: as move_phases. Returns [(body, old, new event ids)]."""
    report = []
    for arena in sorted(swaps):
        origin = swaps[arena]
        old = part_events(events, arena) + list(BODY_EVENTS.get(arena, ()))
        for event in old:
            events.silence(event)
        added = []
        if origin is not None:
            source = _source(origin_events, origin)
            runs = _callers(source)
            renames = _renames(origin, arena)
            templates = part_events(source, origin)
            moved = dict(zip(templates, _free_ids(events, arena, len(templates),
                                                  [max([w[0] for _, _, w in runs.get(t, ())] or [0]) for t in templates])))
            for template in templates:
                body = source.instructions(template)
                new_body = _rewrite(body, origin, arena, moved)
                dropped = {i for i, (a, b) in enumerate(zip(body, new_body)) if a[:2] != b[:2]}
                params = [p for p in source.param_entries(template) if p[0] not in dropped]
                events.add_event(moved[template], source.restart(template), new_body, params,
                                 source=(template, arena, "part"))
                floors = _correction_args(body, params)
                for _, _, words in runs[template]:
                    args = [renames.get(w, w) for w in words[2:]]
                    for k in floors:
                        if k < len(args) and _float(args[k]) < 1.0:
                            args[k] = ONE
                    events.append_instruction(0, CALL[0], CALL[1], [words[0], moved[template]] + args)
                added.append(moved[template])
            added += _copy_plain(events, arena, origin, source, list(BODY_EVENTS.get(origin, ())), "body")
        report.append((arena, old, added))
    return report


def bar_name(events, entity):
    """The name text id the boss's own map gives its health bar, or None."""
    for event in events.event_ids():
        for bank, iid, words in events.instructions(event):
            if (bank, iid) == HEALTH_BAR and len(words) > 3 and words[1] == entity:
                return words[3]
    return None


def name_bars(events, names):
    """names: {entity: name text id}. Every health bar on one of these entities shows that name; a bar whose name
    comes from an event parameter stays. Returns [(entity, event, old name, new name)]."""
    done = []
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) != HEALTH_BAR or len(words) < 4 or names.get(words[1]) is None or 12 in params.get(i, {}):
                continue
            if names[words[1]] != words[3]:
                events.set_word(event, i, 3, names[words[1]])
                done.append((words[1], event, words[3], names[words[1]]))
    return done


def origin_name(origin_events, origin):
    """The bar name a boss brings from its own map: its bar, else the bar of the body that carries it at home (the
    Orphan's phase two 3600801, helpers.FIGHTS bar), else the bar of its fight's HP pool."""
    source = _source(origin_events, origin)
    for entity in (origin, getattr(helpers.FIGHTS.get(origin), "bar", None), arenas.arena_of(origin).pool):
        name = bar_name(source, entity) if entity is not None else None
        if name is not None:
            return name
    return None


def move_names(events, swaps, origin_events):
    """Every health bar of a swapped body shows the new boss's name, read from the bar in its own map. A bar whose name
    comes from an event parameter stays. swaps: {body: origin}. Returns [(body, event, old name, new name)]."""
    return name_bars(events, {e: origin_name(origin_events, o) for e, o in swaps.items() if o is not None})


def intro_event(events, arena):
    """The arena's first-meeting event as the fog wall finds it (the fog event jumps to the fog when that event's flag
    is on, which is what a second attempt finds). None when no fog wall event names it. arenas.py holds the checked ids
    (this detection returns the defeat event for the Shadows)."""
    started = ARENA_FLAGS[arena][1]
    ids = set(events.event_ids())
    for event in events.event_ids():
        body = events.instructions(event)
        if not any((b, i) == FOG_BUTTON for b, i, _ in body):
            continue
        if not any((b, i) == SET_FLAG and w[:1] == (started,) for b, i, w in body):
            continue
        for b, i, w in body:
            if (b, i) == GOTO_IF_FLAG and len(w) > 1 and w[1] in ids:
                return w[1]
    return None


def _swapped_arenas(entities):
    """{arena: first body of it among entities} of the arenas that open as on a second attempt."""
    out = {}
    for entity in sorted(entities):
        arena = arenas.arena_of(entity)
        if arena is not None and not arena.story_start and arena.intro is not None:
            out.setdefault(arena, entity)
    return out


def retry_starts(events, entities):
    """Each swapped arena opens as on a second attempt: its first-meeting event (arenas.py) only marks itself done, so
    the fog wall stands, the new boss waits at its place in the map, and passing the fog starts the fight. Returns
    [(body, event)]."""
    done = []
    for arena, entity in sorted(_swapped_arenas(entities).items(), key=lambda kv: kv[1]):
        if arena.intro not in events.index:
            continue
        body = events.instructions(arena.intro)
        kept = [(b, i, list(w)) for k, (b, i, w) in enumerate(body) if k in RETRY_KEEP.get(arena.intro, ())]
        events.set_body(arena.intro, kept + [(SET_FLAG[0], SET_FLAG[1], [arena.intro, 1])])
        done.append((entity, arena.intro))
    return done


def first_attempt_flags(events, entities):
    """The first-meeting flag of each swapped arena is set as the first instruction of event 0, before any event of
    the map reads it (lead decision 4 Nov, D9: Laurence's 13404861 and Ludwig's 13404811 hold their boss off until it is
    on). Returns [(arena key, flag)]."""
    done = []
    for arena in sorted(_swapped_arenas(entities), key=lambda a: a.intro):
        if arena.intro not in events.index:
            continue
        events.splice(0, 0, [(SET_FLAG[0], SET_FLAG[1], [arena.intro, 1]), events.instructions(0)[0]])
        done.append((arena.key, arena.intro))
    return done


def silence_scenes(events, entities):
    """Every scene in an event that names a swapped boss becomes a label. Returns [(event, instruction, scene)]."""
    done = []
    for event in events.event_ids():
        instructions = events.instructions(event)
        if not any(e in w for _, _, w in instructions for e in entities):
            continue
        for i, (bank, iid, words) in enumerate(instructions):
            if bank == SCENE_BANK:
                events.neutralize(event, i)
                done.append((event, i, words[0] if words else -1))
    return done


# ---------------------------------------------------------------- what each kind of fight needs

def _at(events, event, bank, iid, words):
    """Index of the instruction (bank, iid, words) in event; ValueError when the map's events differ from vanilla's."""
    for i, (b, d, w) in enumerate(events.instructions(event)):
        if (b, d) == (bank, iid) and tuple(w) == tuple(words):
            return i
    raise ValueError("event %d has no %d[%02d] %r" % (event, bank, iid, tuple(words)))


def _label(events, done, event, bank, iid, words):
    i = _at(events, event, bank, iid, words)
    events.neutralize(event, i)
    done.append(("label", event, i))


def _silence(events, done, *ids):
    for event in ids:
        events.silence(event)
        done.append(("silence", event, None))


def _keep_out(events, done, entities):
    """The entities leave the fight: every plain enable of them becomes a label. Returns their disables at load."""
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) == ENABLE and tuple(words) in {(e, 1) for e in entities} and not params.get(i):
                events.neutralize(event, i)
                done.append(("label", event, i))
    return [(ENABLE[0], ENABLE[1], [e, 0]) for e in entities]


def _generators(arena, entities, state):
    return [(GENERATOR[0], GENERATOR[1], [arena.generators[e], state]) for e in entities if e in arena.generators]


def _seconds(value):
    return struct.unpack("<i", struct.pack("<f", value))[0]


def _group(events, arena, placed, mode, done):
    if arena.key == "witch" and 2200800 in placed:  # lead decision 4 Nov (D2): body 1's think switch to the Witch's
        _label(events, done, 12204811, 2004, 19, (2200800, 210021))
        _label(events, done, 12204811, 2004, 19, (2200800, 210020))
    if mode != "normal":
        return []
    # mix off (user, 8 Nov: "off is just one boss", the easy form): the boss on body 1 alone. The other bodies leave as
    # vanilla's beaten fight has them (DATA: 12201800 / 12701800 disable and kill every body when the defeat flag is on):
    # disabled and dead at load, their bars gone, and the defeat waits on body 1's death alone. The bodies' shared
    # template goes: the Witches' immortality until both are at HP 1 (else the fallen one revives after 45 s), the
    # Shadows' phases that test all three bodies' HP (a dead body's 0 fires them at once).
    leaving = [e for e in arena.bodies if e != arena.main]
    for body in leaving:
        _label(events, done, arena.flags[0], *IF_DEAD, (1, body, 1))
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) == HEALTH_BAR and len(words) > 1 and words[1] in leaving and not params.get(i):
                events.neutralize(event, i)
                done.append(("label", event, i))
    _silence(events, done, GROUP_TEMPLATE[arena.key])
    return _keep_out(events, done, leaving) + [(KILL[0], KILL[1], [body, 0]) for body in leaving]


def _emissary(events, arena, placed, mode, done):
    """Lead decision 4 Nov (D3). DATA m24_02: 12424790 grows 2420810 into 2420811 at 2420810's HP 60 % and its AI message
    30; 12424791/792 bring the tentacles; 12424785/787 send AI commands 10/20 to the bodies near the player."""
    big, small = arena.main, 2420810
    _label(events, done, 12424790, *DAMAGE_LINK, (small, big))
    _label(events, done, 12424790, *AI_COMMAND, (small, 40, 1))
    _label(events, done, 12424790, *REPLAN, (small,))
    _label(events, done, 12424790, 4, 8, (0, small, 30, 1))
    i = _at(events, 12424790, *HP_RATIO, (1, small, 3, 1058642330))
    events.splice(12424790, i, [(IF_FLAG[0], IF_FLAG[1], [1 | 1 << 8, arena.flags[1]])])  # grows when the fight starts
    done.append(("replace", 12424790, i))
    _label(events, done, 12424702, *HEALTH_BAR, (1, small, 0, 257000))  # only 2420811 shows a bar
    _silence(events, done, 12424791, 12424792)
    for i, (bank, iid, words) in enumerate(events.instructions(0)):
        if (bank, iid) == CALL and len(words) > 2 and (words[1] == 12424787 or words[1:3] == (12424785, big)):
            events.neutralize(0, i)
            done.append(("label", 0, i))
    smalls = [e for e in arena.bodies if e != big]
    leaving = [2420750, 2420751] + (smalls if mode == "normal" else [])
    _dope(events, done, [e for e in arena.bodies if e in placed])
    return _keep_out(events, done, leaving) + _generators(arena, smalls, 0)


def _failures(events, arena, placed, mode, done):
    """Lead decision 4 Nov (D4). DATA m35: 13505662 respawns a dead Failure from its generator; 13504852 links each
    Failure's damage to the pool 3500850 and puts bar 403000 on it; 13504865 rains meteors on the Failures' AI messages
    50/30; 13504890 sends them AI command 120 on the player's effects."""
    for body, generator in sorted(arena.generators.items()):
        _label(events, done, 13505662, *GENERATOR, (generator, 1))
    _silence(events, done, 13504865, 13504890)
    others = [e for e in arena.bodies if e != arena.main]
    _dope(events, done, [arena.main] if arena.main in placed else [])  # it exists at load: doped with the pool
    if mode != "mix":
        return _keep_out(events, done, others) + _generators(arena, others, 0) + [
            (IMMORTAL[0], IMMORTAL[1], [arena.main, 1])]
    for body in arena.bodies:
        _label(events, done, 13504852, *DAMAGE_LINK, (body, arena.pool))
    i = _at(events, 13504852, *HEALTH_BAR, (1, arena.pool, 0, 403000))
    events.set_word(13504852, i, 1, arena.main)
    done.append(("bar", 13504852, i))
    defeat, started = arena.flags
    spawn = [(END_IF_FLAG[0], END_IF_FLAG[1], [256, defeat]), (IF_FLAG[0], IF_FLAG[1], [256, started])]
    spawn += _generators(arena, others, 1) + [(WAIT_SECONDS[0], WAIT_SECONDS[1], [_seconds(3.0)])]
    spawn += _generators(arena, arena.bodies, 0)
    # once spawned, two more bars: vanilla shows up to three (slots 0-2, DATA: the Shadows' 12704802); name_bars names them
    spawn += [(HEALTH_BAR[0], HEALTH_BAR[1], [1, body, slot, 403000]) for slot, body in enumerate(others[:BAR_SLOTS - 1], 1)]
    # and doped like the pool, now that they exist (the set-up event's doping ran when they did not yet)
    spawned = [e for e in others if e in placed]
    if spawned:
        path = helpers.dope_path(events, arena.dope_as, spawned, started)
        if not path:
            raise ValueError("no doping path for %r" % spawned)
        spawn += path
        done.append(("dope", None, tuple(spawned)))
    # the defeat also fires when all four are dead, not only on the pool's HP (the kill below makes it so as well)
    i = _at(events, defeat, *HP_RATIO, (0, arena.pool, 0, 0))
    events.splice(defeat, i, [(HP_RATIO[0], HP_RATIO[1], [0xff, arena.pool, 0, 0])]
                  + [(IF_DEAD[0], IF_DEAD[1], [1, body, 1]) for body in arena.bodies]
                  + [(GROUP_WAIT[0], GROUP_WAIT[1], [0xff | 1 << 8 | 1 << 16]),
                     (GROUP_WAIT[0], GROUP_WAIT[1], [0 | 1 << 8 | 0xff << 16])])
    done.append(("replace", defeat, i))
    end = [(END_IF_FLAG[0], END_IF_FLAG[1], [256, defeat])]
    end += [(IF_DEAD[0], IF_DEAD[1], [1, body, 1]) for body in arena.bodies]
    end += [(GROUP_WAIT[0], GROUP_WAIT[1], [0 | 1 << 8 | 1 << 16]), (KILL[0], KILL[1], [arena.pool, 0])]
    for event in _new_events(events, arena.main, [spawn, end]):
        done.append(("event", event, None))
    return []


def _one_reborn(events, arena, placed, mode, done):
    """Lead decision 4 Nov (D5). DATA m28: 12804806 warps the upper body 2800801 onto 2800800; 12804840 (run for two
    bell maidens) makes the upper body scream at a maiden's death; 12804870/871 send the bodies and the maidens AI
    commands at the pool's HP: the lines for the bodies go (the bodies' own phases follow them)."""
    _silence(events, done, 12804806, 12804840)
    for event in (12804870, 12804871):
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) in (AI_COMMAND, REPLAN, FORCED) and words and words[0] in (arena.main, 2800801):
                events.neutralize(event, i)
                done.append(("label", event, i))
    return _keep_out(events, done, [e for e in arena.extras])


def _dope(events, done, bodies):
    """Lead decision 5 Nov (co-op audit risk 3): an arena body vanilla never dopes (the Emissary's, the Failures') is
    doped like a boss when it holds one: the calls of its arena's `dope_as` entity, next to them (helpers.dope_like)."""
    by_anchor = {}
    for body in bodies:
        anchor = helpers.dope_anchor(events, body)
        if anchor is None:
            raise ValueError("body %d: no doping calls to copy" % body)
        by_anchor.setdefault(anchor, []).append(body)
    for anchor, group in sorted(by_anchor.items()):
        for event, entity in helpers.dope_like(events, anchor, group):
            done.append(("dope", event, entity))


AUTHORITY = (2004, 28)  # [entity, 4095]: network update authority (co-op), set by a fight's set-up event


def _phase_body(events, arena, entity, done):
    """A boss in the arena's phase body (option coiled_host; Nocturne sets up its Coiled Host with neither) is set up as
    vanilla sets up a parked second body: DATA m24_01 12414802, m34 13404802, m36 13604802 give 2410811 / 3400801 /
    3600801 the network authority and the co-op doping of body 1, in the same set-up event at the fight's start, though
    they only fight later. So every authority and doping call on the arena's main body gets the same call for it."""
    for event in events.event_ids():
        if events.params(event):
            continue
        body = events.instructions(event)
        if any((b, i) == AUTHORITY and w[:1] == (entity,) for b, i, w in body):
            continue
        for i in reversed(range(len(body))):
            bank, iid, words = body[i]
            if (bank, iid) == AUTHORITY and words[:1] == (arena.main,):
                events.splice(event, i, [(bank, iid, list(words)), (bank, iid, [entity] + list(words[1:]))])
                done.append(("authority", event, i + 1))
    for event, e in helpers.dope_like(events, arena.main, [entity]):
        done.append(("dope", event, e))


KIND_EDITS = {"witch": _group, "shadows": _group, "emissary": _emissary, "failures": _failures, "one_reborn": _one_reborn}


def mode_of(arena, placed, modes):
    """The arena's mode: modes[key] when given, else "mix" for an ARENA_ONLY fight with bosses beyond its main body,
    "normal" for one with only that, "swap" for the rest."""
    if arena.key in modes:
        return modes[arena.key]
    if arena.kind == arenas.ARENA_ONLY:
        return "mix" if any(e in placed for e in arena.bodies if e != arena.main) else "normal"
    return "swap"


def phase_coop(events, stem):
    """Every phase body of this map's fights (arenas.PHASES; only a base mod adds one) gets the co-op set-up of its
    arena's main body, whoever fights in it, option coiled_host on or off (user, 8 Nov: Nocturne v2.0.0's 12704802
    gives its Coiled Host neither 2004[28] nor doping, a co-op gap of that mod). Same edit as a boss drawn into his
    place (_phase_body), and as harmless when that already ran. Returns {arena key: [(what, event, instruction)]}."""
    report = {}
    for entity, key in sorted(arenas.PHASES.items()):
        arena = arenas.BY_KEY[key]
        if arena.stem == stem:
            done = []
            _phase_body(events, arena, entity, done)
            if done:
                report[key] = done
    return report


def fight_edits(events, placed, modes):
    """What each kind of fight needs (KIND_EDITS) for every arena with a body in placed, plus one event at load that
    disables the bodies that leave and the generators that would bring them back. Returns {arena key: [(what, event,
    instruction)]}."""
    report = {}
    for arena in sorted({arenas.arena_of(e) for e in placed}, key=lambda a: a.key):
        edit = KIND_EDITS.get(arena.key)
        if edit is None:
            continue
        done = []
        at_load = edit(events, arena, placed, mode_of(arena, placed, modes), done)
        if at_load:
            event, = _new_events(events, arena.main, [at_load])
            done.append(("event", event, None))
        report[arena.key] = done
    for entity in sorted(e for e in placed if e in arenas.PHASES):
        arena = arenas.BY_KEY[arenas.PHASES[entity]]
        _phase_body(events, arena, entity, report.setdefault(arena.key, []))
    return report


def arena_events(events, placed, fighters, modes, origin_events):
    """Every arena edit of one map. placed: {body: fighter key} of this map's bodies with a boss; fighters: {key: fighter
    dict} (arenas.py contract; a chalice fighter has origin None and may carry "bar_name"); modes: {arena key: mode};
    origin_events(entity) -> the Emevd of that entity's home map. The helpers' dismissals and travels
    (helpers.apply_stem, which also adds their MSB parts) run before this, on the same events: they move the bars that
    name_bars reads. Returns a report."""
    fighter = {e: fighters[k] for e, k in placed.items()}
    origins = {e: f.get("origin") for e, f in fighter.items()}
    report = {"fights": fight_edits(events, placed, modes)}
    report["starts"] = retry_starts(events, set(placed))
    report["scenes"] = silence_scenes(events, set(placed))
    report["parts"] = move_parts(events, origins, origin_events)
    names = {e: origin_name(origin_events, o) if o is not None else fighter[e].get("bar_name")
             for e, o in origins.items()}
    for arena in {arenas.arena_of(e) for e in placed}:
        if arena.pool is not None and names.get(arena.main) is not None:
            names.setdefault(arena.pool, names[arena.main])  # a pool's bar is the boss's bar
    report["names"] = name_bars(events, names)
    report["phases"] = move_phases(events, origins, origin_events)
    report["first"] = first_attempt_flags(events, set(placed))
    return report
