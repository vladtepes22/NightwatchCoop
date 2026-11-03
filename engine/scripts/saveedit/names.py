"""Item names and valid ids from the player's OWN game folder (never shipped, no game text in the release).

Reads <game>\\msg\\<lang>\\item.msgbnd.dcx (the FMG name files: goods, weapons, armor, accessories) and
<game>\\param\\gameparam\\gameparam.parambnd.dcx (the row ids of EquipParamWeapon / Protector / Accessory /
Goods, which are the ids the game can load), once, and caches the result as JSON in the cache folder
(re-read when either game file changes). Missing files mean no names: callers show ids and keep weapon
levels read-only.
"""
import json
import re
import struct
import zlib
from pathlib import Path

# BND entry names inside item.msgbnd.dcx are Japanese: goods, weapon, armor, accessory names.
FMG_FILES = {"goods": "アイテム名", "weapon": "武器名", "armor": "防具名", "accessory": "アクセサリ名"}
PARAMS = {"weapon": "EquipParamWeapon", "armor": "EquipParamProtector", "accessory": "EquipParamAccessory",
          "goods": "EquipParamGoods"}
_PLACEHOLDER = re.compile(r"^(\*|NOT NEEDED|en\d+)?$", re.I)
CACHE_VERSION = 7
EVENT_TEXT = "イベントテキスト"  # menu.msgbnd FMG: the lamp warp list names (id 7000AABn)
NPC_NAMES = "NPC名"  # item.msgbnd FMG: boss and NPC names by character id
GEM_NAMES = "魔石名"  # menu.msgbnd FMG: blood gem and rune names (key built in gems.name_key)
GEM_EFFECTS = "魔石効果"  # menu.msgbnd FMG: gem effect texts by GemeffectParam id


def _dcx(b):
    if b[:4] != b"DCX\0":
        return b
    if b[0x24:0x28] != b"DCP\0" or b[0x28:0x2C] != b"DFLT":
        raise ValueError("unsupported DCX compression")
    usz, csz = struct.unpack_from(">II", b, 0x1C)
    d = zlib.decompress(b[0x4C:0x4C + csz])
    if len(d) != usz:
        raise ValueError("DCX size mismatch")
    return d


def _bnd4(b):
    if b[:4] != b"BND4":
        raise ValueError("not BND4")
    count, = struct.unpack_from("<I", b, 0x0C)
    esz, = struct.unpack_from("<Q", b, 0x20)
    out = {}
    for i in range(count):
        o = 0x40 + i * esz
        csz, = struct.unpack_from("<q", b, o + 8)
        off, _fid, noff = struct.unpack_from("<IiI", b, o + 0x18)
        e = noff
        while e + 1 < len(b) and b[e:e + 2] != b"\0\0":
            e += 2
        if off + csz > len(b):
            raise ValueError("BND4 entry out of range")
        out[b[noff:e].decode("utf-16le")] = b[off:off + csz]
    return out


def _fmg(b):
    """{id: text} of a FMG v2 file."""
    groups, = struct.unpack_from("<i", b, 0x0C)
    table, = struct.unpack_from("<q", b, 0x18)
    out = {}
    for g in range(groups):
        idx, first, last = struct.unpack_from("<Iii", b, 0x28 + g * 0x10)
        for k in range(last - first + 1):
            so, = struct.unpack_from("<q", b, table + 8 * (idx + k))
            if so <= 0:
                continue
            e = so
            while e + 1 < len(b) and b[e:e + 2] != b"\0\0":
                e += 2
            text = b[so:e].decode("utf-16le", "replace").strip()
            if not _PLACEHOLDER.match(text):
                out[first + k] = text
    return out


def _param_ids(b):
    cnt, = struct.unpack_from("<H", b, 0x0A)
    return [struct.unpack_from("<i", b, 0x40 + i * 0x18)[0] for i in range(cnt)]


def _entry(bnd, suffix):
    for k, v in bnd.items():
        if k.replace("/", "\\").split("\\")[-1] == suffix:
            return v
    raise KeyError(suffix)


def read_game(game_dir, lang="engus"):
    """Parse the game's files: {"names": {kind: {id: name}}, "ids": {kind: [id, ...]}}."""
    game = Path(game_dir)
    msg = _bnd4(_dcx((game / "msg" / lang / "item.msgbnd.dcx").read_bytes()))
    par = _bnd4(_dcx((game / "param" / "gameparam" / "gameparam.parambnd.dcx").read_bytes()))
    names = {kind: _fmg(_entry(msg, fn + ".fmg")) for kind, fn in FMG_FILES.items()}
    names["npc"] = _fmg(_entry(msg, NPC_NAMES + ".fmg"))
    menu = _bnd4(_dcx((game / "msg" / lang / "menu.msgbnd.dcx").read_bytes()))
    names["lamp"] = {i: t for i, t in _fmg(_entry(menu, EVENT_TEXT + ".fmg")).items() if 70002100 <= i < 70003700}
    names["gem"] = _fmg(_entry(menu, GEM_NAMES + ".fmg"))
    names["effect"] = _fmg(_entry(menu, GEM_EFFECTS + ".fmg"))
    ids = {kind: sorted(_param_ids(_entry(par, pn + ".param"))) for kind, pn in PARAMS.items()}
    data = {"names": names, "ids": ids}
    from .params import Params  # params reads its files with this module's helpers
    try:  # hand, icon and gem rows need the paramdefs too; without them the editor still has names
        p = Params(game)
        data["goods_sort"] = {i: v[0] for i, v in p.table("EquipParamGoods", ["sortId"]).items()}
        data["weapon_hands"] = {i: "left" if left and not right else "right"
                                for i, (right, left) in p.table("EquipParamWeapon",
                                    ["rightHandEquipable", "leftHandEquipable"]).items()}
        data["icons"] = {kind: {i: v[0] for i, v in p.table(pn, ["iconIdM" if kind == "armor" else "iconId"]).items()
                                if v[0] > 0 and not (kind == "weapon" and (i % 10000) // 100)}
                         for kind, pn in PARAMS.items()}
        negative = {i for i, v in p.table("GemCategoryParam", ["isNegative"]).items() if v[0]}
        # effect id -> [cursed, icon offset, name offset, sort category (0 blood gem, 1 Caryll rune, 2 oath rune)]
        data["effects"] = {i: [int(cat in negative), icon, name, sort] for i, (cat, icon, name, sort) in
                           p.table("GemeffectParam", ["categoryId", "gemIconIdOffset", "gemNameIdOffset",
                                                      "sortCategory"]).items()}
        data["gemgen"] = {i: v[0] for i, v in p.table("GemGenParam", ["gemNameIdOffset"]).items()}
        # Rows that make one fixed effect: id -> [name offset, shapes (bit per slotTypeRate column A-F), effect id].
        # The shape and effect of every gem and rune in a party save follow from these (see Catalogue.caryll_runes).
        cols = ["gemNameIdOffset"] + [f"slotTypeRate{c}" for c in "ABCDEF"] + ["gemeffectGenParamType_0", "gemeffectGenParam_0"]
        data["gemrows"] = {i: [v[0], sum(1 << k for k in range(6) if v[1 + k] > 0), v[8]]
                           for i, v in p.table("GemGenParam", cols).items()
                           if v[7] == 1 and v[8] > 0}
        # Effects the game can produce: a fixed effect of a generator row (type 1) or any effect of a category a
        # generator draws from (type 2). Rows no generator reaches are cut or unused content and are not offered.
        gen_cols = [f"gemeffectGenParam{k}_{i}" for i in range(6) for k in ("Type", "")]
        fixed, drawn = set(), set()
        for v in p.table("GemGenParam", gen_cols).values():
            for i in range(6):
                if v[2 * i] == 1 and v[2 * i + 1] > 0:
                    fixed.add(v[2 * i + 1])
                elif v[2 * i] == 2 and v[2 * i + 1] > 0:
                    drawn.add(v[2 * i + 1])
        data["reach"] = sorted(fixed | {e for e, (cat, *_rest) in p.table("GemeffectParam", ["categoryId"]).items()
                                        if cat in drawn})
    except (OSError, ValueError, KeyError, struct.error, zlib.error):
        pass
    return data


MOD_PREFIX = "Nocturne"  # the mod folders whose new items the editor's Nocturne list offers


def read_mod_items(game_dir, mods_dir, config, lang="engus"):
    """{(category, id): [name, sortId or None, icon id or None]} the installed, enabled Nocturne mod adds: goods (4),
    weapons (0, the +0 rows) and armour (1) whose rows the game folder does not have. Empty when no such mod, no game
    paramdefs or anything unreadable."""
    from . import params
    import mods  # scripts/mods.py (scripts is on the path: saveedit_cli.py and the tests)
    game = Path(game_dir)
    if not (game / "msg").is_dir() and (game / "dvdroot_ps4" / "msg").is_dir():
        game = game / "dvdroot_ps4"
    out = {}
    try:
        vanilla = _bnd4(_dcx((game / "param" / "gameparam" / "gameparam.parambnd.dcx").read_bytes()))
        for name in mods.selected(mods_dir, config):
            if not name.casefold().startswith(MOD_PREFIX.casefold()):
                continue
            base, prefix = mods.content_root(Path(mods_dir) / name)
            root = mods.child(base, "dvdroot_ps4") if not prefix else base  # prefix: the game folders sit in base
            gameparam = mods.child(mods.child(mods.child(root, "param"), "gameparam"), "gameparam.parambnd.dcx")
            msg = _bnd4(_dcx(mods.child(mods.child(mods.child(root, "msg"), lang), "item.msgbnd.dcx").read_bytes()))
            table = params.Params(game, gameparam)
            for category, kind in ((4, "goods"), (0, "weapon"), (1, "armor")):
                known = set(_param_ids(_entry(vanilla, PARAMS[kind] + ".param")))
                texts = _fmg(_entry(msg, FMG_FILES[kind] + ".fmg"))
                icon_field = "iconIdM" if kind == "armor" else "iconId"
                rows = table.table(PARAMS[kind], ["sortId", icon_field])
                for i, (sort_id, icon) in rows.items():
                    if kind == "weapon" and (i % 10000) // 100:
                        continue  # the +0 row names the weapon; its level rows follow below
                    if i not in known and texts.get(i) and not _PLACEHOLDER.match(texts[i]):
                        out[(category, i)] = [texts[i], sort_id if kind == "goods" else None, icon]
                if kind == "weapon":  # the level rows of a new weapon: the save editor raises its level by them
                    for i in rows:
                        base = (0, i - (i % 10000) // 100 * 100)
                        if (i % 10000) // 100 and i not in known and base in out:
                            out[(0, i)] = out[base]
    except (OSError, ValueError, KeyError, TypeError, struct.error, zlib.error):
        return {}
    return out


def _stamp(game, lang):
    out = []
    for p in (game / "msg" / lang / "item.msgbnd.dcx", game / "param" / "gameparam" / "gameparam.parambnd.dcx",
              game / "msg" / lang / "menu.msgbnd.dcx", game / "paramdef" / "paramdef.paramdefbnd.dcx"):
        try:
            st = p.stat()
            out += [st.st_size, int(st.st_mtime)]
        except OSError:
            out += [0, 0]
    return out


# Rows the game has no item name for (cut content and a few boss weapon variants), named by Noxde's editor.
_FALLBACK = json.loads((Path(__file__).with_name("fallbacknames.json")).read_text(encoding="utf-8"))


# Two armour rows the game gives one name: the garb with the cape and the one without. The caped row is named so, as
# Noxde's editor does; else both read "Hunter Garb" and the capeless one looks missing from the Add item list.
_CAPED = {71000: ("Hunter Garb", "Hunter Garb (Cape)"), 191000: ("Student Uniform", "Student Uniform (Cape)")}


class Catalogue:
    """Names and ids for the UI. Empty (ok False) when the game files cannot be read."""

    KIND_OF_CATEGORY = {0: "weapon", 1: "armor", 2: "accessory", 4: "goods"}

    def __init__(self, data=None):
        self.ok = bool(data)
        self._names = {k: {int(i): n for i, n in v.items()} for k, v in (data or {}).get("names", {}).items()}
        self._ids = {k: set(v) for k, v in (data or {}).get("ids", {}).items()}
        self._icons = {k: {int(i): n for i, n in v.items()} for k, v in (data or {}).get("icons", {}).items()}
        self._effects = {int(i): v for i, v in (data or {}).get("effects", {}).items()}
        self._gemgen = {int(i): v for i, v in (data or {}).get("gemgen", {}).items()}
        self._gemrows = {int(i): v for i, v in (data or {}).get("gemrows", {}).items()}
        self._goods_sort = {int(i): v for i, v in (data or {}).get("goods_sort", {}).items()}
        self.mod_items = {}  # (category, id) -> [name, goods sortId, icon id] of the Nocturne mod (set_mod_items)
        self._reach = set((data or {}).get("reach", []))
        self._weapon_hands = {int(i): v for i, v in (data or {}).get("weapon_hands", {}).items()}

    @classmethod
    def load(cls, game_dir, cache_dir, lang="engus"):
        """From the cache when the game files are unchanged, else read and cache; empty on failure."""
        if not game_dir:
            return cls()
        game, cache = Path(game_dir), Path(cache_dir) / f"saveedit_names_{lang}.json"
        if not (game / "msg").is_dir() and (game / "dvdroot_ps4" / "msg").is_dir():
            game = game / "dvdroot_ps4"  # the folder with eboot.bin holds the game data in dvdroot_ps4
        stamp = _stamp(game, lang)
        try:
            c = json.loads(cache.read_text(encoding="utf-8"))
            if c.get("version") == CACHE_VERSION and c.get("stamp") == stamp:
                return cls(c["data"])
        except (OSError, ValueError, KeyError):
            pass
        try:
            data = read_game(game, lang)
        except (OSError, ValueError, KeyError, struct.error, zlib.error):
            return cls()
        try:
            cache.parent.mkdir(parents=True, exist_ok=True)
            cache.write_text(json.dumps({"version": CACHE_VERSION, "stamp": stamp, "data": data}), encoding="utf-8")
        except OSError:
            pass
        return cls(data)

    def set_mod_items(self, items):
        self.mod_items = dict(items)

    def goods_sort(self, number):
        """The goods row's sortId (menu order), from the mod's row when the mod adds the item; None when unknown."""
        mod = self.mod_items.get((4, number))
        return mod[1] if mod else self._goods_sort.get(number)

    def search_mod_items(self, text=""):
        """[(category, id, name)] of the mod's items matching text."""
        text = text.lower()
        return [(c, i, n) for (c, i), (n, *_) in sorted(self.mod_items.items(), key=lambda kv: (kv[0][0] != 4, kv[0][0], kv[0][1]))
                if not (c == 0 and (i % 10000) // 100) and (text in n.lower() or text in str(i))]  # weapons at +0 only

    def name(self, category, number):
        kind = self.KIND_OF_CATEGORY.get(category)
        if kind is None:
            return None
        n = self._names.get(kind, {})
        if kind == "weapon":
            number = number - (number % 10000) // 100 * 100  # id = base + 100 * reinforcement level
        elif kind == "armor" and number in _CAPED and n.get(number) == _CAPED[number][0]:
            return _CAPED[number][1]
        mod = self.mod_items.get((category, number))
        return n.get(number) or (mod[0] if mod else None) or self.fallback_name(category, number)

    def fallback_name(self, category, number):
        """Name of a row the game does not name (cut content), from Noxde's list; such rows are shown, never added."""
        kind = self.KIND_OF_CATEGORY.get(category)
        if kind == "weapon":
            number = number - (number % 10000) // 100 * 100
        return _FALLBACK.get(kind, {}).get(str(number)) if kind in ("armor", "weapon") else None

    def weapon_hand(self, number):
        """Equip hand; unknown or dual-hand rows stay in the right-hand filter."""
        base = number - (number % 10000) // 100 * 100
        return self._weapon_hands.get(number, self._weapon_hands.get(base, "right"))

    def icon(self, category, number):
        """Icon id of an inventory item (the menu sheets' numbering), None when unknown."""
        kind = self.KIND_OF_CATEGORY.get(category)
        if kind == "weapon":
            number = number - (number % 10000) // 100 * 100
        mod = self.mod_items.get((category, number))
        return self._icons.get(kind, {}).get(number) or (mod[2] if mod else None)

    # Gem shape bit -> the digit in the gem name key and the icon block (checked on all 77 gems and runes of a
    # level 90 save: name key = GemGen.gemNameIdOffset + digit * 1e6 + 1e5 if cursed + first effect's
    # gemNameIdOffset; icon = 3000 + block + 100 if cursed + first effect's gemIconIdOffset).
    SHAPES = {1: (1, 0), 2: (2, 200), 4: (3, 400), 8: (4, 600), 63: (6, 1000)}

    def effect(self, eid):
        """[cursed, icon offset, name offset, sort category] of a GemeffectParam row, None when unknown."""
        return self._effects.get(eid)

    def effect_text(self, eid):
        return self._names.get("effect", {}).get(eid)

    def gem(self, gen_id, shape, effects):
        """(name, icon id, cursed) of a gem or rune from its GemGen row, shape and effect ids."""
        rows = [self._effects.get(e) for e in effects]
        if not rows or rows[0] is None or shape not in self.SHAPES:
            return None, None, False
        cursed = any(r and r[0] for r in rows)
        digit, block = self.SHAPES[shape]
        key = self._gemgen.get(gen_id, 0) + digit * 1000000 + (100000 if cursed else 0) + rows[0][2]
        return self._names.get("gem", {}).get(key), 3000 + block + (100 if cursed else 0) + rows[0][1], cursed

    # New records: the shape column of a GemGen row (checked on all 11 gems and runes of a party save: radial = column
    # A, triangle = B; saves also hold waning = 4 and circle = 8 blood gems). Every shape a blood gem can have is offered.
    ADD_SHAPES = (1, 2, 4, 8, 63)
    CARYLL_NAME_OFFSET = 10000000

    # GemGen rows of name offset 0 that roll one shape: 127000 radial and 127002 triangle (the gems in a party save use
    # them), 128000 waning, 123000 circle, 6 droplet (the only row that rolls 63).
    BLOOD_GENS = {1: 127000, 2: 127002, 4: 128000, 8: 123000, 63: 6}

    def blood_gem_gen(self, shape):
        """GemGen id for a new blood gem of this shape, None when the game's row is not what the saves show."""
        gen = self.BLOOD_GENS.get(shape)
        row = self._gemrows.get(gen)
        return gen if row and row[0] == 0 and row[1] == shape else None

    # Rune GemGen rows by sort category: (ids, shape). Caryll runes are 1xxxxx rows that roll radial (the rows the runes
    # in a save come from); oath runes are the 2000x0 rows that roll triangle, the oath slot's shape, and only that shape
    # gives their names. The 4xx and 2xxxxxx rows are other tables.
    RUNE_ROWS = {1: (range(100000, 200000), 1), 2: (range(200000, 300000), 2)}

    def caryll_runes(self):
        """[(GemGen id, effect id, shape)] of the Caryll and oath runes that can be added, in row order: the lowest row
        of the Caryll name offset in RUNE_ROWS for one fixed rune effect of that sort whose rune has a name."""
        best = {}
        for gen, (offset, shapes, eff) in sorted(self._gemrows.items()):
            sort = (self._effects.get(eff) or [None] * 4)[3]
            ids, shape = self.RUNE_ROWS.get(sort, ((), 0))
            if (offset == self.CARYLL_NAME_OFFSET and shapes == shape and gen in ids
                    and eff in self._names.get("effect", {}) and self.gem(gen, shape, [eff])[0] is not None):
                best.setdefault(eff, (gen, shape))
        return sorted((gen, eff, shape) for eff, (gen, shape) in best.items())

    def gem_icon_ids(self):
        """Every icon a blood gem can show, so a preview never waits for the icon sheet."""
        return {3000 + block + cursed + row[1] for _, block in self.SHAPES.values() for cursed in (0, 100)
                for row in self._effects.values() if row[3] == 0}

    def effects_for(self, sort_category):
        """[(id, text)] of the named effects a gem of this sort category can carry, in id order. Only effects the game can
        produce (see "reach" in read_game); a catalogue without that list offers every named one."""
        texts = self._names.get("effect", {})
        return [(i, texts[i]) for i, v in sorted(self._effects.items())
                if v[3] == sort_category and i in texts and (not self._reach or i in self._reach)]

    def lamp_name(self, flag):
        """Name of the lamp whose lit flag is 7AAB02nn (the warp list text 7000AABn), None when unknown."""
        return self._names.get("lamp", {}).get(70000000 + (flag // 10000 % 1000) * 10 + flag % 100)

    def boss_name(self, chr_id):
        return self._names.get("npc", {}).get(chr_id)

    def valid_ids(self, category):
        """The set of row ids the game has for this category (None when unknown). Weapons include the rows the
        enabled Nocturne mod adds (every level), so a mod weapon's level can be raised."""
        ids = self._ids.get(self.KIND_OF_CATEGORY.get(category))
        if ids and category == 0:
            ids = ids | {i for c, i in self.mod_items if c == 0}
        return ids or None

    def known(self, category, number):
        ids = self.valid_ids(category)
        return None if ids is None else number in ids

    def search(self, category, text=""):
        """[(id, name)] named entries of one category matching text, then the unnamed ones last."""
        kind = self.KIND_OF_CATEGORY.get(category)
        text = text.lower()
        named, unnamed = [], []
        for i in sorted(self._ids.get(kind, ())):
            if kind == "weapon" and (i % 10000) // 100:
                continue  # list the +0 row; levels are set on the item
            n = self.name(category, i)
            if n is None:
                if text in str(i):
                    unnamed.append((i, f"(unnamed {i})"))
            elif text in n.lower() or text in str(i):
                named.append((i, n))
        return named + unnamed
