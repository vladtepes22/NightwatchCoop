"""Every boss fight of the main maps as data: its bodies, the role each body plays, its fight flags and first-meeting event.
Pure data plus small lookups; no file access. Sources: the boss round 2 surveys of 4 Nov 2026 (bb-modding
mods\\randomizer\\STATUS.md), vanilla MSB, NpcParam and EMEVD. Labels: DATA unless a line says LIKELY.

Kinds (how the planner fills an arena; the user's rules, STATUS.md 4 Nov evening):
- SOLO: one boss body. The fighter is that body. Helpers (extras) belong to the boss: dismissed when another boss takes
  the arena, travelling with the boss when it goes elsewhere (helpers module).
  Gascoigne, Ludwig and the Orphan are SOLO too: one fighter, whose second body is an extra that travels with it
  (helpers.py); the second body is also a fighter of its own elsewhere ("<arena>_2").
- GROUP: Witch, Shadows: mix off = one boss on body 1, the other bodies leave the fight (user, 8 Nov: off is the easy
  form); mix on = one draw per body. The fighter is body 1.
- ARENA_ONLY: Celestial Emissary, Living Failures: never a fighter elsewhere. Normal = one boss on `main`, the other bodies
  leave the fight; mix = a boss on every body in `bodies`, each with that body's own HP.
- OUT: Micolash: the arena keeps its boss, who goes nowhere.

Contract with the planner and the generator (planner.plan result):
- result["bosses"]: {entity: (stem, part)} of every body that got a boss (as before, now per body).
- result["boss_fighters"]: {entity: fighter key} for the same bodies; result["fighters"]: {key: fighter dict}.
- result["arena_modes"]: {arena key: "swap" | "mix" | "normal"} for every arena that changed.
A fighter dict: {"key", "model", "think", "npc", "origin" (entity whose own map events and bar it brings, None for a
chalice boss), "home" (arena key or None), "extras" (entities that travel with it), "extra_models" (their models, counted
against the new map's budget)}."""

SOLO, GROUP, ARENA_ONLY, OUT = "solo", "group", "arena_only", "out"


class Arena:
    def __init__(self, key, stem, kind, bodies, flags, intro, main=None, extras=(), pool=None, mix_option=None,
                 fighters=None, story_start=False, escorts=(), generators=None, ends_on=None, dope_as=None, notes="",
                 option=None, model=None):
        self.key = key
        self.stem = stem
        self.kind = kind
        self.bodies = tuple(bodies)          # boss bodies, body 1 first
        self.flags = tuple(flags)            # (defeat flag, fight-started flag), shared by every body
        self.intro = intro                   # first-meeting event id (its flag on = second attempt), None = no fog start
        self.main = main if main is not None else self.bodies[0]
        self.extras = tuple(extras)          # bodies that belong to the boss but are not slots (helpers, hidden HP pools)
        self.pool = pool                     # a hidden HP pool body that takes the others' damage and carries the bar
        self.mix_option = mix_option         # the seedcode option that turns this fight's mix on
        # entities whose boss is a fighter elsewhere (SOLO: its body; GROUP: body 1; ARENA_ONLY/OUT: none). Gascoigne,
        # Ludwig and the Orphan add their second phase (user, 8 Nov: second phases in the
        # pool): a boss of its own elsewhere, while at home it stays the extra that travels with body 1
        self.fighters = tuple(fighters) if fighters is not None else (
            self.bodies if kind == SOLO else self.bodies[:1] if kind == GROUP else ())
        self.story_start = story_start       # the Hunter's Dream fights start as story steps: no second-attempt start
        self.escorts = tuple(escorts)        # ordinary enemies of the fight (One Reborn's bell maidens)
        self.generators = dict(generators or {})  # {body: the generator that respawns it}
        self.ends_on = ends_on               # a global flag every event of the fight ends on (Ludwig: 9471)
        # bodies vanilla never dopes for co-op (2004[8] SpEffect 7500/7501): the entity whose doping calls in this arena's
        # events they take when they hold a boss (helpers.dope_anchor)
        self.dope_as = dope_as
        self.notes = notes
        # A fight only a base mod adds (Nocturne v5.0.0: the Scorched Beast). option: the seedcode option that turns it on
        # (the fight is neither an arena nor a fighter without it); model: the body's model, so a mod that puts another
        # model on the entity does not count as the fight
        self.option = option
        self.model = model


# The fights with a second body: SOLO, the second body an extra that travels with body 1 (helpers.py). DATA as in the notes.
_GASCOIGNE_NOTES = ("handover 12414807 (body 1 < 34% HP, AI command 40, waits for body 1's message 30 at i13); damage link "
                    "12414802 i35; body 2 parked under body 1; 2410158 is the guest Gascoigne, not in the fight")
_GASCOIGNE_ONE = Arena("gascoigne", "m24_01", SOLO, (2410810,), (12411800, 12414800), 12411802, extras=(2410811,),
                       fighters=(2410810, 2410811),
                       notes=_GASCOIGNE_NOTES + "; one boss: the beast form travels with him (helpers.py)")
_LUDWIG_NOTES = ("handover 13404824 (body 1 < 50%) and 13404825 (body 2 warped in, bar moved); damage link 13404802 i5; "
                 "every Ludwig event ends on global flag 9471; 13404811 (pre-intro) disables 3400800 until the intro flag")
_LUDWIG_ONE = Arena("ludwig", "m34_00", SOLO, (3400800,), (13401800, 13404808), 13401801, extras=(3400801,), ends_on=9471,
                    fighters=(3400800, 3400801),
                    notes=_LUDWIG_NOTES + "; one boss: the Holy Blade form travels with him (helpers.py)")
_ORPHAN_NOTES = ("DATA (helpers survey): 3600800 c4540 phase one; 3600801 c4541 phase two (82 units under the arena, "
                 "gravity off, carries bar 453000, phase one's damage linked to it), handover 13604820 at phase one's half "
                 "HP; 3600803 c4543 bullet dummy commanded by phase two; ends on either body's death. 13604800 is the fog "
                 "event, the started flag is 13604808. 3600802 c4550 (no AI) is not in the fight")
_ORPHAN_ONE = Arena("orphan", "m36_00", SOLO, (3600800,), (13601800, 13604808), 13601801, extras=(3600801, 3600803),
                    fighters=(3600800, 3600801),
                    notes=_ORPHAN_NOTES)


ARENAS = (
    # the 11 of round 1 (ARENA_FLAGS, boss_events): one body each
    Arena("gehrman", "m21_00", SOLO, (2100800,), (12101800, 12104800), 12101802, story_start=True),
    Arena("moon_presence", "m21_00", SOLO, (2100810,), (12101850, 12104850), 12101852, story_start=True),
    Arena("blood_starved_beast", "m23_00", SOLO, (2300800,), (12301800, 12304800), 12301802),
    Arena("paarl", "m23_00", SOLO, (2300810,), (12301700, 12304700), 12301702),
    Arena("amelia", "m24_00", SOLO, (2400800,), (12401800, 12404800), 12401802),
    Arena("cleric_beast", "m24_01", SOLO, (2410800,), (12411700, 12414700), 12411702),
    Arena("ebrietas", "m24_02", SOLO, (2420800,), (12421800, 12424800), 12421802),
    Arena("rom", "m32_00", SOLO, (3200800,), (13201800, 13204800), None, notes="walk-in, no fog event"),
    Arena("amygdala", "m33_00", SOLO, (3300800,), (13301800, 13304800), 13301802),
    Arena("laurence", "m34_00", SOLO, (3400850,), (13401850, 13404858), 13401851,
          notes="13404861 (started just before the intro) holds 3400850 invincible until the intro flag is on"),
    Arena("maria", "m35_00", SOLO, (3500800,), (13501800, 13504808), 13501801),
    # round 2
    Arena("witch", "m22_00", GROUP, (2200800, 2200801), (12201800, 12204800), 12201802, mix_option="witch_mix",
          notes="both immortal until both reach HP 1 together (12204812); body 2 enabled at body 1 HP <= 50% (12204810); "
                "12204811 i2/i7 switch body 1's think to the Witch's; Mad One adds 2200810-812 stay arena parts"),
    _GASCOIGNE_ONE,
    Arena("emissary", "m24_02", ARENA_ONLY, (2420811, 2420711, 2420712, 2420713, 2420716, 2420717, 2420719, 2420720),
          (12421700, 12424700), 12421702, main=2420811, extras=(2420810, 2420750, 2420751), mix_option="emissary_mix",
          generators={e: e + 3000 for e in (2420711, 2420712, 2420713, 2420716, 2420717, 2420719, 2420720)},
          dope_as=2800800,
          notes="2420810 is the real Emissary's small form (immortal, grows into 2420811 by 12424790 at an HP test and an AI "
                "message); the fight ends on 2420811's death only; the 7 small ones respawn from generators 24237xx; "
                "2420750/751 are the big form's tentacles; the set-up event 12424702 dopes m28's 2800800-803 (none of them "
                "in m24_02), no Emissary body (DATA): a boss here takes the calls on 2800800"),
    Arena("logarius", "m25_00", SOLO, (2500800,), (12501800, 12504800), 12501802, extras=(2500801, 2500802),
          notes="DATA (helpers survey): 2500801 c2321 the sword his AI summons (message 100), 2500802 c9010 invisible "
                "bullet owner of his enraged aura (SpEffect 5633); the fight ends on 2500800's death alone"),
    Arena("wet_nurse", "m26_00", SOLO, (2600800,), (12601800, 12604800), 12601802, extras=(2600801,), pool=2600802,
          notes="DATA (helpers survey): 2600800 the body the player fights (immortal, damage linked to the pool), "
                "2600801 the clone (appears on message 20), 2600802 HP pool 3,000 units above the arena carrying bar "
                "551000; the fight ends on the pool's HP 0. A boss on 2600802 would float out of reach: the slot is 800"),
    Arena("micolash", "m26_00", OUT, (2600850,), (12601850, 12604850), 12601852, notes="player model c0000, stays"),
    Arena("shadows", "m27_00", GROUP, (2700800, 2700801, 2700802), (12701800, 12704800), 12701802, mix_option="shadows_mix",
          notes="ends when all three are dead (12701800 i17-19); snakes 2700810-814 (c2121) and snake balls 2700803-805 "
                "(c5033) stay arena parts; intro_event() detection returns the defeat event here: use this table"),
    Arena("one_reborn", "m28_00", SOLO, (2800800,), (12801800, 12804800), 12801802, extras=(2800801, 2800802),
          pool=2800803, escorts=(2800520, 2800522, 2800524, 2800525, 2800527, 2800529),
          notes="2800800 lower body (immortal, damage linked to the pool), 2800801 upper body warped onto it, 2800802 "
                "invisible caster, 2800803 hidden HP pool under the floor carrying bar 507000; the fight ends on the pool's "
                "HP 0. Elsewhere its upper body and caster travel with it (helpers module), the pool stays home"),
    _LUDWIG_ONE,
    Arena("failures", "m35_00", ARENA_ONLY, (3500851, 3500852, 3500853, 3500854), (13501850, 13504858), 13501851,
          main=3500851, pool=3500850, extras=(3500860,), mix_option="failures_mix",
          generators={3500851: 3503814, 3500852: 3503815, 3500853: 3503816, 3500854: 3503817}, dope_as=3500850,
          notes="each Failure respawned by its own generator, only the first exists at load; 3500850 hidden HP pool "
                "(2700 HP) takes their damage, carries bar 403000, its death ends the fight; 3500860 meteor launcher; "
                "13504852 dopes the pool alone (DATA): a boss on a Failure takes the pool's calls"),
    _ORPHAN_ONE,
    # a base mod's fight, last so worlds whose inputs lack it draw exactly as before (planner.arena_units skips bodies the
    # census lacks)
    Arena("lesser_demon", "m23_00", SOLO, (2300820,), (12301820, 12304820), None, extras=(2300821,),
          option="scorched_beast", model="c5500",
          notes="Nocturne v5.0.0 (DATA, its m23 events): walk-in like Rom (12304840 starts the fight when the host is in region "
                "2302820, 12304820); 2300821 c5501 the second head, carried on the body's dummy poly 100 by 12304823, immortal, "
                "its damage linked to the body, killed with him by 12301820; the Cinder Orb and the echoes come from 12301820 "
                "(2003[4], 2003[12]), the arena's, not the body's"),
)

# Option bosses_as_enemies (user, 7 Nov: "bosses can replace enemies"): the fighters (by fighter key) that never stand in an
# ordinary enemy slot, and why. A boss there is a plain enemy: its plain row, none of its arena's events, parts or
# helpers. So out go the bosses whose own moves or body need what only their arena gives. DATA unless marked.
ENEMY_OUT = {
    "moon_presence": "her Doom Gaze keeps the player alive only through her map's event 12104870 (boss_events.BODY_EVENTS): "
                     "without it the gaze kills outright",
    "witch": "two bodies; her map's 12204811 switches body 1's think to the Witch's, so the slot's row is not hers alone",
    "shadows": "three bodies that summon snakes through their arena's event (gamedata.SUMMONS)",
    "logarius": "his sword (2500801) and enraged-aura bullet owner (2500802) are parts of his arena (helpers.py)",
    "wet_nurse": "her clone, HP keeper and teleport regions are her arena's (helpers.py)",
    "one_reborn": "its upper body and caster are separate parts, its HP an arena pool (helpers.py)",
    "orphan_2": "phase two commands its bullet dummy 3600803, a part of its arena (helpers.py)",
    "lesser_demon": "his head is a separate part (helpers.py) and his arena is a walk-in set-up the plain row lacks",
    "blood_starved_beast": "LIKELY like its model's other row (gamedata.HOME_ONLY_MODELS, LIVE 6 Nov): placed away from home "
                           "it sees the player but never attacks",
}

ARENAS_OF_GAME = tuple(a for a in ARENAS if a.option is None)  # the fights the game itself has (tests of the game alone)
BY_KEY = {a.key: a for a in ARENAS}
BY_ENTITY = {e: a for a in ARENAS for e in a.bodies + a.extras + ((a.pool,) if a.pool else ())}
BOSS_BODIES = frozenset(e for a in ARENAS for e in a.bodies)
ESCORTS = frozenset(e for a in ARENAS for e in a.escorts)


# Phase bodies (option coiled_host, user 7 Nov): a body a base mod adds to a fight, woken by the arena's defeat event once
# the table's bodies are dead, whose own death then ends the fight (Nocturne v2.0.0+: the Coiled Host 2700806 after the
# Shadows of Yharnam). Never a fixed table: the census finds them in the player's inputs (gamedata.phase_bodies) and
# registers them here, {entity: arena key}; empty without such a mod.
PHASES = {}


def set_phases(phases):
    """The phase bodies of this world's inputs (gamedata.Census calls it): arena_of knows them from now on."""
    PHASES.clear()
    PHASES.update(phases)


def arena_of(entity):
    arena = BY_ENTITY.get(entity)
    if arena is None and entity in PHASES:
        return BY_KEY[PHASES[entity]]
    return arena


def ends_on_death(entity):
    """Whether the arena's defeat event tests this body's own death or HP (DATA, vanilla defeat events): every body but
    the ARENA_ONLY ones other than a main body without HP pool (the Emissary small ones end nothing, the Failures end on
    their pool). A boss whose second body ends the fight in its stead (the Orphan's phase two) needs such a body."""
    arena = arena_of(entity)
    return arena is not None and (arena.kind != ARENA_ONLY or (entity == arena.main and arena.pool is None))


def fighter_entities():
    """Entities whose boss can fight elsewhere, in a stable order."""
    return [e for a in ARENAS_OF_GAME for e in a.fighters]  # a base mod's fight (option scorched_beast) is none of the game alone
