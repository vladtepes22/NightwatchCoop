import re, subprocess, sys
out = subprocess.run([sys.executable, "-I", sys.argv[1], sys.argv[2], sys.argv[3]], capture_output=True, text=True, encoding="utf-8").stdout
for block in out.split("\n== ")[1:]:
    head = block.splitlines()[0]
    g = lambda k: (re.search(r"^\s+%s (.*)$" % k, block, re.M) or [None, "?"])[1]
    d = g("defeat"); dflag = "ON" if "'1@" in d else "off"
    print("%-70s attacks %-22s r1_damage %-22s defeat %s faults %s" % (head[:70], g("attacks"), g("r1_damage"), dflag, g("faults")))
