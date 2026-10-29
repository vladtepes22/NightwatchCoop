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
CACHE_VERSION = 3
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
    names["lamp"] = {i: t for i, t in _fmg(_entry(menu, EVENT_TEXT + ".fmg")).items() if 70002100 <= i < 70003600}
    names["gem"] = _fmg(_entry(menu, GEM_NAMES + ".fmg"))
    names["effect"] = _fmg(_entry(menu, GEM_EFFECTS + ".fmg"))
    ids = {kind: sorted(_param_ids(_entry(par, pn + ".param"))) for kind, pn in PARAMS.items()}
    data = {"names": names, "ids": ids}
    from .params import Params  # params reads its files with this module's helpers
    try:  # icons and gem rows need the paramdefs too; without them the editor still has names
        p = Params(game)
        data["icons"] = {kind: {i: v[0] for i, v in p.table(pn, ["iconIdM" if kind == "armor" else "iconId"]).items()
                                if v[0] > 0 and not (kind == "weapon" and (i % 10000) // 100)}
                         for kind, pn in PARAMS.items()}
        negative = {i for i, v in p.table("GemCategoryParam", ["isNegative"]).items() if v[0]}
        # effect id -> [cursed, icon offset, name offset, sort category (0 blood gem, 1 Caryll rune, 2 oath rune)]
        data["effects"] = {i: [int(cat in negative), icon, name, sort] for i, (cat, icon, name, sort) in
                           p.table("GemeffectParam", ["categoryId", "gemIconIdOffset", "gemNameIdOffset",
                                                      "sortCategory"]).items()}
        data["gemgen"] = {i: v[0] for i, v in p.table("GemGenParam", ["gemNameIdOffset"]).items()}
    except (OSError, ValueError, KeyError, struct.error, zlib.error):
        pass
    return data


def _stamp(game, lang):
    out = []
    for p in (game / "msg" / lang / "item.msgbnd.dcx", game / "param" / "gameparam" / "gameparam.parambnd.dcx",
              game / "msg" / lang / "menu.msgbnd.dcx"):
        try:
            st = p.stat()
            out += [st.st_size, int(st.st_mtime)]
        except OSError:
            out += [0, 0]
    return out


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

    def name(self, category, number):
        kind = self.KIND_OF_CATEGORY.get(category)
        if kind is None:
            return None
        n = self._names.get(kind, {})
        if kind == "weapon":
            number = number - (number % 10000) // 100 * 100  # id = base + 100 * reinforcement level
        elif kind == "armor":
            pass
        return n.get(number)

    def icon(self, category, number):
        """Icon id of an inventory item (the menu sheets' numbering), None when unknown."""
        kind = self.KIND_OF_CATEGORY.get(category)
        if kind == "weapon":
            number = number - (number % 10000) // 100 * 100
        return self._icons.get(kind, {}).get(number)

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

    def effects_for(self, sort_category):
        """[(id, text)] of the named effects a gem of this sort category can carry, in id order."""
        texts = self._names.get("effect", {})
        return [(i, texts[i]) for i, v in sorted(self._effects.items()) if v[3] == sort_category and i in texts]

    def lamp_name(self, flag):
        """Name of the lamp whose lit flag is 7AAB02nn (the warp list text 7000AABn), None when unknown."""
        return self._names.get("lamp", {}).get(70000000 + (flag // 10000 % 1000) * 10 + flag % 100)

    def boss_name(self, chr_id):
        return self._names.get("npc", {}).get(chr_id)

    def valid_ids(self, category):
        """The set of row ids the game has for this category (None when unknown)."""
        return self._ids.get(self.KIND_OF_CATEGORY.get(category)) or None

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
