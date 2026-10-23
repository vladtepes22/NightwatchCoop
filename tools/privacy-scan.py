"""Privacy scan of a package: each pattern in ASCII and UTF-16LE, case-insensitive, with context.

Hits listed in privacy-allow.txt next to this script (tab-separated: pattern, then a file glob
relative to the package root, posix slashes) are known and benign; they are counted, not printed.
Exit code 1 when any other hit is found, so a clean run is one line.
"""
import fnmatch, pathlib, re, sys

ALLOW_FILE = pathlib.Path(__file__).resolve().parent / "privacy-allow.txt"


def load_allow():
    rules = []
    if ALLOW_FILE.exists():
        for line in ALLOW_FILE.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            pattern, glob = line.split("\t", 1)
            rules.append((pattern.strip().lower(), glob.strip()))
    return rules


def main():
    root = pathlib.Path(sys.argv[1])
    patterns = [p.lower() for p in sys.argv[2:]]
    allow = load_allow()
    known = new = 0
    for f in sorted(root.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(root).as_posix()
        data = f.read_bytes()
        low = data.lower()
        for p in patterns:
            for enc in ("ascii", "utf-16-le"):
                needle = p.encode(enc)
                at = low.find(needle)
                while at >= 0:
                    # UTF-16 characters start on even offsets; an odd match is not a real hit.
                    if enc == "ascii" or at % 2 == 0:
                        if any(p == ap and fnmatch.fnmatch(rel, g) for ap, g in allow):
                            known += 1
                        else:
                            new += 1
                            start = max(0, at - 60) & ~1 if enc != "ascii" else max(0, at - 60)
                            end = at + len(needle) + 60
                            text = data[start:end].decode(enc, "replace")
                            text = re.sub(r"[^\x20-\x7e]", ".", text)
                            print(f"{rel} [{p}/{enc}]: {text}")
                    at = low.find(needle, at + 1)
    print(f"privacy scan: {new} new hit(s), {known} known hit(s) suppressed", file=sys.stderr)
    sys.exit(1 if new else 0)


if __name__ == "__main__":
    main()
