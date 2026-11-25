"""nightwatch.json and nightwatch\\<feature>.json tables (docs\\modding\\manifest.md): merged per key, a later mod's
row wins, as the engine merges them at start (engine\\start\\features.h). Sound files a kept row names are copied."""
import json
import os
import re

KEYS = {
    "hit_effects": None,  # two kinds of rows, see _hit_key
    "weapon_clip_groups": ("weapon",),
    "sounds": ("event",),
}
LIST = {"animated_attachments": "attachments"}  # tables whose list is not "rows"
# The engine's limits per table file (engine\start\features.h kMaxFileBytes, kMax*Rows); a merged table must fit them.
MAX_FILE_BYTES = 1 << 20
MAX_ROWS = {"hit_effects": 4096, "weapon_clip_groups": 4096, "sounds": 256, "animated_attachments": 64}
_MAX_ID = 0x7FFFFFFF


def _whole(value, lo=None, hi=None):
    """A JSON whole number (not true / false, not 1.0) in lo..hi."""
    if isinstance(value, bool) or not isinstance(value, int):
        return False
    return (lo is None or value >= lo) and (hi is None or value <= hi)


def sound_path(text):
    """engine SoundFilePath: a path inside the mod (1..200 bytes, no drive, no leading slash, no empty, . or ..
    part, no control character)."""
    if not isinstance(text, str) or not text or len(text.encode("utf-8", "surrogatepass")) > 200:
        return False
    p = text.replace("\\", "/")
    if p[0] == "/" or ":" in p or any(ord(c) < 0x20 for c in p):
        return False
    return all(part not in ("", ".", "..") for part in p.split("/"))


def row_problem(feature, row):
    """Why the engine refuses one row of a table (engine\\start\\features.h Parse*), or None for a good row (and for
    a feature this merger does not know)."""
    if feature not in MAX_ROWS:
        return None
    if not isinstance(row, dict):
        return "is not a JSON object"
    if feature == "hit_effects":
        if "sp_attribute" in row:
            a = row.get("sp_attribute")
            if not _whole(a, 1, 255) or a in (2, 6) or not _whole(row.get("fx"), 1, _MAX_ID) or \
                    not _whole(row.get("sound"), 1, _MAX_ID) or "attacker_speffect" in row or "base_fx" in row:
                return ("(a draw row) needs sp_attribute 1..255 but not 2 or 6, and whole numbers 1..2147483647 for "
                        "fx and sound (no attacker_speffect or base_fx)")
            return None
        if not all(_whole(row.get(k), 1, _MAX_ID) for k in ("attacker_speffect", "base_fx", "fx")):
            return "needs whole numbers 1..2147483647 for attacker_speffect, base_fx and fx"
        return None
    if feature == "weapon_clip_groups":
        weapon = row.get("weapon")
        if not _whole(weapon, 10000, 2147480000) or weapon % 10000 or not _whole(row.get("clip_group"), 1, 999):
            return 'needs "weapon" (a multiple of 10000) and "clip_group" (1..999)'
        return None
    if feature == "sounds":
        event, volume = row.get("event"), row.get("volume", 1.0)
        ok = isinstance(event, str) and re.fullmatch(r"[acfopsmvxbgyz][0-9]{9}", event) is not None and \
            sound_path(row.get("file")) and not isinstance(volume, bool) and isinstance(volume, (int, float)) and \
            0.0 <= volume <= 4.0 and isinstance(row.get("replace", False), bool)
        return None if ok else ('needs "event" (e.g. "s000252531"), "file" (a path inside the mod), "volume" 0..4 '
                                'and "replace" true/false')
    part = row.get("part")
    if not _whole(row.get("speffect"), 1, _MAX_ID) or not isinstance(part, str) or \
            re.fullmatch(r"[A-Za-z0-9_]{1,32}", part) is None:
        return 'needs "speffect" (1..2147483647) and "part" (a parts file name: letters, digits, _; at most 32)'
    return None


def _table_bytes(list_name, rows):
    table = {"version": 1, list_name: rows} if list_name == "rows" else {list_name: rows}
    return len((json.dumps(table, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def _fit(feature, ordered, sources, list_name, lines):
    """`ordered` [(mod, row, folder)] cut to the engine's row and size limits: the earliest mods' rows go first."""
    limit = MAX_ROWS.get(feature)
    if limit is None:
        return ordered
    rank = {mod: i for i, mod in enumerate(sources)}
    drop_order = sorted(range(len(ordered)), key=lambda i: (rank[ordered[i][0]], i))
    keep = set(range(len(ordered)))
    dropped = []
    queue = iter(drop_order)
    while len(keep) > limit:
        index = next(queue)
        keep.discard(index)
        dropped.append(ordered[index][0])
    while keep:
        rows = [ordered[i][1] for i in sorted(keep)]
        # Sounds rows get a longer file path when a sound is renamed on copy: room is kept for that.
        if _table_bytes(list_name, rows) + (300 * len(rows) if feature == "sounds" else 0) <= MAX_FILE_BYTES:
            break
        # Over 1 MiB: drop a batch of the earliest rows at once (each step at least 1/64 of the rows).
        for _ in range(max(1, len(keep) // 64)):
            index = next(queue)
            keep.discard(index)
            dropped.append(ordered[index][0])
    if dropped:
        mods = ", ".join("%s %d" % (m, dropped.count(m)) for m in sorted(set(dropped), key=rank.get))
        lines.append("INFO nightwatch\\%s.json: more than the game takes (%d rows, 1 MiB): %d row%s of the earliest "
                     "mods left out (%s)" % (feature, limit, len(dropped), "" if len(dropped) == 1 else "s", mods))
    return [ordered[i] for i in sorted(keep)]


def _hit_key(row):
    if isinstance(row, dict) and "sp_attribute" in row:
        return ("draw", row.get("sp_attribute"))
    return ("swap", row.get("attacker_speffect"), row.get("base_fx")) if isinstance(row, dict) else ("bad", repr(row))


def row_key(feature, row):
    if feature == "hit_effects":
        return _hit_key(row)
    if feature == "animated_attachments":
        return (row.get("speffect"), str(row.get("part")).upper()) if isinstance(row, dict) else ("bad", repr(row))
    fields = KEYS.get(feature)
    if fields and isinstance(row, dict):
        return tuple(row.get(f) for f in fields)
    return ("row", json.dumps(row, sort_keys=True))


def _show_key(feature, key):
    if feature == "hit_effects":
        return ("draw row sp_attribute %s" % key[1]) if key[0] == "draw" else (
            "attacker_speffect %s base_fx %s" % (key[1], key[2]))
    if feature == "animated_attachments":
        return "speffect %s part %s" % key
    fields = KEYS.get(feature)
    if fields:
        return " ".join("%s %s" % (f, v) for f, v in zip(fields, key))
    return "row %s" % key[1]


def _version_tuple(text):
    parts = re.match(r"\s*(\d+(?:\.\d+)*)", str(text or ""))
    return tuple(int(p) for p in parts.group(1).split(".")) if parts else ()


def read_json(path):
    with open(path, "rb") as f:
        data = f.read()
    return json.loads(data.decode("utf-8-sig"))


def merge_tables(mods, out_dir, out_name, log):
    """`mods`: [(mod name, mod folder)] in merge order. Writes nightwatch.json and nightwatch\\*.json into `out_dir`
    when any mod has a manifest; returns the relative paths of the files written (sound files included). `log(line)`
    writes a line."""
    manifests = []
    for mod, folder in mods:
        path = os.path.join(folder, "nightwatch.json")
        if not os.path.isfile(path):
            continue
        try:
            manifest = read_json(path)
            if not isinstance(manifest, dict):
                raise ValueError("not a JSON object")
        except (OSError, ValueError) as error:
            log("INFO nightwatch.json of %s cannot be read (%s): its feature tables are left out" % (mod, error))
            continue
        manifests.append((mod, folder, manifest))
    if not manifests:
        return []
    features, requires = [], ""
    for _, _, manifest in manifests:
        for feature in manifest.get("features") or []:
            if isinstance(feature, str) and feature not in features:
                features.append(feature)
        if _version_tuple(manifest.get("requires")) > _version_tuple(requires):
            requires = str(manifest.get("requires"))
    written = []
    copied = {}  # (mod, sound file) -> where it went
    merged_features = []
    for feature in features:
        rows, sources, conflicts, lines = {}, [], 0, []
        list_name = LIST.get(feature, "rows")
        for mod, folder, manifest in manifests:
            if feature not in (manifest.get("features") or []):
                continue
            path = os.path.join(folder, "nightwatch", feature + ".json")
            if not os.path.isfile(path):
                continue
            try:
                if os.path.getsize(path) > MAX_FILE_BYTES:
                    raise ValueError("larger than 1 MiB")
                table = read_json(path)
                own = table.get(list_name) if isinstance(table, dict) else None
                if not isinstance(own, list):
                    raise ValueError('no "%s" list' % list_name)
                version = table.get("version", None if list_name != "rows" else "missing")
                if version is not None and (not _whole(version) or version != 1):
                    raise ValueError('"version" is not 1')
            except (OSError, ValueError) as error:
                log("INFO nightwatch\\%s.json of %s cannot be read (%s): left out, as the game would leave it out" % (
                    feature, mod, error))
                continue
            sources.append(mod)
            mine = {}
            for number, row in enumerate(own, 1):
                why = row_problem(feature, row)
                if why:
                    # The game drops a mod's whole table for one bad row; here only that row goes.
                    lines.append("INFO nightwatch\\%s.json of %s: row %d %s: left out" % (feature, mod, number, why))
                    continue
                key = row_key(feature, row)
                if key in mine and feature != "hit_effects":
                    # The game refuses a table that lists a key twice; the later row is kept (the merge rule).
                    lines.append("INFO nightwatch\\%s.json of %s: row %d lists %s a second time: the later row is "
                                 "kept" % (feature, mod, number, _show_key(feature, key)))
                mine.pop(key, None)  # inside one table the last row of a key wins, where it stands
                mine[key] = row
            for key, row in mine.items():
                # Two sounds rows can be equal and still name different files under one name: that differs too.
                if key in rows and (rows[key][1] != row or (feature == "sounds" and _sound_data(
                        rows[key][2], row) != _sound_data(folder, row))):
                    lines.append("CONFLICT nightwatch\\%s.json %s: %s %s, %s %s -> %s wins" % (
                        feature, _show_key(feature, key), rows[key][0], json.dumps(rows[key][1], sort_keys=True),
                        mod, json.dumps(row, sort_keys=True), mod))
                    conflicts += 1
                if key in rows:
                    del rows[key]  # re-added below, so the later mod's row comes after (the engine's order)
                rows[key] = (mod, row, folder)
        if not sources:
            continue
        ordered = list(rows.values())
        if feature == "hit_effects":
            # The engine checks the latest mod's rows first; inside one file the first matching row wins, so the
            # merged file lists the later mods' rows first.
            ordered = [r for mod in reversed(sources) for r in ordered if r[0] == mod]
        ordered = _fit(feature, ordered, sources, list_name, lines)
        out_rows = []
        for mod, row, folder in ordered:
            if feature == "sounds" and isinstance(row, dict) and isinstance(row.get("file"), str):
                row = dict(row)
                if (mod, row["file"]) not in copied:
                    copied[(mod, row["file"])] = _copy_sound(mod, folder, row["file"], out_dir, written, lines.append)
                row["file"] = copied[(mod, row["file"])]
            out_rows.append(row)
        table = {"version": 1, list_name: out_rows} if list_name == "rows" else {list_name: out_rows}
        rel = os.path.join("nightwatch", feature + ".json")
        _write_json(os.path.join(out_dir, rel), table)
        written.append(rel)
        merged_features.append(feature)
        log("MERGED nightwatch\\%s.json from %d mod%s: %d row%s, %d conflict%s" % (
            feature, len(sources), "" if len(sources) == 1 else "s", len(out_rows), "" if len(out_rows) == 1 else "s",
            conflicts, "" if conflicts == 1 else "s"))
        for line in lines:
            log(line)
    manifest = {"id": re.sub(r"[^a-z0-9]+", "-", out_name.lower()).strip("-") or "merged", "name": out_name,
                "requires": requires or "0.3.0", "features": merged_features}
    _write_json(os.path.join(out_dir, "nightwatch.json"), manifest)
    written.append("nightwatch.json")
    log("INFO nightwatch.json written: requires %s, features %s" % (
        manifest["requires"], ", ".join(merged_features) or "none"))
    return written


HOST_ONLY = "party_host_only.txt"
HOST_ONLY_MAX = 4096  # engine\start\host_only.h kMax


def host_only_ids(data):
    """The event ids of one mod's party_host_only.txt as the engine reads it (engine\\start\\host_only.h ParseModList):
    UTF-8 (a BOM dropped), `#` starts a comment, spaces, tabs and CRs around an id dropped, ids 1..4294967295. Raises
    ValueError on a line the engine refuses (it then leaves out that mod's whole list)."""
    text = data.decode("utf-8-sig")
    ids = []
    for number, line in enumerate(text.splitlines(), 1):
        value = line.split("#", 1)[0].strip(" \t\r")
        if not value:
            continue
        if not re.fullmatch(r"[0-9]+", value) or not 0 < int(value) <= 0xFFFFFFFF:
            raise ValueError("line %d is not an event id: %s" % (number, line.strip()))
        ids.append(int(value))
    return ids


def merge_host_only(mods, out_dir, log):
    """party_host_only.txt (boss phase events only the party host decides; at the mod folder's root, beside
    dvdroot_ps4): the engine merges the enabled mods' lists sorted and unique, so the merged mod gets that union."""
    ids, sources = set(), []
    for mod, folder in mods:
        path = os.path.join(folder, HOST_ONLY)
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as f:
                own = host_only_ids(f.read())
        except (OSError, ValueError) as error:
            log("INFO %s of %s cannot be read (%s): left out, as the game would leave it out" % (HOST_ONLY, mod, error))
            continue
        sources.append(mod)
        ids.update(own)
    if not ids:
        return None
    if len(ids) > HOST_ONLY_MAX:
        log("INFO %s: %d events, more than the game takes (%d): the game will use none of them" % (
            HOST_ONLY, len(ids), HOST_ONLY_MAX))
    with open(os.path.join(out_dir, HOST_ONLY), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Boss phase events only the party host decides, merged from: %s\n" % ", ".join(sources))
        f.write("".join("%d\n" % i for i in sorted(ids)))
    log("MERGED %s from %d mod%s: %d event%s" % (HOST_ONLY, len(sources), "" if len(sources) == 1 else "s", len(ids),
                                                 "" if len(ids) == 1 else "s"))
    return HOST_ONLY


def _sound_data(folder, row):
    """The bytes of the sound file a row names (None when it names none or it cannot be read)."""
    rel = row.get("file") if isinstance(row, dict) else None
    if not isinstance(rel, str) or not sound_path(rel):
        return None
    try:
        with open(os.path.join(folder, rel.replace("/", os.sep).replace("\\", os.sep)), "rb") as f:
            return f.read()
    except OSError:
        return None


def _copy_sound(mod, folder, rel, out_dir, written, log):
    """Copies a sound file a row names; when another mod already put a different file at that path, this one goes
    under merged\\<mod>\\ and the row is pointed there."""
    if not sound_path(rel):
        log("INFO %s: sound file %s is outside the mod folder: not copied, the row is kept as it is" % (mod, rel))
        return rel
    source = os.path.join(folder, rel.replace("/", os.sep).replace("\\", os.sep))
    if not os.path.isfile(source):
        log("INFO %s: sound file %s is missing: the row is kept as it is" % (mod, rel))
        return rel
    with open(source, "rb") as f:
        data = f.read()
    target_rel = rel
    target = os.path.join(out_dir, target_rel.replace("/", os.sep).replace("\\", os.sep))
    if os.path.isfile(target):
        with open(target, "rb") as f:
            if f.read() == data:
                return rel
        target_rel = "merged/%s/%s" % (re.sub(r"[^A-Za-z0-9_.-]+", "_", mod), rel.replace("\\", "/"))
        target = os.path.join(out_dir, target_rel.replace("/", os.sep))
        log("INFO %s: sound file %s clashes with another mod's file of that name: copied as %s" % (
            mod, rel, target_rel))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "wb") as f:
        f.write(data)
    written.append(target_rel)
    return target_rel


def _write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(value, f, indent=2, ensure_ascii=False)
        f.write("\n")
