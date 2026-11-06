"""Adding items and editing gems: the checks that stand between the editor's UI and the SaveModel.

The record layouts are in model.py (adding) and instances.py (gems); the id lists come from the player's own
game (names.Catalogue). Every function checks first and writes nothing when a check fails. Adding a weapon,
armour piece or gem moves the data behind the item instance table, so callers add last and re-read the
instance table (instances.read) after every add.
Reference: Noxde's Bloodborne-save-editor (GPL-3.0) and the Nightwatch save tools (saveedit_cli.py), whose
rules these follow.
"""
from . import instances

ADD_CATEGORIES = (0, 1, 4)  # weapons, armour, goods
ADD_MAX_COUNT = 99
GEM_EFFECTS = 3  # blood gem effects the game rolls (the record has room for 6)
# Weapon rows the game keeps in every inventory and never shows: 1000 holds the Memory Altar's runes and the oath
# in its slots, 1000000 (Bare Fists) fills the empty hand slots. Never listed, never editable.
PLACEHOLDERS = {(0, 1000), (0, 1000000)}


def max_level(catalogue, item):
    """Highest reinforcement level the game has a row for, None when the item or the game list is unknown."""
    ids = catalogue.valid_ids(0)
    if item.level is None or not ids:
        return None
    base = item.number - item.level * 100
    return max((lv for lv in range(item.level + 11) if base + 100 * lv in ids), default=None)


def addable(catalogue, category, number):
    """[(id, name)] helper: True when the game has this row and a name for it (cut content has none)."""
    return ((category, number) not in PLACEHOLDERS and bool(catalogue.known(category, number))
            and catalogue.name(category, number) is not None)


def catalogue_rows(catalogue, category, text="", limit=200):
    """[(id, name)] of what Add item offers in one category (0 weapon, 1 armour, 4 goods)."""
    if category not in ADD_CATEGORIES:
        raise ValueError("weapons (0), armour (1) or goods (4)")
    rows = [(n, name) for n, name in catalogue.search(category, text) if addable(catalogue, category, n)]
    return rows[:limit]


def add_item(model, catalogue, category, number, count=1):
    if category not in ADD_CATEGORIES or not isinstance(number, int) or not addable(catalogue, category, number):
        raise ValueError("the game has no such item")
    if category == 0 and (number % 10000) // 100:
        raise ValueError("add a weapon at +0; raise its level afterwards")
    if category == 4:
        top = min(ADD_MAX_COUNT, catalogue.goods_max(number) or ADD_MAX_COUNT)
        if not isinstance(count, int) or not 1 <= count <= top:
            raise ValueError(f"the game holds 1 to {top} of this item")
        model.add_goods(number, count, catalogue.goods_sort(number))
    else:
        model.add_gear(category, number)


# --- gems and runes -------------------------------------------------------------------------------------------
def gem_sort(gem, catalogue):
    """The GemeffectParam sort category a gem's effects come from: 0 blood gem, 1 Caryll rune, 2 oath rune."""
    if gem.type == 1:
        return 0
    first = catalogue.effect(gem.effects[0]) if gem.effects else None
    return first[3] if first else None


def gem_label(gem, catalogue):
    """(name or None, cursed) of a gem record."""
    name, _icon, cursed = catalogue.gem(gem.gen, gem.shape, gem.effects)
    return name, cursed


def gem_rows(model, catalogue):
    """[dict(handle, name, type, shape, effects [(id, text)], where)] of the loose gems and runes in the inventory
    plus the ones sitting in a weapon's slot. [] when the save's instance table does not resolve."""
    table = instances.read(model)
    if table is None:
        return []
    out = []

    def row(gem, where):
        name, cursed = gem_label(gem, catalogue)
        return dict(handle=gem.handle, name=name or f"(unnamed gem {gem.gen})", type=gem.type, shape=gem.shape,
                    cursed=cursed, effects=[(e, catalogue.effect_text(e) or f"(effect {e})") for e in gem.effects],
                    where=where)

    for it in model.items((0,)):
        if it.category == 8 and it.handle in table.gems:
            out.append(row(table.gems[it.handle], "inventory"))
    owned = {i.handle: i for i in model.items((0,)) if i.category == 0}
    for gh, wh in sorted(table.socketed().items()):
        if wh in owned:
            out.append(row(table.gems[gh], "in " + (catalogue.name(0, owned[wh].number) or str(owned[wh].number))))
    return out


def set_gem_effects(model, catalogue, handle, effects):
    """Replace a blood gem's effects (1 to 3, all of the gem's sort category, ones the game can roll or ones it
    already has). Runes carry hidden effects that make the rune and are not edited."""
    table = instances.read(model)
    if table is None or handle not in table.gems or not catalogue.effects_for(0):
        raise ValueError("gem effects need the save's gem table and the game's gem list")
    gem = table.gems[handle]
    if gem.type != 1:
        raise ValueError("only blood gems can be edited")
    pool = {e for e, _ in catalogue.effects_for(0)}
    if (not isinstance(effects, (list, tuple)) or not 1 <= len(effects) <= GEM_EFFECTS
            or not all(type(e) is int for e in effects)
            or any((catalogue.effect(e) or [None] * 4)[3] != 0 for e in effects)
            or any(e not in pool and e not in gem.effects for e in effects)):
        raise ValueError("an effect does not fit this gem")
    instances.set_effects(model, gem, list(effects))


def add_blood_gem(model, catalogue, shape, effects):
    gen = catalogue.blood_gem_gen(shape) if shape in catalogue.ADD_SHAPES else None
    texts = {e for e, _ in catalogue.effects_for(0)}
    if (gen is None or not isinstance(effects, (list, tuple)) or not 1 <= len(effects) <= GEM_EFFECTS
            or not all(type(e) is int and e in texts for e in effects)):
        raise ValueError("an effect or the shape does not fit a new blood gem")
    model.add_gem(gen, 1, shape, list(effects))


def rune_rows(catalogue):
    """[(rune effect id, name, shape)] of the Caryll and oath runes that can be added."""
    out = []
    for gen, effect, shape in catalogue.caryll_runes():
        name, _icon, _cursed = catalogue.gem(gen, shape, [effect])
        if name is not None:
            out.append((effect, name, shape))
    return out


def add_rune(model, catalogue, effect):
    runes = {e: (g, s) for g, e, s in catalogue.caryll_runes()}
    if type(effect) is not int or effect not in runes:
        raise ValueError("the game has no such rune")
    gen, shape = runes[effect]
    model.add_gem(gen, 2, shape, catalogue.rune_effects(gen))  # every fixed effect of the row, as the game writes


def copy_gem(model, handle):
    table = instances.read(model)
    if table is None or handle not in table.gems:
        raise ValueError("invalid gem to copy")
    model.copy_gem(table.gems[handle].at)
