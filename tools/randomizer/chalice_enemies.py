"""Chalice Dungeon enemies in the main maps' enemy pool (option chalice_enemies; PLAN 14 "Include chalice enemies").

Nothing here is a hand list: the kinds and rows come from the chalice maps the player's game holds, read only when the
option is on (they then count as inputs of the world). The chalice maps are never written.

What is read (DATA, 8 Nov 2026 survey of the vanilla game):
- A chalice map is map/mapstudio/m29_AA_BB_00/m29_AA_BB_xx.msb.dcx; it belongs to chalice goods id 6000 + AA * 10 + B
  (B = the first digit of BB; survey 3). A map is real when EquipParamGoods has that row: the 25 dungeons a chalice item
  makes. The others (6000-6090: m29_00 to m29_09) are test layouts no item opens.
- Each map has its own event file event/m29_AA_BB_00/m29_AA_BB_xx.emevd.dcx (8 of 2,318 real maps have none and are
  skipped: what their events do to an enemy is unknown) and shares the templates of event/m29.emevd.dcx.
- A boss is an enemy whose entity a health bar (2003[11]) names, written in the instruction or passed to a template.

Who joins (each a placement of a real chalice map, then its NpcParam/NpcThinkParam row pair, then its model):
- placements left out: a boss (a bar names it), one with a talk script (an NPC), one in a scripted pose (a start
  animation other than -1 or 0, the idle every model has), one an event forces an animation on (a scripted entrance).
- rows left out, by the main census' own rules (gamedata): no AI (think 0/1 or a missing row), an NPC think (6000-6999),
  HP from gamedata.SPECIAL_HP up, a battle or logic script found nowhere (main maps, aicommon, the chalice archive), a
  team the main pool never uses (the pool's rows are teams 23-25: hostile; others are LIKELY friends or summons).
- models left out: the human model (hunters: NPCs or the player's), dummies (c9xxx), the turret models and the models
  that do not fight away from home (gamedata.TURRET_MODELS, HOME_ONLY_MODELS), a main game boss model, a boss kind (a
  model whose bar placements outnumber its other placements: the chalice bosses; the regular kinds with a boss variant
  have far more regular placements, the nearest call being 804 bars to 1,335 others), an invisible or missing model
  (gamedata.INVISIBLE_BYTES), and every kind the main maps already place as an ordinary enemy (the option adds kinds; it
  leaves the main kinds' own rows as they are).
- rows kept: of each model and think, the rows with the weakest layer effect (NpcParam spEffectID6, -1 < main area
  effects 70xx < chalice layers 7300 < 7310 < 7320). With the default slot scaling the slot's own effect replaces it
  anyway; with "the replacement's own strength" a chalice enemy fights at its shallowest layer."""
import struct
from concurrent.futures import ThreadPoolExecutor

import chalice
import gamedata
from containers import Param, bnd4_find, bnd4_read, dcx_read, dcx_size
from msb import ENEMY, MODELS, PARTS, sections, _i64, _utf16z

MAPS = "map/mapstudio"
SHARED_EVENTS = "event/m29.emevd.dcx"
CUP_BASE = 6000
BOSS_BAR = (2003, 11)  # [shown, entity, bar slot, name text id]
CALLS = (0, 6)  # 2000[0] and 2000[6] start an event with arguments
PLAIN_ANIMS = (-1, 0)
NPC_THINK = (6000, 7000)


def cup(name):
    """The chalice goods id of a chalice map file name m29_AA_BB_xx."""
    return CUP_BASE + int(name[4:6]) * 10 + int(name[7])


def maps(files):
    """[(folder, stem)] of every real chalice map that has its own event file, sorted."""
    bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
    goods = Param(bnd4_find(bnd, "\\EquipParamGoods.param")[3]).first()
    out = []
    for folder in files.listdir(MAPS):
        if not folder.startswith("m29_") or "." in folder:
            continue
        for name in files.listdir("%s/%s" % (MAPS, folder)):
            if not name.endswith(".msb.dcx") or cup(name) not in goods:
                continue
            stem = name[:-len(".msb.dcx")]
            if files.path("event/%s/%s.emevd.dcx" % (folder, stem)):
                out.append((folder, stem))
    return out


def _templates(events):
    """{event: ([source offsets of a bar's entity], [(source offset of a forced entity, of its animation or None, the
    literal animation)])} of the events that take those from their arguments."""
    out = {}
    for event_id, (instructions, params) in events.items():
        bars, anims = [], []
        for i, (bank, iid, words) in enumerate(instructions):
            fed = params.get(i, {})
            if (bank, iid) == BOSS_BAR and 4 in fed:
                bars.append(fed[4])
            elif (bank, iid) == gamedata.FORCED_ANIM and 0 in fed:
                anims.append((fed[0], fed.get(4), words[1] if len(words) > 1 and 4 not in fed else None))
        if bars or anims:
            out[event_id] = (bars, anims)
    return out


def _note(forced, entity, animation):
    """forced[entity] gains animation, in a new set (a map's dict starts as a copy of the shared one)."""
    forced[entity] = forced.get(entity, frozenset()) | {animation}


def _literal(events, bars, forced):
    """Adds the bars and forced animations events write in their own words."""
    for instructions, params in events.values():
        for i, (bank, iid, words) in enumerate(instructions):
            fed = params.get(i, {})
            if (bank, iid) == BOSS_BAR and 4 not in fed and len(words) > 1:
                bars.add(words[1])
            elif (bank, iid) == gamedata.FORCED_ANIM and 0 not in fed and words:
                _note(forced, words[0], words[1] if len(words) > 1 and 4 not in fed else None)


def _calls(events):
    """[words] of every 2000[0] / 2000[6] these events hold (an event started with arguments)."""
    return [words for instructions, _ in events.values() for bank, iid, words in instructions
            if bank == 2000 and iid in CALLS and len(words) >= 2]


def _runs(calls, known, templates, bars, forced):
    """Adds the bars and forced animations of the template runs these calls start. known: every event id the map has."""
    for words in calls:
        target, skip = (words[1], 2) if words[1] in known else (words[0], 1)
        if target not in templates:
            continue
        word = lambda offset: words[skip + offset // 4] if skip + offset // 4 < len(words) else None
        bar_offsets, anim_offsets = templates[target]
        for offset in bar_offsets:
            bars.add(word(offset))
        for entity_at, anim_at, literal in anim_offsets:
            _note(forced, word(entity_at), literal if anim_at is None else word(anim_at))


class _Shared:
    """What the shared event file does on its own (the same in every chalice map)."""

    def __init__(self, files):
        self.events = gamedata._events(dcx_read(files.read(SHARED_EVENTS))[0])
        self.templates = _templates(self.events)
        self.calls = _calls(self.events)
        self.bars, self.forced = set(), {}
        _literal(self.events, self.bars, self.forced)
        _runs(self.calls, set(self.events), self.templates, self.bars, self.forced)


_PART = struct.Struct("<i4xi")  # part record +0x14: type, +0x1c: model index (msb.py)
_ENEMY = struct.Struct("<iii4xi")  # enemy type data +8: think, NPC row, talk, (+0x14 skipped), start animation


def enemies(data):
    """Msb.enemies() of a map payload, only the fields read here and faster (a chalice map has ~280 enemies, 2,310 maps)."""
    found = sections(data)
    models = [_utf16z(data, at + _i64(data, at)) for at in found[MODELS][2][:-1]]
    out = []
    for at in found[PARTS][2][:-1]:
        kind, model = _PART.unpack_from(data, at + 0x14)
        if kind != ENEMY:
            continue
        base, typed = struct.unpack_from("<qq", data, at + 0xb0)
        think, npc, talk, anim = _ENEMY.unpack_from(data, at + typed + 8)
        out.append({"model": models[model], "entity": struct.unpack_from("<i", data, at + base)[0], "think": think,
                    "npc": npc, "talk": talk, "anim": anim})
    return out


def _payloads(files, found, window=16):
    """(MSB, events) payloads of each map in order. Reading and unpacking run ahead on threads (zlib releases the GIL),
    at most `window` maps at once; parsing stays on the caller's thread (pure Python: threads only slow it down)."""
    def load(folder, stem):
        return (dcx_read(files.read("%s/%s/%s.msb.dcx" % (MAPS, folder, stem)))[0],
                dcx_read(files.read("event/%s/%s.emevd.dcx" % (folder, stem)))[0])
    with ThreadPoolExecutor(max_workers=8) as pool:
        pending = [pool.submit(load, *m) for m in found[:window]]
        for k in range(len(found)):
            ready = pending.pop(0).result()
            if k + window < len(found):
                pending.append(pool.submit(load, *found[k + window]))
            yield ready


def _read_map(shared, msb, events):
    """(enemies, bars, forced) of one chalice map: its enemies, and the bars and forced animations of its events and the
    shared ones."""
    found = enemies(msb)
    own = gamedata._events(events)
    bars, forced = set(shared.bars), dict(shared.forced)
    own_templates = _templates(own)
    _literal(own, bars, forced)
    known = set(shared.events) | set(own)
    _runs(_calls(own), known, {**shared.templates, **own_templates}, bars, forced)
    if own_templates:  # the shared events' runs of this map's own templates
        _runs(shared.calls, known, own_templates, bars, forced)
    return found, bars, forced


def placement_reason(e, bars, forced):
    """Why a chalice placement is no enemy for the pool, or None."""
    if e["entity"] > 0 and e["entity"] in bars:
        return "boss"
    if e["talk"] > 0:
        return "npc"
    if e["anim"] not in PLAIN_ANIMS:
        return "posed"
    if e["entity"] > 0 and forced.get(e["entity"]):
        return "scripted"
    return None


def row_reason(census, think, npc, scripts, teams):
    """Why a chalice row pair is no enemy for the pool, or None (the main census' rules, gamedata.Census)."""
    if NPC_THINK[0] <= think < NPC_THINK[1]:
        return "npc"
    if think <= 1 or think not in census.think_rows or npc not in census.npc_rows:
        return "no ai"
    row = census.npc_rows[npc]
    if census.npc_layout.get(row, "hp") >= gamedata.SPECIAL_HP:
        return "special"
    if census.npc_layout.get(row, "teamType") not in teams:
        return "team"
    battle, logic = census.goals(think, npc)
    if "%06d_battle.lua" % battle not in scripts or (logic > 0 and "%06d_logic.lua" % logic not in scripts):
        return "no script"
    return None


def model_reason(census, model, main_bosses):
    if model == gamedata.HUMAN:
        return "human"
    if model.startswith("c9"):
        return "dummy"
    if model in gamedata.TURRET_MODELS:
        return "turret"
    if model in gamedata.HOME_ONLY_MODELS:
        return "home only"
    if model in main_bosses:
        return "main boss"
    if model in census.candidates:
        return "main kind"
    return None


def survey(files, census):
    """{'rows': {model: [(think, npc)]} that join the pool, 'excluded': {model: reason}, 'maps': count, 'placements':
    {model: (bar placements, other placements)}}. Reads every real chalice map, its events, and the chalice AI archive."""
    shared = _Shared(files)
    chalice.archive(census)  # its script names; also a script source once a chalice enemy is placed (generate.script_source)
    scripts = set().union(census.common_scripts, census.chalice_script_files, *census.script_files.values())
    teams = {census.npc_layout.get(census.npc_rows[npc], "teamType")
             for rows in census.candidates.values() for _, npc in rows}
    main_bosses = {s["model"] for slots in census.slots.values() for s in slots if s["role"] in ("body", "extra")}
    found = maps(files)
    tally, plain, boss_rows, why = {}, {}, set(), {}
    for payloads in _payloads(files, found):
        placed, bars, forced = _read_map(shared, *payloads)
        for e in placed:
            key = (e["think"], e["npc"])
            reason = placement_reason(e, bars, forced)
            counts = tally.setdefault(e["model"], [0, 0])
            counts[reason != "boss"] += 1
            if reason == "boss":
                boss_rows.add((e["model"],) + key)
            elif reason is None:
                plain.setdefault(e["model"], set()).add(key)
            else:
                why.setdefault(e["model"], reason)
    rows, excluded = {}, {}
    for model in sorted(set(tally)):
        reason = model_reason(census, model, main_bosses)
        bar_count, other_count = tally[model]
        if reason is None and bar_count > other_count:
            reason = "boss"
        if reason is None:
            head = files.head("chr/%s.chrbnd.dcx" % model, 0x24)
            size = dcx_size(head) if head else 0
            if size < gamedata.INVISIBLE_BYTES:
                reason = "invisible" if size else "no model"
        pairs = set()
        if reason is None:
            pairs = {p for p in plain.get(model, ()) if (model,) + p not in boss_rows}
            row_why = {p: row_reason(census, p[0], p[1], scripts, teams) for p in pairs}
            pairs = {p for p, r in row_why.items() if r is None}
            if not pairs:
                reason = sorted(set(row_why.values()))[0] if row_why else why.get(model, "boss")
        if reason is not None:
            excluded[model] = reason
            continue
        weakest = {}
        for think, npc in pairs:
            strength = census.npc_layout.get(census.npc_rows[npc], "spEffectID6")
            weakest[think] = min(weakest.get(think, strength), strength)
        rows[model] = sorted((think, npc) for think, npc in pairs
                             if census.npc_layout.get(census.npc_rows[npc], "spEffectID6") == weakest[think])
    return {"rows": rows, "excluded": excluded, "maps": len(found),
            "placements": {m: tuple(v) for m, v in sorted(tally.items())}}


def candidates(files, census):
    """{model: [(think, npc)]}: the chalice enemies that join the pool (survey's rows)."""
    return survey(files, census)["rows"]
