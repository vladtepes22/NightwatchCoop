#!/usr/bin/env python3
"""Live test of the in-game party flow through the REAL menus (docs/party/ingame_flow.md): two
instances start as the single-player game; the title's Party page is walked with the game's own pad
path (BB_TEST_MENU_FILE: MenuScript lines, test_newgame.h; text typed into the window's text box as
SDL key events, no OS input) and every restart goes through run.bat's exit-75 loop.

  python tools/mp/flow_live.py fresh    --root DIR --game DIR   # 1+2: New party game / Join party game
  python tools/mp/flow_live.py continue --root DIR --game DIR   # 3: Continue party game, both
      [--guest-world SCRIPT]     # 4: then in the world (MenuScript): e.g. Party > Leave, Join party

Screenshots go to ROOT/shots/<phase>/ (tools/mp/shot.py --watch on the run logs). The password is
PASSWORD below; the guest types the host's 127.0.0.1:<port> address.
"""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import instances  # noqa: E402

PASSWORD = "Moon-42"

# The title menu (empty single-player save): New Game, Party, System, Log In. The Party page opens
# on its first row: New party game, Join party game, [Continue party game,] Host address, Password,
# Party name. Every row is a button acting on Cross ("Confirm" rows: twice).
TITLE_TO_PARTY = "~down@New Game/cross/!~mark=page@Host address"


def to_party_page():
    """The in-world walk from the Options grid to System > Party: Right, Right, Cross (System), then
    Up from the top (wrapping to the bottom: Exit Game, Network, Brightness, then the port's pages
    after Party, read from bbport_game_menu_pages.cpp so a new page does not break the walks), Cross."""
    import re as _re
    src = (REPO / "gpu" / "shim" / "bbport_game_menu_pages.cpp").read_text(encoding="utf-8", errors="replace")
    pages = _re.findall(r'AddPage\("([^"]+)"', src)
    after = len(pages) - pages.index("Party") - 1
    return "options/right/right/cross/" + "/".join(["up"] * (3 + after + 1)) + "/cross"


def scripts(phase, port0):
    host_addr = f"127.0.0.1:{port0}"
    if phase == "fresh":
        host = [TITLE_TO_PARTY + "/down/down/down/cross/type=" + PASSWORD + "/mark=h_pw_typed/enter/mark=h_pw_saved"
                "/up/up/up/mark=h_new/cross/mark=h_new_again/cross/mark=h_restart", "-"]
        guest = [TITLE_TO_PARTY + "/down/down/cross/type=" + host_addr + "/mark=g_addr_typed/enter/mark=g_addr_saved"
                 "/down/cross/type=" + PASSWORD + "/mark=g_pw_typed/enter/mark=g_pw_saved"
                 "/up/up/mark=g_join/cross/mark=g_join_again/cross/mark=g_restart", "-"]
        return host, guest
    if phase == "continue":
        # With a party character: New party game, Join party game, Continue party game, Host address ...
        line = TITLE_TO_PARTY + "/down/down/mark={w}_cont/cross/mark={w}_cont_again/cross/mark={w}_restart"
        # The guest first opens Host address twice (the saved address comes up; Enter keeps it, Esc
        # cancels): every press of a text row opens the box.
        guest = (TITLE_TO_PARTY + "/down/down/down/cross/mark=g_addr_open/enter/mark=g_addr_kept/cross"
                 "/mark=g_addr_open2/esc/up/mark=g_cont/cross/mark=g_cont_again/cross/mark=g_restart")
        return [line.format(w="h"), "-"], [guest, "-"]
    raise SystemExit("unknown phase " + phase)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("phase", choices=["fresh", "continue", "world"])
    ap.add_argument("--root", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--seconds", type=int, default=420)
    ap.add_argument("--guest-world", default="", help="the guest's in-world walk after the restart")
    ap.add_argument("--host-world", default="", help="the host's in-world walk after the restart")
    ap.add_argument("--until", action="append", default=[])
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for both instances")
    ap.add_argument("--host-env", action="append", default=[], help="KEY=VALUE for the host only")
    ap.add_argument("--guest-env", action="append", default=[], help="KEY=VALUE for the guest (inst1) only")
    ap.add_argument("--count", type=int, default=2, help="continue: 3 = a second guest (a copy of inst1's party "
                    "character and party.json under another name)")
    ap.add_argument("--host-delay", type=int, default=8, help="seconds in the joined world before --host-world")
    a = ap.parse_args()
    root = Path(a.root)
    port0 = instances.base_port(root)
    # world: both start straight in the party game (the launcher's way: BB_PARTY, party.json via
    # BB_PARTY_MENU, Continue on its own) - no title walk, no restart: short runs for in-world tests.
    host, guest = scripts("continue" if a.phase == "world" else a.phase, port0)
    if a.phase == "world":
        host, guest = ["-", "-"], ["-", "-"]
    host = [("title 5 " + l) if l != "-" else l for l in host]
    guest = [("title 5 " + l) if l != "-" else l for l in guest]
    # The party process after the restart: an in-world walk (Options > System > Party ...).
    # {PARTY} in a walk: Options > System > Party (to_party_page).
    a.host_world = a.host_world.replace("{PARTY}", to_party_page())
    a.guest_world = a.guest_world.replace("{PARTY}", to_party_page())
    if a.host_world:
        host[1] = f"joined {a.host_delay} " + a.host_world
    if a.guest_world:
        guest[1] = "joined 8 " + a.guest_world
    if a.phase == "world":
        host = [host[1]] + ["-"] * 3
        guest = [guest[1]] + ["-"] * 3
    if a.count > 2 and a.phase in ("continue", "world"):
        import json as _json
        for k in range(2, a.count):
            src, dst = root / "inst1" / "user", root / f"inst{k}" / "user"
            dst.mkdir(parents=True, exist_ok=True)
            if not (dst / "savedata_party").exists():
                shutil.copytree(src / "savedata_party", dst / "savedata_party")
            pj = _json.loads((src / "party.json").read_text(encoding="utf-8"))
            pj["name"] = (pj.get("name") or "Hunter")[:14] + f"-{k}"
            (dst / "party.json").write_text(_json.dumps(pj, indent=2), encoding="utf-8")
    all_lines = [host, guest] + [[l if not l.startswith("joined") else "-" for l in guest]] * max(0, a.count - 2)
    for i, lines in enumerate(all_lines):
        user = root / f"inst{i}" / "user"
        user.mkdir(parents=True, exist_ok=True)
        if a.phase == "fresh":
            for name in ("party.json", "party_code.txt", "party_relaunch.bat", "party_state.json"):
                (user / name).unlink(missing_ok=True)
            for d in ("savedata", "savedata_party"):
                shutil.rmtree(user / d, ignore_errors=True)
        (root / f"menu{i}.txt").write_text("\n".join(lines) + "\n", encoding="ascii")
    shots = root / "shots" / a.phase
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True)
    watch = []
    for tag in ("page", "h_pw_typed", "h_pw_saved", "h_new", "h_new_again", "g_addr_typed", "g_addr_saved",
                "g_pw_typed", "g_pw_saved", "g_join", "g_join_again", "g_addr_open", "g_addr_kept", "g_addr_open2", "h_cont", "h_cont_again", "g_cont", "g_cont_again"):
        watch += ["--watch", rf"step \d+/\d+: ~?mark={tag}(@|\s)={tag}@1.2"]
    for n in range(1, 16):  # in-world walks: mark=w1 ... mark=w15
        watch += ["--watch", rf"step \d+/\d+: ~?mark=w{n}(@|\s)=w{n}@1.2"]
    watch += ["--watch", r"relaunching \(exit code 75=relaunch@0.3",
              "--watch", r"Party: starting the party game \(join\)=restarted@25,45",
              "--watch", r"newgame: .*done=newgame_done@2",
              "--watch", r"session trying-to-host .* -> host=hosting@3",
              "--watch", r"session trying-to-join .* -> client \(6=summoned@1,3",
              "--watch", r"test menu: step .*(leave|join)\w*=world_step@1.5",
              "--watch", r"Party flow: leave=left@4",
              "--watch", r"STUCK=stuck@0",
              # Every live check needs pictures of all windows at join, arrival and leave.
              "--watch", r"cooperators 0 -> 1=arrival1@2,8",
              "--watch", r"cooperators 1 -> 2=arrival2@2,8,20",
              "--watch", r"Leave pressed=leave@1,4,8",
              "--watch", r"host lost .*home now=hostlost@2,6",
              "--watch", r"boss health bar shown=bossbar@1",
              "--watch", r"boss health bar: off=bossoff@1,10",
              # Title join (BB_PARTY_TITLE_JOIN): the title, the load, the arrival.
              "--watch", r"title join: host snapshot applied=tj_ready@0.3",
              "--watch", r"title join: loading=tj_load@1",
              "--watch", r"title join: in the world=tj_world@1,4,8",
              "--watch", r"title join: FAILED=tj_fail@1,6"]
    shot = subprocess.Popen([sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                             "--seconds", "7200"] + watch)
    env = [f"BB_TEST_MENU_FILE={root}\\menu{{i}}.txt", "BB_PARTY_PORT={port}", "BB_PARTY_UPNP=0",
           "BB_PARTY_STUN=off", "BB_AUDIO=none", "BB_TEST_NEWGAME=party", "BB_TEST_PROLOGUE_SKIP=party",
           "BB_TEST_NAME=Hunter{i}"]
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(a.count), "--root", str(root), "--game", a.game,
           "--saves", str(root / "nosaves"), "--build", str(REPO), "--seconds", str(a.seconds), "--idle", "0",
           "--label", f"flowlive {a.phase}", "--timestamps"]
    if a.phase == "world":
        # Pre-made characters: no new-game driver (its "skip the opening" Options presses opened
        # the in-game menu on a loaded character and kept the guests in item screens).
        env = [e for e in env if not e.startswith(("BB_TEST_NEWGAME=", "BB_TEST_PROLOGUE_SKIP=", "BB_TEST_NAME="))]
        env += ["BB_PARTY_MENU=1", "BB_PARTY_SAVE=separate", "BB_PARTY_AUTOCONTINUE=1",
                "BB_SKIP_NETWORK_CHOICE=online", "BB_PATCHES=Skip Intro"]
        cmd += ["--ienv", "0:BB_PARTY=host"] + sum((["--ienv", f"{k}:BB_PARTY=join"] for k in range(1, a.count)), [])
        import json as _json
        for k in range(a.count):  # the relaunch sets BB_PARTY_NAME from party.json; so does this
            pj = root / f"inst{k}" / "user" / "party.json"
            if pj.exists() and _json.loads(pj.read_text(encoding="utf-8")).get("name"):
                cmd += ["--ienv", f"{k}:BB_PARTY_NAME=" + _json.loads(pj.read_text(encoding="utf-8"))["name"]]
    for e in env + a.env:
        cmd += ["--env", e]
    for e in a.host_env:
        cmd += ["--ienv", "0:" + e]
    for e in a.guest_env:
        cmd += ["--ienv", "1:" + e]
    # The summon, and a little of the log after it (the screenshots at +3 s).
    for u in a.until or [r"Party: session trying-to-join .* -> client \(6[\s\S]{1500}"]:
        cmd += ["--until", u]
    subprocess.run([sys.executable, str(HERE / "instances.py"), "setup", "--count", str(a.count), "--root", str(root),
                    "--build", str(REPO), "--game", a.game, "--saves", str(root / "nosaves")], check=True)
    r = subprocess.run(cmd)
    shot.terminate()
    for i in range(a.count):
        pj = root / f"inst{i}" / "user" / "party.json"
        print(f"inst{i} party.json:", pj.read_text(encoding="utf-8") if pj.exists() else "(none)")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
