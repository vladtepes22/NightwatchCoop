#!/usr/bin/env python3
"""The two parity numbers of docs/party/ERSC_PARITY.md, recomputed from the tables.

  python -I tools/party/parity_score.py            # print per area: plan done, ERSC parity (evidence-weighted)
  python -I tools/party/parity_score.py --write    # also rewrite the Summary table and the README bars

Plan done: the `%` column of the A-H rows. ERSC parity: the "Parity %" column of the table under
"## Evidence and ERSC parity (evidence-weighted)" (the plan score capped: 95 for loopback-only
evidence and for by-decision rows, 60 for unit-only). Rows scored "—" (not planned) and section I
are left out; every number is an unweighted mean of the area's rows. A row missing from the evidence
table counts with min(plan, 95) and is reported.
"""
import re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "party" / "ERSC_PARITY.md"
README = REPO / "README.md"
AREAS = [("A", "A. Session & joining", "Session & joining"),
         ("B", "B. World & travel", "World & travel"),
         ("C", "C. Death & respawn", "Death & respawn"),
         ("D", "D. Progress (anyone can progress)", "Progress (anyone can progress)"),
         ("E", "E. Services & UI", "Services & UI"),
         ("F", "F. Network & robustness", "Network & robustness"),
         ("G", "G. BB-specific (chalice, insight, bath)", "Bloodborne-specific (chalice, Insight)"),
         ("H", "H. Bloodborne rule (feels like SP in a party)", "Feels like single player in a party")]


def plan_scores(text):
    rows = {}
    text = text[text.index("## A. Session"):]  # the row tables (not the evidence table above them)
    for line in text.splitlines():
        m = re.match(r"\| ([A-H]\d+) \|", line)
        if not m or m.group(1) in rows:
            continue
        cells = [c.strip() for c in line.split("|")]
        for c in cells[4:]:
            if re.fullmatch(r"\d+", c):
                rows[m.group(1)] = int(c)
                break
            if c in ("—", "-"):
                break
    return rows


def parity_scores(text):
    i = text.find("## Evidence and ERSC parity")
    if i < 0:
        return {}
    j = text.find("\n## ", i + 5)
    rows = {}
    for line in text[i:j].splitlines():
        m = re.match(r"\| ([A-H]\d+) \| (\d+) \|[^|]*\|[^|]*\| (\d+) \|", line)
        if m:
            rows[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return rows


def bar(p):
    full = int(round(p / 5.0))
    return "`" + "█" * full + "░" * (20 - full) + "`"


def main():
    text = DOC.read_text(encoding="utf-8")
    plan = plan_scores(text)
    par = parity_scores(text)
    res = {}
    for key, name, _ in AREAS:
        ids = [r for r in plan if r[0] == key]
        p = [plan[r] for r in ids]
        q = []
        for r in ids:
            if r in par:
                if par[r][0] != plan[r]:
                    print("note: %s plan %d in its row, %d in the evidence table" % (r, plan[r], par[r][0]))
                q.append(min(par[r][1], plan[r]))
            else:
                print("note: %s missing from the evidence table (counted as min(plan, 95))" % r)
                q.append(min(plan[r], 95))
        res[key] = (sum(p) / len(p), sum(q) / len(q), len(ids))
    allp = [plan[r] for r in plan]
    allq = [min(par[r][1], plan[r]) if r in par else min(plan[r], 95) for r in plan]
    tot = (sum(allp) / len(allp), sum(allq) / len(allq), len(allp))
    for key, name, _ in AREAS:
        print("%-48s plan %5.1f   parity %5.1f   (%d rows)" % (name, *res[key]))
    print("%-48s plan %5.1f   parity %5.1f   (%d rows)" % ("Overall", *tot))
    if "--write" not in sys.argv:
        return
    rnd = lambda v: int(v + 0.5)
    s_rows = ["| Area | Plan done | ERSC parity (evidence-weighted) |", "|---|---|---|"]
    for key, name, _ in AREAS:
        s_rows.append("| %s | %d%% | %d%% |" % (name, rnd(res[key][0]), rnd(res[key][1])))
    s_rows.append("| **Overall (unweighted mean of the %d scored rows; \"not planned\" rows and section I excluded)** "
                  "| **~%d%%** | **~%d%%** |" % (tot[2], rnd(tot[0]), rnd(tot[1])))
    i = text.index("## Summary")
    a = text.index("| Area |", i)
    b = text.index("\n\n", a)
    text = text[:a] + "\n".join(s_rows) + text[b:]
    DOC.write_text(text, encoding="utf-8")
    rd = README.read_text(encoding="utf-8")
    i = rd.index("### Seamless co-op progress")
    a = rd.index("| Area |", i)
    b = rd.index("\n\n", a)
    r_rows = ["| Area | Plan done | | ERSC parity (evidence-weighted) | |", "|---|---|--:|---|--:|"]
    r_rows.append("| **Overall** | %s | %d%% | %s | %d%% |" % (bar(tot[0]), rnd(tot[0]), bar(tot[1]), rnd(tot[1])))
    for key, _, short in AREAS:
        r_rows.append("| %s | %s | %d%% | %s | %d%% |" % (short, bar(res[key][0]), rnd(res[key][0]), bar(res[key][1]),
                                                          rnd(res[key][1])))
    rd = rd[:a] + "\n".join(r_rows) + rd[b:]
    README.write_text(rd, encoding="utf-8")
    print("Summary and README bars written")


if __name__ == "__main__":
    main()
