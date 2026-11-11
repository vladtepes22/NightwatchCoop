#!/usr/bin/env python3
"""Session kit scorer: every ERSC parity row of docs/party/ERSC_PARITY.md, scored from a real party's logs.

  python -I tools/party/session_score.py LOGDIR [--json OUT.json] [--md OUT.md]
  python -I tools/party/session_score.py --hash-saves GAME_OR_USER_DIR -o savehash_pre.json   (= scripts/save_hash.py)
  python -I tools/party/session_score.py --list-rows

LOGDIR holds the logs of ONE session: the host's and every guest's (bbport logs\\last_session.log, a support
bundle zip, Nightwatch logs, the same text as .txt), in any folder layout. Zips are read in place (their log
members only). Log files without party networking are ignored.

Every row gets one of:
  PROVEN         the row's markers were seen (each marker on the right kind of PC);
  FAILED         a failure marker was seen (a row can be FAILED and still list the markers that worked);
  NOT EXERCISED  neither (the table names the markers that are missing).
Each verdict cites file:line (the first line of each marker). Never printed: IP addresses (masked as
scripts/net_doctor.py does), player names (Host / Guest N), party codes, user names in paths.

Reused, not forked:
  scripts/net_doctor.py       parse() and its marker regexes (R) and diagnose() for the network rows
                              (A1 A9 F1 F2 F3 F5) and the masking (redact)
  tools/mp/smoke_party.py     the check regexes of the smoke phases, read from the source with ast
                              (smoke_pattern(); the checks are functions of a scripted run, so their regex
                              literals are taken out, not the functions run)

A2 (the single-player save untouched): run `scripts/save_hash.py <game dir>` (the release ships it) BEFORE and AFTER the session on every PC
and put savehash_pre*.json / savehash_post*.json next to that PC's logs (the support bundle does not carry
saves). A8 / E2 accept screenshots (a file name with the row id, e.g. a8_system_menu.png).

Standard library only (net_doctor too). Exit status: 0 scored, 2 no party logs found.
"""
import argparse
import ast
import hashlib
import json
import re
import sys
import tempfile
import zipfile
from collections import Counter, OrderedDict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import net_doctor as nd  # noqa: E402
import save_hash  # noqa: E402

DOC = REPO / "docs" / "party" / "ERSC_PARITY.md"
SMOKE = REPO / "tools" / "mp" / "smoke_party.py"

PROVEN, FAILED, NOT = "PROVEN", "FAILED", "NOT EXERCISED"

# ---------------------------------------------------------------- smoke_party check regexes

_SMOKE_POS = {"need": 4, "none_of": 3}  # position of the regex in need(v, L, name, i, rx) / none_of(v, L, name, rx)
_smoke_cache = {}


def _smoke_checks():
    """{check name: [regex literals]} of tools/mp/smoke_party.py (every check_* function)."""
    if not _smoke_cache:
        tree = ast.parse(SMOKE.read_text(encoding="utf-8", errors="replace"))
        for fn in tree.body:
            if not (isinstance(fn, ast.FunctionDef) and fn.name.startswith("check_")):
                continue
            pats = []
            for n in ast.walk(fn):
                if not isinstance(n, ast.Call):
                    continue
                f = n.func
                idx = _SMOKE_POS.get(f.id) if isinstance(f, ast.Name) else \
                    (1 if isinstance(f, ast.Attribute) and f.attr in ("find", "all") else None)
                if idx is not None and len(n.args) > idx:
                    a = n.args[idx]
                    if isinstance(a, ast.Constant) and isinstance(a.value, str) and a.value not in pats:
                        pats.append(a.value)
            _smoke_cache[fn.name] = pats
    return _smoke_cache


def smoke_pattern(check, contains):
    """The regex of smoke check `check` that contains the text `contains` (KeyError when the smoke changed)."""
    for p in _smoke_checks().get(check, []):
        if contains in p:
            return p
    raise KeyError(f"smoke_party.{check} has no regex containing {contains!r}")


# ---------------------------------------------------------------- the rows

class M:
    """One marker: a regex that must match a log line of a PC of kind `who` (host / guest / any), `n` times."""

    def __init__(self, label, rx, who="any", n=1):
        self.label, self.rx, self.who, self.n = label, re.compile(rx), who, n


def P(label, rx, who="any", n=1):
    return M(label, rx, who, n)


ROWS = []


def row(rid, name, proof=(), fail=(), files=(), note=""):
    ROWS.append(dict(id=rid, name=name, proof=list(proof), fail=list(fail), files=list(files), note=note))


R_ = nd.R
sp = smoke_pattern
LIFT_FAIL = [
    P("a guest rode a lift with no car playing", r"lift ride: riding an object at .* with no table car playing", "guest"),
    P("a rider dropped through the moving car", r"lift ride: the local chr dropped through the moving car"),
]
JOIN_FAIL = [
    P("a direct summon failed", r"direct summon: FAILED \(delivered"),
    P("the join snapshot failed", r"join snapshot re-sent \(\d+, flags FAILED"),
]

row("A1", "Friends-only, password, no randoms",
    [P("guest connected to the host", R_["connected"].pattern, "guest"),
     P("host admitted a guest", R_["guest_joined"].pattern, "host")])
row("A2", "Separate co-op save (single-player save untouched)", [], [],
    note="needs savehash_pre / savehash_post (--hash-saves) beside a PC's logs")
row("A3", "Start / join in game (Party > Leave and Rejoin)",
    [P("guest pressed Leave", r"Leave: left the party \(BYE sent", "guest"),
     P("guest joined again", r"link: \S+ rejoined \(slot|direct summon: slot \d+ .* reports it joined", "host")],
    [P("session lost", r"Session lost")])
row("A4", "Join speed",
    [P("guest re-join done after N s", r"Party B\d: guest: re-join done after [\d.]+ s", "guest"),
     P("a direct summon reached the host's room", r"direct summon: slot \d+ \(\S+\) reports it joined", "host")])
row("A5", "Join any time / late joiner",
    [P("a late guest arrived in the host's world", r"Party B\d: host: slot \d+ arrived", "host"),
     P("guest in the host's world", r"guest: arrived in the host world", "guest")],
    JOIN_FAIL)
row("A6", "Leave cleanly",
    [P("guest pressed Leave", r"Leave: left the party \(BYE sent", "guest"),
     P("host saw the guest leave", r"link: \S+ left the party|member \d+ \(\S+\) left room \d+ \(leave_room\)", "host")],
    [P("session lost", r"Session lost")])
row("A7", "Player count (host + 3)",
    [P("host counts 3 cooperators", r"Party: cooperators \d+ -> 3\b", "host"),
     P("roster lists slot 3", r"Party: roster: .*\(slot 3, ", "host")])
row("A8", "Settings in game (every System / Party page)",
    [P("a game menu page was walked", r"Game menu: page \d+ rows", n=1)],
    files=[r"(?i)(^|[^a-z0-9])a8([^a-z0-9]|$)"])
row("A9", "Version check", [], [], note="a deliberately mismatched guest is refused with its reason (net_doctor)")
row("A11", "Joining during a boss fight / cutscene",
    [P("the host held its bell for a boss or cutscene", r"AN?\d?: the host's hold: (boss|cutscene)", "host"),
     P("and rang it again after", r"the host's hold: none \(its bell rings again\)", "host")])
row("B1", "No tether / roam anywhere",
    [P("a guest travelled alone while the host stayed", r"host: slot \d+ travels alone into (another|this) area", "host")])
row("B2", "Fog / boss gates (a guest first)",
    [P("a guest went through a boss fog first", r"boss fog: slot \d+ went in", "host"),
     P("the guest's walk through the fog", sp("check_emf", "guest: went through boss fog"), "guest")],
    [P("a guest-first fight start was dropped", r"first-fight start for guest event \d+ dropped")])
row("B3", "Fast travel (lamp to lamp, travel vote)",
    [P("lamp travel arrived", r"Lamp travel: arrived: lamp \d+", "any"),
     P("the guest followed the host's travel", r"guest: host travels \(#\d+ lamp", "guest")])
row("B4", "Loading screens (one per travel)",
    [P("a loading screen came and went", r"Party: loading screen up"),
     P("tied to a lamp travel", r"Lamp travel: arrived")],
    note="loads per PC are counted in the table note")
row("B5", "Independent travel (split party)",
    [P("a guest travelled alone", r"guest: (travels alone into the host's area|arrived alone at)", "guest"),
     P("the host saw it", r"host: slot \d+ travels alone into", "host")])
row("B6", "Scripted transitions, Hunter's Mark, headstones",
    [P("a transform / story warp travel", r"(host travels \(#\d+ transform|travelling alone to #\d+ transform|loading #\d+ transform)")])
row("C1", "Guest dies alone",
    [P("a guest died alone and was revived at its lamp", r"guest: died alone: revived at its own last lamp|guest revive #\d+: after", "guest"),
     P("the host saw the revive in place", r"slot \d+ was revived in place \(no load\)", "host")],
    [])
row("C2", "Host dies alone",
    [P("host death taken, no party move", r"host death taken: no stage request, no party move", "host"),
     P("the host got up", sp("check_cd", "host revive #"), "host")])
row("C3", "Party wipe",
    [P("a wipe: every member down", r"host death: every member went down|member\(s\) out of the fight: nobody fights", "host")],
    [P("a boss fight restarted at the lamp on a guest", r"(?i)lost boss fight .* restarted")])
row("C4", "Death penalty (rot, host rules)",
    [P("a death with rot on", r"death \d+; rot on, \d stack\(s\)"),
     P("a cleanse", r"rot: (a boss died|the bloodstain was picked up|an enemy died)")],
    [P("rot failed to read or write", r"rot: .*(could not|cannot) (write|read)")])
row("C5", "Spectate while dead",
    [P("a dead member spectated a living one", r"spectating: following slot \d+|slot \d+ spectates \(boss-fight limbo\)"),
     P("spectating ended", r"spectating ended")])
row("C6", "Rejoin after a crash",
    [P("a member's crash was seen", r"C\d: \S+ crashed: looking for its game connection", "host"),
     P("and it came back", r"link: \S+ rejoined \(slot", "host")])
row("C7", "Resting while others are in a boss room",
    [P("a rest travel after the load", r"guest: HP \d+ after the load \(a rest travel"),
     P("a boss fight was on", r"boss attention: boss|host limbo: \d+ member")],
    note="co-occurrence in the logs only: confirm with the runbook step")
row("D1", "Boss kills count for everyone",
    [P("a boss was killed", r"boss fight: the boss was killed|boss defeat"),
     P("the guest kept the session after the clear", r"boss-clear leave suppressed \(guest; party stays", "guest")],
    [P("a boss that may be stuck", r"may be stuck")])
row("D2", "World state shared (doors, shortcuts, lamps)",
    [P("host flags sent to the party", r"Party progress: host flag \d+ = \d+ \((shortcut_door|lamp_unlocked|key_event)\)", "host"),
     P("a guest applied them", r"Party progress: guest applied \d+ flags in load mode", "guest")],
    JOIN_FAIL[1:] + [P("desync", r"desync.*(MISMATCH|differ|mismatch)")])
row("D3", "Guest actions change the host's world (lifts, levers)",
    [P("a guest's object action reached the host's world", r"Party: world: host: slot \d+ changed the host's world", "host")],
    LIFT_FAIL)
row("D4", "Items & pickups (once each)",
    [P("an item lot was given to a player", r"Party items: gave #\d+ lot \d+"),
     P("a boss / key award went out", r"Party items: award lot \d+", "host")],
    [P("an item could not be given", r"GiveItem FAILED")])
row("D5", "Quests, NPCs, story",
    [P("an NPC quest flag went to the party", r"host flag \d+ = \d+ \(npc_quest\)", "host"),
     P("a guest heard a shared NPC line", r"npc_talk: shared line from", "guest")])
row("D6", "Full campaign (several bosses)",
    [P("three or more boss flags set", r"host flag \d+ = \d+ \(boss_defeated\)", "host", 3)],
    note="a single session scores this only as far as it goes; the soak is the real proof")
row("D7", "Enemy scaling",
    [P("the boss scaling was applied", r"Party balance: boss chr \w+: SpEffect \d+")])
row("D8", "NPC dialogue / talk events synchronised",
    [P("a guest's talk line reached the party", r"npc_talk: line from \S+ \(slot \d+\)", "host"),
     P("a shared line played", r"npc_talk: shared line from")])
row("D9", "A grace rest resets the world for everyone",
    [P("a rest travel reached a guest", r"guest: HP \d+ after the load \(a rest travel", "guest"),
     P("the kill record was cleared", r"kill record: \d+ death\(s\) here|Party progress: kill record", "host")])
row("D10", "Cutscenes play for the whole party",
    [P("a host cutscene was sent", r"Party story: host #\d+ cutscene \d+ \(mode", "host"),
     P("a guest played it", r"Party story: guest #\d+", "guest")])
row("D11", "Scripted NPC hunter fights",
    [P("an NPC hunter fight started for the party", r"(?i)Party[^:]*: (npc[ _]fights?|NPC hunter)[^\n]*(started|fight is on|joined)")])
row("E1", "Rest / level / shop for guests",
    [P("a guest used a service in the host's Dream", r"Party guest services: (Doll level-up|shop|workbench|a guest (levelled|bought|used))[^\n]*(guest|summoned)", "guest")])
row("E2", "Party HUD / names",
    [P("party tags drawn", r"Party HUD: tag \S+ out from behind")],
    files=[r"(?i)(^|[^a-z0-9])e2([^a-z0-9]|$)"])
row("E4", "Item / summon messages fast",
    [P("a summon reached the host's room", r"direct summon: slot \d+ \(\S+\) reports it joined", "host"),
     P("fast multiplayer messages applied", r"Fast multiplayer messages: applied")])
row("F1", "Real internet play (latency / loss)", [], [],
    note="a guest on a public path, no freezing or dying link (net_doctor)")
row("F2", "NAT traversal", [], [], note="a guest joined across the Internet (net_doctor)")
row("F3", "Security (a stranger is refused)", [], [], note="a wrong-password join refused with its reason (net_doctor)")
row("F5", "Watchdog / link loss recovery",
    [P("a link was lost and the slot kept", r"link: lost \S+ \(slot \d+\): .*keeping the slot", "host"),
     P("and the member came back", r"link: \S+ rejoined \(slot|outage resume: \S+ heard again after", "host")])
row("G1", "Chalice dungeons in party",
    [P("the party's dungeon was entered", r"altar: (the party's dungeon is loading|entering the party's dungeon)", "host"),
     P("a guest followed into the dungeon", r"map [^\n]*-> m29_00", "guest")])
row("G2", "Insight, messengers' bath",
    [P("Insight went up", r"Party: (\[[\d:.]+\] )?Insight \d+ -> \d+", "guest")])
row("G3", "Blood Moon / world phase",
    [P("the Blood Moon cutscene played", r"Party story: (host|guest) #\d+ cutscene 32000000")])
row("H1", "No unnecessary loading screens (revive in place)",
    [P("a revive in place, no load", r"was revived in place \(no load\)")])
row("H2", "No repeated summons",
    [P("a re-join without a new summon banner", r"banner \d+ hidden \(a re-join, not a new summon\)")],
    [P("a summon entry never came", r"direct summon: no waiting summon entry")])
row("H3", "No progress lost by anyone",
    [P("a guest applied the host's flags", r"Party progress: guest applied \d+ flags in load mode", "guest"),
     P("its gestures / items were kept", r"Party gestures: the host's gestures for the party|Party items: gave #", "any")],
    JOIN_FAIL[1:])
row("H4", "No unnecessary respawns",
    [P("kills of a guest reached the host's game", r"kill record: \d+ kill\(s\) seen on slot \d+", "host"),
     P("a death record was sent", r"kill record: \d+ death\(s\) here", "any")])
row("H5", "Character freedom (look, gear)",
    [P("the host's gestures went to the party", r"Party gestures: the host's gestures for the party"),
     P("a guest's key items were kept", r"Party items: .*(key item|shop gate repair)", "guest")],
    note="the creator-glasses report is not reproduced; this row is scored on gear and gestures only")

ROW_IDS = [r["id"] for r in ROWS]
ROWINDEX = {r["id"]: r for r in ROWS}


def doc_row_ids():
    """The parity rows of the doc (A1 ... H5 that carry a plan score), in order."""
    text = DOC.read_text(encoding="utf-8")
    text = text[text.index("## Evidence and ERSC parity"):]
    ids = []
    for m in re.finditer(r"^\| ([A-H]\d+) \| (\d+) \|", text.split("\n## A.")[0], re.M):
        ids.append(m.group(1))
    return ids


# ---------------------------------------------------------------- reading the logs

LINE_RX = re.compile(r"(?:\[\+ *[\d.]+\] )?(?:Party|Lamp|Net:|Game menu|Leave|rot:)")
SKIP_NAME = re.compile(r"(?i)^(launcher|info|summary|heartbeat|crash|hang|device)")


class PlayerLog:
    def __init__(self, path, label, parsed):
        self.path, self.label, self.nd = path, label, parsed
        self.role = parsed.role
        self.who = "?"
        self.loads = 0


def _members(zpath, tmp):
    """Log members of a zip, extracted under tmp (never trusting member names)."""
    out = []
    with zipfile.ZipFile(zpath) as z:
        for i, info in enumerate(z.infolist()):
            base = Path(info.filename).name
            if info.is_dir() or not re.search(r"(?i)\.(log|txt)$", base) or SKIP_NAME.match(base):
                continue
            if info.file_size > 400 * 1024 * 1024:
                continue
            dest = Path(tmp) / f"{zpath.stem[:20]}_{i}_{re.sub(r'[^A-Za-z0-9._-]', '_', base)}"
            dest.write_bytes(z.read(info))
            out.append((dest, f"{zpath.name}!/{info.filename}"))
    return out


def collect(logdir, tmp):
    """[(real path, display label)] of every candidate log under logdir."""
    logdir = Path(logdir)
    found = []
    for p in sorted(logdir.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(logdir).as_posix()
        if p.suffix.lower() == ".zip":
            try:
                found.extend((d, f"{rel[:-len(p.name)]}{lab}") for d, lab in _members(p, tmp))
            except (zipfile.BadZipFile, OSError):
                pass
        elif re.search(r"(?i)\.(log|txt)$", p.name) and not SKIP_NAME.match(p.name):
            found.append((p, rel))
    return found


def load_players(logdir, tmp):
    players = []
    for path, label in collect(logdir, tmp):
        lg = nd.parse(path)
        if lg.role is None:
            continue
        players.append(PlayerLog(path, label, lg))
    if not players:
        return [], []
    kept, _notes = nd.one_log_per_player([p.nd for p in players])
    players = [p for p in players if p.nd in kept]
    players.sort(key=lambda p: (p.role != "host", p.label))
    g = 0
    for p in players:
        if p.role == "host":
            p.who = "host"
        else:
            g += 1
            p.who = f"guest{g}"
    return players, [p.nd for p in players]


# ---------------------------------------------------------------- masking

PARTY_CODE = re.compile(r"\bBB[PG][0-9A-Z](?:-[0-9A-Za-z]+)+\b", re.I)
IPV6 = re.compile(r"(?<![\w:])(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}(?![\w:])|(?<![\w:])(?:[0-9A-Fa-f]{1,4}:){1,7}:(?:[0-9A-Fa-f]{1,4})?(?![\w:])")


def mask(text, ndlogs):
    """net_doctor's masking (names -> Host / Guest N, public IPs hidden, VPN / LAN IPs cut to the first number)
    plus IPv6 and party codes."""
    # IPv6 first: net_doctor's redact keeps an address's first two groups (2001:db8:x:...), this kit hides it whole.
    text = IPV6.sub(lambda m: m.group(0) if m.group(0).count(":") < 3 or re.fullmatch(r"[\d:.]+", m.group(0)) else "<ip6>", text)
    text = nd.redact(text, ndlogs, True, True)
    return PARTY_CODE.sub("<party code>", text)


# ---------------------------------------------------------------- scoring

def scan(players):
    """{(row id, 'p'|'f', marker index): [count, [(player, lineno, text)...]]} in one pass over the logs."""
    marks = []
    for r in ROWS:
        for kind, lst in (("p", r["proof"]), ("f", r["fail"])):
            for i, m in enumerate(lst):
                marks.append(((r["id"], kind, i), m))
    hits = {k: [0, []] for k, _ in marks}
    for pl in players:
        who = "host" if pl.role == "host" else "guest"
        todo = [(k, m) for k, m in marks if m.who in ("any", who)]
        with open(pl.path, "r", encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                if "loading screen up" in line and "Party" in line:
                    pl.loads += 1
                if "[coop]" not in line and not LINE_RX.match(line):
                    continue
                for k, m in todo:
                    if m.rx.search(line):
                        h = hits[k]
                        h[0] += 1
                        if len(h[1]) < 3:
                            h[1].append((pl, n, line.strip()))
    return hits


def cite(pl, n, text, ndlogs):
    return {"file": pl.label, "line": n, "who": pl.who, "text": mask(text[:200], ndlogs)}


def netdoc_notes(players, ndlogs):
    """net_doctor's findings mapped to parity rows: {row: [(kind, text, evidence key)]}"""
    issues = nd.diagnose(ndlogs, True)
    mapping = [(r"never reached the host", "A1", "F", "no_traffic"),
               (r"cannot send UDP|does not answer|traffic never reaches|Hole punching failed|own address|home network address",
                "F2", "F", "no_public_udp"),
               (r"keeps dying", "F1", "F", "no_traffic"),
               (r"keeps freezing", "F1", "F", "outage"),
               (r"lost the link", "F5", "N", "lost"),
               (r"keeps leaving and re-entering", "F1", "N", "lost"),
               (r"refused by the host", "A9", "N", "refused")]
    out = {}
    for prio, text, _fix in issues:
        for rx, rid, kind, key in mapping:
            if re.search(rx, text):
                out.setdefault(rid, []).append((kind, re.sub(r"[*_]", "", text), key))
    return out


def first_evidence(players, key):
    for pl in players:
        rows = pl.nd.evidence.get(key)
        if rows:
            n, line = rows[0]
            return pl, n, line
    return None


def reason_class(reason):
    r = reason.lower()
    if re.search(r"version|mismatch|patch|build|differ|params|eboot", r):
        return "version"
    if re.search(r"password|code", r):
        return "password"
    return "other"


def score(players, ndlogs, saves):
    hits = scan(players)
    notes = netdoc_notes(players, ndlogs)
    results = OrderedDict()
    host = next((p for p in players if p.role == "host"), None)
    for r in ROWS:
        rid = r["id"]
        ev, missing, fails, extra = [], [], [], []
        ok = bool(r["proof"])
        for i, m in enumerate(r["proof"]):
            cnt, samples = hits[(rid, "p", i)]
            if cnt >= m.n:
                pl, n, line = samples[0]
                ev.append(dict(cite(pl, n, line, ndlogs), marker=m.label, hits=cnt))
            else:
                ok = False
                missing.append(m.label + (f" ({cnt} of {m.n})" if m.n > 1 else ""))
        for i, m in enumerate(r["fail"]):
            cnt, samples = hits[(rid, "f", i)]
            if cnt:
                pl, n, line = samples[0]
                fails.append(dict(cite(pl, n, line, ndlogs), marker=m.label, hits=cnt))
        results[rid] = dict(id=rid, name=r["name"], ok=ok, ev=ev, missing=missing, fails=fails, notes=[r["note"]] if r["note"] else [])

    # ---- net_doctor rows
    res = results
    # A9 / F3: refusals by reason
    refused = [(pl, reason, cnt) for pl in players for reason, cnt in pl.nd.refused.items()]
    for rid, cls in (("A9", "version"), ("F3", "password")):
        hit = [(pl, reason, cnt) for pl, reason, cnt in refused if reason_class(reason) == cls]
        if hit:
            pl, reason, cnt = hit[0]
            e = first_evidence([pl], "refused")
            ev = dict(cite(pl, e[1], e[2], ndlogs) if e else dict(file=pl.label, line=0, who=pl.who, text=mask(reason, ndlogs)),
                      marker=f"a {cls} refusal ({cnt}x) with its reason", hits=cnt)
            res[rid].update(ok=True, ev=[ev], missing=[])
        else:
            res[rid]["missing"] = [f"a join refused for a {cls} problem (the runbook's {'mismatched guest' if cls == 'version' else 'stranger'} step)"]
    # A9 also needs the version-check line on every PC
    vc = [p for p in players if p.nd.version]
    res["A9"]["notes"].append(f"version check line on {len(vc)} of {len(players)} logs")
    # F1 / F2
    joined_public = []
    if host:
        for name, ip in host.nd.guests.items():
            if ip and nd.is_public(ip):
                joined_public.append(name)
    if joined_public and host:
        pl = host
        evs = pl.nd.evidence.get("nat") or []
        txt = f"host log: {len(joined_public)} guest(s) joined from a public address; NAT {pl.nd.nat or 'unknown'}"
        ev = dict(file=pl.label, line=evs[0][0] if evs else 0, who="host", text=txt, marker="a guest joined across the Internet", hits=len(joined_public))
        res["F2"].update(ok=True, ev=[ev], missing=[])
        res["F1"].update(ok=True, ev=[dict(ev, marker="guests played over a public path")], missing=[])
    else:
        res["F2"]["missing"] = ["a guest that joined from a public (non-VPN, non-LAN) address"]
        res["F1"]["missing"] = ["a guest on a public path"]
    # F5 markers handled by the rows table; outage counts as a note
    outs = sum(len(p.nd.outages) for p in players)
    if outs:
        res["F5"]["notes"].append(f"{outs} link dropout(s) resumed (net_doctor)")
    # net_doctor failures
    for rid, lst in notes.items():
        for kind, text, key in lst:
            if kind == "F":
                e = first_evidence(players, key)
                ev = dict(cite(e[0], e[1], e[2], ndlogs) if e else dict(file="(net_doctor)", line=0, who="?", text=""),
                          marker="net_doctor: " + mask(text, ndlogs)[:160], hits=1)
                res[rid]["fails"].append(ev)
            else:
                res[rid]["notes"].append("net_doctor: " + mask(text, ndlogs)[:160])
    # B4 loads
    res["B4"]["notes"].append("loading screens per PC: " + ", ".join(f"{p.who} {p.loads}" for p in players))

    # ---- screenshots as proof (A8 / E2)
    for r in ROWS:
        for rx in r["files"]:
            for img in saves["images"]:
                if re.search(rx, img["name"]):
                    res[r["id"]]["ev"].append(dict(file=img["rel"], line=0, who="?", text="screenshot", marker="a screenshot of the row", hits=1))
                    res[r["id"]]["ok"] = True
                    res[r["id"]]["missing"] = []
                    break

    # ---- A2
    a2 = res["A2"]
    if saves["pairs"]:
        bad = [p for p in saves["pairs"] if p["sp_changed"]]
        if bad:
            a2["fails"].append(dict(file=bad[0]["post"], line=0, who="?", marker="a single-player save file changed",
                                    text=f"{len(bad[0]['sp_changed'])} file(s) differ: " + ", ".join(bad[0]["sp_changed"][:4]), hits=len(bad)))
        else:
            p = saves["pairs"][0]
            a2.update(ok=True, ev=[dict(file=p["post"], line=0, who="?", marker="single-player saves identical before and after",
                                        text=f"{p['sp_files']} single-player file(s) unchanged; party saves: {p['party_changed']} changed", hits=len(saves["pairs"]))],
                      missing=[])
    else:
        a2["missing"] = ["savehash_pre / savehash_post pair (run --hash-saves before and after, on every PC)"]

    # ---- verdicts
    for rid, r in res.items():
        if r["fails"]:
            r["status"] = FAILED
        elif r["ok"]:
            r["status"] = PROVEN
        else:
            r["status"] = NOT
    return res


# ---------------------------------------------------------------- save hashes

hash_saves = save_hash.hash_saves  # scripts/save_hash.py ships with the release; players run it


def compare_hashes(pre, post):
    a, b = pre["files"], post["files"]
    sp_all = {k for k in set(a) | set(b) if not k.startswith("savedata_party/")}
    sp_changed = sorted(k for k in sp_all if a.get(k) != b.get(k))
    party = {k for k in set(a) | set(b) if k.startswith("savedata_party/")}
    return dict(sp_files=len([k for k in sp_all if k in b]), sp_changed=sp_changed,
                party_changed=len([k for k in party if a.get(k) != b.get(k)]))


def collect_extras(logdir):
    """save hash pairs and screenshots under logdir."""
    logdir = Path(logdir)
    pairs, images = [], []
    for pre in sorted(logdir.rglob("savehash_pre*.json")):
        post = pre.with_name(pre.name.replace("savehash_pre", "savehash_post"))
        if not post.exists():
            continue
        try:
            a, b = json.loads(pre.read_text()), json.loads(post.read_text())
            c = compare_hashes(a, b)
        except (ValueError, KeyError, OSError):
            continue
        c.update(pre=pre.relative_to(logdir).as_posix(), post=post.relative_to(logdir).as_posix())
        pairs.append(c)
    for img in sorted(logdir.rglob("*")):
        if img.is_file() and img.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp"):
            images.append(dict(name=img.name, rel=img.relative_to(logdir).as_posix()))
    return dict(pairs=pairs, images=images)


# ---------------------------------------------------------------- output

def to_markdown(res, players, ndlogs, extras):
    cnt = Counter(r["status"] for r in res.values())
    out = ["# Session score", "",
           f"Logs: {len(players)} PC(s): " + ", ".join(f"{p.who} ({p.label})" for p in players), "",
           f"{cnt[PROVEN]} PROVEN, {cnt[FAILED]} FAILED, {cnt[NOT]} NOT EXERCISED of {len(res)} rows.", "",
           "| Row | Name | Status | Evidence (file:line) | Missing / notes |", "|---|---|---|---|---|"]
    for r in res.values():
        e = r["fails"] if r["status"] == FAILED else r["ev"]
        cites = "; ".join(f"{x['file']}:{x['line']} ({x['marker']})" if x["line"] else f"{x['file']} ({x['marker']})" for x in e[:3])
        tail = []
        if r["status"] == FAILED and r["ok"]:
            tail.append("proof markers were also seen")
        if r["missing"] and r["status"] != PROVEN:
            tail.append("missing: " + "; ".join(r["missing"]))
        tail.extend(r["notes"])
        out.append("| %s | %s | %s | %s | %s |" % (r["id"], r["name"], r["status"], cites.replace("|", "/"), " | ".join(tail).replace("|", "/")))
    return mask("\n".join(out), ndlogs) + "\n"


def to_json(res, players, ndlogs, extras):
    cnt = Counter(r["status"] for r in res.values())
    doc = dict(logs=[dict(who=p.who, file=p.label, role=p.role, loading_screens=p.loads) for p in players],
               summary=dict(proven=cnt[PROVEN], failed=cnt[FAILED], not_exercised=cnt[NOT], rows=len(res)),
               saves=extras["pairs"], rows=[dict(id=r["id"], name=r["name"], status=r["status"], evidence=r["ev"],
                                                 failures=r["fails"], missing=r["missing"], notes=r["notes"]) for r in res.values()])
    return mask(json.dumps(doc, indent=1), ndlogs)


def run(logdir):
    with tempfile.TemporaryDirectory(prefix="session_score_") as tmp:
        players, ndlogs = load_players(logdir, tmp)
        if not players:
            return None
        extras = collect_extras(logdir)
        res = score(players, ndlogs, extras)
        return to_markdown(res, players, ndlogs, extras), to_json(res, players, ndlogs, extras), res


def main(argv=None):
    ap = argparse.ArgumentParser(description="Score a real party session per ERSC parity row.")
    ap.add_argument("logdir", nargs="?", help="folder with the host's and the guests' logs")
    ap.add_argument("--json", help="write the JSON here")
    ap.add_argument("--md", help="write the markdown table here")
    ap.add_argument("--hash-saves", metavar="DIR", help="hash the game's user\\savedata and savedata_party into -o")
    ap.add_argument("-o", help="output of --hash-saves")
    ap.add_argument("--list-rows", action="store_true", help="print the scored row ids and check them against the doc")
    a = ap.parse_args(argv)
    if a.hash_saves:
        doc = hash_saves(a.hash_saves)
        text = json.dumps(doc, indent=1)
        if a.o:
            Path(a.o).write_text(text)
            print(f"{len(doc['files'])} save file(s) hashed into {a.o}")
        else:
            print(text)
        return 0
    if a.list_rows:
        mine, theirs = set(ROW_IDS), set(doc_row_ids())
        print(" ".join(ROW_IDS))
        print("not in the doc:", sorted(mine - theirs), "| missing here:", sorted(theirs - mine))
        return 0
    if not a.logdir:
        ap.error("a log folder is required")
    out = run(a.logdir)
    if out is None:
        print("No party logs found in that folder (host and guest logs with 'party networking on').", file=sys.stderr)
        return 2
    md, js, _ = out
    if a.json:
        Path(a.json).write_text(js)
    if a.md:
        Path(a.md).write_text(md)
    sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
