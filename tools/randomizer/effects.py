"""Enemy effects travel with the enemy. Map effect archives load only in their own map, so an enemy moved elsewhere
fires invisible projectiles. Each map family's archive `frpg_sfxbnd_mNN` gets what its new enemies use and it lacks:
the effect plus every child effect, texture and model it names, the way `tools\\fxclosure.py` follows them."""
import os
import re
import struct
from concurrent.futures import ThreadPoolExecutor

from containers import Layout, Param, bnd4_find, bnd4_read, bnd4_write, dcx_read, dcx_write

FOLDER = "sfx"
RESIDENT = ("commoneffects", "dlceffects", "patcheffects")
# first argument = effect id. 118 = SpawnFFX_Blade, the trail along a weapon on a swing (8 Nov: the Keeper of the Old
# Lords' flaming swings, f000621600, only in frpg_sfxbnd_m29b, were missing away from the chalice); 122 =
# SpawnFFX_BySpEffect1 (the effect plays while the SpEffect is on)
TAE_EFFECTS = frozenset((95, 96, 99, 100, 108, 109, 112, 118, 122))
TAE_SPEFFECTS = frozenset((66, 67, 302, 401))  # AddSpEffect kinds: first argument = SpEffect id
BULLET_EFFECTS = ("sfxId_Bullet", "sfxId_Hit", "sfxId_Flick")
# the SpEffects a bullet or attack hands out (on the target, or the shooter); their look is the SpEffect's vfx row
BULLET_SPEFFECTS = ("spEffectIDForShooter", "spEffectId0", "spEffectId1", "spEffectId2", "spEffectId3", "spEffectId4")
ATTACK_SPEFFECTS = ("spEffectId0", "spEffectId1", "spEffectId2", "spEffectId3", "spEffectId4")
# an SpEffect leads to the next one through these
SPEFFECT_LINKS = ("replaceSpEffectId", "cycleOccurrenceSpEffectId", "atkOccurrenceSpEffectId")
VFX_EFFECTS = ("midstSfxId", "initSfxId", "finishSfxId")
ENTRY = re.compile(r"[sf]0*(\d+)(_[a-z])?\.(tpf|flver|fxr)$")
SMALLEST_NUMBER = 100  # ints below this inside an effect are counts and sizes, not file numbers
ID_BASE = {"fxr": 0, "tpf": 100000, "flver": 200000}


def _archive_rel(name):
    return "%s/frpg_sfxbnd_%s.ffxbnd.dcx" % (FOLDER, name)


def tae_effects(files, model, speffects=None):
    """Effect ids in a model's TAE. A model with no animation file of its own uses its base model's (c1001 uses c1000).
    `speffects`: a set that also gets the SpEffect ids its animations add (their look is a vfx row, not a TAE event)."""
    rel = "chr/%s.anibnd.dcx" % model
    if files.path(rel) is None:
        rel = "chr/c%04d.anibnd.dcx" % (int(model[1:]) // 10 * 10)
        if files.path(rel) is None:
            return set()
    found = set()
    for entry in bnd4_read(dcx_read(files.read(rel))[0])["entries"]:
        if not entry[2].lower().endswith(".tae"):
            continue
        t = entry[3]
        q = lambda at: struct.unpack_from("<q", t, at)[0]
        table, count = q(0x58), q(0x70)
        for k in range(count):
            at = q(table + k * 16 + 8)
            head, events = q(at), struct.unpack_from("<i", t, at + 0x20)[0]
            for e in range(events):
                data = q(head + e * 0x18 + 0x10)
                kind = struct.unpack_from("<i", t, data)[0]
                if kind in TAE_EFFECTS or (speffects is not None and kind in TAE_SPEFFECTS):
                    value = struct.unpack_from("<i", t, q(data + 8))[0]
                    if value > 0:
                        (found if kind in TAE_EFFECTS else speffects).add(value)
    return found


class RowEffects:
    """What an NpcParam row shows beyond its model's TAE, from the params: the effects of every bullet chain its
    behaviour variation fires, and the vfx of every SpEffect it carries (spEffectID0-7), its behaviours, attacks and
    bullets hand out, or its TAE adds. An SpEffect's look is the SpEffectVfxParam row of its `stateInfo` (knowledge
    sfx.md, "Effects that stay on a weapon")."""

    def __init__(self, files):
        bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        layout = lambda name: Layout(bnd4_find(defs, "\\%s.paramdef" % name)[3])
        rows = lambda name: Param(bnd4_find(bnd, "\\%s.param" % name)[3]).first()
        self.behaviour, self.bullet, self.attack = layout("BehaviorParam"), layout("BulletParam"), layout("AtkParam")
        self.speffect, self.vfx = layout("SpEffect"), layout("SpEffectVfx")
        self.bullets, self.attacks = rows("Bullet"), rows("AtkParam_Npc")
        self.speffects, self.vfxs = rows("SpEffectParam"), rows("SpEffectVfxParam")
        self.roots = {}  # variation -> [(refType, refId)]
        for row_id, row in rows("BehaviorParam").items():
            if row_id >= 200000000:
                self.roots.setdefault((row_id - 200000000) // 1000, []).append(
                    (self.behaviour.get(row, "refType"), self.behaviour.get(row, "refId")))
        self._cache = {}

    def _get(self, layout, row, names):
        return [v for v in (layout.get(row, n) for n in names) if v > 0]

    def variation(self, variation):
        """Effect ids of a behaviour variation: its bullets' own effects plus every SpEffect look it can hand out."""
        if variation in self._cache:
            return self._cache[variation]
        found, speffects, attacks, todo, seen = set(), set(), set(), [], set()
        for kind, ref in self.roots.get(variation, ()):
            if kind == 0:
                attacks.add(ref)
            elif kind == 1:
                todo.append(ref)
            elif kind == 2:
                speffects.add(ref)
        while todo:
            b = todo.pop()
            if b in seen or b not in self.bullets:
                continue
            seen.add(b)
            row = self.bullets[b]
            found.update(self._get(self.bullet, row, BULLET_EFFECTS))
            speffects.update(self._get(self.bullet, row, BULLET_SPEFFECTS))
            attacks.add(self.bullet.get(row, "atkId_Bullet"))
            todo.append(self.bullet.get(row, "HitBulletID"))
        for a in attacks:
            if a in self.attacks:
                speffects.update(self._get(self.attack, self.attacks[a], ATTACK_SPEFFECTS))
        found |= self.speffect_effects(speffects)
        self._cache[variation] = found
        return found

    def speffect_effects(self, ids):
        """Effect ids an SpEffect shows (its vfx row's init / midst / finish), following the SpEffects it leads to."""
        found, todo, seen = set(), list(ids), set()
        while todo:
            s = todo.pop()
            if s in seen or s not in self.speffects:
                continue
            seen.add(s)
            row = self.speffects[s]
            state = self.speffect.get(row, "stateInfo")
            if state > 0 and state in self.vfxs:
                found.update(self._get(self.vfx, self.vfxs[state], VFX_EFFECTS))
            todo.extend(self._get(self.speffect, row, SPEFFECT_LINKS))
        return found

    def row(self, layout, row):
        """Effect ids of one NpcParam row (its own SpEffects and its variation)."""
        own = self._get(layout, row, ["spEffectID%d" % k for k in range(8)])
        return self.variation(layout.get(row, "behaviorVariationId")) | self.speffect_effects(own)


def _index(entries):
    """{file name: entry}, {number: [names]}"""
    names, numbers = {}, {}
    for entry in entries:
        base = entry[2].replace("/", "\\").rsplit("\\", 1)[-1].lower()
        m = ENTRY.match(base)
        if m:
            names[base] = entry
            numbers.setdefault(int(m.group(1)), []).append(base)
    return names, numbers


def _closure(start, names, numbers, held):
    """The files of source archive `names` that effect `start` needs and `held` lacks, sorted."""
    need, todo = set(), [start]
    while todo:
        base = todo.pop()
        if base in need or base in held or base not in names:
            continue
        need.add(base)
        if base.endswith(".fxr"):
            body = names[base][3]
            for value in set(struct.unpack_from("<%di" % (len(body) // 4), body)):
                if value >= SMALLEST_NUMBER:
                    todo.extend(numbers.get(value, ()))
    return sorted(need)


SPAWN_SFX = ((2006, 2), (2006, 3))  # one-shot and following SFX: [entity kind, entity, dummy poly, effect id]


def spawned(events):
    """The effect ids an Emevd's SFX lines spawn."""
    return {w[3] for e in events.event_ids() for b, i, w in events.instructions(e)
            if (b, i) in SPAWN_SFX and len(w) > 3 and w[3] > 0}


def add_files(bnd, files_to_add):
    """Adds entries, ids continuing each type's range, the list kept in id order."""
    top = {}
    for entry in bnd["entries"]:
        kind = entry[2].rsplit(".", 1)[-1].lower()
        top[kind] = max(top.get(kind, ID_BASE.get(kind, 0) - 1), entry[1])
    for flags, name, body in files_to_add:
        kind = name.rsplit(".", 1)[-1].lower()
        top[kind] = top.get(kind, ID_BASE[kind] - 1) + 1
        bnd["entries"].append([flags, top[kind], name, body])
    bnd["entries"].sort(key=lambda e: e[1])


def plan(files, census, changes, progress=None, travelled=None, event_effects=None):
    """({archive rel: dcx bytes}, report). changes: {stem: {part: (model, think, npc)}}. travelled: {stem: {(model, home
    npc row)}}, the bodies that came along with a boss (helpers' parts: they are in no slot change, and the Scorched
    Beast's head fired invisible fireballs away from Hemwick, 17 Nov). progress(done, total): called as the archives
    finish packing, which is most of the step's time (zlib level 9, several seconds per archive). event_effects: {stem:
    effect ids} the map's events spawn that its vanilla events did not (events.spawned: a helper's events that travelled)."""
    wanted, placed_rows, spawned = {}, {}, {}
    for stem, ids in (event_effects or {}).items():
        wanted.setdefault(stem[:3], set())
        spawned.setdefault(stem[:3], set()).update(ids)
    for stem, bodies in (travelled or {}).items():
        wanted.setdefault(stem[:3], set()).update(model for model, _ in bodies)
        placed_rows.setdefault(stem[:3], set()).update(npc for _, npc in bodies)
    for stem, parts in changes.items():
        wanted.setdefault(stem[:3], set()).update(p[0] for p in parts.values())
        # the row each part really fights with: a boss (or a hard copy) has none among the pool candidates, and its
        # bullets' effects were missed (7 Nov: the One Reborn's projectiles were invisible in Gascoigne's arena)
        placed_rows.setdefault(stem[:3], set()).update(p[2] for p in parts.values())
    params = RowEffects(files)

    def row_effects(npc):
        row = census.npc_rows.get(npc)
        return params.row(census.npc_layout, row) if row is not None else set()

    model_effects = {}
    for model in sorted(set().union(*wanted.values())) if wanted else ():
        speffects = set()
        found = tae_effects(files, model, speffects) | params.speffect_effects(speffects)
        for _, npc in census.candidates.get(model, ()):
            found |= row_effects(npc)
        model_effects[model] = found

    archives = {}
    for name in files.listdir(FOLDER):
        m = re.match(r"frpg_sfxbnd_(.+)\.ffxbnd\.dcx$", name)
        if m:
            archives[m.group(1)] = _index(bnd4_read(dcx_read(files.read(_archive_rel(m.group(1))))[0])["entries"])
    resident = set()
    for name in RESIDENT:
        resident.update(archives[name][0])
    sources = sorted(n for n in archives if n not in RESIDENT)

    payloads, report = {}, {}
    for family in sorted(wanted):
        if family not in archives:
            continue
        rel = _archive_rel(family)
        payload, header = dcx_read(files.read(rel))
        bnd = bnd4_read(payload)
        held = resident | set(archives[family][0])
        adding, unfound = {}, 0
        wanted_effects = set().union(*(model_effects[m] for m in wanted[family]),
                                     *(row_effects(npc) for npc in placed_rows.get(family, ())), spawned.get(family, ()))
        for effect in sorted(wanted_effects):
            base = "f%09d.fxr" % effect
            if base in adding or base in archives[family][0]:
                continue
            source = next((s for s in sources if base in archives[s][0]), None)
            if base in resident:
                # a resident effect can draw with files only a map archive holds (user 25 Nov: the Coiled Host's
                # puddle f000651207 is in patcheffects, its model and textures s25070 only in m27 / m33's archives;
                # "every map should load every asset it needs"): those files come along, as for any effect
                if source is not None:
                    names, numbers = archives[source]
                    for name in _closure(base, names, numbers, (held | set(adding)) - {base}):
                        if name != base:
                            adding[name] = names[name]
                continue
            if source is None:
                unfound += 1  # vanilla names effects that exist nowhere: they play nothing there either
                continue
            names, numbers = archives[source]
            for name in _closure(base, names, numbers, held | set(adding)):
                adding[name] = names[name]
        if adding:
            add_files(bnd, [(adding[n][0], adding[n][2], adding[n][3]) for n in sorted(adding)])
            payloads[rel] = (bnd4_write(bnd), header)
        report[family] = {"files": len(adding), "bytes": sum(len(e[3]) for e in adding.values()), "in_no_archive": unfound}
    # zlib releases the GIL while it packs, so threads pack the archives side by side; the bytes are the same either way
    outputs = {}
    with ThreadPoolExecutor(max_workers=max(1, min(len(payloads), os.cpu_count() or 1))) as pool:
        jobs = {rel: pool.submit(dcx_write, *payloads[rel]) for rel in sorted(payloads)}
        for done, rel in enumerate(jobs, 1):
            outputs[rel] = jobs[rel].result()
            if progress:
                progress(done, len(jobs))
    return outputs, report
