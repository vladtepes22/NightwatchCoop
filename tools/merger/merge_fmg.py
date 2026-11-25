"""FMG merge: one unit per text id, against the game's FMG (a text id a mod lacks counts as deleted)."""
import os

from common import containers, show_text


def shown_values(values):
    """[(mod, shown text)]: texts that start alike are shown from a little before where they first differ."""
    texts = [v for _, v in values if v is not None]
    common = os.path.commonprefix(texts) if len(texts) > 1 else ""
    cut = len(common) - 15 if len(common) > 40 else 0
    return [(m, show_text(v if v is None or not cut else "..." + v[cut:])) for m, v in values]


def merge_fmg(label, vanilla, mods, report):
    """`vanilla`: the game's FMG bytes, or None for an FMG the game lacks (every text is then new); `mods`: [(mod, FMG
    bytes)] in merge order. Returns (FMG bytes, texts taken from mods)."""
    parsed = [(mod, containers.fmg_read(data)) for mod, data in mods]
    if vanilla is not None:
        base = containers.fmg_read(vanilla)
    else:
        first = parsed[0][1]
        base = {"header": first["header"], "texts": {}, "ranges": [], "size": 0}
    texts = dict(base["texts"])
    changed = {}  # id -> [(mod, text)]
    for mod, fmg in parsed:
        own = fmg["texts"]
        for text_id in sorted(set(own) | set(base["texts"])):
            mine = own.get(text_id)
            if mine != base["texts"].get(text_id):
                changed.setdefault(text_id, []).append((mod, mine))
    for text_id in sorted(changed):
        values = changed[text_id]
        if len({v for _, v in values}) > 1:
            report.conflict("%s text %d" % (label, text_id), shown_values(values), values[-1][0])
        final = values[-1][1]
        if final is None and text_id not in base["texts"]:
            texts.pop(text_id, None)
        else:
            texts[text_id] = final
    out = dict(base)
    out["texts"] = texts
    return containers.fmg_write(out), len(changed)
