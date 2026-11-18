"""The generator's own build: SHA-256 over its source modules, part of every world's input hash.

A world depends on the code, the files read and the generator that read them. GENERATOR_MAJOR marks deliberate breaks
(and the code format); this id changes with ANY change to a module that makes worlds, so two generator builds never
share a world folder name or a lobby check (the w: label part) unless they write the same bytes from the same code.

Counted: every *.py in this folder (not tests\\, which is a subfolder) whose name does not start with "_". Each file:
its name, then SHA-256 of its bytes with CRLF read as LF (a checkout with CRLF gives the same id); names sorted
(ordinal), one "name sha256" line each, the id is SHA-256 of those lines.
  buildid.py           prints the id of the sources next to it
"""
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))


class BuildIdError(Exception):
    pass


def modules(folder=HERE):
    """The file names that count, sorted (none when the folder is not there)."""
    if not os.path.isdir(folder):
        return []
    return sorted(n for n in os.listdir(folder)
                  if n.endswith(".py") and not n.startswith("_") and os.path.isfile(os.path.join(folder, n)))


def compute(folder=HERE):
    """The id of the sources in this folder, or None when there are none."""
    names = modules(folder)
    if not names:
        return None
    lines = []
    for name in names:
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")
        lines.append("%s %s\n" % (name, hashlib.sha256(data).hexdigest()))
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


_cached = None


def build_id():
    """This generator's id (64 hex), from the sources next to this file. Raises BuildIdError when there are none."""
    global _cached
    if _cached is None:
        found = compute()
        if not found:
            raise BuildIdError("no generator build id (no sources next to buildid.py)")
        _cached = found
    return _cached


if __name__ == "__main__":
    print(compute())
