"""Privacy scan of a package: each pattern in ASCII and UTF-16LE, case-insensitive, with context."""
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
patterns = sys.argv[2:]
for f in root.rglob("*"):
    if not f.is_file():
        continue
    data = f.read_bytes()
    low = data.lower()
    for p in patterns:
        for enc in ("ascii", "utf-16-le"):
            needle = p.lower().encode(enc)
            start = 0
            while (at := low.find(needle, start)) >= 0:
                ctx = data[max(0, at - 60): at + len(needle) + 60]
                text = ctx.decode(enc if enc == "ascii" else "utf-16-le", "replace") if enc == "ascii" else \
                    data[max(0, (at - 60) & ~1): at + len(needle) + 60].decode("utf-16-le", "replace")
                text = re.sub(r"[^\x20-\x7e]", ".", text)
                print(f"{f.relative_to(root)} [{p}/{enc}]: {text}")
                start = at + 1
