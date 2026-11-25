"""hks merge: plain-text behaviour scripts (Havok Lua), one unit per top-level statement (a function, a global
assignment).

The game's c0000.hks / c9999.hks are compiled bytecode; mods ship the community decomp as text. Which units a mod
changed is told against hks_base.json: per unit, a hash of the community decomp's code (comments dropped, white space
collapsed), made by gen_hks_base.py from a local copy of the decomp. Only the hashes ship, never the decomp. For a
script that table does not cover, the first mod's text stands for the game's (its own changes then count as the
game's, and the log says so).
"""
import hashlib
import json
import os
import re

from common import MergeError

_OPEN = frozenset(("function", "do", "if", "repeat"))
_CLOSE = frozenset(("end", "until"))
_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_LONG = re.compile(r"\[(=*)\[")
_BASE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hks_base.json")
_base_cache = {}


def scan(text):
    """[(line, starts a statement at the top level, the line's code without comments)]; MergeError when the text does
    not close every block, bracket and long string."""
    out = []
    depth = paren = 0
    long_close, long_comment = None, False
    for line in text.splitlines(keepends=True):
        top = depth == 0 and paren == 0 and long_close is None
        code = []
        i, n = 0, len(line)
        while i < n:
            if long_close:
                j = line.find(long_close, i)
                if j < 0:
                    if not long_comment:
                        code.append(line[i:])
                    break
                if not long_comment:
                    code.append(line[i:j + len(long_close)])
                i = j + len(long_close)
                long_close = None
                continue
            c = line[i]
            if c == "-" and line.startswith("--", i):
                m = _LONG.match(line, i + 2)
                if m:
                    long_close, long_comment = "]" + m.group(1) + "]", True
                    i = m.end()
                    continue
                break
            if c == "[":
                m = _LONG.match(line, i)
                if m:
                    long_close, long_comment = "]" + m.group(1) + "]", False
                    code.append(m.group(0))
                    i = m.end()
                    continue
            if c in "\"'":
                j = i + 1
                while j < n and line[j] != c and line[j] not in "\r\n":
                    j += 2 if line[j] == "\\" else 1
                code.append(line[i:j + 1])
                i = j + 1
                continue
            if c.isalpha() or c == "_":
                m = _WORD.match(line, i)
                if m:
                    word = m.group(0)
                    depth += word in _OPEN
                    depth -= word in _CLOSE
                    code.append(word)
                    i = m.end()
                    continue
            if c in "({[":
                paren += 1
            elif c in ")}]":
                paren -= 1
            code.append(c)
            i += 1
        code_text = "".join(code)
        out.append((line, top and bool(code_text.strip()), code_text))
        if depth < 0 or paren < 0:
            raise MergeError("unbalanced block or bracket near: %s" % line.strip()[:60])
    if depth or paren or long_close:
        raise MergeError("the script does not close every block")
    return out


def _key_of(code):
    code = code.strip()
    m = re.match(r"(local\s+)?function\s+([\w.:]+)", code)
    if m:
        return ("local function " if m.group(1) else "function ") + m.group(2)
    m = re.match(r"(local\s+)?([\w.]+(?:\s*\[[^\]]*\])*(?:\s*,\s*[\w.]+)*)\s*=(?!=)", code)
    if m:
        return ("local " if m.group(1) else "") + re.sub(r"\s+", "", m.group(2))
    return "statement " + re.sub(r"\s+", " ", code)[:50]


def units(text):
    """[(key, text, hash)] of a script in order; the lines before the first statement are the unit "(file start)".
    A key that repeats gets " #2", " #3"... ."""
    out = []
    seen = {}
    current = ["(file start)", [], []]
    for line, starts, code in scan(text):
        if starts:
            out.append(current)
            key = _key_of(code)
            seen[key] = seen.get(key, 0) + 1
            current = [key if seen[key] == 1 else "%s #%d" % (key, seen[key]), [], []]
        current[1].append(line)
        current[2].append(code)
    out.append(current)
    result = []
    for key, lines, codes in out:
        if key == "(file start)" and not lines:
            continue
        body = "".join(lines)
        if body and not body.endswith("\n"):
            body += "\n"
        result.append((key, body, code_hash(codes)))
    return result


def code_hash(code_lines):
    norm = "\n".join(re.sub(r"\s+", " ", c).strip() for c in code_lines if c.strip())
    return hashlib.sha256(norm.encode("utf-8")).hexdigest()[:20]


def base_hashes(name):
    """{unit key: hash} of the community decomp of `name` (c0000.hks), or None."""
    if "table" not in _base_cache:
        try:
            with open(_BASE_FILE, encoding="utf-8") as f:
                _base_cache["table"] = json.load(f)
        except (OSError, ValueError):
            _base_cache["table"] = {}
    return _base_cache["table"].get(name.lower())


def decode(mod, data):
    if data[:1] == b"\x1b":
        raise MergeError("%s ships compiled hks, which cannot be merged with text" % mod)
    try:
        return data.decode("utf-8-sig"), data.startswith(b"\xef\xbb\xbf")
    except UnicodeDecodeError:
        raise MergeError("%s's hks is not UTF-8 text" % mod)


def merge_hks(label, file_name, vanilla, mods, report):
    """`vanilla`: the game's file (bytecode, or text when a game copy holds text) or None; `mods`: [(mod, bytes)].
    Returns (bytes, units taken)."""
    parsed = []
    for mod, data in mods:
        text, bom = decode(mod, data)
        try:
            parsed.append((mod, units(text), bom))
        except MergeError as error:
            raise MergeError("%s's hks cannot be split into functions: %s" % (mod, error))
    if vanilla is not None and vanilla[:1] != b"\x1b":
        base = {key: h for key, _, h in units(decode("the game", vanilla)[0])}
    else:
        base = base_hashes(file_name)
        if base is None:
            base = {key: h for key, _, h in parsed[0][1]}
            report.info("no function table for the game's compiled %s: %s's text stands for the game's, so its own "
                        "changes count as the game's" % (file_name, parsed[0][0]))
    changes = {}  # key -> [(mod, hash or None, text)]
    for mod, own, _ in parsed:
        have = {key: (h, body) for key, body, h in own}
        for key in list(have) + [k for k in base if k not in have and k != "(file start)"]:
            h, body = have.get(key, (None, None))
            if h != base.get(key):
                changes.setdefault(key, []).append((mod, h, body))
    final = {}
    for key, values in changes.items():
        if len({h for _, h, _ in values}) > 1:
            report.conflict("%s %s" % (label, key),
                            [(m, "deletes it" if h is None else "%d lines" % body.count("\n")) for m, h, body in values],
                            values[-1][0])
        final[key] = values[-1]
    # The order: the last mod's, with units only earlier mods have placed after the unit before them in their file.
    order = [key for key, _, _ in parsed[-1][1]]
    for _, own, _ in reversed(parsed[:-1]):
        keys = [key for key, _, _ in own]
        for index, key in enumerate(keys):
            if key in order:
                continue
            before = next((k for k in reversed(keys[:index]) if k in order), None)
            order.insert(order.index(before) + 1 if before else 0, key)
    texts = {}
    for _, own, _ in parsed:
        for key, body, h in own:
            texts[key] = body  # unchanged units: the last mod's text
    out = []
    for key in order:
        if key in final:
            body = final[key][2]
            if body is None:
                continue
        else:
            body = texts[key]
        out.append(body)
    result = "".join(out)
    scan(result)
    data = result.encode("utf-8")
    if parsed[-1][2]:
        data = b"\xef\xbb\xbf" + data
    return data, len(changes)
