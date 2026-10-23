#!/usr/bin/env python3
"""Party co-op H3 save-consistency check: did every player's OWN save keep the party's progress?

  python -I tools/party/save_check.py ROOT [--host 0] [--inc gpu/shim/party/party_flags.inc] [--json OUT]

ROOT is a test root of tools/mp/instances.py (inst0 = host, inst1.. = guests: each with run.log and,
with BB_PARTY_FLAG_DUMP=1, user/party/flags_*.bbpf), or a list of game folders / logs given as
ROOT=host_dir,guest_dir[,...] (each folder: user/party/*.bbpf and logs/*.log or run.log).

Checks, per guest:

  flags   The host's last own-world snapshot (load mode 0) against the guest's last snapshot of its
          OWN save pool (the "<label>-pool" dump, or a load-mode-0 dump in its own world) taken
          after it: every host flag of a set-only synced category (boss_defeated, lamp_unlocked,
          shortcut_door, key_event, cutscene_seen; world_state as a warning: some are rebuilt per
          load) must be set in the guest's own pool. Blocks the guest's pool does not hold are skipped.
  items   From the guest's log (all of its runs, restarts included): no item lot given twice
          ("gave #N lot L ..." more than once without a skip), and every host award of a ledger or
          flagged lot ("award lot L" on the host while the guest was in the party) was given or
          skipped as already given on the guest at least once.
  echoes  From the guest's log: the own Echoes / level the game reported never go down across a
          crash-restart except through a death (a lost save would show as a drop after "Boot").
  saves   The guest's character saves (user/savedata_party or user/savedata, userdata0000..0009): a save
          written in the host's world has an empty world-state section (docs/party/save_safety.md 5), which
          save editors cannot read; the launcher's Export for save editor (scripts/save_editor.py) must
          turn every save into one Noxde's editor reads (editor_check.py) and its import must give the
          same bytes back: EDITOR when not.

Exit status 0 = consistent, 1 = something lost or doubled, 2 = nothing to compare (no dumps / logs).
The report names each missing flag with its category and each lot with the log line numbers.
"""
import argparse, json, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flag_tool as ft  # noqa: E402
import save_decode as sd  # noqa: E402
import editor_check as ec  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import save_editor as se  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
MUST = ("boss_defeated", "lamp_unlocked", "shortcut_door", "key_event", "cutscene_seen")
WARN = ("world_state",)

GAVE_RX = re.compile(r"\bgave #\d+ lot (\d+)\b")
SKIP_RX = re.compile(r"\bskip #\d+ lot (\d+)\b.*already given")
AWARD_RX = re.compile(r"\baward lot (\d+) \(hostOnly (\d), flag (-?\d+)\)")
JOIN_RX = re.compile(r"cooperators (-?\d+) -> (-?\d+)")
BOOT_RX = re.compile(r"\bBoot: launching\b|^\[\+ *[\d.]+\] (?:\[coop\] )?(?:Party: )?(?:\[[^\]]*\] )?boot\b|BB_PARTY_RESTART|restart \d+ of \d+")
ECHO_RX = re.compile(r"\bEchoes (\d+)\b")
DEATH_RX = re.compile(r"\bdied\b|\bdeath\b|\bdown\b|bloodstain", re.I)


def snapshots(folder):
    """Every readable BBPF snapshot under folder/user/party (or folder itself), oldest first."""
    out = []
    for d in (Path(folder) / "user" / "party", Path(folder) / "party", Path(folder)):
        if d.is_dir():
            for p in d.glob("flags_*.bbpf"):
                try:
                    s = ft.Snapshot.read(p)
                except (OSError, ValueError):
                    continue
                s.path = p
                out.append(s)
            if out:
                break
    out.sort(key=lambda s: (s.unix_ms, s.path.stat().st_mtime_ns))
    return out


def log_lines(folder):
    folder = Path(folder)
    if folder.is_file():
        files = [folder]
    else:
        files = [folder / "run.log"] if (folder / "run.log").is_file() else sorted((folder / "logs").glob("*.log"))
    lines = []
    for f in files:
        lines += f.read_text(encoding="utf-8", errors="replace").splitlines()
    return lines


def own_pool(snaps, after_ms=0):
    """The guest's last own-save view: a '-pool' dump, else a load-mode-0 dump."""
    pools = [s for s in snaps if s.label.endswith("-pool") and s.unix_ms >= after_ms]
    if pools:
        return pools[-1]
    own = [s for s in snaps if s.load_mode == 0 and s.unix_ms >= after_ms]
    return own[-1] if own else None


def host_world(snaps, before_ms=None):
    own = [s for s in snaps if s.load_mode == 0 and not s.label.endswith("-pool") and
           (before_ms is None or s.unix_ms <= before_ms)]
    return own[-1] if own else None


def check_flags(host_snaps, guest_snaps, rows):
    g = own_pool(guest_snaps)
    if not g:
        return {"status": "no guest own-pool dump"}
    h = host_world(host_snaps, g.unix_ms)
    if not h:
        return {"status": "no host own-world dump before the guest's"}
    missing, warn = [], []
    gset = set(g.set_ids())
    for f in h.set_ids():
        if f // h.bits not in g.blocks:
            continue
        cat = ft.categorize(rows, f)[0]
        if f in gset:
            continue
        if cat in MUST:
            missing.append((f, cat))
        elif cat in WARN:
            warn.append((f, cat))
    return {"status": "ok" if not missing else "LOST", "host": h.path.name, "guest": g.path.name,
            "host_age_s": round((g.unix_ms - h.unix_ms) / 1000.0, 1), "missing": missing, "warn": warn}


def check_items(host_lines, guest_lines):
    gave, skipped = {}, {}
    for n, line in enumerate(guest_lines, 1):
        m = GAVE_RX.search(line)
        if m:
            gave.setdefault(int(m.group(1)), []).append(n)
        m = SKIP_RX.search(line)
        if m:
            skipped.setdefault(int(m.group(1)), []).append(n)
    doubled = {lot: lines for lot, lines in gave.items() if len(lines) > 1}
    awarded, coop = {}, 0
    for n, line in enumerate(host_lines, 1):
        m = JOIN_RX.search(line)
        if m:
            coop = max(0, int(m.group(2)))
        m = AWARD_RX.search(line)
        if m and coop > 0:
            awarded.setdefault(int(m.group(1)), n)
    missing = {lot: n for lot, n in awarded.items() if lot not in gave and lot not in skipped}
    return {"status": "ok" if not doubled and not missing else "LOST" if missing else "DOUBLED",
            "given": len(gave), "skipped": len(skipped), "awarded": len(awarded),
            "doubled": doubled, "missing": missing}


def check_echoes(guest_lines):
    """Echoes seen right before a restart vs the first value after it (a drop = an unsaved loss)."""
    last, drops, died, boots = None, [], False, 0
    for n, line in enumerate(guest_lines, 1):
        if BOOT_RX.search(line):
            boots += 1
            continue
        if DEATH_RX.search(line):
            died = True
        m = ECHO_RX.search(line)
        if m:
            v = int(m.group(1))
            if last is not None and v < last[0] and not died:
                drops.append((last[0], v, last[1], n))
            last, died = (v, n), False
    return {"status": "ok" if not drops else "LOST", "restarts": boots, "drops": drops}


def check_saves(folder):
    """Every character save of the folder's game (user/savedata_party, else user/savedata): the export for
    a save editor reads in the editor (editor_check) and its import gives the same bytes back."""
    folder = Path(folder)
    files = []
    for d in ("savedata_party", "savedata"):
        files = sorted((folder / "user" / d).glob("**/userdata000[0-9]"))
        if files:
            break
    checked, guest, bad = 0, [], []
    for f in files:
        try:
            data = f.read_bytes()
        except OSError:
            continue
        if len(data) < 0x54 or not any(data[:16]):
            continue  # an empty slot
        checked += 1
        try:
            empty = se.world_empty(data)
            out, changed = se.editor_copy(data)
            back = se.restore_from_editor(out, changed)
        except ValueError as ex:
            bad.append(f"{f.name}: {ex}")
            continue
        if empty:
            guest.append(f.name)
        probs = ec.parse(out)["problems"]
        if probs or back != data:
            bad.append(f"{f.name}: " + ("; ".join(probs) if probs else "the import does not give the save back"))
    if not checked:
        return {"status": "no saves"}
    return {"status": "EDITOR" if bad else "ok", "checked": checked, "guest_written": guest, "bad": bad}


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root")
    ap.add_argument("--host", type=int, default=0)
    ap.add_argument("--inc", default=str(REPO / "gpu" / "shim" / "party" / "party_flags.inc"))
    ap.add_argument("--json")
    a = ap.parse_args()
    if "," in a.root:
        folders = [Path(x) for x in a.root.split(",")]
    else:
        folders = sorted((p for p in Path(a.root).glob("inst*") if p.is_dir()), key=lambda p: int(p.name[4:] or 0))
    if len(folders) < 2:
        print("need a host and at least one guest folder")
        return 2
    rows = ft.load_inc(a.inc)
    host = folders[a.host]
    hs, hl = snapshots(host), log_lines(host)
    report, bad, compared = {}, False, False
    for i, folder in enumerate(folders):
        if i == a.host:
            continue
        gs, gl = snapshots(folder), log_lines(folder)
        r = {"flags": check_flags(hs, gs, rows) if hs and gs else {"status": "no dumps (BB_PARTY_FLAG_DUMP=1)"},
             "items": check_items(hl, gl) if gl else {"status": "no log"},
             "echoes": check_echoes(gl) if gl else {"status": "no log"},
             "saves": check_saves(folder) if folder.is_dir() else {"status": "no saves"}}
        report[folder.name] = r
        print(f"== {folder.name}")
        f = r["flags"]
        if "missing" in f:
            compared = True
            print(f"flags:  {f['status']}: host {f['host']} vs guest own pool {f['guest']} ({f['host_age_s']} s apart); "
                  f"{len(f['missing'])} missing, {len(f['warn'])} world_state warnings")
            for fid, cat in f["missing"][:40]:
                print(f"  missing {fid} ({cat})")
            for fid, cat in f["warn"][:10]:
                print(f"  warning {fid} ({cat})")
        else:
            print(f"flags:  {f['status']}")
        it = r["items"]
        if "given" in it:
            compared = True
            print(f"items:  {it['status']}: {it['given']} lots given, {it['skipped']} skipped as already given, "
                  f"{it['awarded']} host awards while partied")
            for lot, lines in it["doubled"].items():
                print(f"  lot {lot} given {len(lines)} times (guest log lines {lines})")
            for lot, n in it["missing"].items():
                print(f"  lot {lot} awarded on the host (host log line {n}) never reached this guest")
        else:
            print(f"items:  {it['status']}")
        e = r["echoes"]
        if "drops" in e:
            compared = True
            print(f"echoes: {e['status']} ({e['restarts']} restart(s))")
            for before, after, n0, n1 in e["drops"]:
                print(f"  Echoes {before} (line {n0}) -> {after} (line {n1}) without a death")
        else:
            print(f"echoes: {e['status']}")
        sv = r["saves"]
        if "checked" in sv:
            compared = True
            print(f"saves:  {sv['status']}: {sv['checked']} character save(s), {len(sv['guest_written'])} written in "
                  f"the host's world (editors need the launcher's export)")
            for line in sv["bad"]:
                print("  " + line)
        else:
            print(f"saves:  {sv['status']}")
        bad = bad or any(x.get("status") in ("LOST", "DOUBLED", "EDITOR") for x in r.values())
    if a.json:
        Path(a.json).write_text(json.dumps(report, indent=1, default=str), encoding="utf-8")
    if not compared:
        return 2
    print("RESULT:", "INCONSISTENT" if bad else "consistent")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
