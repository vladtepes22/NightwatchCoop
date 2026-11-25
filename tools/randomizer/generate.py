"""Turns a plan into a mod folder: maps, AI archives and NpcParam clones, with a spoiler and a manifest."""
import hashlib
import json
import os
import re
import shutil
import struct
import zlib

from ailua import Archive, script_name
from containers import Layout, Param, bnd4_find, bnd4_read, bnd4_write, dcx_read, dcx_write, fmg_read, fmg_write
from arenas import ARENA_ONLY, BY_KEY, ESCORTS
import arenas
from emevd import Emevd
from gamedata import (BOSS_ENTITIES, CANCEL_ANIMATION, Census, ESCORT_ARENA, FORCED_ANIM, GameFiles, STEMS,
                      SUMMON_MESSAGE, SUMMONS, map_variants)
from msb import Msb
from neighbours import group_limits, load_groups
from planner import away_slot, check, escort_slot, plan, posed_slot, replacement_pool
from seedcode import GENERATOR_MAJOR, decode, encode, normalize
import drops as drop_shuffle
import shops as shop_shuffle
import boss_events
import hostonly
import buildid
import chalice
import chalice_enemies
import difficult
import effects as effect_copies
import gems as gem_roulette
import helpers
import readable
import starters as starter_weapons
import treasure as treasure_shuffle

CLONE_BASE = 950000000
AI_ID = (2004, 19)  # Set Character AI ID: entity, NpcThinkParam id
SLOT_STRENGTH = ("spEffectID6", "GameClearSpEffectID")
SLOT_REWARDS = ("getSoul", "itemLotId_1", "itemLotId_2", "itemLotId_3", "itemLotId_4", "itemLotId_5",
                "itemLotId_6", "humanityLotId")


# lead decision 4 Nov (D3, D10): the arena whose boss row gives this arena's main body its HP under slot scaling in normal
# mode (in mix every body keeps its own HP). The Emissary's big form has 400 HP, a weak boss; Ebrietas' arena row is on the
# same map with the same area scaling.
HP_FROM_ARENA = {"emissary": "ebrietas"}


def boss_hp_rows(census, result):
    """{(stem, part): (NpcParam row whose HP the new boss takes, whether it does so whatever the scaling option)} of every
    boss body (D10): the slot's own row under slot scaling; HP_FROM_ARENA's row for those mains in normal mode (D3); every
    body of an ARENA_ONLY mix, its main too, its own HP always (user, 4 Nov: 'each keeps that emissary's own HP', 'that
    Failure's own HP')."""
    index = {s["entity"]: s for stem in census.slots for s in census.slots[stem]}
    out = {}
    for entity, (stem, part) in list(result["bosses"].items()) + list(result.get("hard_bodies", {}).items()):
        slot = index[entity]
        arena = BY_KEY.get(slot["arena"])
        row, always = slot["npc"], False
        mix = arena is not None and arena.kind == ARENA_ONLY and result["arena_modes"].get(arena.key) == "mix"
        if mix:
            always = True
        elif arena is not None and entity == arena.main and arena.key in HP_FROM_ARENA:
            source = BY_KEY[HP_FROM_ARENA[arena.key]].main
            if source in index:
                row = index[source]["npc"]
        out[(stem, part)] = (row, always)
    return out


def _no_summons(events):
    """Nothing is ever summoned (option shadows_summons off; user, 8 Nov: "no one wants a shark every 20 seconds"): the
    summon event's three waits for a Shadow body's AI message 20 wait for a message no script sends. The event still
    disables its summon at load (its lines 2-3), and 12704830 still ends it and kills the unseen summon at the fight's end.
    The summon event is the one the map runs for the summons (DATA: vanilla 12704807; Nocturne v2.0.0 runs its copy
    12704813 instead, which also waits for the Coiled Host's message 20), every wait of it."""
    runs = {w[1] for b, i, w in events.instructions(0) if (b, i) == (2000, 0) and len(w) > 2 and set(w[2:]) & set(SUMMONS)}
    found = 0
    for event in sorted(runs & set(events.event_ids())):
        waits = [i for i, (bank, iid, words) in enumerate(events.instructions(event))
                 if (bank, iid) == (4, 8) and len(words) == 4 and words[2] == SUMMON_MESSAGE]
        for i in waits:
            events.set_word(event, i, 2, NEVER_SENT)
        found += len(waits)
    if found < 3:
        raise RuntimeError("summon events %r: %d message waits, not 3 or more" % (sorted(runs), found))


NEVER_SENT = 0x7fffffff  # an AI message id no battle script sends


def _mark(layout, row, effect):
    """The row also carries `effect`, in its first free SpEffect slot (slot 6 is the area scaling, SLOT_STRENGTH). A row
    with none free (DATA 8 Nov: 1 of 356 pool rows, c2730 273400) stays as it is: its summon despawns after 5 s, as
    vanilla's does once it stops."""
    effects = [layout.get(row, "spEffectID%d" % k) for k in range(8)]
    free = next((k for k in range(8) if k != 6 and effects[k] <= 0), None)
    if effect not in effects and free is not None:
        layout.set(row, "spEffectID%d" % free, effect)


def clone_rows(census, changes, options, bosses=None, enemy_hp=None):
    """D5: each changed slot gets a row made of the replacement's combat data plus the slot's strength and
    rewards. Equal rows are shared. bosses: boss_hp_rows(), the HP each boss body takes. enemy_hp: {(stem, part): base
    HP} of the bosses standing as enemies (boss_enemy_hp); those slots also keep their own rewards whatever option
    rewards says (DATA 8 Nov: every boss row's getSoul is 0 and its item lots -1 but Logarius', since a boss's rewards
    come from its arena). A chalice enemy (a model census.add_candidates joined, option chalice_enemies) keeps the slot's
    rewards likewise (user 8 Nov, PLAN 14 fix 5): never its dungeon rows' loot; with gem_roulette the slot's lots hold the
    roulette gems. Returns ({(stem, part): row id}, [(id, bytes)]), ids above every input id."""
    bosses = bosses or {}
    enemy_hp = enemy_hp or {}
    layout = census.npc_layout
    start = max(CLONE_BASE, max(r[0] for r in census.npc.rows) + 1)
    by_bytes, rows, slot_row = {}, [], {}
    for stem in sorted(changes):
        originals = {s["part"]: s for s in census.slots[stem]}
        for part in sorted(changes[stem]):
            model, _, npc = changes[stem][part]
            row = bytearray(census.npc_rows[npc])
            source = census.npc_rows[originals[part]["npc"]]
            if options["scaling"] == 0:
                for name in SLOT_STRENGTH:
                    layout.set(row, name, layout.get(source, name))
            elif options["scaling"] == 2:
                layout.set(row, "spEffectID6", options["fixed_effect"])
            hp = bosses.get((stem, part))
            if hp is not None and (options["scaling"] == 0 or hp[1]):
                # a boss's fight length is the arena's: its effect row expects this HP
                layout.set(row, "hp", layout.get(census.npc_rows[hp[0]], "hp"))
            if (stem, part) in enemy_hp:
                layout.set(row, "hp", enemy_hp[(stem, part)])
            if options["rewards"] == 0 or (stem, part) in enemy_hp or model in census.joined:
                for name in SLOT_REWARDS:
                    layout.set(row, name, layout.get(source, name))
            if originals[part]["entity"] in SUMMONS:  # its summon event despawns what lacks the marker (gamedata)
                _mark(layout, row, SUMMONS[originals[part]["entity"]])
            body = bytes(row)
            row_id = by_bytes.get(body)
            if row_id is None:
                row_id = start + len(rows)
                by_bytes[body] = row_id
                rows.append((row_id, body))
            slot_row[(stem, part)] = row_id
    return slot_row, rows


# A boss standing as an ordinary enemy (option bosses_as_enemies; user, 8 Nov: its HP cut). An enemy's real HP is its row's
# base `hp` times the maxHpRate of its area SpEffect (spEffectID6; DATA, knowledge params.md); option scaling decides which
# area effect the row takes (the slot's, the replacement's own, or the fixed one: area_effect). The base HP becomes
# option boss_enemy_hp (8-16, user 8 Nov) times the median base HP of the area's ordinary enemies (difficult.area_median:
# the same measure that calls an enemy difficult at 3 times it; the toughest ordinary rows reach 3-6 times it), never less
# than the slot's own enemy. Then the cap (user 8 Nov, PLAN 14 fix 4): the FINAL HP (base x the slot's area rate) never
# exceeds the boss's HP in its own fight (its row's base HP x its own row's area rate: Gascoigne 1,440 x 1.411 = 2,031).
# Central Yharnam (median 80): 8x = 640 base, 640-1,110 HP under its area effects 1.0-1.735; the Hunter's Nightmare's
# Gascoigne (median 80, rate 6.911) was 640 x 6.911 = 4,423, now 293 x 6.911 = 2,024 (his fight: 2,031).


def area_effect(options, slot_npc_row, layout, npc_row):
    """The area SpEffect (spEffectID6) a replacement row in a slot ends with (clone_rows: scaling 0 the slot's, 1 the
    replacement's own, 2 the fixed one)."""
    if options["scaling"] == 2:
        return options["fixed_effect"]
    return layout.get(slot_npc_row if options["scaling"] == 0 else npc_row, "spEffectID6")


def fight_hp(census, npc):
    """The HP of NpcParam row `npc` in its own place: base HP x its own area rate, as the game truncates it."""
    row = census.npc_rows[npc]
    return int(census.npc_layout.get(row, "hp") * census.hp_rate(census.npc_layout.get(row, "spEffectID6")))


def capped_hp(base, rate, most):
    """The highest base HP <= base whose final HP (int(base x rate)) is at most `most`."""
    if rate > 0 and int(base * rate) > most:
        base = int(most / rate)
        while base > 0 and int(base * rate) > most:
            base -= 1
    return base


def boss_enemy_hp(census, result):
    """{(stem, part): base HP} of every boss standing as an enemy in this plan (the rule above)."""
    layout = census.npc_layout
    options = result["options"]
    times = options.get("boss_enemy_hp", 8)
    out = {}
    for stem, parts in result.get("boss_enemies", {}).items():
        median = difficult.area_median(census, stem)
        own = {s["part"]: s for s in census.slots[stem]}
        for part, key in parts.items():
            npc = result["enemy_fighters"][key]["npc"]
            slot_row = census.npc_rows[own[part]["npc"]]
            base = max(layout.get(slot_row, "hp"), int(times * median))
            rate = census.hp_rate(area_effect(options, slot_row, layout, census.npc_rows[npc]))
            out[(stem, part)] = capped_hp(base, rate, fight_hp(census, npc))
    return out


def huge_rows(census, npcs):
    """The NpcParam rows among `npcs` wider than every ordinary enemy (option huge_bosses_as_enemies, user 8 Nov): hitRadius
    above the largest of the pool's rows (DATA 8 Nov: 1.3, c1250 and c2530; Paarl 3.0, Ebrietas 2.2, Rom 1.9, Amelia 1.4,
    Ludwig 2.4 and 1.6, the Loran Darkbeast 3.0, the Bloodletting Beast 2.5, the Watchdog 2.2)."""
    layout = census.npc_layout
    widest = max(layout.get(census.npc_rows[npc], "hitRadius") for rows in census.candidates.values() for _, npc in rows)
    return frozenset(n for n in npcs if layout.get(census.npc_rows[n], "hitRadius") > widest)


def helper_rows(census, stem, placed, fighters, slot_row, new_rows):
    """NPC rows for the bodies that travel with a boss (helpers.Fight `travel`): a copy of the body's home row with the
    strength of the boss body's new row (SLOT_STRENGTH: the slot's area scaling or the option's, as the boss has it; the
    home row would bring the home map's), and for `same_hp` bodies its HP too (the Orphan's phase two takes phase one's
    damage). The copies join new_rows; a copy equal to the home row is the home row. Keys: (boss body, home entity) for
    each boss body of this map, so two Orphans in one map each get their own body's row; and home entity alone for the
    first of them (what helpers.add_parts reads when it has no per-body key)."""
    out = {}
    layout = census.npc_layout
    made = dict(new_rows)
    by_entity = {s["entity"]: s for slots in census.slots.values() for s in slots}
    for body in sorted(placed):
        fighter = fighters[placed[body]]
        fight = helpers.fight_of(fighter)
        if fight is None:
            continue
        boss_row = made[slot_row[(stem, by_entity[body]["part"])]]
        for home in fight.travel:
            if fighter.get("stage") and (home == fight.stage or home in fight.stage_extra):
                continue  # a stage boss brings its own row; the second phase's extras stay home
            home_row = by_entity[home]["npc"]
            row = bytearray(census.npc_rows[home_row])
            for name in (("hp",) if home in fight.same_hp else ()) + SLOT_STRENGTH:
                if name in SLOT_STRENGTH and layout.get(row, name) <= 0:
                    continue  # unscaled at home stays unscaled (user 25 Nov: the Coiled Host's puddle owners, none
                    # at home, took the slot's area effect and hit harder in easier areas)
                layout.set(row, name, layout.get(boss_row, name))
            body_bytes = bytes(row)
            if body_bytes == census.npc_rows[home_row]:
                row_id = home_row
            else:
                row_id = next((i for i, b in new_rows if b == body_bytes), None)
            if row_id is None:
                row_id = max(i for i, _ in new_rows) + 1
                new_rows.append((row_id, body_bytes))
            out[(body, home)] = row_id
            out.setdefault(home, row_id)
    return out


LOGIC, BATTLE = 0x100, 1


def script_source(census, file_name, goal, kind_code, prefer):
    """(archive, entry, info row) of a script, from the first archive that has it: `prefer`, then map order, then the
    chalice maps' archive (read-only, read only when no main map has the script; lead decision 4 Nov, D13)."""
    for stem in sorted(census.archives, key=lambda s: (s != prefer, s)):
        if file_name in census.script_files[stem]:
            archive = Archive(stem + "_00_00", census.archives[stem])
            return archive, archive.files[file_name], archive.info_row(goal, kind_code)
    if census.files.path(chalice.ARCHIVE):
        bnd = chalice.archive(census)
        if file_name in census.chalice_script_files:
            archive = Archive(chalice.ARCHIVE_STEM + "_00_00", bnd)
            return archive, archive.files[file_name], archive.info_row(goal, kind_code)
    return None, None, None


def merged_archive(census, stem, changes, model_home):
    """The stem's AI archive with the scripts its new enemies need, or None when it needs none."""
    if stem not in census.archives:
        return None, []
    needed = {}
    for model, think, npc in sorted(set(changes.values())):
        battle, logic = census.goals(think, npc)
        needed[(script_name(battle, "battle"), battle, BATTLE)] = model
        if logic > 0:
            needed[(script_name(logic, "logic"), logic, LOGIC)] = model
    additions, missing = [], []
    have = census.script_files[stem]
    for (file_name, goal, kind), model in sorted(needed.items()):
        if file_name in have or file_name in census.common_scripts:
            continue
        source, entry, row = script_source(census, file_name, goal, kind, model_home.get(model))
        if source is None:
            missing.append(file_name)
            continue
        additions.append((file_name, entry[3], row, source.globals_of(entry[3])))
    if not additions:
        return None, missing
    original = census.archives[stem]
    copy = {"header": original["header"], "entries": [list(e) for e in original["entries"]]}
    archive = Archive(stem + "_00_00", copy)
    archive.add_scripts(additions)
    return bnd4_write(copy), missing


def patched_events(data, stem, placed, fighters, modes, animations, origin_events=None, sources=None,
                   helper_models=None, host_only=frozenset()):
    """The map's event file for its new bosses (boss_events.arena_events: second-attempt starts, scenes off, part and
    phase events and bar names per body, what each kind of fight needs), then with the forced animations a body's new
    model lacks made inert. placed: {entity: fighter key} of this stem's bodies with a boss; fighters: {key: fighter
    dict}; modes: {arena key: "swap" | "mix" | "normal"}; animations: {model: animation ids}; origin_events(entity) ->
    the Emevd of that entity's home map (data already carries the helpers' edits,
    helpers.apply_stem); sources: the helpers' Emevd.sources (their copies, for hostonly.cover);
    helper_models: {entity: model} of the bodies the helpers added (a stage boss lacks the second phase's animations);
    host_only: the event ids the inputs' mods list host-only (hostonly.mod_listed).
    Returns (bytes or None, [(entity, event, instruction, animation)], report)."""
    events = Emevd(data)
    events.sources.update(sources or {})
    report = boss_events.arena_events(events, placed, fighters, modes or {}, origin_events)
    # after the arena edits: the animation pass must see the bodies the arena really runs
    models = {e: fighters[k]["model"] for e, k in placed.items()}
    models.update(helper_models or {})
    done = _inert_animations(events, models, animations, foreign=frozenset(placed))
    changed = done or any(report.values())
    # last: copies of host-only phase events take this map's emptied listed ids (co-op, hostonly.py)
    covered = hostonly.cover(events, host_only)
    for copy, listed, *_ in covered.get("covered", ()):  # the report and the patches name the ids the file ends with
        report, done = _swapped((report, done), copy, listed)
    report["host_only"] = covered
    changed = changed or bool(covered.get("covered"))
    return (events.bytes() if changed else None), done, report


def _swapped(value, a, b):
    """value (nested lists, tuples and dicts of ints) with a and b traded, as Emevd.swap_ids trades every word naming
    them; other ids (entities, bar names, indices) never equal an event id."""
    if isinstance(value, dict):
        return {_swapped(k, a, b): _swapped(v, a, b) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return type(value)(_swapped(v, a, b) for v in value)
    return b if value == a else a if value == b else value


def released_runs(census, stem, final):
    """[(entity, calling event, call line, template, kind)] of the held runs (gamedata.held_runs) of this map's parts that
    get another model (final: {part: (model, ...)}); a part that draws its own kind again keeps them."""
    out = []
    for slot in census.slots[stem]:
        if slot.get("held") and slot["part"] in final and final[slot["part"]][0] != slot["model"]:
            out.extend((slot["entity"],) + tuple(run) for run in slot["held"])
    return sorted(out)


def replaced_entities(census, stem, final):
    """The entities of this map's ordinary parts that get another model (final: {part: (model, ...)}); a fight's bodies,
    escorts and summons are the boss passes' business (gamedata.Census `fight`)."""
    fight = BOSS_ENTITIES | set(ESCORT_ARENA) | set(SUMMONS)
    return frozenset(s["entity"] for s in census.slots[stem] if s["entity"] > 0 and s["entity"] not in fight
                     and s["part"] in final and final[s["part"]][0] != s["model"])


PARK = 80.0  # metres under the part that needs it (vanilla parks the Orphan's phase two 82 units under his arena)


def texture_parts(census, variants, events):
    """A hidden part for every model the map's enemies draw with but no enemy part of the map loads (textures.py,
    planner.with_sources: the Orphan's phase two c4541 alone needs c4540): a copy of the first enemy part that needs it,
    with that model and one of its own rows, PARK metres under it, held at load (event 0) the way vanilla parks a waiting
    body (the Orphan's phase two 82 units under his arena): AI, gravity and its HP bar off, invincible. A character's
    model loads only when the player comes near it, and never while it is disabled (LIVE 23 Nov: a disabled c4540 part
    in m22 never loaded). variants: {variant: MSB bytes} after every other part change; events: this map's Emevd
    (edited in place). Returns ({variant: bytes}, [{model, entity, name, think, npc, for}])."""
    import planner
    msbs = {v: Msb(b) for v, b in variants.items()}
    present = set().union(*({e["model"] for e in m.enemies()} for m in msbs.values()))
    planner.TEXTURE_SOURCES.clear()
    planner.TEXTURE_SOURCES.update(census.texture_sources)
    missing = sorted(planner.with_sources(present) - present)
    if not missing:
        return variants, []
    rows = {}
    for stem in sorted(census.slots):
        for slot in census.slots[stem]:
            rows.setdefault(slot["model"], (slot["think"], slot["npc"]))
    for model, pairs in sorted(census.candidates.items()):
        rows.setdefault(model, tuple(pairs[0]))
    named = set()
    for event in events.event_ids():
        for _, _, words in events.instructions(event):
            named.update(words)
    blobs = [bytes(m.data) for m in msbs.values()]
    names = set().union(*(m.part_names() for m in msbs.values()))
    first = msbs[sorted(msbs)[0]].enemies()
    base = next(e["entity"] for e in first if e["entity"] > 0) // 10000 * 10000
    hidden = []
    for model in missing:
        if model not in rows:
            raise RuntimeError("no NPC row for texture source %s" % model)
        entity = next(c for c in range(base + 9999, base + 9000, -1)
                      if c not in named and not any(struct.pack("<i", c) in b for b in blobs))
        named.add(entity)
        name = next("%s_%04d" % (model, k) for k in range(9000, 10000) if "%s_%04d" % (model, k) not in names)
        names.add(name)
        think, npc = rows[model]
        users = sorted(m for m in present if model in planner.with_sources({m}) and m != model)
        hidden.append({"model": model, "entity": entity, "name": name, "think": think, "npc": npc, "for": users})
    out = {}
    for variant, msb in msbs.items():
        for h in hidden:
            # the first part of this variant that needs it (a variant without one: the first anywhere)
            users = [e for e in msb.enemies() if e["model"] in h["for"] and len(e["part"]) == len(h["name"])]
            if not users:
                users = [e for m in msbs.values() for e in m.enemies() if e["model"] in h["for"]]
                users = [e for e in msb.enemies() if e["part"] == users[0]["part"]] if users else []
            user = users[0] if users else next(e for e in msb.enemies() if len(e["part"]) == len(h["name"]))
            x, y, z = struct.unpack_from("<3f", msb.data, user["at"] + 0x28)
            msb.add_enemy(user["part"], h["name"], h["entity"], h["model"], h["think"], h["npc"], (x, y - PARK, z))
        out[variant] = msb.bytes()
    for h in hidden:
        for bank, iid, state in ((2004, 1, 0), (2004, 10, 0), (2004, 30, 0), (2004, 15, 1)):
            events.append_instruction(0, bank, iid, [h["entity"], state])
    return out, hidden


def riders_off(census, stem, replaced):
    """The riding parts (Census.riders) whose host gets another model: their rider goes too (user, 10 Nov)."""
    return sorted(r for r, h in census.riders[stem].items() if h in replaced)


def release(payload, released, replaced=frozenset(), report=None, riders=()):
    """The event file with the call lines of these runs turned into labels: the template never runs for that part. Then
    every Set Character AI ID line aimed at a replaced entity goes too (release_ai; its edits land in `report`), and the
    riders are disabled at load (event 0; their warp runs then move a part that is not there)."""
    if not released and not replaced and not riders:
        return payload
    events = Emevd(payload)
    for rider in riders:
        events.append_instruction(0, 2004, 5, [rider, 0])
    for _, event, line, template, _ in released:
        bank, iid, words = events.instructions(event)[line]
        if bank != 2000 or iid not in (0, 6) or template not in words[:2]:
            raise RuntimeError("event %d line %d is no run of %d" % (event, line, template))
        events.neutralize(event, line)
    done = release_ai(events, replaced)
    if report is not None:
        report.extend(done)
    return events.bytes() if released or done or riders else payload


def _calls(events):
    """{template: [(calling event, call line, index of the template's word, first argument word, words)]} of this file's
    2000[0]/[6] runs (a label made of a released call is no run)."""
    ids = set(events.event_ids())
    calls = {}
    for event in events.event_ids():
        for k, (bank, iid, words) in enumerate(events.instructions(event)):
            if bank == 2000 and iid in (0, 6) and len(words) >= 2:
                at = 1 if words[1] in ids else 0
                calls.setdefault(words[at], []).append((event, k, at, at + 1, words))
    return calls


def ai_targets(events):
    """[(entity, event, line, call)]: every Set Character AI ID line (2004[19]) of this file and the entity it sets,
    written in the line (call None) or given by a run of its template (call = (calling event, call line))."""
    out, calls = [], _calls(events)
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) != AI_ID or not words:
                continue
            src = params.get(i, {}).get(0)
            if src is None:
                out.append((words[0], event, i, None))
                continue
            for caller, k, _, first, run in calls.get(event, ()):
                if first + src // 4 < len(run):
                    out.append((run[first + src // 4], event, i, (caller, k)))
    return out


def _slot_flags(events, calls):
    """Ids a template run's slot can hold as its own flag (the template's id + the slot), kept from new event ids."""
    return {template + run[0] for template, sites in calls.items() for _, _, at, _, run in sites if at == 1}


def release_ai(events, replaced):
    """User 8 Nov (replaced resting Yharnamites "look at me and don't react"): a map event that sets a part's AI ID
    (2004[19]) gives it its vanilla kind's AI, whose script the new model cannot run, so it stands. Every such line aimed at
    a replaced entity becomes a label; the rest of the event stays (its animations as the pose patches leave them, its
    gravity off/on pairs). A line written for the entity goes in place. A template line goes in place when every run of
    the template loses the same lines; otherwise the runs that lose lines get a copy of the template with those lines
    labelled (a new id that no event, word, flag range or run slot here takes, in the template's thousand block or
    another this file's events use), and the vanilla kind's runs keep the original. Returns [(entity, event, line, call or None, copy or None)]."""
    if not replaced:
        return []
    found = [t for t in ai_targets(events) if t[0] in replaced]
    # where a copy's id may go: the template's thousand block, else another block this file's vanilla events use (DATA:
    # m24_01's 12410xxx is all taken by events, words and flag ranges); a block no event uses may hold no flags
    used = sorted({e // 1000 * 1000 for e in events.event_ids() if e >= 10000000}, reverse=True)
    blocks = lambda template: [template // 1000 * 1000] + [b for b in used if b != template // 1000 * 1000]
    done = []
    for entity, event, line, call in sorted(t for t in found if t[3] is None):
        events.neutralize(event, line)
        done.append((entity, event, line, None, None))
    calls = _calls(events)
    drop = {}  # template: {(caller, call line): {lines}}
    for entity, event, line, call in found:
        if call is not None:
            drop.setdefault(event, {}).setdefault(call, set()).add(line)
    who = {(event, line, call): entity for entity, event, line, call in found if call is not None}
    for template in sorted(drop):
        sites = {(caller, k): (at, run) for caller, k, at, _, run in calls[template]}
        lose = {site: frozenset(lines) for site, lines in drop[template].items()}
        if len(set(lose.values())) == 1 and set(lose) == set(sites):
            for line in sorted(next(iter(lose.values()))):
                events.neutralize(template, line)
            for site, lines in sorted(lose.items()):
                done.extend((who[(template, line, site)], template, line, site, None) for line in sorted(lines))
            continue
        for lines in sorted(set(lose.values()), key=sorted):
            group = sorted(site for site, l in lose.items() if l == lines)
            taken = boss_events._taken(events) | _slot_flags(events, _calls(events))
            slots = [sites[site][1][0] if sites[site][0] == 1 else 0 for site in group]
            copy = next((c for block in blocks(template) for c in range(block + 999 - max(slots, default=0), block, -1)
                         if all(c + s not in taken for s in [0] + slots)), None)
            if copy is None:
                raise RuntimeError("no free event id for a copy of %d" % template)
            events.add_event(copy, events.restart(template), [(b, i, list(w)) for b, i, w in events.instructions(template)],
                             events.param_entries(template))
            for line in sorted(lines):
                events.neutralize(copy, line)
            for caller, k in group:
                events.set_word(caller, k, sites[(caller, k)][0], copy)
                done.extend((who[(template, line, (caller, k))], template, line, (caller, k), copy) for line in sorted(lines))
    return sorted(done, key=lambda d: (d[0], d[1], d[2], d[3] or (), d[4] or 0))


def _inert_animations(events, models, animations, foreign=frozenset()):
    """Every forced animation on a body (models: {entity: its new model}) that the model lacks is turned off, so the body
    stands (user 7 Nov: "if the replaced enemy has the animation play, else stand"). foreign: bodies holding a boss from
    elsewhere. On them every forced animation of the map's own events goes too, had or not: a boss's event animations
    (7000s and the like) are its own scripted moves, so the same id on another boss is another move (DATA 9 Nov,
    NW-b6df2d1: Amelia's retry start 12404802 forces 7002, her rise; Maria's c4520 has a 7002 and held it, unhittable,
    the fight never began). The boss's own events copied in with it (Emevd.sources) keep theirs. Written plainly: the instruction
    becomes a label. Fed by a template's parameters: when the run gives the animation, only that run's argument becomes
    CANCEL_ANIMATION, so the template's other bodies keep theirs; when the animation is written in the template, or its
    argument feeds another instruction too, the instruction goes for every run (one run's body lacking it is enough). Returns [(entity, event, instruction, animation)]."""
    ids = set(events.event_ids())
    calls = {}  # template event: [(calling event, instruction, first argument word, arguments)]
    for event in events.event_ids():
        for k, (bank, iid, words) in enumerate(events.instructions(event)):
            if bank == 2000 and iid in (0, 6) and len(words) >= 2:
                target, skip = (words[1], 2) if words[1] in ids else (words[0], 1)
                calls.setdefault(target, []).append((event, k, skip, words[skip:]))
    def lacks(e, a, event):
        if a is None or a == CANCEL_ANIMATION:
            return False
        if e in foreign and event not in events.sources:
            return True
        return e in models and models[e] in animations and a not in animations[models[e]]
    done = []
    for event in events.event_ids():
        params = events.params(event)
        for i, (bank, iid, words) in enumerate(events.instructions(event)):
            if (bank, iid) != FORCED_ANIM or len(words) < 2:
                continue
            fed = params.get(i, {})
            if not (0 in fed or 4 in fed):
                if lacks(words[0], words[1], event):
                    events.neutralize(event, i)
                    done.append((words[0], event, i, words[1]))
                continue
            pick = lambda run, at, plain: plain if at is None else (run[at // 4] if at // 4 < len(run) else None)
            lacking = [(caller, k, skip, entity, animation) for caller, k, skip, run in calls.get(event, ())
                       for entity, animation in [(pick(run, fed.get(0), words[0]), pick(run, fed.get(4), words[1]))]
                       if lacks(entity, animation, event)]
            if not lacking:
                continue
            # the run's argument is the animation's alone when no other instruction of the template reads it
            own = 4 in fed and not any(fed[4] in other.values() for j, other in params.items() if j != i)
            if own:
                for caller, k, skip, entity, animation in lacking:
                    events.set_word(caller, k, skip + fed[4] // 4, CANCEL_ANIMATION)
                    done.append((entity, event, i, animation))
            else:
                events.neutralize(event, i)
                done.append((lacking[0][3], event, i, lacking[0][4]))
    return done


def write_file(root, rel, data, manifest):
    path = os.path.join(root, "dvdroot_ps4", rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    manifest[rel] = hashlib.sha256(data).hexdigest()


BASE_LABEL = re.compile(r"[a-z0-9@._:+-]{1,180}")


PROGRESS_STEPS = ("read", "plan", "maps", "effects", "items", "text", "write")


LABEL_MAX = 255  # the lobby's mods label (VpnLobby.ValidModsLabel in the launcher)


def lobby_label(base_label, code, input_hash, build, roots=()):
    """The lobby's mods label for a world: the base mods' part (given by the launcher, by content and in load order:
    nocturne@<version> for a recognised Nocturne, m:<8 hex> for any other mod, joined by '+'), the generator version and
    the code (lowercase), w: = the input hash, which a guest compares after building its own copy, g: = the generator
    build (buildid.py, also inside w:), so a guest whose copy differs can say why, and i: = 8 hex per root of the input
    hash's parts (GameFiles.root_input: the game, then each mod in load order; 00000000 for a root nothing was read
    from), so it can also say whether its game files or a mod's differ. i: is left out when the label would pass
    LABEL_MAX."""
    part = "rando%d:%s+w:%s+g:%s" % (GENERATOR_MAJOR, code.lower(), input_hash[:8], build[:8])
    label = base_label + "+" + part if base_label else part
    if roots:
        longer = label + "+i:" + "".join((h or "0" * 8)[:8] for h in roots)
        if len(longer) <= LABEL_MAX:
            label = longer
    return label


def world_plan(files, census, seed, options):
    """The plan of a world (planner.plan with what the options read in: chalice enemies and bosses, hard rows, huge
    rows, the slots as the options shape them), checked. Adds the chalice enemies to census.candidates when that option
    is on. Returns (result, wanted, slots, unposed, extra_fighters)."""
    groups = load_groups()
    limits = group_limits(census, groups)
    wanted = normalize(options)
    # the chalice dungeons' regular kinds join the pool (their maps are read only then: they count as inputs); first, so
    # every pool use below (away_from_home, the plan, the effects) sees them
    if wanted["chalice_enemies"]:
        census.add_candidates(chalice_enemies.candidates(files, census))
    # the bell maidens by their option (census.slots stays as read: drops and the spoiler see the vanilla kept state)
    slots = {stem: [escort_slot(s, wanted["bell_maidens"]) if s["entity"] in ESCORTS else s for s in v]
             for stem, v in census.slots.items()}
    if wanted["shadows_summons"]:  # the Shadows' summons are random enemies; off, there are none (_no_summons)
        slots = {stem: [dict(s, kept=None) if s["entity"] in SUMMONS else s for s in v] for stem, v in slots.items()}
    if wanted["free_poses"]:  # poses are no need: a replacement without them stands (posed_slot; turned off below)
        slots = {stem: [posed_slot(s) for s in v] for stem, v in slots.items()}
    elif wanted["away_from_home"]:  # the posed slots only the map's own kinds could take: likewise (away_slot)
        pool = replacement_pool(wanted, census.candidates, census.sizes)
        slots = {stem: [away_slot(s, pool - {x["model"] for x in census.slots[stem]}, census.animations) for s in v]
                 for stem, v in slots.items()}
    # the parts whose poses are no longer needed (every posed part with free_poses): turned off below where the new model lacks them
    unposed = {stem: {o["part"] for s, o in zip(v, census.slots[stem])
                      if o.get("poses") and (wanted["free_poses"] or s.get("needs") != o.get("needs"))}
               for stem, v in slots.items()}
    # chalice bosses join the fighters (read only then: their files count as inputs), or fill the boss pool for second
    # stages (planner.plan, user 10 Nov); hard copies bring their deep rows
    extra_fighters = (chalice.fighters(files, census)
                      if (wanted["chalice_bosses"] and (wanted["boss_swap"] or wanted["bosses_as_enemies"]))
                      or (wanted["boss_swap"] and wanted["second_stage"]) else [])
    hard_rows = {}
    if any(wanted[name] for name in chalice.hard_options().values()):
        on = {e for e, name in chalice.hard_options().items() if wanted[name]}
        hard_rows = {e: row for e, row in chalice.hard_rows(files, census).items() if e in on}
    huge = (huge_rows(census, [s["npc"] for v in census.slots.values() for s in v if s["role"] in ("body", "extra")]
                      + [f["npc"] for f in extra_fighters]) if wanted["bosses_as_enemies"] else frozenset())
    result = plan(slots, census.candidates, census.sizes, groups, limits, seed, options, census.animations,
                  difficult.candidates(census), extra_fighters, hard_rows, huge=huge,
                  texture_sources=census.texture_sources)
    problems = check(result, slots, census.candidates, census.sizes, groups, limits, census.animations)
    if problems:
        raise RuntimeError("the plan broke its own rules: " + "; ".join(problems[:5]))
    return result, wanted, slots, unposed, extra_fighters


def generate(game, mods, out_root, code, label=None, base_label=None, progress=None, update=None):
    """progress(step, done, total): called as the work goes, step one of PROGRESS_STEPS."""
    def step(name, done=0, total=1):
        if progress:
            progress(name, done, total)
    seed, options = decode(code)
    buildid.build_id()  # no world without the generator's own id (fails before any work)
    code = encode(seed, options)  # the one spelling: case and look-alikes never make two folders for one world
    if base_label is not None and not BASE_LABEL.fullmatch(base_label):
        raise ValueError("base label: 1-180 characters of a-z 0-9 @ . _ : + -")
    files = GameFiles(game, mods, update)  # update: the game's update folder when beside the game (GameFiles)
    files.check_updated()  # OldGame: a game without its 1.09 files, said before any edit misses its line
    step("read")
    census = Census(files)
    step("plan")
    result, wanted, slots, unposed, extra_fighters = world_plan(files, census, seed, options)
    changes = result["changes"]

    # a boss as an enemy (option bosses_as_enemies) takes a cut HP (boss_enemy_hp) and the slot's rewards
    slot_row, new_rows = clone_rows(census, changes, result["options"], boss_hp_rows(census, result),
                                    boss_enemy_hp(census, result))
    fighters = result["fighters"]
    model_home = {}
    for stem in STEMS:
        for slot in census.slots[stem]:
            if slot["kept"] is None:
                model_home.setdefault(slot["model"], stem)

    # chalice ritual materials stay where they are (map pickups, shop rows, enemy drops) unless the option shuffles them
    # too (user, 7 Nov: "very literally")
    materials = frozenset() if result["options"]["chalice_materials"] else treasure_shuffle.chalice_materials(files)
    # shop trades first, on the vanilla lot rows; the treasure shuffle then moves lot ids, so a traded pickup row travels
    # with its lot. Both are drawn from vanilla data, so neither changes the other's draw.
    shop_found, shop_plan, shop_deal = None, {}, {}
    if result["options"]["shops"]:
        joining = treasure_shuffle.chalice_materials(files) if result["options"]["chalice_materials"] else frozenset()
        shop_found = shop_shuffle.census(files, joining, treasure_shuffle.chalice_materials(files))
        shop_plan = shop_shuffle.plan(shop_found, seed, result["options_hash"])
        shop_deal = shop_shuffle.deal(shop_found, seed, result["options_hash"], shop_plan)  # the rows no trade touches
    lots = {}
    if result["options"]["treasure"]:
        lots = treasure_shuffle.plan(treasure_shuffle.census(files, materials), seed, result["options_hash"], bool(result["options"]["treasure_by_type"]))
    outputs, missing_scripts, boss_patches, pose_patches, released_report, ai_report, rider_report = {}, {}, [], [], {}, {}, {}
    home_stem = {s["entity"]: stem for stem in STEMS for s in census.slots[stem]}
    event_reports = {}
    texture_report = {}  # {stem: [[source model, entity, models that need it]]} (texture_parts)
    event_effects = {}  # {stem: effect ids its events newly spawn} (effects.plan)
    travelled = {}  # {stem: {(model, home npc row)}} of the bodies that came with a boss, for their effects
    mod_host_only = hostonly.mod_listed(files.mod_folders)  # a copy of a mod's host-only event stays host-only

    def origin_events(entity):
        return Emevd(dcx_read(files.read("event/%s_00_00.emevd.dcx" % home_stem[entity]))[0])

    def origin_msb(entity):
        variant = map_variants(files, home_stem[entity])[0]
        return dcx_read(files.read("map/mapstudio/%s.msb.dcx" % variant))[0]
    for number, stem in enumerate(STEMS):
        step("maps", number, len(STEMS))
        placed = {entity: result["boss_fighters"][entity] for entity, (boss_stem, _) in result["bosses"].items()
                  if boss_stem == stem}
        modes = {key: mode for key, mode in result["arena_modes"].items() if BY_KEY[key].stem == stem}
        if not changes[stem] and not lots and not placed and not modes:
            continue
        final = {part: (model, think, slot_row[(stem, part)]) for part, (model, think, _) in changes[stem].items()}
        # free_poses: the poses a replaced enemy's new model lacks; the map's start pose is cleared, the events' go inert
        posed = {s["part"]: s for s in census.slots[stem] if s["part"] in unposed.get(stem, ())}
        lacking = {part: final[part][0] for part, s in posed.items() if part in final
                   and any(a not in census.animations.get(final[part][0], ()) for a in s["poses"])}
        start_cleared = {part for part in lacking if posed[part]["start_anim"] in posed[part]["poses"]
                         and posed[part]["start_anim"] not in census.animations.get(lacking[part], ())}
        loose = {posed[part]["entity"]: model for part, model in lacking.items()}
        variants = {}  # variant: (MSB bytes after the swap, vanilla bytes, dcx header, vanilla part names)
        for variant in map_variants(files, stem):
            rel = "map/mapstudio/%s.msb.dcx" % variant
            payload, header = dcx_read(files.read(rel))
            msb = Msb(payload)
            before = msb.part_names()
            done = msb.swap_enemies(final)
            if msb.clear_start_animations(start_cleared) != start_cleared:
                raise RuntimeError(rel + ": a posed part not found")
            if msb.part_names() != before:
                raise RuntimeError(rel + ": the part list changed")
            if done != set(final):
                raise RuntimeError(rel + ": %d parts not found" % len(set(final) - done))
            variants[variant] = (msb.bytes(), payload, header, before)
        added, helper_scripts = [], []
        # a replaced part goes without the runs that hold its vanilla kind (gamedata.held_runs: caged, immortal)
        released = released_runs(census, stem, final)
        if released:
            released_report[stem] = [list(r) for r in released]
        # and the event lines that would give a replaced part its vanilla kind's AI (release_ai)
        replaced, ai_done = replaced_entities(census, stem, final), []
        off = riders_off(census, stem, replaced)
        if off:
            rider_report[stem] = off
        if placed or modes:
            rel = "event/%s_00_00.emevd.dcx" % stem
            payload, header = dcx_read(files.read(rel))
            payload = release(payload, released, replaced, ai_done, off)
            events = Emevd(payload)
            # helpers first (they re-point the bars that boss_events reads), on the swapped maps (they add parts)
            edited, helper_report = helpers.apply_stem({e: fighters[k] for e, k in placed.items()}, events,
                                                       {v: b[0] for v, b in variants.items()}, origin_events, origin_msb,
                                                       helper_rows(census, stem, placed, fighters, slot_row, new_rows))
            for variant, data in edited.items():
                variants[variant] = (data,) + variants[variant][1:]
            added = [b["name"] for bodies in helper_report["bodies"].values() for b in bodies]
            travelled[stem] = {(b["model"], b["npc"])  # the home part's row: its bullets' effects
                               for bodies in helper_report["bodies"].values() for b in bodies}
            helper_scripts = helper_report["scripts"]
            helped = bool(helper_report["dismissed"] or helper_report["travelled"])
            patched, done, event_reports[stem] = patched_events(events.bytes() if helped else payload, stem, placed,
                                                                fighters, modes, census.animations, origin_events,
                                                                events.sources if helped else None,
                                                                {**loose, **{b["entity"]: b["model"]
                                                                 for bodies in helper_report["bodies"].values()
                                                                 for b in bodies}}, mod_host_only)
            event_reports[stem]["helpers"] = {"dismissed": len(helper_report["dismissed"]),
                                              "travelled": len(helper_report["travelled"]), "parts": added}
            boss_patches.extend(d for d in done if d[0] not in loose)
            pose_patches.extend(d for d in done if d[0] in loose)
            if patched is None and helped:
                patched = events.bytes()
            if patched is None and (released or ai_done or off):
                patched = payload
            if patched is not None:
                outputs[rel] = dcx_write(patched, header)
        elif loose or released or replaced:
            rel = "event/%s_00_00.emevd.dcx" % stem
            payload, header = dcx_read(files.read(rel))
            events = Emevd(release(payload, released, replaced, ai_done, off))
            done = _inert_animations(events, loose, census.animations)
            pose_patches.extend(done)
            if done or released or ai_done or off:
                outputs[rel] = dcx_write(events.bytes(), header)
        if ai_done:
            ai_report[stem] = [[entity, event, line, list(call) if call else None, copy]
                               for entity, event, line, call, copy in ai_done]
        # the models whose textures this map's enemies draw with get a hidden part each (textures.py)
        rel = "event/%s_00_00.emevd.dcx" % stem
        events = Emevd(dcx_read(outputs[rel] if rel in outputs else files.read(rel))[0])
        if rel in outputs:  # effects the travelled events spawn (the Coiled Host's puddle look 651207, user 25 Nov)
            new_effects = effect_copies.spawned(events) - effect_copies.spawned(Emevd(dcx_read(files.read(rel))[0]))
            if new_effects:
                event_effects[stem] = new_effects
        edited, hidden = texture_parts(census, {v: b[0] for v, b in variants.items()}, events)
        if hidden:
            for variant, data in edited.items():
                variants[variant] = (data,) + variants[variant][1:]
            added = added + [h["name"] for h in hidden]
            helper_scripts = helper_scripts + sorted({(h["model"], h["think"], h["npc"]) for h in hidden})
            outputs[rel] = dcx_write(events.bytes(), dcx_read(files.read(rel))[1])
            texture_report[stem] = [[h["model"], h["entity"], h["for"]] for h in hidden]
        for variant, (data, payload, header, before) in sorted(variants.items()):
            rel = "map/mapstudio/%s.msb.dcx" % variant
            if sorted(Msb(data).part_names()) != sorted(before + added):
                raise RuntimeError(rel + ": the part list changed beyond the helpers' parts")
            edited = treasure_shuffle.trade_lots(data, lots) if lots else data
            if edited != payload:
                outputs[rel] = dcx_write(edited, header)
        if not changes[stem] and not helper_scripts:
            continue
        needs = dict(changes[stem])
        needs.update((("helper", n), row) for n, row in enumerate(helper_scripts))  # their scripts come along too
        bnd, missing = merged_archive(census, stem, needs, model_home)
        if missing:
            missing_scripts[stem] = missing
        if bnd is not None:
            rel = "script/%s_00_00.luabnd.dcx" % stem
            outputs[rel] = dcx_write(bnd, dcx_read(files.read(rel))[1])
    if missing_scripts:
        raise RuntimeError("scripts found nowhere: %r" % missing_scripts)
    for stem in sorted({arenas.BY_KEY[k].stem for k in arenas.PHASES.values()}):  # the phase bodies' co-op set-up
        rel = "event/%s_00_00.emevd.dcx" % stem
        payload, header = dcx_read(outputs[rel] if rel in outputs else files.read(rel))
        events = Emevd(payload)
        coop = boss_events.phase_coop(events, stem)
        if coop:
            event_reports.setdefault(stem, {})["phase_coop"] = coop
            outputs[rel] = dcx_write(events.bytes(), header)
    if not result["options"]["shadows_summons"]:  # on top of whatever the boss pass made of the file
        rel = "event/m27_00_00_00.emevd.dcx"
        payload, header = dcx_read(outputs[rel] if rel in outputs else files.read(rel))
        events = Emevd(payload)
        _no_summons(events)
        outputs[rel] = dcx_write(events.bytes(), header)
    # always (an enemy moved without its effects loses its projectiles), and before the items: the NpcParam row marks
    # take files.read_hash(), which must already cover every file the world reads
    step("effects")
    effect_outputs, effect_report = effect_copies.plan(files, census, changes,
                                                       lambda done, total: step("effects", done, total), travelled,
                                                       event_effects)
    outputs.update(effect_outputs)
    step("items")
    drop_plan = {}
    if result["options"]["drops"]:
        drop_plan = drop_shuffle.plan(drop_shuffle.census(files, census, materials), seed, result["options_hash"])
    starter_plan, starter_copies = {}, {}
    melee, guns = result["options"]["starter_melee"], result["options"]["starter_guns"]
    hidden, added = starter_weapons.hidden_rows(melee, guns), starter_weapons.added_rows(melee, guns)
    if result["options"]["starters"] or added:
        starter_plan = starter_weapons.plan(starter_weapons.census(files), seed, result["options_hash"], melee, guns,
                                            bool(result["options"]["starters"]))
    outfit = []
    if result["options"]["starter_armor"]:
        outfit = starter_weapons.armor_plan(starter_weapons.armor_census(files), seed, result["options_hash"])
    # gem roulette (user 8 Nov): after the treasure, shop and drop plans, so a gem's tier is the map its pickup or drop
    # ends up in; shop-traded pickup rows are left to the shops
    gem_found, gem_plan, gem_lots = None, None, {}
    if result["options"]["gem_roulette"]:
        gem_found = gem_roulette.census(files)
        gem_plan = gem_roulette.plan(gem_found, seed, result["options_hash"], gem_roulette.pickup_stems(files, lots),
                                     gem_roulette.drop_stems(census, changes, slot_row, new_rows, drop_plan),
                                     bool(result["options"]["cursed_gems"]), frozenset(shop_plan.values()))
        gem_lots = gem_roulette.lot_rows(gem_found, gem_plan)
    # the blood stone floor (shops.STONES, user 9 Nov) whenever an item shuffle can move the stones
    stone_floor = bool(result["options"]["treasure"] or result["options"]["shops"] or result["options"]["drops"])
    if new_rows or shop_plan or shop_deal or drop_plan or starter_plan or hidden or outfit or gem_lots or stone_floor:
        rel = "param/gameparam/gameparam.parambnd.dcx"
        payload, header = dcx_read(files.read(rel))
        bnd = bnd4_read(payload)
        if new_rows or drop_plan:
            entry = bnd4_find(bnd, "\\NpcParam.param")
            param = Param(entry[3])
            param.append_sorted(new_rows)
            drop_shuffle.apply(param, census.npc_layout, drop_plan)
            entry[3] = param.write()
        if shop_plan:
            defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
            entry = bnd4_find(bnd, "\\ItemLotParam.param")
            param = Param(entry[3])
            shop_shuffle.apply_lots(param, Layout(bnd4_find(defs, "\\ItemLotParam.paramdef")[3]), shop_found, shop_plan)
            entry[3] = param.write()
        if gem_lots:
            gen_entry, lot_entry = bnd4_find(bnd, "\\GemGenParam.param"), bnd4_find(bnd, "\\ItemLotParam.param")
            gen_param, lot_param = Param(gen_entry[3]), Param(lot_entry[3])
            gem_roulette.apply(gen_param, lot_param, gem_found, gem_plan)
            gen_entry[3], lot_entry[3] = gen_param.write(), lot_param.write()
        if shop_plan or shop_deal or starter_plan or hidden or stone_floor:
            defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
            entry = bnd4_find(bnd, "\\ShopLineupParam.param")
            param = Param(entry[3])
            layout = Layout(bnd4_find(defs, "\\ShopLineupParam.paramdef")[3])
            if shop_plan:
                shop_shuffle.apply(param, layout, shop_found, shop_plan)
            if shop_deal:
                shop_shuffle.apply_deal(param, layout, shop_found, shop_deal)
            if added:
                starter_weapons.add_rows(param, added)
            if starter_plan:
                weapons_entry = bnd4_find(bnd, "\\EquipParamWeapon.param")
                gems_entry = bnd4_find(bnd, "\\WeaponGenParam.param")
                weapons, gems = Param(weapons_entry[3]), Param(gems_entry[3])
                starter_copies = starter_weapons.copy_ids(weapons.first(), gems.first(), starter_plan.values())
                starter_weapons.apply_shop(param, layout, starter_plan, starter_copies)
                starter_weapons.apply_weapons(weapons, Layout(bnd4_find(defs, "\\EquipParamWeapon.paramdef")[3]), starter_copies)
                starter_weapons.apply_gems(gems, Layout(bnd4_find(defs, "\\WeaponGenParam.paramdef")[3]), starter_copies)
                weapons_entry[3], gems_entry[3] = weapons.write(), gems.write()
            if hidden:
                starter_weapons.hide_rows(param, hidden)
            if stone_floor:
                # the rows as read (the deal leaves them alone); without the shop shuffle the table is still as read
                stones = shop_found["stones"] if shop_found else shop_shuffle.stone_rows(layout, param.first())
                shop_shuffle.apply_stones(param, layout, stones)
            entry[3] = param.write()
        if outfit:
            defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
            entry = bnd4_find(bnd, "\\CharaInitParam.param")
            param = Param(entry[3])
            starter_weapons.apply_armor(param, Layout(bnd4_find(defs, "\\CharaInitParam.paramdef")[3]), outfit)
            entry[3] = param.write()
        outputs[rel] = dcx_write(bnd4_write(bnd), header)
    if starter_copies:
        step("text")
        for language in files.listdir("msg"):
            rel = "msg/%s/item.msgbnd.dcx" % language
            if files.path(rel) is None:
                continue
            payload, header = dcx_read(files.read(rel))
            bnd = bnd4_read(payload)
            for entry in bnd["entries"]:
                if entry[1] in starter_weapons.TEXTS:
                    fmg = fmg_read(entry[3])
                    starter_weapons.copy_text(fmg, starter_copies)
                    entry[3] = fmg_write(fmg)
            outputs[rel] = dcx_write(bnd4_write(bnd), header)

    # starter copies of weapons with their own clip group (Nightwatch 0.3.0, e.g. Nocturne v3.0.0's Reverie) get the same
    # group under their own ids: the world's nightwatch.json and nightwatch/weapon_clip_groups.json (starters.clip_files)
    clip_rows = starter_weapons.copy_clip_groups(starter_weapons.clip_groups(files), starter_copies) if starter_copies else []

    input_hash = files.input_hash()
    build = buildid.build_id()
    step("write")
    name = label or folder_name(code, input_hash)
    final_dir = os.path.join(out_root, name)
    temp_dir = final_dir + ".tmp"
    if os.path.exists(temp_dir):
        shutil.rmtree(temp_dir)
    manifest = {}
    for rel in sorted(outputs):
        write_file(temp_dir, rel, outputs[rel], manifest)
    # co-op: the copies no listed id could hold, for the host's list (hostonly.py); a root file like the spoiler, so no
    # game file and in no party hash; none: no file
    host_list = hostonly.party_text(hostonly.party_ids(
        {stem: r.get("host_only") for stem, r in event_reports.items() if "event/%s_00_00.emevd.dcx" % stem in outputs}))
    if host_list:
        with open(os.path.join(temp_dir, hostonly.PARTY_FILE), "w", encoding="ascii", newline="\n") as f:
            f.write(host_list)
    feature_files = starter_weapons.clip_files(clip_rows, "Randomizer %s" % code)
    for rel, data in feature_files.items():
        path = os.path.join(temp_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    spoiler = {
        "generator": GENERATOR_MAJOR, "generator_build": build, "code": code, "seed": seed, "options": _plain(result["options"]),
        "input_hash": input_hash, "read_hash": files.read_hash(), "report": result["report"], "npc_rows_added": len(new_rows),
        "effects": effect_report, "boss_events": event_reports, "boss_patches": boss_patches, "pose_patches": pose_patches,
        "released_runs": released_report, "ai_released": ai_report, "riders_off": rider_report, "texture_parts": texture_report, "treasure": {str(a): b for a, b in sorted(lots.items())},
        "starters": {str(a): b for a, b in sorted(starter_plan.items())},
        "starter_copies": {str(a): b for a, b in sorted(starter_copies.items())},
        "starter_armor": outfit,
        "starter_clip_groups": {str(r["weapon"]): r["clip_group"] for r in clip_rows},
        # shop row (first block, index) -> the pickup lot row it traded with
        "shops": {str(first): {str(index): lot for (f, index), lot in sorted(shop_plan.items()) if f == first}
                  for first in sorted({f for f, _ in shop_plan})},
        # consumable row "first:index" -> "first:index" of the row whose item it now sells
        "shop_goods": {"%d:%d" % a: "%d:%d" % b for a, b in sorted(shop_deal.items()) if a != b},
        "drops": {str(a): b for a, b in sorted(drop_plan.items())},
        # gem id -> its effects (GemeffectParam ids), curse, tier and where it is: [pickup or drop, lot row, maps]
        "gems": {str(i): {"effects": g["effects"], "cursed": int(g["cursed"]), "tier": g["tier"],
                          "where": list(gem_plan["where"][i])} for i, g in sorted((gem_plan or {}).get("gems", {}).items())},
        "readable": _readable(files, census, changes, result["bosses"], lots, shop_plan, drop_plan, starter_plan,
                              result, shop_found, shop_deal, gem_plan, gem_lots),
        "slots": {stem: {part: {"from": _from(census, stem, part), "to": changes[stem][part][0],
                                "think": changes[stem][part][1], "row": slot_row[(stem, part)]}
                         for part in sorted(changes[stem])} for stem in sorted(changes)},
    }
    _dump(os.path.join(temp_dir, "randomizer_spoiler.json"), spoiler)
    manifest_path = os.path.join(temp_dir, "randomizer_manifest.json")
    _dump(manifest_path,
          {"generator": GENERATOR_MAJOR, "generator_build": build, "code": code, "input_hash": input_hash,
           "mods": files.mod_names, "base_label": base_label,
           "lobby_label": lobby_label(base_label, code, input_hash, build,
                                      [files.root_input(i)[1] for i in range(len(files.mod_names) + 1)]),
           "game_input": files.root_input(0)[1], "mod_inputs": files.mod_inputs(),
           "zlib": zlib.ZLIB_RUNTIME_VERSION,  # the compressor of every .dcx: another version may pack other bytes
           "inputs": files.read_hashes, "files": manifest,
           # Nightwatch's manifest and feature tables beside dvdroot_ps4 (none for most worlds)
           "nightwatch": {rel: hashlib.sha256(data).hexdigest() for rel, data in sorted(feature_files.items())}})
    install(temp_dir, final_dir)
    return final_dir, spoiler


def folder_name(code, input_hash):
    """The world's folder: 32 characters for any code. The code is in the manifest, not here: it grows with its lists, and
    in the name a long one put the world's files past Windows' 259-character paths (the launcher's Randomizer.FolderName)."""
    return "Randomizer %s %s" % (hashlib.sha256(code.encode("ascii")).hexdigest()[:12], input_hash[:8])


class FolderInUse(RuntimeError):
    """The world's folder could not be replaced (randomizer.py: exit 5)."""


def install(temp_dir, final_dir):
    """Puts the finished world in place of any folder of that name (an older build, another generator's), whole: the old
    folder is moved aside first and deleted only once the new one is in, so a world is never a mix of two builds. When the
    old folder can't be moved (the game has it open), nothing changes: the new build is removed and the error raised."""
    head, name = os.path.split(final_dir)
    old = os.path.join(head, "." + name + ".old")  # a dot folder: the launcher's mod list skips it
    if os.path.exists(old):
        shutil.rmtree(old)
    if not os.path.exists(final_dir):
        os.replace(temp_dir, final_dir)
        return
    try:
        os.replace(final_dir, old)
    except OSError as e:
        shutil.rmtree(temp_dir, ignore_errors=True)
        raise FolderInUse("the world folder %s is in use (close the game) or can't be replaced: %s" % (name, e)) from e
    try:
        os.replace(temp_dir, final_dir)
    except OSError:
        os.replace(old, final_dir)
        shutil.rmtree(temp_dir, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)


def _readable(*args):
    """The viewer's text is a nicety: a base mod with odd text files must not stop the world from being made."""
    try:
        return readable.build(*args)
    except Exception as e:  # noqa: BLE001 - any failure leaves the viewer without names, never the world unmade
        return {"error": "%s: %s" % (type(e).__name__, e)}


def _from(census, stem, part):
    return next(s["model"] for s in census.slots[stem] if s["part"] == part)


def _plain(options):
    return {k: list(v) if isinstance(v, tuple) else v for k, v in options.items()}


def _dump(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")


__all__ = ["generate", "encode", "decode"]
