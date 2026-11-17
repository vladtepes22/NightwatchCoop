"""The newest installed Nocturne, for the tests that need a base mod. A fixed version path silently skipped these cases
once a newer version replaced it (10 Nov, again 16 Nov); BB_NOCTURNE names one explicitly."""
import glob
import os
import re

MODS = r"C:\Games\NightwatchCoop\Mods"


def newest(extra=()):
    """The folder of the highest `Nocturne v<x.y.z>` in the Mods folder (or in `extra` folders), "" when none."""
    def version(path):
        found = re.search(r"Nocturne v(\d+(?:\.\d+)*)\s*$", path)
        return tuple(int(n) for n in found.group(1).split(".")) if found else ()
    folders = [p for pattern in (os.path.join(MODS, "Nocturne v*"),) + tuple(extra) for p in glob.glob(pattern)
               if os.path.isdir(p) and version(p)]
    return max(folders, key=version) if folders else ""


NOCTURNE = os.environ.get("BB_NOCTURNE") or newest()
