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


def _hit_key(row):
    if isinstance(row, dict) and "sp_attribute" in row:
        return ("draw", row.get("sp_attribute"))
    return ("swap", row.get("attacker_speffect"), row.get("base_fx")) if isinstance(row, dict) else ("bad", repr(row))


def row_key(feature, row):
    if feature == "hit_effects":
        return _hit_key(row)
    if feature == "animated_attachments":
        return (row.get("speffect"), row.get("part")) if isinstance(row, dict) else ("bad", repr(row))
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
                table = read_json(path)
                own = table.get(list_name) if isinstance(table, dict) else None
                if not isinstance(own, list):
                    raise ValueError('no "%s" list' % list_name)
            except (OSError, ValueError) as error:
                log("INFO nightwatch\\%s.json of %s cannot be read (%s): left out" % (feature, mod, error))
                continue
            sources.append(mod)
            mine = {}
            for row in own:  # inside one table the last row of a key wins, as in the engine
                mine[row_key(feature, row)] = row
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


def _inside(rel):
    """True for a path inside the mod folder: relative, no drive, no `..` (the engine refuses the others too)."""
    parts = rel.replace("\\", "/").split("/")
    return bool(rel) and ":" not in rel and not rel.startswith(("/", "\\")) and ".." not in parts


def _sound_data(folder, row):
    """The bytes of the sound file a row names (None when it names none or it cannot be read)."""
    rel = row.get("file") if isinstance(row, dict) else None
    if not isinstance(rel, str) or not _inside(rel):
        return None
    try:
        with open(os.path.join(folder, rel.replace("/", os.sep).replace("\\", os.sep)), "rb") as f:
            return f.read()
    except OSError:
        return None


def _copy_sound(mod, folder, rel, out_dir, written, log):
    """Copies a sound file a row names; when another mod already put a different file at that path, this one goes
    under merged\\<mod>\\ and the row is pointed there."""
    if not _inside(rel):
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
