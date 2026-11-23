"""Pure logic: which slot becomes what. No file access; everything comes from the census values.

Budget: a map's replacement models are chosen first, as a set that fits, then slots draw from the set.
Limits hold for one map and for each group of maps that load together. Bosses are planned first (they get memory before
ordinary enemies), by the arena table (arenas.py)."""
from collections import Counter

from arenas import ARENA_ONLY, ARENAS, BY_KEY, ENEMY_OUT, GROUP, OUT, SOLO, ends_on_death
from helpers import FIGHTS, extras_of, with_stage
from neighbours import connections
from seedcode import Stream, normalize, options_hash

BOSS_ORDER_TRIES = 200


def area_number(stem):
    return int(stem[1:3]) * 100 + int(stem[4:6])


def model_number(model):
    return int(model[1:])


class Limits:
    def __init__(self, options, group_models, group_bytes):
        scale = options["headroom"]
        self.map_models = options["map_models"] * scale // 100
        self.map_bytes = (options["map_mb"] << 20) * scale // 100
        self.group_models = group_models * scale // 100
        self.group_bytes = group_bytes * scale // 100


# {model: models whose textures it draws with} (textures.sources, set by plan): a map that loads the model loads them too
# (generate.texture_parts adds a hidden part of each), so they count against its budget
TEXTURE_SOURCES = {}


def with_sources(models):
    """The models and every model their textures need, transitively (c2712 -> c2711 -> c2710)."""
    out, todo = set(models), list(models)
    while todo:
        for source in TEXTURE_SOURCES.get(todo.pop(), ()):
            if source not in out:
                out.add(source)
                todo.append(source)
    return out


def _fits(models, sizes, limits, max_models, max_bytes):
    models = with_sources(models)
    return len(models) <= max_models and sum(sizes.get(m, 0) for m in models) <= max_bytes


def _room(stem, models, loaded, sizes, limits, membership):
    """Whether `models` fit map `stem` alone and with the models `loaded` in each neighbour group (membership[stem])."""
    if not _fits(models, sizes, limits, limits.map_models, limits.map_bytes):
        return False
    for group in membership[stem]:
        union = set(models)
        for other in group:
            if other != stem:
                union |= loaded[other]
        if not _fits(union, sizes, limits, limits.group_models, limits.group_bytes):
            return False
    return True


def _index(slots):
    """{entity: (stem, slot)} of every slot that names an entity."""
    return {s["entity"]: (stem, s) for stem in sorted(slots) for s in slots[stem] if s.get("entity")}


PHASE = "phase"  # a slot's role when it is a fight's phase body (gamedata.PHASE, arenas.PHASES)


def phase_entities(index, arena_key=None):
    """The phase bodies among the slots (census role "phase"; Nocturne's Coiled Host), of one arena when given, sorted."""
    return sorted(e for e, (_, s) in index.items()
                  if s.get("role") == PHASE and (arena_key is None or s.get("arena") == arena_key))


def _in_play(arena, index, options):
    """Whether a base mod's fight (arenas.Arena option / model) counts: its option is on and its body has the model the
    fight is made for. Every other arena always counts. options: None counts no optional fight."""
    if arena.option is None:
        return True
    body = index.get(arena.bodies[0])
    return bool(options and options.get(arena.option)) and body is not None and body[1]["model"] == arena.model


def arena_units(norm, index):
    """The draws of this world's boss fights: [(arena, mode, [unit])], a unit being the bodies that share one draw.
    SOLO: its body; GROUP: body 1 alone (mix off, the others leave) or one per body (mix on); ARENA_ONLY: the main
    body (normal) or every body, main first (mix); OUT: nothing. Bodies the census lacks are left out. Option coiled_host: the arena's phase bodies
    (phase_entities) draw a boss each too, last, whatever the mode."""
    out = []
    for arena in ARENAS:
        bodies = [e for e in arena.bodies if e in index]
        if arena.kind == OUT or not bodies or not _in_play(arena, index, norm):
            continue
        mix = bool(arena.mix_option and norm.get(arena.mix_option))
        if arena.kind == SOLO:
            units, mode = [bodies[:1]], "swap"
        elif arena.kind == GROUP:
            units, mode = ([[e] for e in bodies], "mix") if mix else ([bodies[:1]], "normal")
        elif arena.kind == ARENA_ONLY:
            if mix:
                units = [[e] for e in sorted(bodies, key=lambda e: e != arena.main)]
            else:
                units = [[arena.main]] if arena.main in index else []
            mode = "mix" if mix else "normal"
        else:
            raise ValueError("arena %s: unknown kind %s" % (arena.key, arena.kind))
        if norm.get("coiled_host"):
            units = units + [[e] for e in phase_entities(index, arena.key)]
        if units:
            out.append((arena, mode, units))
    return out


# The Winter Lantern (option no_winter_lanterns, user 10 Nov): its Frenzy reaches the player from afar (DATA: its bullets
# 256050/256697/256699 and its riding part c2561's 256100 apply SpEffects 5280-5289,
# which build Frenzy through 5290-5299, registCurse > 0).
WINTER_LANTERN = "c2560"


def excluded_models(options):
    """The models that never replace an enemy: option exclude, plus the Winter Lantern with no_winter_lanterns."""
    out = {"c%04d" % n for n in options["exclude"]}
    if options["no_winter_lanterns"]:
        out.add(WINTER_LANTERN)
    return out


def replacement_pool(options, candidates, sizes):
    """The models that may replace an ordinary enemy: every candidate with a model file but excluded_models; with option
    include_only (user 11 Nov) only the listed ones. Bosses (boss swaps, bosses as enemies) are not limited by it."""
    only = {"c%04d" % n for n in options["include_only"]}
    return {m for m in candidates if sizes.get(m, 0) > 0 and (not only or m in only)} - excluded_models(options)


def boss_fighters(slots, index, excluded, hard_rows=None, extra=(), phases=(), options=None):
    """The bosses that can fight elsewhere, as fighter dicts (arenas.py contract), in a stable order: the arena table's
    fighters with their slot rows (or their hard copy's) and the bodies that
    travel with them (helpers.extras_of), then the phase bodies `phases` (option coiled_host: "<arena>_phase", with the
    helpers their attacks need: helpers.found_helpers), then `extra` (chalice bosses: home None, origin None)."""
    hard_rows = hard_rows or {}
    out = []
    for arena in ARENAS:
        if not _in_play(arena, index, options):
            continue
        present = [e for e in arena.fighters if e in index]
        for n, entity in enumerate(present):
            slot = index[entity][1]
            if slot["model"] in excluded:
                continue
            think, npc = hard_rows.get(entity, (slot["think"], slot["npc"]))
            extras, extra_models = extras_of(slots, entity)
            # a second phase is "<arena>_2" beside its boss's plain key
            out.append({"key": arena.key if n == 0 else "%s_%d" % (arena.key, n + 1),
                        "model": slot["model"], "think": think, "npc": npc, "origin": entity, "home": arena.key,
                        "extras": list(extras), "extra_models": list(extra_models),
                        # its own death never ends a fight: a body whose death does (helpers.travel or-s it in)
                        "needs_end": entity in FIGHTS and FIGHTS[entity].end_with is not None})
    for entity in phases:
        slot = index[entity][1]
        if slot["model"] not in excluded:
            extras, extra_models = extras_of(slots, entity)  # its helpers (helpers.found_helpers), when it has any
            out.append({"key": "%s_phase" % slot["arena"], "model": slot["model"], "think": slot["think"],
                        "npc": slot["npc"], "origin": entity, "home": slot["arena"], "extras": list(extras),
                        "extra_models": list(extra_models), "needs_end": False})
    out.extend(f for f in extra if f["model"] not in excluded)
    return out


def enemy_bosses(slots, index, excluded, extra=(), huge=frozenset()):
    """Option bosses_as_enemies: the bosses that may stand in an ordinary enemy slot, as fighter dicts with their plain
    slot rows (no hard copy, no second stage) and nothing travelling with them (extras empty): a plain enemy, with none
    of its arena's events, bar, fog, music or rewards. arenas.ENEMY_OUT names those that need their arena. extra: the
    chalice fighters (option chalice_bosses, user 8 Nov); huge: NpcParam rows left out (generate.huge_rows, unless option
    huge_bosses_as_enemies)."""
    return [dict(f, extras=[], extra_models=[], needs_end=False) for f in boss_fighters(slots, index, excluded, extra=extra)
            if f["key"] not in ENEMY_OUT and f["npc"] not in huge]


def boss_enemy_slot(slot):
    """Whether an ordinary slot may hold a boss (option bosses_as_enemies): not a fight's (escorts), not one an option
    forces or marks difficult, and not in its map's boss block (entity xx008xx: the boss fights' bodies, extras and
    pools but the Emissary's small ones, DATA; its only free slots are parts a fight spawns, the Witch's Mad Ones
    2200810-812 and the Shadows' summons 2700803-805), so every boss arena stays as the other options leave it."""
    return (slot["kept"] is None and slot.get("arena") is None and not slot.get("force_change")
            and not slot.get("difficult") and (slot.get("entity") or 0) % 1000 // 100 != 8)


def second_bodies(fighters, placed, loaded, slots, index, sizes, limits, groups, staged, seed, opts_hash):
    """Each boss once per run (user, 10 Nov): the second body of a placed Gascoigne, Ludwig or Orphan holds its own second
    phase only while that phase fights in no arena of its own (and option second_stage is off: `staged`); else a boss
    that fights nowhere yet, drawn from those whose model fits every map the boss went to (option second_stage, user
    5 Nov: never its own fight's while another is free); else, with second_stage on, the least used other boss (only a
    mixed world, which repeats bosses anyway, runs out: plan() fills the pool from the chalice bosses otherwise); else
    nobody: phase one fights alone and its death ends the fight (helpers.ALONE). A stage brings none of its own bodies (no chain). placed: {entity: fighter key}; loaded: {stem:
    models} (_plan_bosses), updated with the stages' models. Returns the fighters with those bosses changed."""
    by_key = {f["key"]: f for f in fighters}
    by_origin = {f["origin"]: f["key"] for f in fighters if f.get("origin") is not None}
    stems = {}
    for entity, key in placed.items():
        stems.setdefault(key, set()).add(index[entity][0])
    parents = [k for k in sorted(stems) if FIGHTS.get(by_key[k]["origin"]) and FIGHTS[by_key[k]["origin"]].stage]
    used = set(stems)
    left = []
    for key in parents:  # first every own second phase that has no arena: it stays with its boss
        own = by_origin.get(FIGHTS[by_key[key]["origin"]].stage)
        if not staged and own is not None and own not in used:
            used.add(own)
        else:
            left.append(key)
    membership = {stem: [g for g in groups if stem in g] for stem in loaded}
    counts = Counter(placed.values())
    staged_keys = set()
    for key in Stream(seed, opts_hash, "second-bodies").shuffled(left):  # who gets the few free bosses first
        fighter = by_key[key]
        own = by_origin.get(FIGHTS[fighter["origin"]].stage)

        def room(f):
            return all(_room(stem, loaded[stem] | {f["model"]}, loaded, sizes, limits, membership)
                       for stem in stems[key])
        free = [f for f in fighters if f["key"] not in used and room(f)]
        others = [f for f in free if f["key"] != own and f["model"] != fighter["model"]]
        # else its own second phase when free (vanilla; Ludwig's is his own model)
        choices = others or [f for f in free if f["key"] == own]
        if not choices and staged:  # every boss fights already (a mixed world): the least used, never a stage twice
            spare = [f for f in fighters if f["key"] not in staged_keys and f["key"] != key
                     and f["model"] != fighter["model"] and room(f)]
            fewest = min((counts[f["key"]] for f in spare), default=None)
            choices = [f for f in spare if counts[f["key"]] == fewest]
        if choices:
            pick = choices[Stream(seed, opts_hash, "second-stage", key).below(len(choices))]
            used.add(pick["key"])
            staged_keys.add(pick["key"])
            if pick["key"] == own:
                continue  # its own second phase, as vanilla
            by_key[key] = with_stage(fighter, pick, slots)
            for stem in stems[key]:
                loaded[stem] = loaded[stem] | {pick["model"]}
        else:
            by_key[key] = dict(fighter, extras=[], extra_models=[], needs_end=False, alone=True)
    return [by_key[f["key"]] for f in fighters]


def _plan_bosses(slots, index, units, fighters, sizes, limits, groups, seed, opts_hash, base):
    """Every unit gets a fighter: never one from its own arena or of its own model; a fighter the arena already holds only
    when no other fits (one boss each in a mix), else the least used first; each must fit its new map's budgets with the models it brings (its extras' too). A mix body that fits none takes
    its arena's first draw. Orders are drawn until every unit is filled, else the fullest order is kept.
    base: {stem: models loaded besides the unit bodies}. Returns ({entity: fighter key}, {stem: loaded models})."""
    stems = sorted(slots)
    membership = {stem: [g for g in groups if stem in g] for stem in stems}
    draw = Stream(seed, opts_hash, "boss-order")
    total = sum(len(u) for _, _, us in units for u in us)
    # a phase body wakes after the fight's start: no boss whose travelling bodies wake on the started flag (the One
    # Reborn's upper body and caster would fight before it exists)
    wakes = {origin for origin, fight in FIGHTS.items() if fight.wake}

    def loaded_models(stem, body_model, brought):
        return base[stem] | {body_model[e] for e in body_stem[stem]} | brought[stem]

    body_stem = {stem: [] for stem in stems}
    for _, _, us in units:
        for unit in us:
            for e in unit:
                body_stem[index[e][0]].append(e)

    def fits(stem, models, loaded):
        return _room(stem, models, loaded, sizes, limits, membership)

    def assign(arena_order, fighter_order):
        body_model = {e: index[e][1]["model"] for stem in stems for e in body_stem[stem]}
        brought = {stem: set() for stem in stems}
        loaded = {stem: loaded_models(stem, body_model, brought) for stem in stems}
        used = {f["key"]: 0 for f in fighter_order}
        rank = {f["key"]: n for n, f in enumerate(fighter_order)}
        placed = {}
        for arena, mode, us in arena_order:
            first = None
            here = set()
            for unit in us:
                stem = index[unit[0]][0]
                own = {index[e][1]["model"] for e in unit}
                ends = all(ends_on_death(e) for e in unit)
                phase = any(index[e][1].get("role") == PHASE for e in unit)

                def allowed(f):
                    return (f["home"] != arena.key and f["model"] not in own and (ends or not f.get("needs_end"))
                            and not (phase and f["origin"] in wakes))
                choices = [f for f in fighter_order if allowed(f)]
                # one boss each in a mixed or split fight (user, 4 Nov): a fighter already in this arena only when no other
                # fits; then the least used, so every fighter fights before any fights twice
                choices.sort(key=lambda f: (f["key"] in here, used[f["key"]], rank[f["key"]]))
                chosen = None
                for fighter in choices:
                    trial_body = dict(body_model)
                    for e in unit:
                        trial_body[e] = fighter["model"]
                    trial_brought = dict(brought)
                    trial_brought[stem] = brought[stem] | set(fighter["extra_models"])
                    models = loaded_models(stem, trial_body, trial_brought)
                    if fits(stem, models, loaded):
                        chosen = fighter
                        break
                if chosen is None and mode == "mix" and first is not None and allowed(first):
                    chosen = first  # its model is already loaded here: the shared draw costs no room
                if chosen is None:
                    continue
                for e in unit:
                    body_model[e] = chosen["model"]
                    placed[e] = chosen["key"]
                brought[stem] |= set(chosen["extra_models"])
                loaded[stem] = loaded_models(stem, body_model, brought)
                used[chosen["key"]] += 1
                here.add(chosen["key"])
                first = first or chosen
        return placed, loaded

    best = None
    for _ in range(BOSS_ORDER_TRIES):
        arena_order = draw.shuffled(units)
        fighter_order = draw.shuffled(fighters)
        placed, loaded = assign(arena_order, fighter_order)
        score = (len(placed), len(set(placed.values())))
        if best is None or score > best[0]:
            best = (score, placed, loaded)
        if len(placed) == total:
            break
    return best[1], best[2]


def escort_slot(slot, mode):
    """An escort slot (One Reborn's bell maidens) for option bell_maidens: 0 kept, 1 always replaced from the normal pool,
    2 always replaced from the difficult pool (`difficult` of plan()). Returns a new slot dict for plan()'s `slots`."""
    out = dict(slot)
    if mode == 0:
        out["kept"] = out["kept"] or "escort"
        return out
    out["kept"] = None
    out["force_change"] = True
    if mode == 2:
        out["difficult"] = True
    return out


def posed_slot(slot):
    """A slot for option free_poses (user 6 Nov): its poses (census "poses": the waiting pose and getting up from it) no
    longer needed, so any model may take it; generate.py turns off the ones its model lacks. Returns a new slot dict."""
    if not slot.get("poses"):
        return slot
    return dict(slot, needs=tuple(a for a in slot["needs"] if a not in slot["poses"]))


def away_slot(slot, outside, animations):
    """A slot for option away_from_home without free_poses (user 7 Nov: a map's own kinds never come back): a posed slot
    whose animations no model from outside its map has gets posed_slot, so an outside model takes it and stands.
    outside: the pool's models that are not this map's kinds."""
    if not slot.get("poses") or any(all(a in animations.get(m, ()) for a in slot["needs"]) for m in outside):
        return slot
    return posed_slot(slot)


def plan(slots, candidates, sizes, groups, group_limits, seed, options, animations=None, difficult=None,
         extra_fighters=(), hard_rows=None, connected=None, huge=frozenset(), texture_sources=None):
    """slots: {stem: [slot dict]}; candidates: {model: [(think, npc)]}; sizes: {model: bytes};
    groups: [[stem...]] loaded together; group_limits: (max models, max bytes);
    animations: {model: ids}, for slots whose events force animations (LIVE: a model lacking one freezes);
    difficult: {model: [(think, npc)]} (difficult.candidates): with option difficult_enemies every replaced ordinary slot,
    and every slot marked "difficult" (escort_slot), draws from it where it fits the map's budget, else the normal draw;
    extra_fighters: fighter dicts that join the boss pool (chalice.fighters(), with chalice_bosses);
    hard_rows: {boss entity: (think, npc)} that boss brings wherever it fights, its own arena too when it stays (D12);
    huge: NpcParam rows of bosses too wide for an ordinary slot (generate.huge_rows; option huge_bosses_as_enemies lets them);
    connected: {stem: stems} the player meets right after it (None: neighbours.connections(); {} turns the rule off): a
    map's model order puts the models those maps already hold last (user, 4 Nov: the next map holds fewer of the last's).
    texture_sources: textures.sources (a model's texture sources count in every budget with it).
    Returns {'changes': {stem: {part: (model, think, npc)}}, 'report': {...}, 'bosses', 'boss_fighters', 'fighters',
    'arena_modes', ...}."""
    TEXTURE_SOURCES.clear()
    TEXTURE_SOURCES.update(texture_sources or {})
    norm = normalize(options)
    index = _index(slots)
    skipped = {}
    if norm["coiled_host"] and not phase_entities(index):
        norm["coiled_host"] = 0  # no Coiled Host in the inputs (user: then the option does nothing): the world of it off
        skipped["coiled_host"] = "no Coiled Host in the inputs (needs Nocturne v2.0.0 or later)"
    if norm["scorched_beast"] and not _in_play(BY_KEY["lesser_demon"], index, norm):
        norm["scorched_beast"] = 0  # no Scorched Beast in the inputs (user: then the option does nothing)
        skipped["scorched_beast"] = "no Scorched Beast in the inputs (needs Nocturne v5.0.0 or later)"
    opts_hash = options_hash(norm)
    area = dict(norm["area_chance"])
    excluded = excluded_models(norm)
    pool = sorted(replacement_pool(norm, candidates, sizes))
    if norm["oops_all"]:
        forced = "c%04d" % norm["oops_all"]
        if forced not in candidates or sizes.get(forced, 0) <= 0:
            raise ValueError("%s is not an enemy the game places with AI of its own" % forced)
        pool = [forced]
    limits = Limits(norm, *group_limits)
    hard_rows = hard_rows or {}

    # The first pass rolls which slots change; the models that stay count against every budget.
    stems = sorted(slots)
    change, kept = {}, {}
    for stem in stems:
        chance = area.get(area_number(stem), norm["enemy_chance"])
        change[stem], kept[stem] = [], set()
        for slot in slots[stem]:
            if slot["kept"] is None and (slot.get("force_change")
                                         or Stream(seed, opts_hash, "enemy-roll", stem, slot["part"]).percent(chance)):
                change[stem].append(slot)
            else:
                kept[stem].add(slot["model"])

    report = {"maps": {}, "stayed_no_room": 0, "stayed_no_animation": 0, "bosses": {}, "arenas": {}}
    if skipped:
        report["skipped_options"] = skipped  # asked for, but nothing in the inputs to act on (the spoiler says so)
    # option away_from_home (user, 6 Nov): a map's own vanilla kinds are not drawn for its replaced slots, except where
    # nothing else serves a slot's forced animations or nothing else fits the map at all; report["home_kinds"] counts those
    away = bool(norm["away_from_home"])
    native = {stem: {s["model"] for s in slots[stem]} for stem in stems} if away else {}
    if away:
        report["home_kinds"] = 0
    units = arena_units(norm, index) if norm["boss_swap"] else []
    chalice_pool = [f for f in extra_fighters if sizes.get(f["model"], 0) > 0]
    fighters = boss_fighters(slots, index, excluded, hard_rows, chalice_pool if norm["chalice_bosses"] else (),
                             phase_entities(index) if norm["coiled_host"] else (), norm)
    if units and norm["second_stage"] and not norm["chalice_bosses"]:
        # user, 10 Nov: with second stages the arenas and the three stages want more bosses than travel (the Emissary and
        # the Failures never leave their arena): chalice bosses fill the gap, as many as it takes, when that is enough
        stages = sum(1 for f in fighters if FIGHTS.get(f["origin"]) is not None and FIGHTS[f["origin"]].stage)
        need = sum(len(u) > 0 for _, _, us in units for u in us) + stages - len(fighters)
        if 0 < need <= len(chalice_pool):
            fill = Stream(seed, opts_hash, "chalice-fill").shuffled([f for f in chalice_pool if f["model"] not in excluded])[:need]
            fighters = fighters + sorted(fill, key=lambda f: f["key"])
            report["chalice_fill"] = sorted(f["key"] for f in fill)
    placed = {}
    if units:
        unit_bodies = {e for _, _, us in units for u in us for e in u}
        # a rolled slot with forced animations holds its own model until a pick serves it (as `chosen` below does), so
        # the bosses' budget counts it too: else bosses fill the map and those models push it over
        rolled = {id(s) for stem in stems for s in change[stem] if not s.get("needs")}
        base = {stem: {s["model"] for s in slots[stem] if s.get("entity") not in unit_bodies and id(s) not in rolled}
                for stem in stems}
        placed, loaded = _plan_bosses(slots, index, units, fighters, sizes, limits, groups, seed, opts_hash, base)
        fighters = second_bodies(fighters, placed, loaded, slots, index, sizes, limits, groups, norm["second_stage"],
                                 seed, opts_hash)
        kept.update(loaded)
    by_key = {f["key"]: f for f in fighters}
    # which boss each travelling second phase became, and who fights alone, for the spoiler
    report["stages"] = {k: by_key[k]["stage"]["key"] for k in sorted(set(placed.values())) if by_key[k].get("stage")}
    report["alone"] = sorted(k for k in set(placed.values()) if by_key[k].get("alone"))
    arena_modes = {}
    for arena, mode, us in units:
        bodies = {e: placed[e] for u in us for e in u if e in placed}
        if set(bodies) - set(phase_entities(index)):  # a boss in the phase body alone leaves the fight's bodies as they are
            arena_modes[arena.key] = mode
        report["arenas"][arena.key] = {"mode": mode, "bodies": {str(e): k for e, k in sorted(bodies.items())},
                                       "left": sorted(str(e) for u in us for e in u if e not in placed)}
    boss_rows = {}
    for entity, key in placed.items():
        stem, slot = index[entity]
        f = by_key[key]
        boss_rows[entity] = (stem, slot["part"], (f["model"], f["think"], f["npc"]))
        report["bosses"][slot["part"]] = f["model"]
    # D12 with boss swap off: a hard copy fights in its own arena
    hard_bodies = {}
    if not norm["boss_swap"]:
        for entity, (think, npc) in sorted(hard_rows.items()):
            if entity in index:
                stem, slot = index[entity]
                hard_bodies[entity] = (stem, slot["part"], (slot["model"], think, npc))

    # A slot with forced animations may find no fitting pick and stay, so its own model holds room until then.
    chosen = {stem: kept[stem] | {s["model"] for s in change[stem] if s.get("needs")} for stem in stems}
    membership = {stem: [g for g in groups if stem in g] for stem in stems}
    animations = animations or {}

    def fits_map(stem, trial):
        if not _fits(trial, sizes, limits, limits.map_models, limits.map_bytes):
            return False
        for group in membership[stem]:
            union = set(trial)
            for other in group:
                if other != stem:
                    union |= chosen[other]
            if not _fits(union, sizes, limits, limits.group_models, limits.group_bytes):
                return False
        return True

    # difficult enemies (user, 4 Nov): their models are picked for a map first, slots draw from them where one fits
    hard_all = bool(norm["difficult_enemies"])
    hard_pool = {m: [r for r in rows if r in candidates.get(m, ())] for m, rows in (difficult or {}).items() if m in pool}
    hard_pool = {m: rows for m, rows in hard_pool.items() if rows}
    if hard_all or any(s.get("difficult") for v in slots.values() for s in v):
        report["difficult"] = 0
    # bosses as enemies: {key: fighter}; none when one model fills every slot (oops_all)
    enemy_fighters = {}
    if norm["bosses_as_enemies"] and not norm["oops_all"]:
        chalice_extra = [f for f in extra_fighters if sizes.get(f["model"], 0) > 0] if norm["chalice_bosses"] else []
        enemy_fighters = {f["key"]: f for f in enemy_bosses(slots, index, excluded, chalice_extra,
                                                             frozenset() if norm["huge_bosses_as_enemies"] else huge)}
    boss_enemies = {}  # {stem: {part: fighter key}}
    if enemy_fighters:
        report["boss_enemies"] = boss_enemies
    changes = {}
    links = connections() if connected is None else connected
    planned = set()
    for stem in stems:
        changes[stem] = {}
        # how many connected maps hold each model: those planned already as they ended, the later ones what they keep
        # (maps go in stem order, so each connected pair is kept apart by its later map)
        near = {}
        for other in sorted(links.get(stem, ())):
            for model in (chosen[other] if other in planned else kept.get(other, ())):
                near[model] = near.get(model, 0) + 1
        # kinds this map takes: option variety when set, else its vanilla count of free kinds scaled by the headroom
        # (user, 6 Nov: 100 % = the game's own count, 200 % = twice it; "as many as fit" put every kind in every
        # world, R2-4NQJW and R2-4QRQZ: 29-40 kinds a map against vanilla's 5-15, all 88 kinds in both)
        own_kinds = len({s["model"] for s in slots[stem] if s["kept"] is None})
        wanted = norm["variety"] or max(1, (own_kinds * norm["headroom"] + 50) // 100)
        # bosses as enemies: first, so the map's other models are picked around the bosses' room. A map rolls option
        # boss_enemy_chance once; on a hit it takes 1 to boss_enemy_max bosses, each a different one in its own slot
        rolled_here = change[stem]
        if (enemy_fighters and rolled_here
                and Stream(seed, opts_hash, "boss-enemy", stem).percent(norm["boss_enemy_chance"])):
            boss_draw = Stream(seed, opts_hash, "boss-enemy-pick", stem)
            keys = boss_draw.shuffled(sorted(enemy_fighters))
            order = boss_draw.shuffled([s for s in rolled_here if boss_enemy_slot(s)])
            top = norm["boss_enemy_max"]
            count = 1 + boss_draw.below(top) if top > 1 else 1
            here_bosses = {}
            for slot in order:
                if len(here_bosses) >= count:
                    break
                for key in keys:
                    f = enemy_fighters[key]
                    if (key in here_bosses.values() or away and f["model"] in native[stem]
                            or any(a not in animations.get(f["model"], ()) for a in slot.get("needs", ()))):
                        continue
                    if fits_map(stem, chosen[stem] | {f["model"]}):
                        changes[stem][slot["part"]] = (f["model"], f["think"], f["npc"])
                        here_bosses[slot["part"]] = key
                        chosen[stem].add(f["model"])
                        break
            if here_bosses:
                boss_enemies[stem] = here_bosses
                rolled_here = [s for s in rolled_here if s["part"] not in here_bosses]
        hard_here = hard_pool if (hard_all or any(s.get("difficult") for s in rolled_here)) and rolled_here else {}
        needs_count, needs_hard = {}, {}  # the sets of forced animations this map's slots need; drawn difficult?
        for slot in rolled_here:
            if slot.get("needs"):
                key = tuple(sorted(slot["needs"]))
                needs_count[key] = needs_count.get(key, 0) + 1
                needs_hard[key] = needs_hard.get(key, False) or hard_all or bool(slot.get("difficult"))

        def has(model, needs):
            return all(a in animations.get(model, ()) for a in needs)

        # a set is served by a difficult model where its slots draw difficult and a difficult model has it, else by any
        hard_need = {k: needs_hard[k] and any(has(m, k) for m in hard_here) for k in needs_count}

        def serves(model, needs):
            return has(model, needs) and (model in hard_here or not hard_need[needs])

        def pick(key):
            """The models this map takes, by the model order `key` sorts into: every model that fits, in order; then
            each set of forced animations no pick serves (most slots first) gets the first model in order that serves
            it, added if it fits, else in place of the latest pick no other set needs, so the count never drops and
            no slot stays vanilla, or draws a normal model, for want of an animation that a model that fits has."""
            order = []
            if hard_here:
                hard_stream = Stream(seed, opts_hash, "enemy-models-difficult", stem)
                order = hard_stream.shuffled(sorted(hard_here))  # neighbours play no part (user, 7 Nov)
            stream = Stream(seed, opts_hash, "enemy-models", stem)
            order += [m for m in sorted(stream.shuffled(pool), key=key) if m not in hard_here]
            home = native.get(stem, set())
            if home:  # away_from_home: the map's own kinds last, for forced animations only (below)
                order = [m for m in order if m not in home] + [m for m in order if m in home]
            base = set(chosen[stem])
            picked = []
            for model in order:
                if len(picked) >= wanted:
                    break
                if model in home:
                    continue
                if fits_map(stem, base | set(picked) | {model}):
                    picked.append(model)
            if home and not picked:  # nothing else fits this map: its own kinds rather than every slot vanilla
                for model in order:
                    if len(picked) >= wanted:
                        break
                    if fits_map(stem, base | set(picked) | {model}):
                        picked.append(model)
            for needs in sorted(needs_count, key=lambda k: (-needs_count[k], k)):
                if any(serves(m, needs) for m in picked):
                    continue
                for model in order:
                    if model in picked or not serves(model, needs):
                        continue
                    if len(picked) < wanted and fits_map(stem, base | set(picked) | {model}):
                        picked.append(model)
                        break
                    served = [k for k in needs_count if any(serves(m, k) for m in picked)]
                    spare = [m for m in reversed(picked)
                             if all(any(serves(o, k) for o in picked if o != m) for k in served if serves(m, k))]
                    out = next((m for m in spare if fits_map(stem, base | set(picked) - {m} | {model})), None)
                    if out is not None:
                        picked[picked.index(out)] = model
                        break
            return picked

        # the seed's shuffle, with the models of connected maps last (the fewest such maps first); where that leaves this
        # map fewer models (a loaded-together group's budget binds sooner when its maps share less), the shuffle alone.
        # No "least used so far" key (user, 6 Nov: "true, full randomness"; it steered every world to one even spread)
        new_models = pick(lambda m: near.get(m, 0))
        if near:
            plain = pick(lambda m: 0)
            if len(plain) > len(new_models):
                new_models = plain
                report["neighbour_fallback"] = report.get("neighbour_fallback", 0) + 1
        chosen[stem] |= set(new_models)
        picks = sorted(new_models)
        hard_picks = [m for m in picks if m in hard_here]
        n_hard = 0
        here = {}
        stayed = set(kept[stem])
        for slot in rolled_here:
            if not picks:
                report["stayed_no_room"] += 1
                stayed.add(slot["model"])
                continue
            needs = slot.get("needs", ())
            fitting = [m for m in picks if all(a in animations.get(m, ()) for a in needs)] if needs else picks
            if not fitting:
                report["stayed_no_animation"] += 1
                stayed.add(slot["model"])
                continue
            rows = candidates
            if hard_all or slot.get("difficult"):
                hard_fitting = [m for m in fitting if m in hard_picks]
                if hard_fitting:  # else the normal draw (user: where nothing difficult fits)
                    fitting, rows = hard_fitting, hard_here
            elif hard_picks:  # only escorts asked for difficult models here: the other slots keep the normal mix
                fitting = [m for m in fitting if m not in hard_picks] or fitting
            if away:
                fitting = [m for m in fitting if m not in native[stem]] or fitting
            draw = Stream(seed, opts_hash, "enemy-slot", stem, slot["part"])
            # the model this map has used least among the fitting ones; a tie is drawn
            least = min(here.get(m, 0) for m in fitting)
            fitting = [m for m in fitting if here.get(m, 0) == least]
            model = fitting[draw.below(len(fitting))]
            here[model] = here.get(model, 0) + 1
            if away and model in native[stem]:
                report["home_kinds"] += 1
            think, npc = rows[model][draw.below(len(rows[model]))]
            if rows is hard_here:
                n_hard += 1
            changes[stem][slot["part"]] = (model, think, npc)
        # a picked model no slot drew, or a held model no slot kept, costs nothing: drop it before the next map counts it
        chosen[stem] = stayed | {c[0] for c in changes[stem].values()}
        planned.add(stem)
        for boss_stem, part, row in list(boss_rows.values()) + list(hard_bodies.values()):
            if boss_stem == stem:
                changes[stem][part] = row
        if hard_here:
            report["difficult"] += n_hard
        report["maps"][stem] = {"slots": len(slots[stem]), "changed": len(changes[stem]),
                                "models": sorted(chosen[stem]),
                                "mb": sum(sizes[m] for m in chosen[stem]) >> 20}
        if hard_here:
            report["maps"][stem]["difficult"] = n_hard
    return {"changes": changes, "report": report, "options": norm, "options_hash": opts_hash, "chosen": chosen,
            "bosses": {entity: (stem, part) for entity, (stem, part, _) in boss_rows.items()},
            "boss_fighters": dict(placed), "fighters": {f["key"]: f for f in fighters if f["key"] in set(placed.values())},
            "arena_modes": arena_modes,
            "hard_bodies": {entity: (stem, part) for entity, (stem, part, _) in hard_bodies.items()},
            # bosses as enemies: {stem: {part: fighter key}} and those fighters
            "boss_enemies": boss_enemies,
            "enemy_fighters": {k: enemy_fighters[k] for parts in boss_enemies.values() for k in parts.values()}}


def check(result, slots, candidates, sizes, groups, group_limits, animations=None):
    """Everything a plan must satisfy; returns a list of problems (empty = passes)."""
    problems = []
    norm = result["options"]
    limits = Limits(norm, *group_limits)
    excluded = excluded_models(norm)
    index = _index(slots)
    bosses = result.get("bosses", {})
    boss_parts = {(stem, part): entity for entity, (stem, part) in bosses.items()}
    hard_parts = {(stem, part): entity for entity, (stem, part) in result.get("hard_bodies", {}).items()}
    fighters = result.get("fighters", {})
    units = {e for _, _, us in arena_units(norm, index) for u in us for e in u} if norm["boss_swap"] else set()
    for stem, changes in result["changes"].items():
        by_part = {s["part"]: s for s in slots[stem]}
        for part, (model, think, npc) in changes.items():
            slot = by_part.get(part)
            if slot is None:
                problems.append("%s %s is no slot" % (stem, part))
                continue
            entity = boss_parts.get((stem, part))
            if entity is not None:
                f = fighters.get(result["boss_fighters"].get(entity))
                if entity not in units:
                    problems.append("%s %s: %d is no boss body of this world's modes" % (stem, part, entity))
                elif f is None or (model, think, npc) != (f["model"], f["think"], f["npc"]):
                    problems.append("%s %s: %s is not its fighter's row" % (stem, part, model))
                elif f["home"] == slot.get("arena") or model == slot["model"]:
                    problems.append("%s %s: %s is the arena's own boss" % (stem, part, model))
                elif f["origin"] is not None and f["origin"] not in index:
                    problems.append("%s %s: fighter %s comes from nowhere" % (stem, part, f["key"]))
                continue
            key = result.get("boss_enemies", {}).get(stem, {}).get(part)
            if key is not None:
                f = result["enemy_fighters"].get(key)
                if not norm["bosses_as_enemies"] or f is None or (model, think, npc) != (f["model"], f["think"], f["npc"]):
                    problems.append("%s %s: %s is not a boss enemy of this world" % (stem, part, model))
                elif not boss_enemy_slot(slot) or key in ENEMY_OUT:
                    problems.append("%s %s: no slot for a boss enemy" % (stem, part))
                elif model in excluded:
                    problems.append("%s %s: excluded model %s" % (stem, part, model))
                elif any(a not in (animations or {}).get(model, ()) for a in slot.get("needs", ())):
                    problems.append("%s %s: %s lacks a forced animation" % (stem, part, model))
                continue
            if (stem, part) in hard_parts:
                if model != slot["model"] or norm["boss_swap"]:
                    problems.append("%s %s: a hard copy changed its model" % (stem, part))
                continue
            if slot["kept"] is not None:
                problems.append("%s %s is not a free slot" % (stem, part))
            if (think, npc) not in candidates.get(model, ()):
                problems.append("%s %s: %s has no row %d/%d" % (stem, part, model, think, npc))
            if model in excluded:
                problems.append("%s %s: excluded model %s" % (stem, part, model))
            if norm["include_only"] and int(model[1:]) not in norm["include_only"] and not norm["oops_all"]:
                problems.append("%s %s: %s is not in include_only" % (stem, part, model))
            if any(a not in (animations or {}).get(model, ()) for a in slot.get("needs", ())):
                problems.append("%s %s: %s lacks a forced animation" % (stem, part, model))
        loaded = result["chosen"][stem]
        new = {c[0] for c in changes.values()}
        vanilla = {s["model"] for s in slots[stem]}
        brought = set()
        for entity, (bstem, _) in bosses.items():
            if bstem == stem and result["boss_fighters"].get(entity) in fighters:
                brought |= set(fighters[result["boss_fighters"][entity]]["extra_models"])
        if not (new | brought) <= loaded:
            problems.append("%s: models in use that its budget never counted" % stem)
        if (new | brought) - vanilla:  # a map that gained nothing keeps vanilla's own numbers
            if len(loaded) > limits.map_models or sum(sizes[m] for m in loaded) > limits.map_bytes:
                problems.append("%s over the map budget" % stem)
    for group in groups:
        union = set()
        for stem in group:
            union |= result["chosen"][stem]
        gained = any(result["chosen"][stem] - {s["model"] for s in slots[stem]} for stem in group)
        if gained and (len(union) > limits.group_models or sum(sizes[m] for m in union) > limits.group_bytes):
            problems.append("group %s over budget" % ",".join(group))
    return problems
