"""Reads the winning version of each game file (the player's game, then their mods in load order) and
builds the census the planner works from. Only files read count toward the input hash."""
import hashlib
import os
import struct
import zlib

import arenas
import buildid
import seedcode
from containers import Param, Layout, bnd4_find, bnd4_read, dcx_read, dcx_size

STEMS = ("m21_00", "m21_01", "m22_00", "m23_00", "m24_00", "m24_01", "m24_02", "m25_00", "m26_00", "m27_00",
         "m28_00", "m32_00", "m33_00", "m34_00", "m35_00", "m36_00")
# Every part of a boss fight (bodies, extras, HP pools; arenas.py): pinned in the census, the planner alone gives a body a
# boss, by this world's modes. A fight's escorts (One Reborn's bell maidens) are pinned too unless an option frees them.
BOSS_ENTITIES = frozenset(arenas.BY_ENTITY)
# the one-body arenas of round 1 (no extras), kept as a name for older callers
ONE_BODY_ARENAS = frozenset(a.bodies[0] for a in arenas.ARENAS if a.kind == arenas.SOLO and a.fighters and not a.extras)
ESCORT = "escort"  # the kept reason of an escort whose row is otherwise free: bell_maidens 1/2 frees it (planner)
# The Shadows' summoned snakes (user, 8 Nov): with option shadows_summons off nothing is summoned (generate._no_summons);
# on, the summons are random enemies. Their row joins the pool either way. {entity: the marker SpEffect their summon event wants}. DATA m27 12704807 (run per
# snake): disabled at load, enabled on any Shadow body's AI message 20, killed on its own message 10 or once 5 s pass
# without SpEffect 5546 (a no-stat marker the snake gives itself), and by 12704830 when all three Shadows are at HP 0.
SUMMON = "summon"
SUMMONS = {2700803: 5546, 2700804: 5546, 2700805: 5546}
SUMMON_EVENT, SUMMON_MESSAGE = 12704807, 20  # the summon event and the AI message it waits for (4[08], word 2)
INVISIBLE_BYTES = 4096  # lead decision 4 Nov (D11): a chrbnd this small has no visible body (c2561 is 752 bytes)
FORCED_ANIM = (2003, 18)
CANCEL_ANIMATION = -1  # 2003[18] with -1 ends what plays: every model can do that
# A slot's "poses" (option free_poses, user 6 Nov; every animation, user 7 Nov: "if the replaced enemy has the animation
# play, else stand"): the map file's start animation and every 2003[18] the map's own events force on it (waiting poses,
# getting up, appearing 6200, scripted attacks 3xxx). A replacement lacking one does without it (generate.py): it stands,
# then fights as usual. Animations fed by common.emevd templates (9220, 9280: 18 calls) stay required: generate.py only
# edits the map's own events.
# A row's animation offset (DATA, 7 Nov): NpcParam.animIdOffset plus the animIdOffset of its SpEffects (5400-5403 give
# 0, 1000000, 2000000, 3000000: a huntsman's weapon set; -1 = none). A forced id plays as id + offset where the model has
# it, else as the plain id (LIKELY: c2630's 2000000 section has idles 0 and 200 but no walk 100, and those huntsmen walk).
SECTION = 1000000
SPECIAL_HP = 2000  # vanilla's ordinary placed enemies stop at 999; the rows above are 3,000 and up
# Enemies that cannot move on their own stay where they are and as they are, and their models are no replacement
# elsewhere (user, 5 Nov: the Old Yharnam gatling next to Djura stays a gatling; every such part kept the same way).
# DATA, vanilla, every part of STEMS: the model's own animation table has no walk (200-299), run (500-599) or turn
# (600-699) clip, which every walking model has; its think row runs a battle script that acts (not 011000, which does
# nothing); its NpcParam team is not 26 (the Plain Doll's and the Snail Woman's: LIKELY the friendly team). The comments
# name each model's battle script. tests/test_randomizer re-derives this table from the game files.
TURRET = "turret"
TURRET_MODELS = {
    "c7100": "Djura's gatling, m23_00 (RuinsGatling710000)",
    "c2561": "c2560's invisible bullet part, warped onto its c2560 by events, m26/m33/m36 (HozukiBullet256100)",
    "c2501": "spines of a small Celestial Emissary, warped onto their c2500 by events, m24_02/m27 (FromMoonOnlyFeeler250170)",
    "c5160": "the Brain of Mensis once dropped, m26_00 (EvilEye515000)",
}
LOCOMOTION = ((200, 300), (500, 700))  # walk, run, turn animation ids
# NPCs on enemy models keep their slot like every human NPC ("npc"; user, 2 Nov: dummies and NPC slots stay vanilla).
# DATA, vanilla: an otherwise free part is an NPC when (a) its MSB part names a talk script the map's talkesdbnd holds,
# (b) its think id is an NPC think (6000-6999, the range of the human NPCs), or (c) an event that names it writes an NPC's
# quest-state flags (a 2003[22] batch inside 1000-1999). tests/test_randomizer re-derives this table.
# m36_00 3600920 (c4051, the Fishing Hamlet Priest) was left free on 5 Nov for the difficult list; user, 6 Nov: he
# "isn't even supposed to spawn" elsewhere, so he is an NPC like the rest (his row leaves the pool with him).
NPC_ENTITIES = {
    2100700: "the Plain Doll, m21_00 (talk 210304)",
    2400756: "the beast form of the NPC 2400755, m24_00 (talk 240217; flags 1200-1219)",
    2700756: "the beast form of the NPC 2700755, m27_00 (talk 270217; flags 1200-1219)",
    2410771: "c2521 of the NPC 2410770's quest, m24_01 (flags 1220-1239: 12410830, 12410833)",
    2410781: "c1091, an NPC's beast form, m24_01 (its death sets flags 1240-1259: 12410761)",
    3200101: "c1110, m32_00 (talk 320329; flags 1420-1437)",
    3300710: "c1110, m33_00 (NPC think 6200)",
    3400810: "c4090, m34_00 (talk 340540, NPC think 6550)",
    3500910: "c4023, m35_00 (talk 350506, NPC think 6560)",
    3500552: "c4150, m35_00 (talk 350572)",
    3500911: "c4160, m35_00 (talk 350512, NPC think 6560; flags 1750-1769)",
    3600920: "c4051, the Fishing Hamlet Priest, m36_00 (talk 360507; flags 1770-1789)",
}
# Kinds that do not fight away from home (LIVE, user, 6 Nov): placed elsewhere they see the player and flinch when hit
# but never attack. They are no replacement elsewhere; their own slots stay free like any other. Cause NOT found offline
# (rows, think fields, scripts, luainfo/luagnl registration all match kinds that do fight); the Blood-starved Beast row
# 209010 is the pool's only one on team 24 (the others 23).
HOME_ONLY_MODELS = {
    "c2090": "Blood-starved Beast (its normal placement m34_00 3400650, row 209010)",
    "c2130": "Cainhurst Bloodlicker (rows 213500 / 213510)",
}
# The human model c0000: a hunter that talks (an MSB talk id), has a name (NpcParam nameId > 0: Alfred, Valtr, Henriett,
# Yamamura, the Madaras twin ...) or is named by an event that writes NPC quest flags (2003[22], 1000-1999) is an NPC and
# stays. Any other c0000 is an ordinary hostile hunter (the Hunter's Nightmare old hunters, Yahar'gul's hunters ...): its
# slot is free like any enemy's (user, 6 Nov: "the one with the saw spear keeps spawning in the same spot"). c0000 is
# never a replacement elsewhere (the player model: no own size, no pool rows).
HUMAN = "c0000"
QUEST_FLAGS = (1000, 2000)
COMMON_SCRIPTS = "script/aicommon.luabnd.dcx"

ESCORT_ARENA = {e: a for a in arenas.ARENAS for e in a.escorts}


def _role(entity, arena):
    if arena is None:
        return None
    if entity in arenas.PHASES:
        return PHASE
    if entity in arena.bodies:
        return "body"
    if entity == arena.pool:
        return "pool"
    return "extra" if entity in arena.extras else "escort"


# A fight's phase body (option coiled_host; arenas.PHASES): the role of its slot. Found, never listed: a placed enemy of the
# arena's map that the arena's defeat event (its id = the defeat flag, DATA: every arena of arenas.py) wakes once the
# table's bodies are dead: it shows a boss bar on it (2003[11] shown), waits for its death (4[00]) and gives the boss
# reward on it (2003[12]). DATA: none in the game alone; Nocturne v2.0.0's m27 12701800 does all three on the Coiled Host
# 2700806 (c5030, row 503000; lines 34, 36, 44).
PHASE = "phase"
BOSS_BAR, BOSS_DEFEAT, IF_DEAD = (2003, 11), (2003, 12), (4, 0)


def phase_bodies(events, enemies):
    """{entity: arena key} of the phase bodies. events: {stem: _events() of its map}; enemies: {stem: {entity: MSB enemy
    part}} (the map's first variant)."""
    out = {}
    for arena in arenas.ARENAS:
        defeat = events.get(arena.stem, {}).get(arena.flags[0])
        if arena.kind == arenas.OUT or defeat is None:
            continue
        lines = defeat[0]
        shown = {w[1] for b, i, w in lines if (b, i) == BOSS_BAR and len(w) > 3 and w[0] == 1}
        awaited = {w[1] for b, i, w in lines if (b, i) == IF_DEAD and len(w) > 2 and w[2] == 1}
        rewarded = {w[0] for b, i, w in lines if (b, i) == BOSS_DEFEAT and w}
        for entity in sorted(shown & awaited & rewarded):
            if entity not in arenas.BY_ENTITY and entity in enemies.get(arena.stem, {}):
                out[entity] = arena.key
    return out


GAME_FOLDERS = frozenset(("action", "adhoc", "chr", "event", "facegen", "font", "map", "menu", "movie", "msg", "mtd",
                          "obj", "other", "param", "paramdef", "parts", "remo", "script", "sfx", "shader", "sound"))
WRAPPERS = ("", "app0", "CUSA03173", "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363", "CUSA03179",
            "CUSA03014", "CUSA03023")


def _child(folder, name):
    """A child by name, ignoring case like the player's file system and the engine's mod loader."""
    if os.path.isdir(folder):
        for entry in sorted(os.listdir(folder)):
            if entry.lower() == name.lower():
                return os.path.join(folder, entry)
    return os.path.join(folder, name)


def content_root(folder):
    """The folder that stands for dvdroot_ps4 in a mod, in every layout the engine accepts; None if there is none."""
    if not os.path.isdir(folder):
        return None
    for wrapper in WRAPPERS:
        base = _child(folder, wrapper) if wrapper else folder
        root = _child(base, "dvdroot_ps4")
        if os.path.isdir(root):
            return root
    entries = [e for e in sorted(os.listdir(folder)) if not e.startswith(".")]
    folders = [e for e in entries if os.path.isdir(os.path.join(folder, e))]
    if folders and all(e.lower() in GAME_FOLDERS for e in folders):
        return folder
    if len(folders) == 1:
        return content_root(os.path.join(folder, folders[0]))
    return None


UPDATE_MARK = "event/m34_00_00_00.emevd.dcx"  # the first Old Hunters area's events: game data since 1.09 (a disc
# game's base lacks them; its 1.09 update brings them with every map's new event files)


class OldGame(RuntimeError):
    """The game's files are from before the 1.09 update (randomizer.py: exit 7)."""


class GameFiles:
    """`roots` = the game, then mod folders; later roots win, like the engine's load order. `update`: the game's update
    folder when it is kept beside the game, not copied into it (the launcher's GameCheck.UpdateFolder: CUSAxxxxx-patch
    or -UPDATE); the engine lays it over the game at every start, so its files win over the game's and count as the
    game's (root 0): a game with its update beside it reads, and hashes, the same as one with the update copied in.
    Without it, such a game would be read as the unpatched base (6 Nov: 19 event, script, map and animation files)."""

    def __init__(self, game, mods=(), update=None):
        self.roots = []
        for folder in [game] + list(mods):
            root = content_root(folder)
            if root is None:
                raise FileNotFoundError("no game files found in " + folder)
            self.roots.append(root)
        self.update = None
        if update:
            self.update = content_root(update)
            if self.update is None:
                raise FileNotFoundError("no game files found in " + update)
        self.mod_names = [os.path.basename(os.path.normpath(m)) for m in mods]
        self.mod_folders = list(mods)
        self.read_hashes = {}
        self.read_from = {}  # relative path -> index into roots of the root that supplied it (0 = the game or its update)

    def _layers(self):
        """(index, root) from the one that wins: the mods last to first, the game's update, the game."""
        out = [(index, self.roots[index]) for index in range(len(self.roots) - 1, 0, -1)]
        if self.update:
            out.append((0, self.update))
        out.append((0, self.roots[0]))
        return out

    def _find(self, rel):
        for index, root in self._layers():
            candidate = os.path.join(root, rel)
            if os.path.isfile(candidate):
                return index, candidate
        return None, None

    def path(self, rel):
        return self._find(rel)[1]

    def check_updated(self):
        """Every edit is written against the 1.09 files. A disc game (CUSA00900) whose update is neither copied in nor
        beside it (or only its eboot.bin and sce_sys were copied) has the base's map events: an arena edit then misses
        its line (NW-bbc8683: 'event 12424790 has no 2004[17]'). Said plainly instead."""
        if self.path(UPDATE_MARK) is None:
            raise OldGame("The game's files are from before the 1.09 update (no Old Hunters events in dvdroot_ps4): "
                          "copy the whole 1.09 update into the game folder (launcher: Game setup, Choose update "
                          "folder) or keep it beside the game as %s-patch, then generate again."
                          % os.path.basename(os.path.normpath(os.path.dirname(self.roots[0]))))

    def read(self, rel):
        index, path = self._find(rel)
        if path is None:
            raise FileNotFoundError(rel)
        with open(path, "rb") as f:
            data = f.read()
        key = _key(rel)
        self.read_hashes[key] = hashlib.sha256(data).hexdigest()
        self.read_from[key] = index
        return data

    def peek(self, rel):
        """A file read for the spoiler's text only: not recorded, so it never changes the input hash (the world)."""
        path = self.path(rel)
        if path is None:
            return None
        with open(path, "rb") as f:
            return f.read()

    def head(self, rel, size):
        """The first bytes of a big file: its size and these bytes are the part of it that counts."""
        index, path = self._find(rel)
        if path is None:
            return None
        with open(path, "rb") as f:
            data = f.read(size)
            f.seek(0, 2)
            key = _key(rel)
            self.read_hashes[key] = hashlib.sha256(data + struct.pack("<q", f.tell())).hexdigest()
            self.read_from[key] = index
        return data

    def listdir(self, rel):
        """The names in this folder of every root, lowercase (the file system and the engine ignore case, so a copy of the
        game or a mod spelled in another case reads, and names its outputs, the same)."""
        out = set()
        for _, root in self._layers():
            folder = os.path.join(root, rel)
            if os.path.isdir(folder):
                out.update(name.lower() for name in os.listdir(folder))
        return sorted(out)

    def read_beside(self, index, rel):
        """A file of mod `index` (1 = the first mod) beside its game files, as the engine reads it from the mod's folder
        (nightwatch.json, nightwatch/<feature>.json: docs/modding/manifest.md); None when the mod has none. Counted in
        the input hash under "mod <index>/<rel>", as that mod's input."""
        path = os.path.join(self.mod_folders[index - 1], rel)
        if not os.path.isfile(path):
            return None
        with open(path, "rb") as f:
            data = f.read()
        key = "mod %d/%s" % (index, _key(rel))
        self.read_hashes[key] = hashlib.sha256(data).hexdigest()
        self.read_from[key] = index
        return data

    def mod_inputs(self):
        """Each mod in load order: its folder name and a hash of the files it supplied to this world ("" when the world
        read nothing of it). Names are the player's own folder names; the hash tells two copies of a mod apart."""
        out = []
        for index, name in enumerate(self.mod_names, start=1):
            files, digest = self.root_input(index)
            out.append({"name": name, "files": files, "hash": digest})
        return out

    def root_input(self, index):
        """(files, hash) of what root `index` (0 = the game, then the mods in load order) supplied to this world; hash ""
        when the world read nothing of it. input_hash's parts by where the files came from, so a world that differs from
        another can say whether the game's files or a mod's did."""
        mine = sorted((rel, h) for rel, h in self.read_hashes.items() if self.read_from[rel] == index)
        text = "".join("%s %s\n" % item for item in mine)
        return len(mine), hashlib.sha256(text.encode("utf-8")).hexdigest() if mine else ""

    def input_hash(self):
        """The world's check: the files read (path and content) and the generator that read them (its version and its own
        build, buildid.py), so a world of another generator build never shares a folder name (or saves, or a lobby check)
        with this one's. No folder name counts."""
        text = "generator %d\nbuild %s\n" % (seedcode.GENERATOR_MAJOR, buildid.build_id())
        text += "".join("%s %s\n" % item for item in sorted(self.read_hashes.items()))
        return hashlib.sha256(text.encode("utf-8")).hexdigest()

    def read_hash(self):
        """input_hash without the generator build: the files read and GENERATOR_MAJOR. What the world's game files may depend
        on (the row marks), so a generator change that changes no output writes the same game files (the golden test)."""
        text = "generator %d\n" % seedcode.GENERATOR_MAJOR
        text += "".join("%s %s\n" % item for item in sorted(self.read_hashes.items()))
        return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _key(rel):
    """A game path as the input hash records it: forward slashes, lowercase (the engine's mod loader ignores case too)."""
    return rel.replace("\\", "/").lower()


def map_variants(files, stem):
    suffix = ".msb.dcx"
    return [n[:-len(suffix)] for n in files.listdir("map/mapstudio")
            if n.startswith(stem + "_") and n.endswith(suffix)]


def _events(data):
    """{event id: ([(bank, id, int words)], {instruction: {arg offset: source offset}})}"""
    q = lambda at: struct.unpack_from("<q", data, at)[0]
    count, events_at, instr_at, args_at, params_at = q(0x10), q(0x18), q(0x28), q(0x78), q(0x58)
    out = {}
    for e in range(count):
        at = events_at + e * 0x30
        event_id, n, first, param_count, param_first = q(at), q(at + 8), q(at + 0x10), q(at + 0x18), q(at + 0x20)
        instructions = []
        for i in range(n):
            o = instr_at + first + i * 0x20
            bank, iid = struct.unpack_from("<ii", data, o)
            length, offset = q(o + 8), q(o + 0x10)
            a = data[args_at + offset:args_at + offset + length]
            instructions.append((bank, iid, struct.unpack_from("<%di" % (len(a) // 4), a, 0)))
        params = {}
        for k in range(param_count):
            p = params_at + param_first + k * 0x20
            params.setdefault(q(p), {})[q(p + 8)] = q(p + 0x10)
        out[event_id] = (instructions, params)
    return out


def _load_events(files, stem_full):
    raw = files.read("event/%s.emevd.dcx" % stem_full)
    return _events(zlib.decompress(raw[0x4c:]) if raw[:4] == b"DCX\0" else raw)


# Template runs that hold an enemy in a state only its own kind is made for (user, 8 Nov: Central Yharnam's caged dogs,
# replaced, "didnt die, some of them flipped upside down after dying but didnt die, some t posed"). Found by their shape,
# DATA (vanilla; m24_01 and m26_00 are the only maps with a free slot in either, tests/test_randomizer re-derives this):
# - IMMORTAL: the template sets immortality on its parameter entity (2004[12] on) and ends it only by a forced death
#   (2004[04]) on that entity, or never ends it. m24_01 12415420, one run per caged Rabid Dog (2410271/272/275/277/278/279):
#   immortal while loaded and inside its cage area, then on the player's hit inside that area SpEffect 5915 and a forced
#   death. m26_00 12604890, one run per Mensis scholar c2040 (16, 2600161-2600179): immortal until Micolash's defeat
#   (12604857, set by 12601850), then SpEffect 5914 and the collapse 7002; disabled for good once he is dead (12601850).
# - CAGED: the template turns its parameter entity's AI off (2004[01] off), forces a looping animation on it (2003[18],
#   loop byte 1), and nothing in the map ever turns that entity's AI on. m24_01 12410380 (2410275/277/278/279): AI off
#   for good once the player is near, then the bark 3020 looped and the event restarted (end type 1) while alive.
# For a slot that gets another model, generate.py drops these runs (their call lines in the map's events become labels):
# the new enemy fights from its spot with its own AI and dies like any other. The vanilla kind keeps them. Their forced
# animations are no need of such a slot (Census). A replaced scholar so also stays after Micolash's defeat (an ordinary
# enemy: the run that collapses and later disables it is gone with its immortality).
HOLD_IMMORTAL, HOLD_CAGED = "immortal", "caged"


def _run_target(words, every):
    """(template, index of its first argument word) of a 2000[0]/[6] call."""
    return (words[1], 2) if words[1] in every else (words[0], 1)


def held_runs(own, common):
    """{entity: [(calling event, call line, template, HOLD_*)]} of the runs above in one map. own, common: _events()."""
    every = dict(common)
    every.update(own)
    kinds = {}  # template: [(argument offset of the entity, kind)]
    ai_on = {}  # template: argument offsets whose entity it turns AI on
    literal_on = set()
    for event, (instructions, params) in every.items():
        lines = {}
        for i, (bank, iid, words) in enumerate(instructions):
            src = params.get(i, {}).get(0)
            if (bank, iid) == (2004, 1) and len(words) >= 2 and words[1] == 1:
                if src is None:
                    if event in own:
                        literal_on.add(words[0])
                else:
                    ai_on.setdefault(event, set()).add(src)
            if src is None or not words:
                continue
            if (bank, iid) == (2004, 12) and len(words) >= 2 and words[1] == 1:
                lines.setdefault(src, set()).add("immortal")
            elif (bank, iid) == (2004, 12):
                lines.setdefault(src, set()).add("mortal")
            elif (bank, iid) == (2004, 4):
                lines.setdefault(src, set()).add("death")
            elif (bank, iid) == (2004, 1) and len(words) >= 2 and words[1] == 0:
                lines.setdefault(src, set()).add("ai off")
            elif (bank, iid) == (2004, 1):
                lines.setdefault(src, set()).add("ai on")
            elif (bank, iid) == FORCED_ANIM and len(words) >= 3 and words[2] & 0xff:
                lines.setdefault(src, set()).add("loop")
        for src, found in lines.items():
            if "immortal" in found and ("death" in found or "mortal" not in found):
                kinds.setdefault(event, []).append((src, HOLD_IMMORTAL))
            if {"ai off", "loop"} <= found and "ai on" not in found:
                kinds.setdefault(event, []).append((src, HOLD_CAGED))
    runs, woken = [], set(literal_on)
    for event, (instructions, _) in own.items():
        for k, (bank, iid, words) in enumerate(instructions):
            if bank != 2000 or iid not in (0, 6) or len(words) < 2:
                continue
            target, skip = _run_target(words, every)
            arg = lambda src: words[skip + src // 4] if skip + src // 4 < len(words) else None
            woken.update(arg(src) for src in ai_on.get(target, ()))
            for src, kind in kinds.get(target, ()):
                if arg(src) is not None:
                    runs.append((arg(src), (event, k, target, kind)))
    out = {}
    for entity, run in runs:
        if run[3] == HOLD_CAGED and entity in woken:
            continue
        out.setdefault(entity, []).append(run)
    return out


WARP_TO_DUMMY = ((2004, 40), (2004, 41))  # [character, ..., target entity, dummy poly, ...]: warp the character onto it


def riding_parts(own, common):
    """{rider: host} of one map: every character a warp line puts onto another character, written in the line or given
    by a run of its template (DATA, vanilla: the 9 c2561 Frenzy bullet parts on Winter Lanterns, the 3 c2501 spine
    parts on small Celestial Emissaries; also bosses' scripted warps, which Census leaves out). own, common: _events()."""
    every = dict(common)
    every.update(own)
    out = {}
    for event, (instructions, params) in own.items():
        for i, (bank, iid, words) in enumerate(instructions):
            if (bank, iid) in WARP_TO_DUMMY and len(words) > 2 and not params.get(i):
                out[words[0]] = words[2]
            if bank != 2000 or iid not in (0, 6) or len(words) < 2:
                continue
            target, skip = _run_target(words, every)
            if target not in every:
                continue
            body, body_params = every[target]
            for k, (b, d, w) in enumerate(body):
                src = body_params.get(k)
                if (b, d) in WARP_TO_DUMMY and len(w) > 2 and src:
                    arg = lambda off, default: (words[skip + src[off] // 4] if off in src and skip + src[off] // 4 < len(words)
                                                else default)
                    out[arg(0, w[0])] = arg(8, w[2])
    return out


def forced_animations(files, stems, shared=None, skip=None):
    """{entity: animations any map event forces on it, directly or through event parameters}.
    None in a set = an animation this scan can't name. shared: a set that receives (entity, animation) of those a
    common.emevd template forces. skip: {stem: {(event, line)}} of template calls not to count (held_runs)."""
    common = _load_events(files, "common")
    skip = skip or {}
    forced = {}

    def note(entity, animation, from_common=False):
        forced.setdefault(entity, set()).add(animation)
        if from_common and shared is not None:
            shared.add((entity, animation))

    for stem in stems:
        own = _load_events(files, stem + "_00_00")
        every = dict(common)
        every.update(own)
        via_param = {}
        for event_id, (instructions, params) in every.items():
            for i, (bank, iid, words) in enumerate(instructions):
                if (bank, iid) != FORCED_ANIM or not words:
                    continue
                entity_from = params.get(i, {}).get(0)
                anim_from = params.get(i, {}).get(4)
                literal = words[1] if len(words) > 1 and anim_from is None else None
                if entity_from is None:
                    note(words[0], literal)
                else:
                    via_param.setdefault(event_id, []).append((entity_from, anim_from, literal, event_id in common
                                                               and event_id not in own))
        for event_id, (instructions, _) in own.items():
            for line, (bank, iid, words) in enumerate(instructions):
                if bank != 2000 or iid not in (0, 6) or len(words) < 2 or (event_id, line) in skip.get(stem, ()):
                    continue
                target, first = _run_target(words, every)
                for entity_from, anim_from, literal, from_common in via_param.get(target, ()):
                    k = first + entity_from // 4
                    if k >= len(words):
                        continue
                    animation = literal
                    if anim_from is not None:
                        j = first + anim_from // 4
                        animation = words[j] if j < len(words) else None
                    note(words[k], animation, from_common)
    return forced


def animations_of(files, model):
    """The animation ids a model can play: its TAE table. An entry can reuse another's clip, so the hkx names are fewer."""
    rel = "chr/%s.anibnd.dcx" % model
    if files.path(rel) is None:
        return frozenset()
    found = set()
    for entry in bnd4_read(dcx_read(files.read(rel))[0])["entries"]:
        if entry[2].lower().endswith(".tae"):
            table, count = struct.unpack_from("<q", entry[3], 0x58)[0], struct.unpack_from("<q", entry[3], 0x70)[0]
            found.update(struct.unpack_from("<q", entry[3], table + k * 16)[0] for k in range(count))
    return frozenset(found)


def playable(ids, offsets):
    """The animation ids an event can force on a model with this animation table (`ids`, from animations_of) under each
    of these row offsets: an id plays as id + offset where the table has it, else as the plain id (SECTION above)."""
    offsets = set(offsets)
    if offsets <= {0}:
        return frozenset(ids)
    shifted = {x - o for x in ids for o in offsets if x >= o > 0}
    return frozenset(set(ids) | {a for a in shifted if all(a + o in ids for o in offsets)})


class Census:
    """What the planner needs: slots per map, candidates per model, sizes. All from `files`."""

    def __init__(self, files):
        self.files = files
        bnd = bnd4_read(dcx_read(files.read("param/gameparam/gameparam.parambnd.dcx"))[0])
        defs = bnd4_read(dcx_read(files.read("paramdef/paramdef.paramdefbnd.dcx"))[0])
        self.npc_layout = Layout(bnd4_find(defs, "\\NpcParam.paramdef")[3])
        self.think_layout = Layout(bnd4_find(defs, "\\NpcThinkParam.paramdef")[3])
        self.npc = Param(bnd4_find(bnd, "\\NpcParam.param")[3])
        self.think = Param(bnd4_find(bnd, "\\NpcThinkParam.param")[3])
        self.npc_rows = self.npc.first()
        self.think_rows = self.think.first()
        self.archives = {}
        for stem in STEMS:
            rel = "script/%s_00_00.luabnd.dcx" % stem
            if files.path(rel):
                self.archives[stem] = bnd4_read(dcx_read(files.read(rel))[0])
        self.script_files = {stem: {e[2].replace("/", "\\").rsplit("\\", 1)[1] for e in b["entries"]}
                             for stem, b in self.archives.items()}
        # the AI scripts every map loads (DATA: the Mad Ones' 205000_battle, 105000/118000 are only here); a map's own
        # archive never needs a copy of them (generate.merged_archive)
        common = bnd4_read(dcx_read(files.read(COMMON_SCRIPTS))[0])
        self.common_scripts = {e[2].replace("/", "\\").rsplit("\\", 1)[1] for e in common["entries"]}
        every_script = set().union(self.common_scripts, *self.script_files.values())
        shared = set()
        forced = forced_animations(files, STEMS, shared)
        # the runs a replacement goes without (held_runs) and the animations it is then never forced to play
        common_events = _load_events(files, "common")
        # (a fight's parts are the boss passes' business: the Witch's pair template 12204812 has this shape too)
        fight = BOSS_ENTITIES | set(ESCORT_ARENA) | set(SUMMONS)
        self.held = {stem: {e: runs for e, runs in held_runs(_load_events(files, stem + "_00_00"), common_events).items()
                            if e not in fight} for stem in STEMS}
        skip = {stem: {run[:2] for runs in v.values() for run in runs} for stem, v in self.held.items()}
        forced_free = forced_animations(files, STEMS, None, skip)
        self.quest_entities = set()  # every number an event names that writes NPC quest flags (HUMAN above)
        stem_events = {stem: _load_events(files, stem + "_00_00") for stem in STEMS}
        for stem in STEMS:
            for instructions, _ in stem_events[stem].values():
                if any((bank, iid) == (2003, 22) and words and QUEST_FLAGS[0] <= words[0] < QUEST_FLAGS[1]
                       for bank, iid, words in instructions):
                    self.quest_entities.update(w for _, _, words in instructions for w in words)
        self.slots = {}
        self.vanilla_models = {}
        candidates = {}
        from msb import Msb
        stem_enemies = {}
        for stem in STEMS:
            first = map_variants(files, stem)[0]
            stem_enemies[stem] = Msb(dcx_read(files.read("map/mapstudio/%s.msb.dcx" % first))[0]).enemies()
        # phase bodies with AI of their own (a fighter elsewhere needs its scripts); arena_of knows them from here on
        found = phase_bodies(stem_events, {stem: {e["entity"]: e for e in v} for stem, v in stem_enemies.items()})
        self.phases = {e: key for e, key in found.items()
                       if self._ordinary_reason(next(p for p in stem_enemies[arenas.BY_KEY[key].stem] if p["entity"] == e),
                                                forced, every_script) in (None, "special")}
        arenas.set_phases(self.phases)
        for stem in STEMS:
            enemies = stem_enemies[stem]
            slots = []
            for e in enemies:
                reason = self._why_kept(e, forced, every_script)
                # vanilla's needs decide whether the row feeds the pool; a replacement's leave out the held runs' (held_runs)
                vanilla_needs = set(forced.get(e["entity"], ())) - {None, CANCEL_ANIMATION}
                needs = set(forced_free.get(e["entity"], ())) - {None, CANCEL_ANIMATION}
                poses = {a for a in needs if (e["entity"], a) not in shared}
                if e["anim"] != -1:
                    needs.add(e["anim"])
                    vanilla_needs.add(e["anim"])
                    if (e["entity"], e["anim"]) not in shared:
                        poses.add(e["anim"])
                arena = arenas.arena_of(e["entity"]) or ESCORT_ARENA.get(e["entity"])
                held = self.held[stem].get(e["entity"], []) if e["entity"] > 0 else []
                slots.append({"part": e["part"], "model": e["model"], "entity": e["entity"], "think": e["think"],
                              "npc": e["npc"], "kept": reason, "needs": tuple(sorted(needs)),
                              "poses": tuple(sorted(poses)), "start_anim": e["anim"],
                              "arena": arena.key if arena else None, "role": _role(e["entity"], arena),
                              "held": tuple(sorted(held))})
                # a row made for a scripted spot stays there: only plain slots feed the pool (an escort's too, as before)
                # (a summon's needs are its summon event's: its row places like any other)
                if (reason in (None, ESCORT) and not vanilla_needs and not held or reason == SUMMON) \
                        and e["model"] != HUMAN \
                        and e["model"] not in HOME_ONLY_MODELS:
                    candidates.setdefault(e["model"], set()).add((e["think"], e["npc"]))
            self.slots[stem] = slots
        self.sizes = {}
        names = set(candidates)
        for slots in self.slots.values():
            names.update(s["model"] for s in slots)
        for model in sorted(names):
            head = files.head("chr/%s.chrbnd.dcx" % model, 0x24)
            self.sizes[model] = dcx_size(head) if head else 0
        # D11: an invisible model is no enemy to meet (a missing one, size 0, stays for the planner to skip as before)
        self.candidates = {m: sorted(v) for m, v in candidates.items() if not 0 < self.sizes[m] < INVISIBLE_BYTES}
        boss_models = {s["model"] for slots in self.slots.values() for s in slots if s["role"] in ("body", "extra", PHASE)}
        sp_layout = Layout(bnd4_find(defs, "\\SpEffect.paramdef")[3])
        sp_rows = Param(bnd4_find(bnd, "\\SpEffectParam.param")[3]).first()
        # the rows a model can come with: its pool rows, else (a boss model) the rows its own slots have
        rows_of = {m: {npc for _, npc in v} for m, v in self.candidates.items()}
        own_rows = {}
        for slots in self.slots.values():
            for s in slots:
                own_rows.setdefault(s["model"], set()).add(s["npc"])
        self.animations = {}
        for m in sorted(set(self.candidates) | boss_models):
            npcs = rows_of.get(m) or own_rows.get(m, ())
            offsets = {self._anim_offset(npc, sp_layout, sp_rows) for npc in npcs if npc in self.npc_rows}
            self.animations[m] = playable(animations_of(files, m), offsets)
        self._effects = (sp_layout, sp_rows)
        # parts that ride on an ordinary enemy (kept as turrets; knowledge msb.md "Riding parts"): {stem: {rider: host}}.
        # A host that gets another model takes the rider along unseen (the Lantern's Frenzy on any enemy): generate.release
        # turns such a rider off (user, 10 Nov)
        self.riders = {}
        for stem in STEMS:
            entities = {s["entity"]: s for s in self.slots[stem] if s["entity"] > 0}
            pairs = riding_parts(stem_events[stem], common_events)
            self.riders[stem] = {r: h for r, h in sorted(pairs.items()) if r in entities and entities[r]["kept"] == TURRET
                                 and h in entities and entities[h]["kept"] is None}
        self.joined = set()  # the models add_candidates joined to the pool (chalice enemies)

    def hp_rate(self, effect):
        """The maxHpRate of SpEffect `effect` (an NpcParam spEffectID6: the area scaling), 1.0 for none or a missing row."""
        sp_layout, sp_rows = self._effects
        return sp_layout.get(sp_rows[effect], "maxHpRate") if effect in sp_rows else 1.0

    def add_candidates(self, extra):
        """Joins rows of models the main maps never place as ordinary enemies ({model: [(think, npc)]}, such as
        chalice_enemies.candidates) to the pool; their sizes and animations are read now (they count as inputs).
        The models are listed in `joined`: generate.clone_rows gives them the slot's rewards always."""
        for model in sorted(extra):
            if model in self.candidates:
                raise ValueError("%s is a main pool kind already" % model)
            self.joined.add(model)
            head = self.files.head("chr/%s.chrbnd.dcx" % model, 0x24)
            self.sizes[model] = dcx_size(head) if head else 0
            offsets = {self._anim_offset(npc, *self._effects) for _, npc in extra[model]}
            self.animations[model] = playable(animations_of(self.files, model), offsets)
            self.candidates[model] = sorted(extra[model])

    def _anim_offset(self, npc, sp_layout, sp_rows):
        """The animation offset of NpcParam row `npc` (SECTION above): its own plus its SpEffects'."""
        row = self.npc_rows[npc]
        offset = max(0, self.npc_layout.get(row, "animIdOffset"))
        for k in range(8):
            effect = self.npc_layout.get(row, "spEffectID%d" % k)
            if effect in sp_rows:
                offset += max(0, sp_layout.get(sp_rows[effect], "animIdOffset"))
        return offset

    def _why_kept(self, e, forced, every_script):
        if e["entity"] in BOSS_ENTITIES or e["entity"] in self.phases:
            return "boss"
        reason = self._ordinary_reason(e, forced, every_script)
        if reason is None and e["entity"] in ESCORT_ARENA:
            return ESCORT
        if reason is None and e["entity"] in SUMMONS:
            return SUMMON
        return reason

    def _ordinary_reason(self, e, forced, every_script):
        if e["model"] == HUMAN and (e["talk"] > 0 or e["entity"] in self.quest_entities or e["npc"] not in self.npc_rows
                                    or self.npc_layout.get(self.npc_rows[e["npc"]], "nameId") > 0):
            return "npc"
        if e["model"].startswith("c9"):
            return "dummy"
        if e["model"] in TURRET_MODELS:
            return TURRET
        if e["entity"] in NPC_ENTITIES:
            return "npc"
        row = self.think_rows.get(e["think"])
        if e["think"] <= 1 or row is None or e["npc"] not in self.npc_rows:
            return "no ai"
        # LIVE: row 454300 stands inert with 141,098 HP in a normal slot. Such a row is no enemy to place
        # or to replace; a stability rule (it cannot be killed), not a balance one.
        if self.npc_layout.get(self.npc_rows[e["npc"]], "hp") >= SPECIAL_HP:
            return "special"
        if None in forced.get(e["entity"], ()):  # an animation we can't name: nothing can be proven to have it
            return "scripted"
        if "%06d_battle.lua" % self.think_layout.get(row, "battleGoalID") not in every_script:
            return "no script"
        return None

    def goals(self, think, npc=None):
        """(battle goal, logic id) of a think row; think 0 or 1 (a boss body such as the One Reborn's) runs its NpcParam
        row's aiThinkId (LIKELY, survey 4 Nov: the bodies' MSB think is 0, their AI scripts are in the map)."""
        if think <= 1 and npc in self.npc_rows:
            think = self.npc_layout.get(self.npc_rows[npc], "aiThinkId")
        row = self.think_rows[think]
        return self.think_layout.get(row, "battleGoalID"), self.think_layout.get(row, "logicId")
