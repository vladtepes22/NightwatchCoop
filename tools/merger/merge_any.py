"""One file's merge: DCX unwrap, the type by its content, BND4 per inner file (recursing), and the fallback to the later
mod's whole file when a type cannot be merged."""
import hashlib
import os
import struct
import zlib

from common import MergeError, containers
from merge_fmg import merge_fmg
from merge_hks import merge_hks
from merge_param import merge_param
from merge_tae import merge_tae
from merge_tpf import merge_tpf

# What each type counts in the MERGED line.
UNIT = {"param": "rows", "fmg": "texts", "tpf": "textures", "tae": "animations", "hks": "functions",
        "bnd": "inner files"}


def is_dcx(data):
    return data is not None and data[:4] == b"DCX\0"


def unpack(data):
    """(payload, DCX header or None). The engine reads a file under a .dcx name that is not DCX as it is."""
    if not is_dcx(data):
        return data, None
    try:
        return containers.dcx_read(data)
    except (ValueError, struct.error, zlib.error) as error:
        raise MergeError("DCX cannot be unpacked: %s" % error)


def kind_of(name, payload):
    low = name.lower()
    if payload[:4] == b"BND4":
        return "bnd"
    if payload[:4] == b"TPF\0":
        return "tpf"
    if payload[:4] == b"TAE ":
        return "tae"
    if low.endswith(".fmg"):
        return "fmg"
    if low.endswith(".param"):
        return "param"
    if low.endswith(".hks"):
        return "hks"
    return None


WHY = {
    ".msb": "map layouts are not merged", ".emevd": "event scripts are not merged",
    ".hkx": "Havok files are not merged", ".flver": "models are not merged", ".fxr": "effects are not merged",
    ".lua": "compiled AI scripts are not merged", ".gfx": "menu files are not merged",
}


def why_not(name):
    low = name.lower()
    if low.endswith(".dcx"):
        low = low[:-4]
    for ext, why in WHY.items():
        if low.endswith(ext) or ext + "." in low:
            return why
    if "behbnd" in low:
        return "behaviour graphs are not merged"
    return "this file type is not merged"


class Context:
    """What a merge needs besides the files: the game's paramdefs and item names for log lines."""

    def __init__(self, paramdefs, item_name=None):
        self.paramdefs = paramdefs
        self.item_name = item_name


def merge_payload(report, place, name, vanilla, mods, ctx, counts):
    """Merges decompressed payloads. `mods`: [(mod, payload)], each different from `vanilla` (None when the game has
    no such file), at least two. Returns the merged payload; raises MergeError when the type cannot be merged."""
    kinds = {kind_of(name, data) for _, data in mods}
    if vanilla is not None:
        kinds.add(kind_of(name, vanilla))
    if len(kinds) != 1 or None in kinds:
        raise MergeError(why_not(name) if kinds == {None} else "the mods' files are of different types")
    kind = kinds.pop()
    if kind == "bnd":
        data, taken = merge_bnd(report, place, vanilla, mods, ctx, counts)
    elif kind == "param":
        if vanilla is None:
            raise MergeError("a PARAM the game does not have")
        table = place or os.path.splitext(os.path.basename(name.replace("\\", "/")))[0]
        try:
            data, taken = merge_param(table, vanilla, mods, report, ctx.paramdefs, ctx.item_name)
        except (ValueError, struct.error) as error:
            raise MergeError("the game's table cannot be read: %s" % error)
    elif kind == "fmg":
        try:
            data, taken = merge_fmg(place, vanilla, mods, report)
        except (ValueError, struct.error, IndexError, UnicodeDecodeError) as error:
            raise MergeError("an FMG cannot be read: %s" % error)
    elif kind == "tpf":
        data, taken = merge_tpf(place, vanilla, mods, report)
    elif kind == "tae":
        data, taken = merge_tae(place, vanilla, mods, report)
    else:
        data, taken = merge_hks(place, os.path.basename(name.replace("\\", "/")), vanilla, mods, report)
    counts[UNIT[kind]] = counts.get(UNIT[kind], 0) + taken
    return data


def _path_key(entry):
    return entry[2].replace("/", "\\").lower()


def _file_key(entry):
    return _path_key(entry).rsplit("\\", 1)[-1]


def _key_function(bnds):
    """Inner files are matched by file name when every binder names each file once (a mod's msgbnd may carry
    another language's folder in its paths: engUS where the game says engGB), else by the whole path."""
    for bnd in bnds:
        names = [_file_key(e) for e in bnd["entries"]]
        if len(set(names)) != len(names):
            return _path_key
    return _file_key


def _same_entry(a, b):
    if a is None or b is None:
        return a is b
    return a[0] == b[0] and a[1] == b[1] and a[3] == b[3]


def _size(data):
    return "%d bytes" % len(data)


def merge_bnd(report, place, vanilla, mods, ctx, counts):
    """BND4 (format 0x74) per inner file by name. Returns (payload, inner files taken from mods)."""
    try:
        base = containers.bnd4_read(vanilla) if vanilla is not None else None
        parsed = [(mod, containers.bnd4_read(data)) for mod, data in mods]
    except (ValueError, struct.error, UnicodeDecodeError, IndexError) as error:
        raise MergeError("unsupported BND4: %s" % error)
    base_entries = base["entries"] if base else []
    _entry_key = _key_function(([base] if base else []) + [bnd for _, bnd in parsed])
    base_by = {}
    for entry in base_entries:
        if _entry_key(entry) in base_by:
            raise MergeError("the game's BND4 holds %s twice" % entry[2])
        base_by[_entry_key(entry)] = entry
    versions, anchors, order = {}, {}, []
    for mod, bnd in parsed:
        own = {}
        previous = None
        for entry in bnd["entries"]:
            key = _entry_key(entry)
            if key in own:
                raise MergeError("%s's BND4 holds %s twice" % (mod, entry[2]))
            own[key] = entry
            if not _same_entry(entry, base_by.get(key)):
                versions.setdefault(key, []).append((mod, entry))
                if key not in base_by and key not in anchors:
                    anchors[key] = previous
                    order.append(key)
            previous = key
        for key in base_by:
            if key not in own:
                versions.setdefault(key, []).append((mod, None))
    out = []
    for entry in base_entries:
        key = _entry_key(entry)
        if key in versions:
            final = _resolve(report, place, entry, versions[key], ctx, counts)
            # The game's path stays: a path that differs only in its folders names the same file.
            entry = None if final is None else [final[0], final[1], entry[2], final[3]]
        if entry is not None:
            out.append(entry)
    for key in order:
        entry = _resolve(report, place, None, versions[key], ctx, counts)
        if entry is None:
            continue
        keys = [_entry_key(e) for e in out]
        at = keys.index(anchors[key]) + 1 if anchors[key] in keys else len(out)
        out.insert(at, entry)
    header = base["header"] if base else parsed[-1][1]["header"]
    return bnd_write({"header": header, "entries": out}), len(versions)


def bnd_write(bnd):
    """containers.bnd4_write, except that an empty inner file sits at the unaligned end of the data before it, as in
    the game's files (c0000_a00_hi.anibnd.dcx: two empty entries at the data start 0x3008, the next file at 0x3010);
    the randomizer's module is not changed from here."""
    entries = bnd["entries"]
    names, name_at = bytearray(), []
    base = 0x40 + len(entries) * 0x24
    for entry in entries:
        name_at.append(base + len(names))
        names += entry[2].encode("utf-16-le") + b"\0\0"
    data_start = base + len(names)
    head = bytearray(bnd["header"])
    struct.pack_into("<I", head, 0x0c, len(entries))
    struct.pack_into("<Q", head, 0x28, data_start)
    table, blob = bytearray(), bytearray()
    for (flags, file_id, _, body), name in zip(entries, name_at):
        if body:
            blob += b"\0" * ((-(data_start + len(blob))) % 16)
        table += flags + struct.pack("<iqqIiI", -1, len(body), len(body), data_start + len(blob), file_id, name)
        blob += body
    return bytes(head) + bytes(table) + bytes(names) + bytes(blob)


def _inner_name(entry_name):
    return entry_name.replace("/", "\\").rsplit("\\", 1)[-1]


def _resolve(report, place, base_entry, values, ctx, counts):
    """The inner file of a BND4 after the mods that changed it (`values` [(mod, entry or None)])."""
    name = values[-1][1][2] if values[-1][1] is not None else base_entry[2]
    shown = _inner_name(base_entry[2] if base_entry else name)
    if shown.lower().endswith(".param"):
        shown = shown[:-6]
    inner = (place + " " + shown).strip()
    distinct = []
    for _, entry in values:
        if not any(_same_entry(entry, d) for d in distinct):
            distinct.append(entry)
    if len(distinct) == 1:
        return values[-1][1]
    if any(entry is None for _, entry in values):
        report.conflict(inner, [(m, "deletes it" if e is None else "changes it") for m, e in values], values[-1][0])
        return values[-1][1]
    flags = _unit(report, inner, "binder flags", [(m, e[0]) for m, e in values], base_entry and base_entry[0],
                  lambda v: v.hex())
    file_id = _unit(report, inner, "binder id", [(m, e[1]) for m, e in values], base_entry and base_entry[1], str)
    body_values = [(m, e[3]) for m, e in values if base_entry is None or e[3] != base_entry[3]]
    bodies = []
    for m, body in body_values:
        if body not in [b for _, b in bodies]:
            bodies.append((m, body))
    if len(bodies) <= 1:
        body = body_values[-1][1] if body_values else base_entry[3]
    else:
        vanilla = base_entry[3] if base_entry else None
        body = merge_inner(report, inner, name, vanilla, body_values, ctx, counts)
    return [flags, file_id, name, body]


def _unit(report, place, what, values, base, show):
    changed = [(m, v) for m, v in values if base is None or v != base]
    if not changed:
        return values[-1][1]
    if len({v for _, v in changed}) > 1:
        report.conflict("%s %s" % (place, what), [(m, show(v)) for m, v in changed], changed[-1][0])
    return changed[-1][1]


def merge_inner(report, place, name, vanilla, mods, ctx, counts):
    """An inner file two or more mods changed: merged by its type, else the later mod's file + a CONFLICT line."""
    try:
        payloads = [(m, unpack(d)) for m, d in mods]
        base, base_header = unpack(vanilla) if vanilla is not None else (None, None)
        merged = merge_payload(report, place, name, base, [(m, p) for m, (p, _) in payloads], ctx, counts)
    except MergeError as error:
        report.conflict(place, [(m, _size(d)) for m, d in mods], mods[-1][0])
        report.info("%s: kept whole from %s (%s)" % (place, mods[-1][0], error))
        return mods[-1][1]
    counts["merged inner files"] = counts.get("merged inner files", 0) + 1
    header = base_header or next((h for _, (_, h) in reversed(payloads) if h), None)
    return containers.dcx_write(merged, header) if header else merged


def describe(counts):
    """The MERGED line's middle: inner files changed, how many of them were merged by content, then the units."""
    parts = []
    files = counts.get("inner files", 0)
    if files:
        merged = counts.get("merged inner files", 0)
        parts.append("%d inner file%s changed (%d merged by content)" % (files, "" if files == 1 else "s", merged))
    for unit in ("rows", "texts", "textures", "animations", "functions"):
        n = counts.get(unit, 0)
        if n:
            parts.append("%d %s merged" % (n, unit[:-1] if n == 1 else unit))
    return ", ".join(parts) or "nothing changed"


def digest(data):
    return hashlib.sha256(data).hexdigest()
