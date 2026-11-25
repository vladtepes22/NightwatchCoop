"""Shared pieces of the mod merger: the randomizer's container module, the per-file report and value formatting."""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
# containers.py (DCX / BND4 / PARAM / FMG, byte-exact) is the randomizer's: imported, never changed from here. In the
# repo it sits in tools\randomizer; in a package the merger is <package>\merger\source and the randomizer
# <package>\randomizer\source.
for _candidate in (os.path.join(_HERE, "..", "randomizer"), os.path.join(_HERE, "..", "..", "randomizer", "source")):
    if os.path.isfile(os.path.join(_candidate, "containers.py")):
        _candidate = os.path.normpath(_candidate)
        if _candidate not in sys.path:
            sys.path.insert(1, _candidate)
        break

import containers  # noqa: E402  (after the path set-up above)


class MergeError(Exception):
    """A file of a mergeable type that cannot be merged after all (unreadable, unsupported variant): the caller falls
    back to the later mod's whole file and says why."""


class Report:
    """The log lines of one game file, in the order they are found. Lines are collected per file so a file merged on
    another thread still writes its lines together and in a fixed order."""

    def __init__(self, label):
        self.label = label
        self.lines = []
        self.conflicts = 0

    def conflict(self, place, values, winner):
        """`values`: [(mod, shown value)] of every mod that changed this place, in merge order."""
        shown = ", ".join("%s %s" % (mod, value) for mod, value in values)
        self.lines.append("CONFLICT %s %s: %s -> %s wins" % (self.label, place.strip(), shown, winner))
        self.conflicts += 1

    def not_merged(self, place, mods, winner, why):
        where = self.label + (" " + place.strip() if place and place.strip() else "")
        self.lines.append("NOT MERGED %s: %s -> %s wins (%s)" % (where, ", ".join(mods), winner, why))

    def info(self, text):
        self.lines.append("INFO %s: %s" % (self.label, text))


def show_text(text, limit=60):
    """An FMG text or a name in a log line: quoted, one line, cut at `limit` characters."""
    if text is None:
        return "(no text)"
    flat = text.replace("\r", "").replace("\n", "\\n")
    if len(flat) > limit:
        flat = flat[:limit - 3] + "..."
    return '"%s"' % flat


def show_float(value):
    text = "%.7g" % value
    return text if any(c in text for c in ".enia") else text + ".0"


def plural(count, word):
    return "%d %s%s" % (count, word, "" if count == 1 else "s")
