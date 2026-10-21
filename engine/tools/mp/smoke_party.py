#!/usr/bin/env python3
"""Post-merge party smoke test: a fixed ~15-20 min 3-player scenario with an automatic verdict.

  python tools/mp/smoke_party.py [--root DIR] [--build DIR] [--game DIR] [--out DIR] [--phase s1,s2]
  python tools/mp/smoke_party.py --check-only --out DIR     (verdict again from the saved logs)

Mandatory after every merge to master (agents/RULES_062.txt). Runs through tools/mp/instances.py
(the shared game queue; 3 players: host and guest 1 rendered, guest 2 headless-lite; 127.0.0.1,
the instance root's own port range, muted, background windows, BB_DLSS=0), the default test save
(E:\\games\\bbport\\test_saves\\savedata, fresh copy each phase), timed screenshots of both rendered
games (tools/mp/shot.py --every) and a PASS / FAIL per step from the logs.

Phase s1 (~8 min, Father Gascoigne un-killed: the d10 fixture flags cleared on every save):
  join (3 players), Leave + Rejoin (Party page), a guest's death and its bloodstain, a party lamp
  travel to the Tomb of Oedon, a guest-started Gascoigne fight (intro on everyone), the host's death
  in the fight (limbo + spectate), the two guests' deaths (a wipe: the party's one respawn load).
Phase s2 (~8 min, the Cleric Beast un-killed: the b2_seamless 6.7 fixture flags):
  join, a party lamp travel to the Cleric Beast's lamp, the fight, guest 2 dead in it (limbo), the
  boss killed by the host: guest 2 up in the arena with no load, then hit by an enemy (group
  `arena`), (echoes and the reward item box on every game: its OK row), Leave + Rejoin right after the
  kill (A3 / K10b), a guest crash and its title rejoin (C6), the host's crash and recovery (the
  guests wait, then are summoned back), the host's Leave (every guest home).
Phase rules (~3 min, 2 players, BB_GAME_MENU_TRACE + BB_TEST_MENU_FILE: the real System menu walked
  by the game's own pad path): host and guest open each party rules page (Deaths & revives, World &
  PvP, Story & dialogue, Party rewards, Enemy scaling, Party display; a screenshot of each); every
  page's rows and values from its trace line ("Game menu: page N rows (k): NAME=VALUE | ...", no
  text cut); the host changes one option per page and sets it back (the menu's "row ... -> ..." line,
  bbport.ini's line, the party's rules line where the rule is the host's); the guest sees the host's
  friendly fire on its read-only row.
Phase summonfail (~3 min, 3 players): guest 1's party port is held by the smoke (every join of it
  fails); guest 2 starts after guest 1's failed direct summon and must be served within 30 s and end
  up in the host's world (a failed join once locked the host's direct summon for minutes).

Test aids used (it is a smoke, not real play): BB_PARTY_DIRECTOR_TEST steps (lamp warps, moves into
the boss trigger, scripted deaths, the boss kill, Party > Leave / Rejoin presses),
BB_PARTY_PROGRESS_TEST_CLEAR (boss fixtures), BB_PARTY_TEST_INVINCIBLE (players moved into a fight
live until their scripted death), instances.py --crash-plan (process kills).

Output: OUT/<phase>/shots/*.png, OUT/<phase>/inst{0,1,2}.log (this run's part of each run.log),
OUT/verdict.txt (one line per step: PASS / FAIL, the evidence line or what was missing, timings).
Exit code 0 only when every step passed. LOOK AT THE SCREENSHOTS too (agents/RULES_062.txt): the
verdict reads logs, it cannot see a grey screen, a camera in a wall or a stuck dialog.
"""
import argparse, os, re, shutil, struct, subprocess, sys, time
from pathlib import Path


class LaterProc:
    """Starts a helper (shot.py, the sampler) only once the harness has started the games (after the
    shared game queue's wait), so its run timer and window watch cover the run, not the wait."""

    def __init__(self, harness_log, args, **kw):
        import threading
        self.p, self.stop = None, False

        def run():
            while not self.stop:
                try:
                    if "instance 0 started" in Path(harness_log).read_text(errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(1)
            if not self.stop:
                self.p = subprocess.Popen(args, **kw)

        threading.Thread(target=run, daemon=True).start()

    def terminate(self):
        self.stop = True
        if self.p:
            self.p.terminate()

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
AGENT_ROOT = REPO.parent  # agents/NAME when run from an agent worktree
DEFAULT_ROOT = (AGENT_ROOT / "mptest") if AGENT_ROOT.parent.name == "agents" else (REPO.parent / "mptest")

# Fixture flags (docs/party/b2_seamless.md 6.7, ERSC_PARITY.md D10).
GASCOIGNE_CLEAR = "12411800,12411802,12411803,12414800,12414801,12414802,12414223,2412,9457,2410"
CLERIC_CLEAR = "12411700,12411701,12411702,12411703,12414700,12414701,12414223,72410511,2411,9456,2410"
# + the boss reward's ledger flag (lot 50000010, Sword Hunter Badge): given again at the kill, so the
# summoned guest gets an item box (its OK row, pard3)
CLERIC_CLEAR_REWARD = CLERIC_CLEAR + ",60009036"
# A treasure lot of one Pebble (goods 1230, maxNum 20, maxRepositoryNum 99 at EquipParamGoods +0x48)
# and its flag (cleared so the host's award rolls it).
PEBBLE_LOT, PEBBLE_FLAG = "2100000", "52100000"
STORAGE_GIVE = (r"lot 2100000 .*goods 1230 x1 \(held 20 -> 20, past the maximum: the game's give list, "
                r"storage room (\d+) -> (\d+)")
GASCOIGNE_TRIGGER = "-48.76:-40.92:72.44"  # region 2412815 (the intro's trigger box)
CLERIC_TRIGGER_HOST = "-136.5:-26.6:52.8"   # region 2412805 (first fight)
CLERIC_SPOT_GUEST = "-134.6:-26.6:54.7"


# Speed (smokefix 2026-10-16, "lots of standing around"): every step waits only as long as the game
# needs (delays measured from the previous step's log, about 10 s of margin each), and a phase ends
# the moment its last checked line is logged (instances.py --until), not after a fixed run length;
# `seconds` is only the cap. Same steps and checks as before.
# The scripted host death, or the boss got there first (the step then logs it found the chr dead)
HOST_DIED = r"travel step \d+/\d+: host_die|travel step \d+ \(host_die\) waits: the own chr is dead"
LEAVE_OUT = r"the host ended the party|host lost \(the host left its room\)"
# The wipe itself ("a wipe; the stock funnel runs" / "a wipe; the party respawns together"), not the
# limbo line, which also says "..., a wipe or a rest" (it ended s1 before the guests died)
WIPE_RX = r"a wipe; the"
WIPE_UNTIL = "0:" + WIPE_RX


def phase_s1(only):
    host = ",".join([
        "host_warp=2412953@70",             # party lamp travel: the Tomb of Oedon (after the leave/rejoin and death)
        "host_die@6%m24_01~",               # in the fight: 6 s after the boss intro (the guest went in first)
    ])
    g1 = ",".join([
        "echoes=5000",                        # Blood Echoes to drop (the bloodstain)
        "guest_leave@10", "guest_rejoin@5",   # Party > Leave, Rejoin
        "guest_die@12",                       # a death outside a fight: bloodstain, respawn at the host's lamp
        "guest_obj=2411810@20%m24_01",        # at Gascoigne's fog, facing it
        "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",  # Cross: the guest goes in first
        "guest_die@20%m24_01~",               # 14 s after the host's death in the fight: the wipe
    ])
    g2 = "guest_die@26%m24_01~"                # the headless guest: down too (everyone down: a wipe)
    return dict(
        name="s1", seconds=420,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2],
        crash=[],
        until=[WIPE_UNTIL, "0:spectating: following"],
    )


def phase_s2(only):
    host_tail = not only or bool({"host_crash", "host_leave"} & only)
    host = ",".join([
        "goods=4114:0",                                           # the test save holds the badge: the kill's reward would say "storage max"
        "host_warp=2412952@15",                                   # party lamp travel: the Cleric Beast's lamp
        "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,          # starts the fight
        "host_kill=2410800@25%m24_01",                            # the boss kill (its defeat path), after guest 2's death
    ])
    g1 = ",".join([
        "goods=1230:20",                    # Pebbles at their inventory maximum (20): the host's Pebble goes to storage
        "watch_text=Unable to acquire",     # the game's "inventory or storage max" box: must not show
        "guest_move=%s@25%%m24_01" % CLERIC_SPOT_GUEST,
        "guest_press=0x4000@29%m24_01", "guest_press=0x4000@4%m24_01",  # Cross: OK on the reward's item box
        "guest_leave@10%m24_01",            # Leave right after the kill (no load between), then Rejoin
        "guest_rejoin@5",
    ])
    # guest 1 crashes 15 s after its Rejoin press (after the boss kill: a title join into a cleared
    # area); the host crashes 15 s after guest 1 is back in its room.
    crash = [r"1:guest_rejoin: Party > Rejoin pressed@15"]
    if host_tail:
        crash.append(r"0:cooperators [01] -> 2@15")
        until = ["0:host_leave: Party > Leave pressed", "1:" + LEAVE_OUT, "2:" + LEAVE_OUT]
    else:
        until = ["0:cooperators [01] -> 2"]  # after the guest crash: back in the host's room
    # Arena revive (arenaguest 2026-10-21): guest 2 dies in the fight before the kill, lies in its limbo
    # when the boss dies and gets up in the arena with no load; then it stands next to a Central
    # Yharnam enemy (the spot where revive1 and arenaguest took hits) and takes its hits, and goes back
    # to the arena (guest_move: invincible again for the rest of s2).
    g2 = ",".join([
        "guest_move=%s@18%%m24_01" % CLERIC_SPOT_GUEST,     # into the fight (invincible)
        "guest_die@3%m24_01",                               # dead in the fight: its limbo
        "guest_press=0x0@3%m24_01!",                        # waits until it is up again (logs its HP)
        "guest_chr=2410100@3%m24_01",                       # next to an enemy on that enemy's floor
    ] + ["guest_press=0x800@1.5%m24_01"] * 4 + [            # R1: draws the enemy (A2: hit 8 s after)
        "guest_press=0x0@12%m24_01",                        # its HP after the enemy's hits
        "guest_move=%s@1%%m24_01" % CLERIC_TRIGGER_HOST,    # back to the arena (invincible)
    ])
    return dict(
        name="s2", seconds=480,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR_REWARD + "," + PEBBLE_FLAG, "BB_PARTY_TEST_INVINCIBLE=1"],
        watch=["--watch", "up again in the arena=arena@1,4", "--watch", r"travel step \d+/\d+: guest_chr=hits@6,10"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              # the host's own award of a Pebble treasure lot (host-only, 20 s after the join): the
              # summoned guest at its Pebble maximum gets it into its storage (items.md 10.4)
              "0:BB_PARTY_ITEMS_HOST_TEST=" + PEBBLE_LOT,
              # crash-restarted games: no second run of the chain; the restarted host Leaves last
              "0:BB_PARTY_DIRECTOR_TEST_RESTART=host_leave@25", "1:BB_PARTY_DIRECTOR_TEST_RESTART=none",
              "2:BB_PARTY_DIRECTOR_TEST_RESTART=none"],
        crash=crash,
        until=until,
    )


def _psf(entries):
    """A PSF file (param.sfo): [(key, fmt, value bytes, max length)], keys sorted, as shadPS4 writes it."""
    entries = sorted(entries)
    keys = b"".join(k.encode() + b"\0" for k, *_ in entries)
    keys += b"\0" * (-len(keys) % 4)
    key_off = 20 + 16 * len(entries)
    index, data, ko = b"", b"", 0
    for k, fmt, v, mx in entries:
        index += struct.pack("<HHIII", ko, fmt, len(v), mx, len(data))
        data += v + b"\0" * (mx - len(v))
        ko += len(k) + 1
    return b"\0PSF" + struct.pack("<IIII", 0x101, key_off, key_off + len(keys), len(entries)) + index + keys + data


def prep_savecopy(root: Path):
    r"""The host's party save as a player brings one from another build (savecopy): a copy of the
    instance's test save in shadPS4's layout (SPRJ0005\sce_sys\param.sfo inside, no SPRJ0005.sce_sys
    beside it), with the Bloodborne Save Editor's *.bak leftovers, over a party folder whose side
    files belong to another character in the same slot (party_characters.txt says so; party_rot.txt
    has its rot). The game must take the save in, drop that rot and load it."""
    user = root / "inst0" / "user"
    solo = user / "savedata"
    if not solo.is_dir():
        subprocess.run([sys.executable, str(HERE / "instances.py"), "setup", "--count", "2", "--root", str(root)], check=False)
    if not solo.is_dir():
        sys.exit("savecopy: %s has no test save (instances.py setup failed)" % solo)
    party = user / "savedata_party"
    if party.exists():
        shutil.rmtree(party)
    shutil.copytree(solo, party)
    for name in ("party_state.json", "party_story.json", "party_rot.txt", "party_prologue.txt", "party_stain.txt",
                 "party_gestures.txt", "party_characters.txt"):
        (user / name).unlink(missing_ok=True)
    slots = []
    for meta in list(party.rglob("*.sce_sys")):
        folder = meta.with_name(meta.name[:-8])
        p = (meta / "param.bin").read_bytes()
        title, sub, detail = (p[a:b].split(b"\0")[0] + b"\0" for a, b in ((0, 128), (128, 256), (256, 1280)))
        sfo = _psf([("ACCOUNT_ID", 0x0004, b"\0" * 8, 8), ("MAINTITLE", 0x0204, title, 128), ("SUBTITLE", 0x0204, sub, 128),
                    ("DETAIL", 0x0204, detail, 1024), ("SAVEDATA_DIRECTORY", 0x0204, folder.name.encode() + b"\0", 32),
                    ("SAVEDATA_LIST_PARAM", 0x0404, p[1280:1284], 4),
                    ("TITLE_ID", 0x0204, folder.parent.name.encode() + b"\0", 16)])
        (folder / "sce_sys").mkdir(exist_ok=True)
        (folder / "sce_sys" / "param.sfo").write_bytes(sfo)
        if (meta / "icon0.png").exists():
            shutil.copy2(meta / "icon0.png", folder / "sce_sys" / "icon0.png")
        shutil.rmtree(meta)
        for f in list(folder.glob("userdata00*")) + list(folder.glob("backup0000")):
            shutil.copy2(f, f.with_name(f.name + ".bak"))
        slots += [int(f.name[-4:]) for f in folder.glob("userdata000[0-9]") if f.read_bytes().strip(b"\0")]
    if not slots:
        sys.exit("savecopy: the test save has no character")
    (user / "party_characters.txt").write_text("".join("slot %d used %d play %u name %016x\n" % (
        s, 1 if s in slots else 0, 99 * 3600000 if s in slots else 0, 0x1234 if s in slots else 0) for s in range(10)))
    (user / "party_rot.txt").write_text("".join("slot %d kinds 1 0 0 0 0\n" % s for s in slots))
    print("[smoke] savecopy: inst0's party save in shadPS4's layout with *.bak files; slot(s) %s marked as another "
          "character's, with its rot" % slots, flush=True)


def phase_sc(only):
    # A save brought in from another build (savecopy): taken in, the old character's rot dropped,
    # loaded by the host's Continue, a guest joins.
    return dict(
        name="sc", seconds=300, count=2, headless=0, fresh=False, prep=prep_savecopy,
        env=[], ienv=[], crash=[],
        until=["0:cooperators 0 -> 1", "1:session .* -> client"],
    )


# ---- Phase rules: the party rules pages in the System menu (2026-10-21) ----
# Each page: (name, expected row labels, the host's change: (row, key presses there, the value it
# shows after, the presses back, bbport.ini key=value after, =value back, the host's rules line
# after or None)). The labels are the shown rows of a host (a guest's read-only lines have the same
# names). Toggles: Left = Off, Right = On; sliders: Right = one step up.
RULES_PAGES = [
    ("Deaths & revives", ["Rot on death", "Guest gets up, no load", "Host gets up, no load", "Up after boss kill",
                          "Respawn at host's lamp", "Refill vials, bullets"],
     ("Rot on death", "", "left", "Off", "right", "party_rot=0", "party_rot=1", '"rot":false')),
    ("World & PvP", ["Friendly fire / PvP", "Shared lamps", "Guests use lamps", "Guests use Hunter's Mark", "Open world"],
     # set back at the end of the walk: the guest checks the host's value meanwhile
     ("Friendly fire / PvP", "", "right", "On", None, "party_friendly_fire=1", "party_friendly_fire=0",
      '"friendly_fire":true')),
    ("Story & dialogue", ["Share NPC dialogue", "NPC dialogue range", "Share story progress", "Share NPC quests",
                          "Share cutscenes, endings", "Cutscenes live with host"],
     ("Share NPC dialogue", "", "left", "Off", "right", "party_npc_talk=0", "party_npc_talk=1", None)),
    ("Party rewards", ["Full rewards for guests", "Host's item rewards", "Give the bells", "Insight for bosses"],
     ("Full rewards for guests", "", "left", "Off", "right", "party_full_rewards=0", "party_full_rewards=1", None)),
    ("Enemy scaling", ["Boss health", "Boss damage", "Enemy health", "Enemy damage", "Boss poise", "Enemy poise"],
     ("Boss health", "", "right", "7", "left", "party_boss_hp=200", "party_boss_hp=150", '"boss_hp":200')),
    ("Party display", ["Party HUD", "Party name tags", "Pings", "Show party through walls", "Bloodstain marker",
                       "Above companions"],
     ("Pings", "down/down/", "left", "Off", "right", "party_pings=0", "party_pings=1", None)),
]
# The values a fresh run starts from (the rows' defaults): these keys are dropped from each
# instance's bbport.ini first, so an earlier run's changes never shift the walk.
RULES_INI_RESET = ["party_rot", "party_friendly_fire", "party_npc_talk", "party_full_rewards", "party_boss_hp",
                   "party_pings", "party_host_revive", "party_share_lamps"]


def rules_walk_to_first():
    """Options > System, then Up from the top (wrapping: Exit Game, Network, Brightness, then the
    port's pages from the last) to the first rules page, Cross. Read from bbport_game_menu_pages.cpp."""
    src = (REPO / "gpu" / "shim" / "bbport_game_menu_pages.cpp").read_text(encoding="utf-8", errors="replace")
    pages = re.findall(r'AddPage\("([^"]+)"', src)
    ups = 3 + (len(pages) - pages.index(RULES_PAGES[0][0]) - 1) + 1
    return "options/right/right/cross/" + "/".join(["up"] * ups) + "/cross"


def phase_rules(only):
    host, guest = rules_walk_to_first(), rules_walk_to_first()
    for k, (page, _, change) in enumerate(RULES_PAGES):
        if k:
            host += "/circle/down/cross"
            guest += "/circle/down/cross"
        host += "/mark=rules_h%d" % (k + 1)
        guest += "/mark=rules_g%d" % (k + 1)
        row, to_row, press, _, back = change[:5]
        host += "/" + to_row + press + "/mark=rules_h%dc" % (k + 1) + ("/" + back if back else "")
    # Friendly fire back off once the guest has seen it (the guest starts 22 s later)
    up = "/".join(["up"] * (len(RULES_PAGES) - 2))
    host += "/" + "/".join(["wait"] * 12) + "/circle/" + up + "/cross/left/mark=rules_h_ffoff/circle/circle/circle"
    guest += "/circle/circle/circle"
    watch = []
    for tag in ["rules_h%d" % k for k in range(1, 7)] + ["rules_g%d" % k for k in range(1, 7)] + \
               ["rules_h%dc" % k for k in range(1, 7)] + ["rules_h_ffoff"]:
        watch += ["--watch", r"step \d+/\d+: ~?mark=%s(@|\s)=%s@1.2" % (tag, tag)]
    return dict(
        name="rules", seconds=300, count=2, headless=0,
        env=["BB_GAME_MENU_TRACE=1", "BB_TEST_MENU_FILE={root}\\menu{i}.txt"],
        ienv=[],
        files={"menu0.txt": "joined 8 %s\n-\n-\n" % host, "menu1.txt": "joined 30 %s\n-\n-\n" % guest},
        ini_reset=RULES_INI_RESET,
        watch=watch,
        crash=[],
        until=GROUP_UNTIL["rules"],
    )


# ---- Phase summonfail: a failed join must not lock the host (signstuck 2026-10-21) ----
# Guest 1's party (P2P) port is held by this script, so every join of it fails (as a blocked port,
# an Internet timeout or a crash mid-join would): its direct summon fails after 8 s and the host's
# game then summons its sign again every ~30 s. Guest 2 starts only after that failure; the host
# must serve its direct summon within 30 s of its first ask and it must end up in the host's world.
# Before the fix every such ask was refused ("the host's game is summoning a sign") for minutes.
SF_FAILED = r"direct summon: FAILED"


def phase_summonfail(only):
    return dict(
        name="summonfail", seconds=360, count=3, headless=1,
        env=[], ienv=["1:BB_PARTY_PORT={hold}"],
        hold_ports=16,
        start_when=["2/1:" + SF_FAILED],
        crash=[],
        until=GROUP_UNTIL["summonfail"],
    )


# Phase s3 (hostcheck, ~6 min, Gascoigne un-killed): the host's revives in place and what must work
# after them. Kills first (host 2, guest 1 one, each game's own death path), then a host death outside
# a fight (lamp revive 1), a death in the Gascoigne fight that the guests' kill ends (the arena
# revive), a death 130 m away at an enemy group (lamp revive 2: the long way back). After each: every
# guest's enemy census against the host's (kills still dead, no respawn on one game only); after the
# first two: the bloodstain walked onto and picked up; after the last two: an enemy's hits, lock-on, a
# Blood Vial. Last, the host dies with its last lamp in another area (a lone load).
S3_TRIGGER = "-48.76:-40.92:72.44"   # Gascoigne's intro trigger (region 2412815), the arena floor
S3_UNTIL = r"0:travel step (\d+)/\1: "  # the host chain's last step


def _s3_stain():
    """Walk onto the own bloodstain, Cross; then 10 s more for the census."""
    return ["host_stick=stain:0.6:40@1%m24_01!", "host_press=0x4000@2%m24_01!", "host_press=0@3%m24_01!",
            "host_press=0@10%m24_01!"]


def _s3_enemy():
    """A nearby enemy on the same floor: R3 lock-on, R1 once (it fights back), HP lines, the kills, a vial
    (killed within ~10 s: two of them took the host's 575 HP in 13 s)."""
    return ["host_chr=0:0.6@2%m24_01!", "host_press=0x4@1%m24_01!", "host_press=0@1%m24_01!",  # R3: lock-on
            "host_press=0x800@1%m24_01!"] + ["host_press=0@2%m24_01!"] * 4 + [                  # R1: it fights back
        "host_kill=0@1%m24_01", "host_kill=0@1%m24_01", "host_press=0@2%m24_01!",
        "host_press=0x1000@1%m24_01!", "host_press=0@3%m24_01!"]                           # Triangle: a Blood Vial


def phase_s3(only):
    host = ",".join([
        "echoes=5000",
        "host_warp=2412953@20",                                    # the Tomb of Oedon lamp (party travel)
        # two kills before the deaths, of this map's own enemies (m24_01, 130 m away: the nearest ones
        # belong to the Cathedral Ward map m24_00, whose enemies come back on a game that streams that
        # map out and in again, any load or none: hostcheck run respawnE)
        "host_kill=2410247@5%m24_01", "host_kill=2410202@2%m24_01",
        "host_stick=-41.5:79.5:1:30@8%m24_01!",                    # off the lamp, walked
        "host_press=0@2%m24_01!", "host_press=0@2%m24_01!", "host_press=0@2%m24_01!", "host_press=0@2%m24_01!",
        "host_die@2%m24_01",                                       # outside a fight: lamp revive 1
        "host_press=0@14%m24_01!",
    ] + _s3_stain() + [
        "host_move=%s@2%%m24_01" % S3_TRIGGER,                     # the fight
        "host_press=0@1%m24_01~", "host_press=0@2%m24_01",
        "host_die@1%m24_01",                                       # in the fight: the limbo
        "host_kill=2410810:hp@22%m24_01",                          # the guests' kill: the arena revive
        "host_press=0@8%m24_01!", "host_press=0x4000@2%m24_01!", "host_press=0@2%m24_01!",  # OK on the key's box
    ] + _s3_stain() + _s3_enemy() + [
        "host_die@2%m24_01",                                       # 130 m from the lamp: lamp revive 2
        "host_press=0@14%m24_01!", "host_press=0@12%m24_01!",
    ] + _s3_enemy() + [
        "host_lastlamp=2402950@1%m24_01", "host_die@2%m24_01",     # last lamp in the Cathedral Ward: a lone load
        "host_press=0@30%m24_00",                                  # its census, then the end
    ])
    # guest 1 kills one too; both guests then stand in the arena, off the host's walk to its stain
    g1 = "god,guest_kill=2410261@10%m24_01,guest_move=-49.9:-40.82:73.3@2%m24_01"
    g2 = "god,guest_move=-49.9:-40.82:73.3@14%m24_01"
    return dict(
        name="s3", seconds=600, pad=True,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_ENEMY_CENSUS=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2],
        crash=[],
        until=[S3_UNTIL],
    )


PHASES = {"s1": phase_s1, "s2": phase_s2, "rules": phase_rules, "sc": phase_sc, "summonfail": phase_summonfail,
          "s3": phase_s3}
# --only groups (a smaller run for repeat cycles): the phase each needs and the verdict rows it keeps
GROUPS = {"join": "s1", "leave": "s1", "death": "s1", "travel": "s1", "boss": "s1", "host_death": "s1",
          "wipe": "s1", "kill": "s2", "arena": "s2", "itembox": "s2", "rejoin_kill": "s2", "c6": "s2", "host_crash": "s2",
          "host_leave": "s2", "keyitems": "s2", "rules": "rules", "savecopy": "sc", "summonfail": "summonfail",
          "host_revive": "s3"}
GROUP_ROWS = {"join": r"join", "leave": r"s1 (Leave|Rejoin)", "death": r"guest death|bloodstain|revived",
              "travel": r"travel", "boss": r"Gascoigne|intro", "host_death": r"host death|limbo",
              "wipe": r"wipe", "kill": r"s2 boss kill|party stays", "arena": r"arena", "itembox": r"item box|reward on the summoned",
              "rejoin_kill": r"s2 (Leave after|Rejoin|no kick)", "c6": r"guest crash",
              "host_crash": r"host crash", "host_leave": r"host Leave", "keyitems": r"key item", "rules": r"^rules",
              "savecopy": r"^sc ", "summonfail": r"^summonfail",
              "host_revive": r"^s3 "}


# --only: the log lines that end each group's checks (instances.py --until "I:RX"); a phase with
# --only ends once the chosen groups' lines are all seen, so the steps after them never run.
GROUP_UNTIL = {
    "join": ["0:cooperators [01] -> 2"],
    "leave": ["1:guest_rejoin: Party > Rejoin pressed", "0:cooperators 1 -> 2"],
    "death": ["1:guest: respawned at the host's lamp|guest: died alone: revived|revived at"],
    "travel": ["1:map - -> m24_01", "2:map - -> m24_01"],
    "boss": ["1:cutscene 24010010"],
    "host_death": ["0:spectating: following"],
    "wipe": [WIPE_UNTIL],
    "kill": ["2:host boss kill", "0:K10: boss-clear leave suppressed"],
    "arena": [r"2:test: guest_move to \(-136\.5"],
    "itembox": ["1:item notice closed|lot 50000010 .*(?:not )?given (?:now )?while summoned|owed key item: lot 50000010",
                "1:" + STORAGE_GIVE],
    "rejoin_kill": ["1:guest_rejoin: Party > Rejoin pressed"],
    "keyitems": [r"1:key goods held: .*load mode 0\)"],  # guest 1 back in its own world after the Leave
    "rules": [r"0:mark=rules_h_ffoff[\s\S]{200}", "1:test menu: done"],
    # guest 2 in the host's world, then ~10 s more log (the screenshots of it in the host's world)
    "summonfail": [r"0:cooperators 0 -> 1[\s\S]{40000}"],
    "host_revive": [S3_UNTIL],
}
NEEDS_CRASH = {"c6", "host_crash", "host_leave"}  # s2 groups that need the crash plan and its full chain


def narrow_phase(ph, only):
    """--only: end the phase on the chosen groups' lines; drop the s2 crash plan nobody asked for."""
    mine = [g for g in only if GROUPS[g] == ph["name"]]
    if not mine or NEEDS_CRASH & set(mine):
        return ph
    ph = dict(ph, until=[u for g in mine for u in GROUP_UNTIL.get(g, [])] or ph["until"], crash=[])
    return ph


def hold_udp_ports(n):
    """n consecutive free UDP ports on 127.0.0.1, bound exclusively (a test game told to use the first
    finds them all "in use by another program": no party port). Closed by the caller."""
    import random, socket
    for _ in range(50):
        start = random.randrange(30000, 40000)
        socks = []
        try:
            for p in range(start, start + n):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                socks.append(s)
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                s.bind(("127.0.0.1", p))
            print("[smoke] holding UDP 127.0.0.1:%d-%d" % (start, start + n - 1), flush=True)
            return socks
        except OSError:
            for s in socks:
                s.close()
    sys.exit("no %d free UDP ports to hold" % n)


def run_phase(a, ph, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    root = Path(a.root)
    (root).mkdir(parents=True, exist_ok=True)
    (root / "noreap").write_text(str(ph["seconds"] + 120))
    for name, text in ph.get("files", {}).items():  # rules: the menu walks (BB_TEST_MENU_FILE)
        (root / name).write_text(text, encoding="ascii")
    held = hold_udp_ports(ph["hold_ports"]) if ph.get("hold_ports") else []  # summonfail: a blocked party port
    hold = str(held[0].getsockname()[1]) if held else ""
    for i in range(ph.get("count", 3)):
        ini = root / f"inst{i}" / "bbport.ini"
        if ph.get("ini_reset") and ini.exists():
            keep = [l for l in ini.read_text(encoding="utf-8", errors="replace").splitlines()
                    if l.partition("=")[0].strip() not in ph["ini_reset"]]
            ini.write_text("\n".join(keep) + "\n", encoding="utf-8")
    starts = {}
    for i in range(3):
        p = root / f"inst{i}" / "run.log"
        # instances.py moves the old run.log to run.prev.log, unless something holds it open (then
        # it appends): remember the file id and size, and copy from there when it is the same file.
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    if ph.get("prep"):
        ph["prep"](root)
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(ph.get("count", 3)),
           "--headless", str(ph.get("headless", 1)), "--party"] + (["--fresh-saves"] if ph.get("fresh", True) else []) + [
           "--timestamps", "--idle", "0", "--seconds", str(ph["seconds"]),
           "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--label", "smoke %s" % ph["name"]]
    for e in ph["env"]:
        cmd += ["--env", e.replace("{root}", str(root))]
    for e in ph["ienv"]:
        cmd += ["--ienv", e.replace("{hold}", hold)]
    for w in ph.get("start_when", []):
        cmd += ["--start-when", w]
    if ph.get("pad"):
        # the stick walks (host_stick) write the in-process pad script
        (root / "pad0.txt").write_text("lx=128 ly=128\n")
        cmd += ["--ienv", "0:BB_PAD_FILE=" + str(root / "pad0.txt")]
    for c in ph["crash"]:
        cmd += ["--crash-plan", c]
    for u in ph.get("until", []):
        cmd += ["--until", u]
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"))
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    print("[smoke] %s: %s" % (ph["name"], " ".join(cmd)), flush=True)
    (out / "harness.log").unlink(missing_ok=True)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", ph["name"], "--every", str(a.every), "--seconds", str(ph["seconds"] + 400)]
                               + ph.get("watch", []),
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
        for h in held:
            h.close()
    print("[smoke] %s: harness exit %d after %.0f s" % (ph["name"], r.returncode, time.time() - t0), flush=True)
    for i in range(ph.get("count", 3)):
        p = root / f"inst{i}" / "run.log"
        if not p.exists():
            continue
        with open(p, "rb") as f:
            f.seek(starts[i][1] if os.fstat(f.fileno()).st_ino == starts[i][0] else 0)
            (out / f"inst{i}.log").write_bytes(f.read())
    try:
        (root / "noreap").unlink()
    except OSError:
        pass


# ---------------------------------------------------------------------------------------------
# Verdict

TS = re.compile(r"^\[\+ *([\d.]+)\] ")


class Logs:
    def __init__(self, d: Path):
        self.lines = {}
        for i in range(3):
            p = d / f"inst{i}.log"
            self.lines[i] = p.read_text(encoding="utf-8", errors="replace").splitlines() if p.exists() else []

    def find(self, i, rx, after=0.0, before=1e9):
        """First (t, line) on instance i matching rx with after <= t < before."""
        r = re.compile(rx)
        for line in self.lines[i]:
            m = TS.match(line)
            t = float(m.group(1)) if m else -1.0
            if t < after or t >= before:
                continue
            if r.search(line):
                return t, line[m.end():] if m else line
        return None

    def all(self, i, rx, after=0.0, before=1e9):
        r = re.compile(rx)
        out = []
        for line in self.lines[i]:
            m = TS.match(line)
            t = float(m.group(1)) if m else -1.0
            if after <= t < before and r.search(line):
                out.append((t, line[m.end():] if m else line))
        return out


class Verdict:
    def __init__(self, only=None):
        self.rows = []
        self.keep = [re.compile(GROUP_ROWS[g]) for g in only] if only else None

    def step(self, name, ok, detail):
        if self.keep and not any(k.search(name) for k in self.keep) and not name.endswith(("no crash / assert", "no 'Session lost'")):
            return
        self.rows.append((name, bool(ok), detail))
        print("%-4s %-40s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def need(v, L, name, i, rx, after=0.0, before=1e9, what=""):
    hit = L.find(i, rx, after, before)
    v.step(name, hit, ("+%.1f s inst%d: %s" % (hit[0], i, hit[1][:150])) if hit else
           "missing on inst%d after +%.0f s: /%s/ %s" % (i, after, rx, what))
    return hit[0] if hit else None


def none_of(v, L, name, rx, after=0.0, before=1e9, insts=(0, 1, 2)):
    bad = [(i, h) for i in insts for h in L.all(i, rx, after, before)]
    v.step(name, not bad, "none" if not bad else "inst%d +%.1f s: %s (%d in all)" % (bad[0][0], bad[0][1][0],
                                                                                    bad[0][1][1][:150], len(bad)))


def check_common(v, L, tag):
    none_of(v, L, tag + ": no crash / assert", r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error")
    none_of(v, L, tag + ": no 'Session lost'", r"Session lost")


def harness_crashes(d: Path):
    p = d / "harness.log"
    text = p.read_text(errors="replace") if p.exists() else ""
    return [(float(t), int(i)) for t, i in re.findall(r"\[harness \+([\d.]+) s\] crash \d+ of \d+: instance (\d+)", text)]


def check_s1(v, L):
    need(v, L, "s1 join: 3 players (host cooperators -> 2)", 0, r"cooperators [01] -> 2")
    lv = need(v, L, "s1 Leave (guest 1, Party page)", 1, r"guest_leave: Party > Leave pressed")
    if lv is not None:
        need(v, L, "s1 Leave: host sees it (2 -> 1)", 0, r"cooperators 2 -> 1", lv)
        rj = need(v, L, "s1 Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv)
        if rj is not None:
            back = need(v, L, "s1 Rejoin: back in (1 -> 2)", 0, r"cooperators 1 -> 2", rj)
            if back is not None:
                v.step("s1 Rejoin time (press -> host room)", back - rj < 30, "%.1f s (limit 30)" % (back - rj))
    d = need(v, L, "s1 guest death (scripted)", 1, r"travel step \d+/\d+: guest_die", lv or 0)
    if d is not None:
        need(v, L, "s1 bloodstain recorded (5000 echoes)", 1, r"guest: bloodstain recorded", d)
        # 0.6.9: a guest's death takes the respawn load (the in-place revive left it unhittable)
    need(v, L, "s1 guest respawned at the lamp", 1, r"guest: respawned at the host's lamp|guest: died alone: revived|revived at", d or 0)
    tr = need(v, L, "s1 party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp 2412953")
    if tr is not None:
        for i in (1, 2):
            need(v, L, "s1 travel: guest %d in m24_01" % i, i, r"map .*-> m24_01", tr)
    gm = need(v, L, "s1 guest at Gascoigne's fog", 1, r"travel step \d+/\d+: guest_obj", tr or 0)
    if gm is not None:
        need(v, L, "s1 guest-started intro on the host", 0, r"(?i)cutscene 24010010|remo 24010010|24010010", gm)
        need(v, L, "s1 guest-started intro on guest 1 (live mirror)", 1, r"(?i)live mirror|24010010", gm)
    hd = need(v, L, "s1 host death in the fight", 0, HOST_DIED, gm or 0)
    if hd is not None:
        need(v, L, "s1 host limbo + spectate", 0, r"spectating: following", hd)
    need(v, L, "s1 wipe: the party respawns together", 0, WIPE_RX, hd or 0)
    check_common(v, L, "s1")


def check_arena(v, L, k):
    """Guest 2 dead in the fight when the boss dies: up in the arena, no load (arenaguest 2026-10-21)."""
    dead = need(v, L, "s2 arena: guest 2 dead in the fight", 2, r"travel step \d+/\d+: guest_die")
    if dead is None:
        return
    if k is not None:
        v.step("s2 arena: guest 2 died before the kill", dead < k, "death +%.1f s, kill +%.1f s" % (dead, k))
    up = need(v, L, "s2 arena: guest 2 up in the arena (no load)", 2, r"up again in the arena", dead)
    if up is None:
        return
    line = L.find(2, r"up again in the arena", dead)[1]
    v.step("s2 arena: not a fall death", "a fall death" not in line, line[:150])
    need(v, L, "s2 arena: host re-created guest 2's chr", 0, r"member revived: its chr", up - 1)
    alive = need(v, L, "s2 arena: guest 2 alive after it", 2, r"test: guest_press 0x0; .*own HP [1-9]", up)
    hit = need(v, L, "s2 arena: guest 2 takes an enemy hit", 2, r"hit: HP \d+ -> \d+ \(-\d+\), max", alive or up)
    none_of(v, L, "s2 arena: no loading screen for guest 2", r"load started", dead, hit or up + 60, (2,))


def check_s2(v, L, d: Path):
    need(v, L, "s2 join: 3 players", 0, r"cooperators [01] -> 2")
    tr = need(v, L, "s2 party lamp travel to the Cleric Beast", 0, r"travel step \d+/\d+: host_warp to lamp 2412952")
    k = need(v, L, "s2 boss kill (host)", 0, r"host boss kill", tr or 0)
    if k is not None:
        need(v, L, "s2 boss kill reached guest 1", 1, r"host boss kill", k)
        need(v, L, "s2 boss kill reached guest 2", 2, r"host boss kill", k)
        need(v, L, "s2 the party stays after the boss (K10)", 0, r"K10: boss-clear leave suppressed", k)
        # The Cleric Beast's reward (lot 50000010, Sword Hunter Badge) is a key item: a summoned guest
        # gets it at once and owed to its own world too (items.md 10.3; pard2, D4); any other reward
        # shows the box.
        ib = need(v, L, "s2 reward on the summoned guest (item box or owed key item)", 1,
                  r"item notice up on a summoned guest|lot 50000010 .*not given while summoned; waits for the own world|"
                  r"owed key item: lot 50000010|gave .*lot 50000010 .*while summoned", 0)
        if ib is not None and L.find(1, r"item notice up on a summoned guest", k):
            need(v, L, "s2 item box OK row closes it (Cross)", 1, r"item notice closed", ib)
    check_arena(v, L, k)
    # Goods past the inventory maximum land in the storage (items.md 10.4: the +0x48 storage limit
    # and the give list), one in, no "Unable to acquire" box and nothing left on the ground.
    sg = L.find(1, STORAGE_GIVE)
    if sg is None:
        need(v, L, "s2 item box: a Pebble past the maximum lands in the storage", 1, STORAGE_GIVE)
    else:
        m = re.search(STORAGE_GIVE, sg[1])
        a, b = int(m.group(1)), int(m.group(2))
        v.step("s2 item box: a Pebble past the maximum lands in the storage", b == a - 1,
               "+%.1f s inst1: storage room %d -> %d" % (sg[0], a, b))
    none_of(v, L, "s2 item box: no 'Unable to acquire' box on guest 1", r"text shows: \"Unable to acquire", 0, 1e9, (1,))
    none_of(v, L, "s2 item box: no Pebble left on the ground (no give counted as Full)",
            r"lot 2100000 .*(Full|skip .*while summoned)", 0, 1e9, (1,))
    # Key items (items.md 10.5). The test save used Amelia's Gold Pendant (4001): 0.6.2 - 0.6.9 gave it
    # back at every own-world load. After the travel the host holds its keys; summoned, a guest's key
    # section reads empty (the game's own view), and at home the save's keys are back, one stack each.
    none_of(v, L, "s2 key item: a used-up item is not given back", r"(catch-up|owed lot|given again).*goods 4001")
    none_of(v, L, "s2 key item: no duplicate stack", r"DUPLICATE STACK")
    if tr is not None:
        need(v, L, "s2 key items after the travel (host)", 0, r"key goods held: .*4000 x1,(?: 4001 x0,)?.*map m24_01", tr)
        # BB_PARTY_KEY_RESTORE=1 (off by default: it duplicates keys in the save): a summoned guest's
        # key items are put back right after each host-world load.
        if L.find(1, r"summoned key items (put back|all held) after the load"):
          need(v, L, "s2 key items put back on the summoned guest after the travel", 1,
               r"summoned key items (put back|all held) after the load \(pass 1", tr)
          need(v, L, "s2 key items held by the summoned guest after the travel", 1,
               r"key goods held: .*4000 x1,.*4114 x1,.*map m24_01.*load mode 1", tr)
    lv = need(v, L, "s2 Leave after the boss kill", 1, r"guest_leave: Party > Leave pressed", k or 0)
    if lv is not None:
        need(v, L, "s2 key items back at home after the travels (guest)", 1,
             r"key goods held: .*4000 x1,(?: 4001 x0,)?.*4114 x1,.*load mode 0\)", lv)
    rj = need(v, L, "s2 Rejoin pressed", 1, r"guest_rejoin: Party > Rejoin pressed", lv or 0) if lv else None
    if rj is not None:
        back = need(v, L, "s2 Rejoin after the kill: back in", 0, r"cooperators 1 -> 2", rj)
        none_of(v, L, "s2 no kick after the rejoin", r"KickoutRoomMember req", rj, (back or rj) + 30, (0,))
    crashes = harness_crashes(d)
    g = [t for t, i in crashes if i == 1]
    h = [t for t, i in crashes if i == 0]
    v.step("s2 guest crash (harness kill)", bool(g), "+%.1f s" % g[0] if g else "the crash plan did not fire")
    if g:
        end = h[0] if h else 1e9
        back = need(v, L, "s2 guest crash: back in the host's room", 0, r"cooperators 1 -> 2", g[0], end)
        if back is not None:
            v.step("s2 guest crash rejoin time (kill -> room)", back - g[0] < 45, "%.1f s (limit 45)" % (back - g[0]))
        none_of(v, L, "s2 guest crash: no failed title join", r"title join: FAILED", g[0], end, (1,))
        none_of(v, L, "s2 guest crash: nobody kicked", r"KickoutRoomMember req", g[0], end, (0,))
    v.step("s2 host crash (harness kill)", bool(h), "+%.1f s" % h[0] if h else "the crash plan did not fire")
    if h:
        need(v, L, "s2 host crash: guest 1 goes home / waits", 1, r"host lost|Waiting for the host", h[0])
        need(v, L, "s2 host crash: the host restarts", 0, r"crash restart: the director test chain", h[0])
        back = need(v, L, "s2 host crash: the party back together", 0, r"cooperators 0 -> [12]", h[0])
        if back is not None:
            v.step("s2 host crash recovery time (kill -> first guest back)", back - h[0] < 90, "%.1f s (limit 90)" % (back - h[0]))
        hl = need(v, L, "s2 host Leave (restarted host)", 0, r"host_leave: Party > Leave pressed", h[0])
        if hl is not None:
            for i in (1, 2):
                t = need(v, L, "s2 host Leave: guest %d out" % i, i, r"the host ended the party|host lost \(the host left its room\)", hl, hl + 15)
    check_common(v, L, "s2")


def check_sc(v, L, d: Path):
    root = Path(RUN_ROOT[0])
    need(v, L, "sc save check OK (*.bak ignored)", 0, r"Saves: check OK: .*savedata_party")
    need(v, L, "sc shadPS4 layout taken in", 0, r"is in shadPS4's layout \(sce_sys/param\.sfo inside\)")
    need(v, L, "sc old character's side files dropped", 0, r"Saves: save slot \d: another character")
    w = need(v, L, "sc host Continue loads the copied character", 0, r"auto-continue: world up after")
    need(v, L, "sc guest joins the host", 0, r"cooperators 0 -> 1", w or 0)
    need(v, L, "sc guest in the host's session", 1, r"session .* -> client")
    party = root / "inst0" / "user" / "savedata_party"
    both = bool(list(party.rglob("*.sce_sys/param.bin")) and list(party.rglob("sce_sys/param.sfo")))
    v.step("sc both layouts' descriptions on disk", both, "param.bin and sce_sys/param.sfo" if both else "missing")
    rot = root / "inst0" / "user" / "party_rot.txt"
    left = rot.read_text().strip() if rot.exists() else ""
    v.step("sc the other character's rot is gone", not left, "gone" if not left else repr(left[:80]))
    none_of(v, L, "sc: no crash / assert", r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error", insts=(0, 1))


def check_rules(v, L):
    need(v, L, "rules join: the guest is in (cooperators -> 1)", 0, r"cooperators 0 -> 1")
    for k, (page, labels, change) in enumerate(RULES_PAGES):
        row, _, _, value, back, ini_after, ini_back, effect = change
        for i, who in ((0, "host"), (1, "guest")):
            hit = need(v, L, "rules %s: %s page opens" % (who, page), i, r"Game menu: page \d+ %s open" % re.escape(page))
            line = L.find(i, r"Game menu: page \d+ rows \(\d+\): ", hit or 0)
            if not line:
                v.step("rules %s: %s rows" % (who, page), False, "no rows line after the page opened")
                continue
            shown = [r.rsplit("=", 1)[0] for r in line[1].split("): ", 1)[1].split(" | ")]
            ok = shown == labels and len(shown) <= 6 and "(CUT)" not in line[1]
            v.step("rules %s: %s rows" % (who, page), ok, line[1][:300] if ok else
                   "expected %s, got %s" % (labels, line[1][:300]))
        t = need(v, L, "rules host: %s changed (%s -> %s)" % (page, row, value), 0,
                 r"Game menu: row '%s' -> %s$" % (re.escape(row), re.escape(value)))
        if t is None:
            continue
        need(v, L, "rules host: %s written to bbport.ini (%s)" % (page, ini_after), 0,
             r"Settings: bbport.ini %s$" % re.escape(ini_after), t - 1)
        if effect:
            need(v, L, "rules host: %s in force (%s)" % (page, effect), 0,
                 r"rules sent to the party: .*%s" % re.escape(effect), t - 1)
        if back or page == "World & PvP":
            b = need(v, L, "rules host: %s set back (bbport.ini %s)" % (page, ini_back), 0,
                     r"Settings: bbport.ini %s$" % re.escape(ini_back), t)
    # The host's rule on the guest: its World & PvP page shows the host's friendly fire (on then)
    ff = L.find(1, r"Game menu: page \d+ rows \(\d+\): Friendly fire / PvP=")
    v.step("rules guest: sees the host's friendly fire (On, read-only)", ff and "Friendly fire / PvP=On" in ff[1],
           ff[1][:200] if ff else "no World & PvP rows line on the guest")
    none_of(v, L, "rules: no menu walk stuck", r"STUCK", insts=(0, 1))
    check_common(v, L, "rules")


def check_summonfail(v, L):
    f = need(v, L, "summonfail: guest 1's join fails (its party port is blocked)", 1, SF_FAILED)
    if f is None:
        return
    ask = need(v, L, "summonfail: guest 2 asks for a direct summon", 2, r"direct summon: asked the host for its room", f)
    if ask is not None:
        served = need(v, L, "summonfail: the host serves guest 2's direct summon", 0,
                      r"direct summon: slot 2 \(.*\) gets this host's room", ask)
        if served is not None:
            v.step("summonfail: served within 30 s of guest 2's first ask", served - ask < 30,
                   "%.1f s (limit 30)" % (served - ask))
        need(v, L, "summonfail: guest 2 in the host's world (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1", ask)
    stuck = L.all(0, r"refused \(the host's game is summoning a sign", f)
    v.step("summonfail: sign-summon refusals after the failure (info)", True, "%d" % len(stuck))
    check_common(v, L, "summonfail")


RUN_ROOT = ["."]  # main: --root (the sc checks read the host's save folder)

HP_RX = re.compile(r"own HP (-?\d+)/")


def _hp(lines):
    return [(t, int(HP_RX.search(l).group(1))) for t, l in lines if HP_RX.search(l)]


def check_s3_revive(v, L, tag, t, end, stain=True, enemy=True):
    """After a host revive at +t (until +end): census, stain, an enemy's hits, lock-on, vial."""
    for i in (1, 2):
        need(v, L, "s3 %s: guest %d census matches the host's" % (tag, i), i,
             r"enemy census: digest \d+ matches the host's .*after the host's revive", t, t + 25)
    none_of(v, L, "s3 %s: no census difference" % tag, r"enemy census: DIFFERS|RESPAWN without a load", t, t + 25)
    if stain:
        st = L.find(0, r"own bloodstain taken: .*echoes (\d+) -> -1", t, end)
        m = re.search(r"echoes (\d+) -> -1.*own echoes (\d+)", st[1]) if st else None
        v.step("s3 %s: bloodstain picked up, echoes back" % tag, bool(m) and 0 < int(m.group(1)) <= int(m.group(2)),
               ("+%.1f s: %s" % (st[0], st[1][:160])) if st else "no 'own bloodstain taken' after +%.0f s" % t)
    if not enemy:
        return
    ch = L.find(0, r"test: host_chr \d+ \(.*standing at", t, end)
    kl = L.find(0, r"test: host_kill: entity", ch[0], end) if ch else None
    if not ch or not kl:
        v.step("s3 %s: an enemy's hits (HP drop)" % tag, False, "no host_chr / host_kill after +%.0f s" % t)
        return
    hp = _hp(L.all(0, r"test: host_press 0x[0-9a-f]+;", ch[0], kl[0]))
    drop = [1 for (_, a), (_, b) in zip(hp, hp[1:]) if b < a]
    v.step("s3 %s: an enemy's hits (HP drop)" % tag, bool(drop),
           ("HP %s" % " -> ".join(str(h) for _, h in hp)) if hp else "no HP lines")
    lk = L.find(0, r"test: host_press 0x0;.*lock-on 1 ", ch[0], kl[0])
    v.step("s3 %s: lock-on (R3)" % tag, bool(lk), lk[1][:160] if lk else "lock-on stayed 0")
    vi = L.find(0, r"test: host_press 0x1000;", kl[0], end)
    before = _hp([vi]) if vi else []
    after = _hp(L.all(0, r"test: host_press 0x0;", vi[0] + 0.1, end))[:1] if vi else []
    v.step("s3 %s: Blood Vial heals" % tag, bool(before and after and after[0][1] >= before[0][1] + 50),
           "HP %d -> %d" % (before[0][1], after[0][1]) if before and after else "no vial press / HP line")


def check_s3(v, L):
    need(v, L, "s3 join: 3 players", 0, r"cooperators [01] -> 2")
    lamp_rx = r"host revive #\d+: after: .* at lamp 2412953"
    lamp = need(v, L, "s3 lamp revive 1 (no load)", 0, lamp_rx)
    ids = [re.search(r"entity (\d+)", l).group(1) for _, l in
           L.all(0, r"test: host_kill: entity \d+ (killed|HP)", 0, lamp or 1e9) +
           L.all(1, r"test: guest_kill: entity \d+ (killed|HP)", 0, lamp or 1e9)]
    v.step("s3 kills before the deaths (host, guest)", len(ids) >= 3, "entities " + " ".join(ids))
    arena = need(v, L, "s3 arena revive (no load)", 0, r"host death: up again in the arena", lamp or 0)
    lamp2 = need(v, L, "s3 lamp revive 2, from 130 m (no load)", 0, lamp_rx, arena or 1e9)
    if lamp is not None:
        check_s3_revive(v, L, "lamp revive 1", lamp, arena or 1e9, enemy=False)
    if arena is not None:
        check_s3_revive(v, L, "arena revive", arena, lamp2 or 1e9)
    if lamp2 is not None:
        check_s3_revive(v, L, "lamp revive 2", lamp2, 1e9, stain=False)
    lone = need(v, L, "s3 host death with its lamp in another area", 0, r"travel step \d+/\d+: host_lastlamp", lamp2 or 0)
    if lone is not None:
        cen = L.all(1, r"enemy census: ", lone, lone + 60)
        v.step("s3 lone load: guest 1 census afterwards", not any("DIFFERS" in l for _, l in cen),
               cen[-1][1][:160] if cen else "no census line (the host in another map: not compared)")
    check_common(v, L, "s3")



CHECKS = {"s1": lambda v, L, d: check_s1(v, L), "s2": check_s2, "rules": lambda v, L, d: check_rules(v, L),
          "sc": check_sc, "summonfail": lambda v, L, d: check_summonfail(v, L),
          "s3": lambda v, L, d: check_s3(v, L)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "smoke" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default="s1,s2,rules,summonfail,s3")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--only", default="", help="comma list of step groups (a smaller run): " + ",".join(GROUPS))
    a = ap.parse_args()
    RUN_ROOT[0] = a.root
    only = {g.strip() for g in a.only.split(",") if g.strip()}
    bad = only - set(GROUPS)
    if bad:
        sys.exit("unknown --only group(s): " + ",".join(sorted(bad)))
    if only:
        a.phase = ",".join(p for p in ("s1", "s2", "rules", "sc", "summonfail", "s3") if p in {GROUPS[g] for g in only})
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out)
    v = Verdict(only)
    t_all = time.time()
    for name in [p.strip() for p in a.phase.split(",") if p.strip()]:
        ph = PHASES[name](only)
        if only:
            ph = narrow_phase(ph, only)
        if not a.check_only:
            run_phase(a, ph, out / name)
        print("[smoke] verdict %s" % name, flush=True)
        CHECKS[name](v, Logs(out / name), out / name)
    fails = [r for r in v.rows if not r[1]]
    text = "\n".join("%s  %s  %s" % ("PASS" if ok else "FAIL", n, d) for n, ok, d in v.rows)
    text += "\n\n%d steps, %d failed\n" % (len(v.rows), len(fails))
    if not a.check_only:
        text += "wall time %.0f s (queue wait included)" % (time.time() - t_all) + chr(10)
    out.mkdir(parents=True, exist_ok=True)
    (out / "verdict.txt").write_text(text, encoding="utf-8")
    print("[smoke] %d steps, %d failed; %s" % (len(v.rows), len(fails), out / "verdict.txt"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
