"""What an Apply will change, in words: the list the editor shows before it writes (and logs after).

Compares the save as opened (a SaveModel of model.orig) with the edited model, field by field, so the player
sees "+ Saw Cleaver", "Blood Echoes 500 -> 9000", "3 event flags set" instead of byte ranges. Pure Python.
"""
from . import model as M, instances as I, flagsdata as F

MAX_LINES = 40


def _inv(model):
    """{(section, handle): (iid, count)} of every inventory entry."""
    return {(i.section, i.handle): (i.iid, i.count) for i in model.items()}


def _name(catalogue, iid):
    category, number = iid >> 28, iid & 0x0FFFFFFF
    base = catalogue.name(category, number) if catalogue is not None else None
    if base is None:
        return f"{M.CATEGORIES.get(category, 'item')} {number}"
    if category == 0 and number >= 1000000 and (number % 10000) // 100:
        return f"{base} +{(number % 10000) // 100}"
    return base


def _flag_bits(old, new):
    """(set, cleared) bit counts of the event flag section."""
    a = bytes(old.d[old.pool:old.pool + M.FLAG_SECTION - 1])
    b = bytes(new.d[new.pool:new.pool + M.FLAG_SECTION - 1])
    if a == b:
        return 0, 0
    on = off = 0
    for x, y in zip(a, b):
        if x != y:
            on += bin(y & ~x & 0xFF).count("1")
            off += bin(x & ~y & 0xFF).count("1")
    return on, off


def changes(model, catalogue=None):
    """[str] of the differences between model.orig and the model now (empty: nothing changed)."""
    old = M.SaveModel(model.orig)
    out = []
    for label, a, b in (("Name", old.name, model.name), ("Blood Echoes", old.echoes, model.echoes),
                        ("Insight", old.insight, model.insight), ("NG cycle", old.ng_cycle, model.ng_cycle),
                        ("Phantom mark", old.phantom_mark, model.phantom_mark)):
        if a != b:
            out.append(f"{label}: {a} -> {b}")
    for k in M.STAT_NAMES:
        if old.stats[k] != model.stats[k]:
            out.append(f"{k.capitalize()}: {old.stats[k]} -> {model.stats[k]}")
    if old.level != model.level:
        out.append(f"Level: {old.level} -> {model.level}")
    a, b = _inv(old), _inv(model)
    for key in sorted(set(a) | set(b)):
        if key not in a:
            out.append(f"+ {_name(catalogue, b[key][0])}" + (f" x{b[key][1]}" if b[key][1] > 1 else ""))
        elif key not in b:
            out.append(f"- {_name(catalogue, a[key][0])}")
        elif a[key] != b[key]:
            before, after = a[key], b[key]
            if before[0] != after[0]:
                out.append(f"{_name(catalogue, before[0])} -> {_name(catalogue, after[0])}")
            if before[1] != after[1]:
                out.append(f"{_name(catalogue, after[0])}: count {before[1]} -> {after[1]}")
    t0, t1 = I.read(old), I.read(model)
    if t0 is not None and t1 is not None:
        for handle, gem in t1.gems.items():
            if handle in t0.gems and t0.gems[handle].effects != gem.effects:
                out.append(f"Blood gem {handle & 0xFFFF:#x}: effects {t0.gems[handle].effects} -> {gem.effects}")
    named = [(f, label) for f, _g, label in F.bosses(catalogue) + F.lamps(catalogue) + F.endings()]
    for f, label in named:
        a1, b1 = old.flag(f), model.flag(f)
        if a1 is not None and a1 != b1:
            out.append(f"{'Set' if b1 else 'Cleared'}: {label}")
    on, off = _flag_bits(old, model)
    if on or off:
        out.append(f"Event flag bits: {on} set, {off} cleared")
    if len(out) > MAX_LINES:
        out = out[:MAX_LINES] + [f"... and {len(out) - MAX_LINES} more"]
    return out
