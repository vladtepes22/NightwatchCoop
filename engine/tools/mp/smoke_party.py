#!/usr/bin/env python3
"""Post-merge party smoke test: a fixed ~15-20 min 3-player scenario with an automatic verdict.

  python tools/mp/smoke_party.py [--root DIR] [--build DIR] [--game DIR] [--out DIR] [--phase s1,s2]
  python tools/mp/smoke_party.py --check-only --out DIR     (verdict again from the saved logs)
  python tools/mp/smoke_party.py --real-net --only join ...  (the same phases on the real network, all
      players on this PC: instances.py --real-net; tools/mp/realnet_party.py is the VPN session)

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
  by the game's own pad path): host and guest open each party settings page (0.7.3: Party rules: World,
  PvP & lamps, Deaths, Rot, Enemies; My party: Deaths, World, Story, Rewards, Display, Look & notices;
  a screenshot of each); every
  page's rows and values from its trace line ("Game menu: page N rows (k): NAME=VALUE | ...", no
  text cut); the host changes one option per page and sets it back (the menu's "row ... -> ..." line,
  bbport.ini's line, the party's rules line where the rule is the host's); the guest sees the host's
  friendly fire and Party rules: World rules on its read-only rows.
Phase summonfail (~3 min, 3 players): guest 1's party port is held by the smoke (every join of it
  fails); guest 2 starts after guest 1's failed direct summon and must be served within 30 s and end
  up in the host's world (a failed join once locked the host's direct summon for minutes).
Phase lf (~3 min, 3 players, groups `lifts` / `ladder`): guest 1 lowers Central Yharnam's lever
  ladder (every game's ladder flag and animation) and rides Cathedral Ward's lift while the host is
  130 m away with that map block unloaded; the ride must reach the host's world, the host's car must
  stand where the guest left it once the block streams in again, and every game ends with the same
  lift flags (D3, docs/party/world_actions.md 10.1).
Phase el (~4 min, 2 players, group `elevator`): the Nightmare of Mensis' 60 m library lift (2601260,
  a player's report: the host warped back to its lamp mid-ride, the guest dead): host and guest ride
  it down together, up together (both out at the arrival), the host alone, the guest alone; after
  every ride both games show the same flag and the car at that end, nobody drops faster than a lift
  or leaves the car mid-ride, nobody is moved by the fall recovery, nobody dies.
Phase bf (~3 min, 3 players, group `fight_load`): while the host fights Father Gascoigne, guest 1
  takes a Hunter's Mark to the same lamp (a re-join load with the host's snapshot): the fight still
  runs for it, and its walk through the fog joins it; guest 2 dies outside the fog (its limbo).
Phase fl (~3 min, 2 players, group `fall`): guest 1 falls out of the world from the Great Bridge (its
  fall recovery off) to the kill plane; it must get up at a lamp of the loaded map in the host's world
  with no loading screen and no session leave, stay standing, and its bloodstain must be on its last
  safe floor; after each revive (guest and host) the camera follows the chr and it is drawn where it
  stands (fallcam).
Phase fd (~3 min, 2 players, group `fall_deep`): guest 1, its own last lamp the Central Yharnam lamp
  ~95 m away, is dropped ~32 m onto the Great Bridge: a real fall death (the game's fall-death camera
  on, not the kill plane of fl). It must get up at that far lamp with no loading screen, the fall-death
  camera released, the render camera on its chr and the chr drawn where it stands (fallcam: the
  "perpetual spectate" report), and walk away from the lamp.
Phase fdj (~4 min, 2 players, group `fall_deep`): the same at the report's own spot, the shaft beside
  Djura's tower in Old Yharnam (guest 1 walks off the ledge; the host's lamp ~150 m away).
Phase sm (~5 min, 2 players, group `scene_mirror`): the host's cutscene live on a guest beside it:
  Father Gascoigne's intro with both games running its event at once (the guest's own copy held,
  the host's played within 1 s, once), then the Tomb of Oedon door into Oedon Chapel (scene 24000020)
  with the guest 3 m behind the host still in m24_01 (played within 1 s; no replay after its walk in).
Phase gl (~4 min, 2 players, group `gather_ground`): Gather the party onto ground only (a player's
  report: the host gathered half way up a ladder, the guest was put in the open shaft beside it). At
  the foot of Cathedral Ward's ladder 2401024 the host gathers; the guest (75 m away at the lamp) is
  put 8 m too high (test aid "yesdrop": no ground there), must fall no further than the landing watch
  lets it, go back where it stood and come by the travel (one load), then stand beside the host. On
  the ladder the host's gather is refused (no question on the guest).
Phase ld (~4 min, 2 players, group `ladder`): host and guest each take Cathedral Ward's ladder 2401024
  from its foot and die on it (only once the ladder state is set), are revived in place at the lamp;
  the ladder state must be gone and a stick walk must reach four points around the lamp.
Phase lp (~3 min, 2 players, group `lamps`): the 0.7.2 lamp rule (Guests bring their lamps off): a
  lamp only the guest's save has stays unlit in the host's world and the host's save; a lamp the
  guest lights in the host's world is lit for the host and saved in both saves; the guest's lamp
  travel list shows only the host's world's lamps (both saves read from copies after the run).

Phase br (~6 min, 2 players, group `boss_rooms`, the DLC of the default save): the party at the
  Nightmare Grand Cathedral lamp (inside Laurence's room) without his skull: the guest walks out
  through his fog and back in, no fight starts; the skull given to the host there starts it
  (scene, bar on both), Laurence killed; then Ludwig killed and host and guest each walk through
  the wall at the back of his arena and back (party_bossfog_rules.h, 2026-10-22 reports).
Phase da (~3 min, 2 players, group `dream_apart`): host and guest in the host's Hunter's Dream; the
  host leaves alone by a headstone; the guest left behind must get its action buttons back (the
  leave state's hold lifted) and use a headstone itself: its menu opens, it travels and arrives
  (party_stay_rules.h, a player's report: the guest was stuck in the Dream). 0.7.3: the guest's trip
  out of the Dream is a plain travel: no rest request, no loading screen for the host.
Phase cv (~3 min, 2 players, groups `covenant` / `valtr_rune`): covenant outcomes per player (0.7.3).
  The host's save has Alfred's kill of Queen Annalise and Valtr dead, the guest's save Annalise
  alive; in the host's world the guest sees the host's state, its own save keeps Annalise alive
  after the session while the shared progress (a boss kill) still reaches it. Valtr's Impurity rune:
  the guest's League talk trigger gives the guest the rune in its own inventory and save, never the
  host. Both saves are read from copies after the run.
Phase ie (~3 min, 2 players, group `items_every`): early items for every player (0.7.3). In the
  host's Hunter's Dream: the guest (without the Old Hunter Bell, goods 4312 and its flag 6670) gets
  the bell from the host's award (its item box); the host's own starting weapon picks cleared, a
  Dream reload brings its messengers up next to the guest (who has picked): the guest's copy of
  their event must not end the host's (the 0x31 state packet dropped): they stay up (no pick is
  scripted: the talk prompt was not reached by a warp next to the messenger, run r5).

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
        "host_warp=2412953@30^2*2",         # party lamp travel: the Tomb of Oedon, 30 s after guest 1's Rejoin
                                            # (cooperators up to 2 the second time; its death and respawn ~19 s
                                            # after it; was @70 from the join: ~25 s idle after the respawn)
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
    if only and "wipe_late" in only:
        # Late-death wipe (player report 2026-10-21, Logarius): guest 2 down 10 s after the host,
        # guest 1 fights on and is the last one down 40 s after the host. Its HP 0 starts the wipe
        # while its death animation runs: it must follow the one respawn load to the host's lamp
        # (its bloodstain in the arena), with no revive of its own after it. No Leave / death
        # before (guest 1 keeps its 5000 echoes for the stain); the travel once both guests are in.
        host = ",".join(["host_warp=2412953@8^2", "host_die@6%m24_01~"])
        g1 = ",".join([
            "echoes=5000",
            "guest_obj=2411810@20%m24_01",
            "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",
            "guest_die@46%m24_01~",               # 40 s after the host's death: the last one down
        ])
        # wipeboss (0.7.2 known issue 2): guest 2 (its scripted death always runs; guest 1's waits
        # forever when Gascoigne kills it first), back at the host's lamp after the respawn load:
        # 30 s near Gascoigne's fog with nobody going in (HP logged every 10 s: no boss, no hits),
        # then a real walk through the fog: the fight must open for everyone from that alone.
        g2 = ",".join([
            "guest_die@16%m24_01~",
            "guest_press=0x0@10%m24_01!", "guest_press=0x0@10%m24_01!", "guest_press=0x0@10%m24_01!",
            "guest_obj=2411810@2%m24_01",
            "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",
        ])
    return dict(
        name="s1", seconds=420,
        # BB_BOOT_WATCHDOG: a thread snapshot if a boot stalls (0.7.1 full smoke: the host on a gray
        # screen after the title's logo, 1 boot in 16, nothing in the log saying where).
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1", "BB_BOOT_WATCHDOG=20"],
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
        "watch_text=Unable to acquire/Oedon Tomb Key",  # the "inventory or storage max" box: must not show;
                                            # a key item's name: the Key Items tab of the menu walk below
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
        watch=["--watch", "up again in the arena=arena@1,4", "--watch", r"travel step \d+/\d+: guest_chr=hits@6,10",
               "--watch", r"test menu: step \d+/\d+: mark=keytab=keytab@0.5",
               # the restarted host's view of the first guest back (look at its phantom tint)
               "--watch", r"its member's data after .*crash-restarted=hostback@3,10"],
        # Guest 1, 4 s after it arrives at the Cleric Beast's lamp (summoned): Options > Inventory, two
        # tabs right = Key Items (items.md 10.7, run10), a shot, back out. Its restarts: no walk.
        files={"menu1.txt": "m24_01 4 options/cross/right/right/mark=keytab/circle/circle\n-\n-\n-\n"},
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              # the host's own award of a Pebble treasure lot (host-only, 20 s after the join): the
              # summoned guest at its Pebble maximum gets it into its storage (items.md 10.4)
              "0:BB_PARTY_ITEMS_HOST_TEST=" + PEBBLE_LOT,
              "1:BB_TEST_MENU_FILE={root}\\menu1.txt",
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
    # 0.7.3: the host's rules (Party rules: ..., read-only lines on a guest), then each player's own
    # (My party: ...), in the System menu's order.
    # 0.7.1 / 0.7.3: the host's world rules
    ("Party rules: World", ["Enemies stay dead", "Host can gather", "NPCs as in host's world", "Covenants per player",
                            "Party votes on travel", "NPC hunters fight all"],
     ("Enemies stay dead", "", "left", "Off", "right", "party_kill_record=0", "party_kill_record=1",
      '"kill_record":false')),
    ("Party rules: PvP & lamps", ["Friendly fire / PvP", "Shared lamps", "Guests bring their lamps"],
     # set back at the end of the walk: the guest checks the host's value meanwhile
     ("Friendly fire / PvP", "", "right", "On", None, "party_friendly_fire=1", "party_friendly_fire=0",
      '"friendly_fire":true')),
    ("Party rules: Deaths", ["Host gets up, no load", "Up after boss kill", "Anyone can be abducted"],
     ("Host gets up, no load", "", "left", "Off", "right", "party_host_revive=0", "party_host_revive=1",
      '"host_revive":false')),
    # 0.7.2: rot (off by default: the change turns it on and back off) and its three cleanses (on)
    ("Party rules: Rot", ["Rot on death", "Rot: boss kill cleanses", "Rot: kill your killer", "Rot: echoes back"],
     ("Rot on death", "", "right", "On", "left", "party_rot=1", "party_rot=0", '"rot":true')),
    ("Party rules: Enemies", ["Boss health", "Boss damage", "Enemy health", "Enemy damage", "Boss poise", "Enemy poise"],
     ("Boss health", "", "right", "7", "left", "party_boss_hp=200", "party_boss_hp=150", '"boss_hp":200')),
    ("My party: Deaths", ["Guest gets up, no load", "Respawn at host's lamp", "Refill vials, bullets"],
     ("Guest gets up, no load", "", "left", "Off", "right", "party_guest_revive_inplace=0",
      "party_guest_revive_inplace=1", None)),
    ("My party: World", ["Guests use lamps", "Guests use Hunter's Mark", "Open world", "Stay in Chalice Dungeons"],
     ("Open world", "down/down/", "left", "Off", "right", "party_open_world=0", "party_open_world=1", None)),
    ("My party: Story", ["Share NPC dialogue", "NPC dialogue range", "Share story progress", "Share NPC quests",
                         "Share cutscenes, endings", "Cutscenes live with host"],
     ("Share NPC dialogue", "", "left", "Off", "right", "party_npc_talk=0", "party_npc_talk=1", None)),
    ("My party: Rewards", ["Full rewards for guests", "Host's item rewards", "Give the bells", "Own key items as guest",
                           "Key items at once", "Insight for bosses"],
     ("Full rewards for guests", "", "left", "Off", "right", "party_full_rewards=0", "party_full_rewards=1", None)),
    ("My party: Display", ["Party HUD", "Party name tags", "Pings", "Show party through walls", "Bloodstain marker",
                           "Above companions"],
     ("Pings", "down/down/", "left", "Off", "right", "party_pings=0", "party_pings=1", None)),
    ("My party: Look & notices", ["No companion glow", "Join/leave notices"],
     ("Join/leave notices", "down/", "left", "Off", "right", "party_toasts=0", "party_toasts=1", None)),
]
# The values a fresh run starts from (the rows' defaults): these keys are dropped from each
# instance's bbport.ini first, so an earlier run's changes never shift the walk.
RULES_INI_RESET = ["party_rot", "party_guest_revive_inplace", "party_friendly_fire", "party_npc_talk", "party_full_rewards",
                   "party_boss_hp", "party_pings", "party_host_revive", "party_share_lamps", "party_guests_bring_lamps",
                   "party_kill_record", "party_gather", "party_travel_vote", "party_covenant_per_player",
                   "party_npc_mirror", "party_key_view", "party_key_items_summoned", "party_rot_cleanse_boss",
                   "party_rot_cleanse_killer", "party_rot_cleanse_echoes", "party_npc_fights", "party_open_world",
                   "party_toasts", "party_any_kidnap", "settings_rev"]


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
    # from the last page back up to Party rules: PvP & lamps
    up = "/".join(["up"] * (len(RULES_PAGES) - 1 - [pg[0] for pg in RULES_PAGES].index("Party rules: PvP & lamps")))
    host += "/" + "/".join(["wait"] * 12) + "/circle/" + up + "/cross/left/mark=rules_h_ffoff/circle/circle/circle"
    guest += "/circle/circle/circle"
    watch = []
    n = len(RULES_PAGES)
    for tag in ["rules_h%d" % k for k in range(1, n + 1)] + ["rules_g%d" % k for k in range(1, n + 1)] + \
               ["rules_h%dc" % k for k in range(1, n + 1)] + ["rules_h_ffoff"]:
        watch += ["--watch", r"step \d+/\d+: ~?mark=%s(@|\s)=%s@1.2" % (tag, tag)]
    return dict(
        name="rules", seconds=300, count=2, headless=0,
        env=["BB_GAME_MENU_TRACE=1", "BB_TEST_MENU_FILE={root}\\menu{i}.txt"],
        ienv=[],
        files={"menu0.txt": "joined 8 %s\n-\n-\n" % host, "menu1.txt": "joined 30 %s\n-\n-\n" % guest},
        ini_reset=RULES_INI_RESET,
        # a 0.7.1 bbport.ini: party_rot=1 saved, no settings_rev: the game turns rot off once (0.7.2)
        ini_preset={"party_rot": "1"},
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
SF_STAY = 30  # s guest 2 must stay in the host's world after its join (an unreachable joiner made it leave)


def phase_summonfail(only):
    return dict(
        name="summonfail", seconds=360, count=3, headless=1,
        env=[], ienv=["1:BB_PARTY_PORT={hold}"],
        hold_ports=16,
        start_when=["2/1:" + SF_FAILED],
        until_hold=SF_STAY + 5,  # guest 2 must stay in the host's world this long
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
    (killed within ~10 s: two of them took the host's 575 HP in 13 s). R3 twice (":lock": the second
    only while not locked on): after the host_chr warp the camera can face away from the enemy, and
    an R3 then only turns the camera behind the player, as on a pad (deaths071: the first R3 missed
    after host_chr with and without a death before it; the second locked on)."""
    return ["host_chr=0:0.6@2%m24_01!", "host_press=0x4:lock@1%m24_01!", "host_press=0@1%m24_01!",  # R3: lock-on
            "host_press=0x4:lock@1%m24_01!", "host_press=0@1%m24_01!",
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


# Phase sk (streamsync, ~3 min, 3 players): a neighbouring map's enemy killed by the host and its map
# streamed out and in again. 2400125 is a Cathedral Ward enemy (m24_00) 28 m from the Tomb of Oedon
# lamp (m24_01); the game brings such an enemy back alive when its map streams in again, with no load
# (vanilla, in single player too: streamsync solo run), and only the game that streamed it showed it
# alive. The host kills it, walks 130 m away within 3 s (the census never saw the kill: respawnE),
# waits there (m24_00's enemies stream out), comes back. Every game must keep it dead: the guests get
# the kill from the kill record, the host puts it to death again with no echoes and no drop, and the
# guests' census then matches the host's.
SK_FAR = "-158.10:-41.72:16.93"   # 130 m from the lamp (m24_01): m24_00 streams out
SK_BACK = "-36.8:-40.75:84.3"     # next to the lamp
SK_ENEMY = "2400125"
SK_UNTIL = r"0:travel step (\d+)/\1: "


def phase_sk(only):
    host = ",".join([
        "god",
        "host_warp=2412953@20",                                    # the Tomb of Oedon lamp (party travel)
        "host_kill=%s:hp@8%%m24_01" % SK_ENEMY,                     # the neighbour map's enemy, 28 m away
        "host_move=%s@3%%m24_01" % SK_FAR,                         # away before any census
        "host_press=0@14%m24_01!",                                 # there: m24_00 streams out
        "host_kill=%s:hp@1%%m24_01" % SK_ENEMY,                    # not loaded here (no target)
        "host_move=%s@1%%m24_01" % SK_BACK,                        # back: m24_00 streams in again
        "host_press=0@14%m24_01!",
        "host_press=0@20%m24_01!",                                 # a census digest or two (every 15 s)
        "host_kill=%s:hp@1%%m24_01" % SK_ENEMY,                    # still dead on the host (no target)
        "host_press=0@3%m24_01!",
    ])
    return dict(
        name="sk", seconds=420,
        env=["BB_PARTY_ENEMY_CENSUS=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god", "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[],
        until=[SK_UNTIL],
    )


# Phase lf (lifts071, ~3 min, 3 players): D3, a guest's world actions on lifts and ladders. The host
# walks 130 m away (Cathedral Ward's map block streams out of its game: its copy of the lift's events
# cannot run); guest 1 rides Cathedral Ward's lift 2401102 up (its own copy; the game's own sync starts
# the copy of every game that has the block loaded: guest 2 at the lamp) and steps out; the ride goes
# up as a world action and the host's world takes it. Guest 1 then pulls the lever 2411316 that lowers
# Central Yharnam's first ladder (fixture: 12410285 cleared on every save). The host comes back: the
# block's init event must put the car where guest 1 left it (before the fix: at the old end, a lift
# that showed the car on one game and an empty shaft on the other), and every game must end with the
# same lift and ladder flags.
LF_FAR = "-158.10:-41.72:16.93"    # 130 m from the Tomb of Oedon lamp: m24_00 streams out
LF_BACK = "-36.8:-40.75:84.3"      # the lamp (m24_00 streams in again)
LF_LIFT = "2401102"                # Cathedral Ward <-> lower Cathedral Ward lift car
LF_CAR_LOW = "24.28:-45.3:194.17"  # region 2402059 (the up event's switch): in the car at the bottom
# the bottom landing between the car (24.28, 194.17) and its bottom lever 2401103 (MSB (29.37, -45.71,
# 196.88)): 1 m in front of the lever (30.11, 197.56) has no floor (the guest fell to y -118, runs 2 / 3)
LF_LOW_SIDE = "30.11:-45.6:197.56:0.82:native:pin=4"  # in front of the lever, held there 4 s while the floor
                                                      # streams in (run 5: no collision at the arrival anywhere
                                                      # near: the guest fell to y -118 from both spots)
LF_FLAGS = "12400827/12400828/12410285"  # lift position / busy, the ladder lever's event (ladder down)
LF_UNTIL = r"0:travel step (\d+)/\1: host_press"


def RIDE_BURST(car, start, end):
    """liftfix: a screenshot of every rendered window every second from a ride's start (the car's ride
    animation on any game) to 2 s after its end (shot.py --burst): ride<N>_<t>s_inst<i>.png."""
    return ["--burst", r"test: object %s anim %s playing 1 => test: object %s anim %s playing 0" % (car, start, car, end)]


def phase_lf(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_objanim=2411204,log_flag=%s" % (LF_LIFT, LF_FLAGS)
    host = ",".join([common,
        "host_move=%s@10%%m24_01" % LF_FAR,                # far: the lift's block streams out here
        "host_move=%s@60%%m24_01" % LF_BACK,               # back after the ladder and the ride
        "host_press=0@12%m24_01!",                         # the block's init has run: the end
    ])
    g1 = ",".join([common,
        "guest_obj=2411316:1.0:0@15%m24_01",               # the ladder's lever (Central Yharnam)
        "guest_press=0x4000@4%m24_01!",                    # pull: the ladder comes down
        "guest_move=%s@6%%m24_01" % LF_BACK,               # back at the lamp: Cathedral Ward streams in
        # liftfix: beside the car at the bottom (its bottom lever), then into the car's switch region:
        # its up event starts and the guest rides it up. (0.7.1 put the guest on the car object's root,
        # which is at the TOP with the car at the bottom: the guest fell the 48 m shaft through the
        # region, the up event ran with nobody in the car and the guest stayed at the bottom; god mode
        # and the flags-only checks hid it.)
        # onto the bottom landing beside the car by the game's own in-world warp, then a few seconds for
        # the car object to be active
        "guest_move=%s@8%%m24_0" % LF_LOW_SIDE,
        "guest_move=%s@4%%m24_0" % LF_CAR_LOW,             # into the car: up it goes
        "guest_obj=2401104:1.0:0@0.5%%m24_0?obj=%s:32" % LF_LIFT,  # out at the top on the arrival
        "guest_press=0@10!",                               # (in m24_00 there: no map condition)
    ])
    g2 = "god,log_objanim=%s,log_objanim=2411204,log_flag=%s" % (LF_LIFT, LF_FLAGS)
    return dict(
        name="lf", seconds=300,
        watch=RIDE_BURST(LF_LIFT, "(1|31)", "(7|29)"),  # the up ride 31, 32, end 7; the down ride 1, 28, end 29
        env=["BB_PARTY_TEST_START_LAMP=2412953", "BB_PARTY_PROGRESS_TEST_CLEAR=12410285"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2],
        crash=[],
        until=[LF_UNTIL, r"1:travel step (\d+)/\1: guest_press"],
    )


# Phase ga (gather, ~4 min, 3 players; ERSC_PARITY I5): the host's System > Party > Gather the party
# three times (round 1 by the hotkey: the touchpad + L3 tapped through the game's pad path). After
# a party travel to the Tomb of Oedon (m24_01) guest 1 stands 130 m away in the
# same map and guest 2 travels alone to the Hunter's Dream. Round 1: both YES (guest 1 an in-world
# warp beside the host, no load; guest 2 the one-load travel, then beside the host). A press 5 s later
# is refused (cooldown). Round 2: guest 1 has its in-game menu open (refused: in a menu), guest 2
# answers No. Then guest 1 travels alone to the Dream; round 3: guest 1 YES (the travel), guest 2 YES
# (already beside the host). Answers: BB_PARTY_GATHER_TEST (party_gather.h).
GA_FAR = SK_FAR  # 130 m from the Tomb of Oedon lamp, same map
GA_ARRIVED = r"guest: arrived in the host's map"
# the host chain's last step (20 s after round 3: guest 1's arrival and its screenshots) and both arrivals
# Every chain's last step (round 3's guest may not log an arrival: the timing drift noted in the
# ga checks); the arrivals are checked by the verdict rows, not waited for here.
GA_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: ", r"2:travel step (\d+)/\1: "]


def phase_ga(only):
    host = ",".join([
        "god",
        "host_warp=2412953@15",          # party lamp travel: the Tomb of Oedon (everyone in m24_01)
        "host_press=0x100002@45%m24_01",  # round 1: the hotkey, touchpad + L3 (runtime_pad.c ping_combo)
        "host_gather@5%m24_01",          # refused: the cooldown
        "host_gather@28%m24_01",         # round 2
        "host_gather@45%m24_01",         # round 3
        "host_press=0@15%m24_01!",
    ])
    # guest 2's alone lamp (a rest) gives the host a rest load that guest 1 follows: guest 1 walks
    # off after it (60 s), and to the Dream after round 2
    g1 = ",".join(["god", "guest_move=%s@60%%m24_01" % GA_FAR,   # far away in the same map
                   "guest_lamp=2102950@75%m24_01"])              # after round 2: alone to the Dream
    g2 = ",".join(["god", "guest_lamp=2102950@35%m24_01"])       # alone to the Dream (another map)
    watch = ["--grey"]
    for rx, tag in ((r"round \d+: Gather the party: \d+ guest", "gather"), (r"gather: put beside the host", "beside"),
                    (GA_ARRIVED, "arrived"), (r"round 2 refused: in a menu", "menu")):
        watch += ["--watch", "%s=%s@0.5,3" % (rx, tag)]
    watch += ["--watch", "round 3 over=r3arrived@14,18"]  # guest 1's travel from the Dream (~12 s)
    return dict(
        name="ga", seconds=360,
        env=[],
        watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              "1:BB_PARTY_TRAVEL_CHOICE=alone", "2:BB_PARTY_TRAVEL_CHOICE=alone",
              "1:BB_PARTY_GATHER_TEST=yes,menu,yes", "2:BB_PARTY_GATHER_TEST=yes,no,yes"],
        crash=[],
        until=GA_UNTIL,
    )


# Phase gl (gatherfix, ~4 min, 2 players, group `gather_ground`): a gather puts guests on ground only.
# A player's report (Old Yharnam, the ladder beside Djura's tower): the host held still half way up a
# ladder and gathered; the guest was put 0.5 m beside it in the open shaft and dropped 3 m (from
# higher up a fall death). The party travels to the Cathedral Ward lamp 2402950; the host stands at the
# foot of the ladder 2401024 (~75 m from the lamp, where the guest stays) and gathers: the guest's YES
# is "yesdrop" (BB_PARTY_GATHER_TEST: the spot 8 m too high, a spot with no ground). The landing watch
# must catch the fall (FELL, 4 m below the spot), put it back where it stood (no load) and send it by
# the travel (one loading screen), then beside the host once arrived, standing. Then the host takes the
# ladder (Cross) and gathers on it: refused (on a ladder), no question on the guest.
GL_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]


def phase_gl(only):
    host = ",".join([
        "god", "log_pos=0.5",
        "host_warp=2402950@15",                     # the party to the Cathedral Ward lamp
        "host_obj=2401024:1.0:180@10%m24_00",       # the ladder's foot, ~75 m from the lamp
        "host_gather@3%m24_00",                     # round 1: on ground (the guest: yesdrop)
        "host_press=0x4000@45%m24_00!",             # Cross: onto the ladder (after the guest's travel)
        "host_gather@3%m24_00",                     # refused: on a ladder
        "host_press=0@12%m24_00!",
    ])
    g1 = ",".join(["god", "log_pos=0.5", "guest_press=0@120%m24_00!"])  # stays at the lamp
    watch = ["--grey"]
    for rx, tag in ((r"round \d+: Gather the party: \d+ guest", "gather"), (r"gather: put beside", "beside"),
                    (r"guest: FELL", "fell"), (r"guest: arrived in the host's map", "arrived"),
                    (r"guest: stands beside the host", "stands"), (r"Gather the party refused: on a ladder", "ladder")):
        watch += ["--watch", "%s=%s@0.5,3" % (rx, tag)]
    return dict(
        name="gl", seconds=300, count=2, headless=0,
        env=[], watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_GATHER_TEST=yesdrop"],
        crash=[], until=GL_UNTIL,
    )


# Phase tv (lampvote, ~4 min, 2 players, group `travel_vote`): lamp travel voting (0.7.3,
# party_vote.h), the host's rule on (BB_PARTY_TRAVEL_VOTE=1). The host's "Travel with the party?" is
# answered YES each time (BB_PARTY_SPLIT_TEST), the guest's vote questions by BB_PARTY_VOTE_TEST.
# (1) all YES: the party travels to the Tomb of Oedon (m24_01). (2) the guest votes NO: cancelled,
# nobody moves, the notice names it. (3) the guest leaves the question open: the timeout (8 s here,
# BB_PARTY_VOTE_TIMEOUT) cancels it and names it; its dialog closes. (4) the rule off (host_rule):
# the party travels as before, no question on the guest (the Cathedral Ward, m24_00). (5) the rule
# on again, the guest's own lamp: its party request becomes a vote of the host (YES): the party
# travels back to m24_01.
TV_UNTIL = [r"1:travel step (\d+)/\1: ", r"0:travel step (\d+)/\1: "]


def phase_tv(only):
    host = ",".join([
        "god",
        "host_lamp=2412953@8^1",                  # (1) all YES
        "host_lamp=2402950@8%m24_01",             # (2) the guest's NO
        "host_lamp=2402950@10%m24_01",            # (3) no answer: the timeout
        "host_rule=party_travel_vote:0@10%m24_01",
        "host_lamp=2402950@3%m24_01",             # (4) the rule off: as before
        "host_rule=party_travel_vote:1@4%m24_00",
    ])
    guest = ",".join(["god",
                      "guest_lamp=2412953@10%m24_00",  # (5) the guest's own travel: the host votes
                      "guest_press=0@8%m24_01!"])      # back in the Tomb of Oedon: the end
    return dict(
        name="tv", seconds=360, count=2, headless=0,
        env=["BB_PARTY_VOTE_TIMEOUT=8"],
        watch=["--watch", r"Party vote: round \d+: question open=q@1.2",
               "--watch", r"Party vote: round \d+ over \(.*cancelled=cancel@1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "0:BB_PARTY_TRAVEL_VOTE=1", "0:BB_PARTY_SPLIT_TEST=yes", "0:BB_PARTY_VOTE_TEST=yes",
              "1:BB_PARTY_SPLIT_TEST=yes", "1:BB_PARTY_VOTE_TEST=yes,no,none"],
        crash=[],
        until=TV_UNTIL,
        until_hold=6,
    )


def check_tv(v, L):
    need(v, L, "tv join: 2 players", 0, r"cooperators 0 -> 1")
    # (1) all YES
    r1 = need(v, L, "tv (1) the host's YES starts a vote", 0, r"Party vote: round 1: the host's lamp / headstone: .* asking 1")
    need(v, L, "tv (1) the guest gets the question", 1, r"Party vote: round 1: question open")
    need(v, L, "tv (1) the guest votes yes", 0, r"Party vote: round 1: .* votes yes")
    g1 = need(v, L, "tv (1) all YES: the party travels", 0, r"Party vote: round 1 over \(.*go\): lamp 2412953 -> the party travels")
    need(v, L, "tv (1) the guest arrives in m24_01", 1, r"map .*-> m24_01", g1 or 0.0)
    # (2) one NO
    need(v, L, "tv (2) the guest votes no", 0, r"Party vote: round 2: .* votes no")
    r2 = need(v, L, "tv (2) one NO cancels, names who", 0,
              r"Party vote: round 2 over \(.*declined\): lamp 2402950 cancelled: Party travel cancelled: .* said no")
    need(v, L, "tv (2) the guest is told", 1, r"Party vote: round 2 over: Party travel cancelled: .* said no")
    # (3) no answer
    r3 = need(v, L, "tv (3) no answer: the timeout cancels, names who", 0,
              r"Party vote: round 3 over \(.*timeout\): lamp 2402950 cancelled: Party travel cancelled: .* did not answer")
    need(v, L, "tv (3) the guest's open question closes", 1, r"Party vote: round 3: this game's question closed \(cancelled\), dialog cancelled")
    for t, tag in ((r2, "(2)"), (r3, "(3)")):
        if t:
            none_of(v, L, "tv %s cancelled: nobody travels" % tag, r"host travel #\d+ lamp .*lamp 2402950", t - 12, t + 4, (0,))
    # (4) the rule off: as before, no question on the guest
    off = need(v, L, "tv (4) the rule off", 0, r"test: host_rule: party_travel_vote 1 -> 0")
    if off:
        need(v, L, "tv (4) rule off: the host's party travel runs at once", 0, r"lamp 2402950 \(host\): YES -> travel with the party", off)
        none_of(v, L, "tv (4) rule off: no vote", r"Party vote: round 4", off, off + 15, (0, 1))
        need(v, L, "tv (4) rule off: the guest follows to m24_00", 1, r"map .*-> m24_00", off)
    # (5) a guest's travel: the host votes
    g5 = need(v, L, "tv (5) the guest's request becomes a vote", 0,
              r"Party vote: round 4: a guest's lamp / headstone: .* wants the party to travel to lamp 2412953; asking 1")
    need(v, L, "tv (5) the starter waits", 1, r"Party vote: round 4: the party votes on this player's travel")
    need(v, L, "tv (5) the host gets the question", 0, r"Party vote: round 4: question open")
    need(v, L, "tv (5) the host's YES: the party travels", 0, r"Party vote: round 4 over \(.*go\): lamp 2412953 -> the party travels")
    if g5:
        need(v, L, "tv (5) the party back in m24_01", 1, r"map .*-> m24_01", g5)
    check_common(v, L, "tv")


# Phase el (liftfall / liftfix, ~3 min, 2 players, group `elevator`): the Nightmare of Mensis' library
# lift 2601260 (60 m, y 1003 -> 943, after Micolash: a player's report, the host warped back to its lamp
# mid-ride and the guest dead). Its car is a one-hunter birdcage (two hunters in it overlap: the 0.7.1
# smoke warped both onto one spot and the pushed one fell through the floor). Its position flag
# 12601253 (1: the car at the top) flips once the rider has left the car at the far end; every game
# runs its own copy of the lift's event and a step into the car starts the copy of every game (the
# game's own sync), so both games' cars ride and arrive together, and the copy of the game whose player
# is out of the car ends at once: its flip reaches the other game while that one's rider is still in
# the car. Fixture: Micolash dead (12601850, the lift's init waits for it), the car at the top.
# (A) the guest rides down alone and steps out at the arrival (the host's copy ends first: the 0.7.1
# race, the guest's own end must stand). (B) the host rides up alone, out at the arrival (the guest's
# copy ends first: its report must not move the host's car). (C) the guest rides down alone and stays
# 2 s in the car (the host's flip waits on the guest until its copy is done). (D) the host rides the
# lift's second cage (2601261, triggers 2602243 / 2602244: it runs the other way in the same event) down
# alone. Each ride starts with
# one step into the car's trigger region (2602242 top / 2602241 bottom); riders step out on the car's
# arrival (`?obj=`), the next ride waits for the flag (`?FLAG=V`) or the other player's spot (`?near=`).
# After every ride both games show the same flag and the car at that end; no rider drops faster than a
# lift or loses the car mid-ride (its ride bit), nobody is moved by the fall recovery, nobody dies.
EL_CAR, EL_POS = "2601260", "12601253"
EL_IN_CAR = "119.4:1003.5:-18.5"       # region 2602242: in the car at the top (its trigger)
EL_IN_CAR_LOW = "119.4:943.5:-18.5"    # region 2602241: in the car at the bottom (its trigger)
EL_TOP = "112.1:1003.8:-23.0"          # the top floor, out of every region of the lift
EL_LOW_OUT_H = "120.0:943.7:-20.15"    # the bottom floor beside the car (the host), out of every region
EL_LOW_OUT_G = "118.1:943.7:-16.8"     # ... (the guest)
EL_DOWN_END, EL_UP_END = "3", "5"      # the car's ride animations that stop at the arrival (down / up)
# both chains' last steps: the guest's last one waits for its own copy's flag at its ride's end
EL_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]
EL_CAGE2_TOP = "117.73:1003.5:-18.51"  # region 2602244: in the second cage (2601261) at the top
EL_RIDERS = ("guest", "host", "guest", "host")  # (A), (B), (C), (D)
EL_AXES = ((119.43, -18.52),) * 3 + ((117.73, -18.51),)  # the cage each ride's rider stands in


def phase_el(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_objanim=2601261,log_flag=%s" % (EL_CAR, EL_POS)
    near = lambda xyz: "?near=%s:1.5" % xyz
    arrived = lambda anim: "?obj=%s:%s" % (EL_CAR, anim)
    host = [common,
            "host_move=%s@3%%m26^1" % EL_TOP,                                    # at the top, out of the lift
            "host_move=%s@3%%m26?%s=0" % (EL_LOW_OUT_H, EL_POS),                 # (A) over here: to the bottom
            "host_move=%s@2%%m26%s" % (EL_IN_CAR_LOW, near(EL_LOW_OUT_G)),       # (B) into the trigger: up
            "host_move=%s@0.3%%m26%s" % (EL_TOP, arrived(EL_UP_END)),            # out at the arrival
            "host_move=%s@3%%m26?%s=0" % (EL_CAGE2_TOP, EL_POS),                 # (D) (C over) the second
            "host_move=%s@0.3%%m26?obj=2601261:3" % EL_LOW_OUT_H,                # cage down; out at arrival
            "host_press=0@3%%m26!?%s=1" % EL_POS]                                # (D) over here
    g1 = [common,
          "guest_move=%s@6%%m26" % EL_TOP,                                       # at the top
          "guest_move=%s@3%%m26%s" % (EL_IN_CAR, near(EL_TOP)),                  # (A) into the trigger: down
          "guest_move=%s@0.3%%m26%s" % (EL_LOW_OUT_G, arrived(EL_DOWN_END)),     # out at the arrival
          "guest_move=%s@3%%m26?%s=1" % (EL_IN_CAR, EL_POS),                     # (C) (B over here) down
          "guest_move=%s@2%%m26%s" % (EL_LOW_OUT_G, arrived(EL_DOWN_END)),       # 2 s in the car, then out
          "guest_press=0@3%%m26!?%s=1" % EL_POS]                                 # (D) over here
    return dict(
        name="el", seconds=360, count=2, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2602953", "BB_PARTY_PROGRESS_TEST_SET=12601850,%s" % EL_POS,
             "BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12601352"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=["--watch", r"object %s stopped at anim [35] now=arrive@0.2,2" % EL_CAR,
               "--watch", r"_move to \(119\.40=incar@1,8"] + RIDE_BURST(EL_CAR, "[68]", "[79]"),
        crash=[], until=EL_UNTIL,
    )


# Phase lt (liftfix, ~2 min, 2 players, group `lifts_together`): host and guest ride Central Yharnam's
# lift 2411320 (14 m, a platform for several hunters; its ride ends once every player is out, 3[7])
# TOGETHER, down and back up: the host stands on the car beside its trigger (a 0.9 m box), the guest
# steps into the trigger once the host is there (`?near=`), both placed by the game's own in-world warp;
# both step out at the arrival. Fixture: the lift's first activation done (12410330) and the car at the
# top (12410350). After each ride both games show the same flags and the car at that end, both hunters
# rode the whole way (at the far end on arrival, the ride bit on, no drop faster than a lift).
LT_CAR, LT_POS, LT_BUSY = "2411320", "12410350", "12415350"
LT_TOP_C = "-128.72:-44.3:-9.73"       # region 2412321's box (the down ride's trigger)
LT_TOP_A = "-128.19:-44.3:-9.20"       # on the car, 0.75 m beside the trigger (across the car's way out)
LT_BOT_C = "-128.72:-58.0:-9.73"       # region 2412322's box (the up ride's trigger)
LT_BOT_A = "-129.25:-58.0:-10.26"      # beside the trigger on the other side (the host's (A) spot arrives
                                       # at (-128.19, -58.1, -9.20): a `?near=` there passed at once, run 3)
LT_TOP_OUT = "-130.27:-44.4:-6.27"     # the top landing (lifts071: players stood here)
LT_BOT_OUT = "-125.28:-58.4:-11.69"    # the bottom landing
LT_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]


def phase_lt(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_flag=%s/%s" % (LT_CAR, LT_POS, LT_BUSY)
    nat = lambda xyz: xyz + ":0:native"
    host = [common,
            "host_move=%s@4%%m24_01^1" % nat(LT_TOP_OUT),                          # at the lift, top
            "host_move=%s@4%%m24_01" % nat(LT_TOP_A),                              # (A) on the car
            "host_move=%s@0.5%%m24_01?obj=%s:1" % (nat(LT_BOT_OUT), LT_CAR),       # out at the arrival
            "host_move=%s@3%%m24_01?%s=0" % (nat(LT_BOT_A), LT_BUSY),              # (B) on the car
            "host_move=%s@0.5%%m24_01?obj=%s:3" % (nat(LT_TOP_OUT), LT_CAR),       # out at the arrival
            "host_press=0@3%%m24_01!?%s=0" % LT_BUSY]                              # (B) over here
    g1 = [common,
          "guest_move=%s@6%%m24_01" % nat(LT_TOP_OUT),
          "guest_move=%s@1%%m24_01?near=%s:0.6" % (nat(LT_TOP_C), LT_TOP_A),        # (A) into the trigger
          "guest_move=%s@0.5%%m24_01?obj=%s:1" % (nat(LT_BOT_OUT), LT_CAR),
          "guest_move=%s@1%%m24_01?near=%s:0.6" % (nat(LT_BOT_C), LT_BOT_A),        # (B) into the trigger
          "guest_move=%s@0.5%%m24_01?obj=%s:3" % (nat(LT_TOP_OUT), LT_CAR),
          "guest_press=0@3%%m24_01!?%s=0" % LT_BUSY]
    return dict(
        name="lt", seconds=300, count=2, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2412953", "BB_PARTY_PROGRESS_TEST_SET=12410330,%s" % LT_POS],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=["--watch", r"object %s stopped at anim [13] now=arrive@0.3,2" % LT_CAR,
               "--watch", r"guest_move to \(-128\.72=together@1,5"] + RIDE_BURST(LT_CAR, "[46]", "[57]"),
        crash=[], until=LT_UNTIL,
    )


# Phase ch (liftfix, ~2.5 min, 2 players, group `cainhurst_lift`): Cainhurst Castle's lift 2501010 (34 m,
# y 65 -> 31; down 12500077: car anims 0 then 1, up 12500076: 10 then 11). Like the library lift it ended
# its ride once the LOCAL player was out of the car (the party rewrites it to every player, as the other
# lifts: liftrules::LocalEndToAllPlayers). Fixture: the car at the top (12500074 cleared). The players
# reach the landings beside its levers by the game's own in-world warp from the Cainhurst lamp. (A) the
# guest rides down alone, out at the arrival; (B) the guest rides up alone and stays 2 s in the car (the
# host's copy would end first without the rewrite); (C) the host rides down alone, out at the arrival.
# After each ride both games show the same flags and the car at that end, the rider rode in the car all
# the way, nobody drops faster than a lift (from the approach on), nobody dies.
CH_CAR, CH_POS, CH_BUSY = "2501010", "12500074", "12505074"
CH_TOP_IN = "-22.78:64.95:-237.74"     # region 2502001 (the down ride's trigger, a 1.4 m box)
CH_BOT_IN = "-22.78:30.9:-237.74"      # region 2502000 (the up ride's trigger)
# The car is a wide platform: 2.1-2.4 m from its axis is still on it (run 7: both hunters rode every
# ride from there). The landings: 1 m in front of each call lever (the operator's spot, 3.2-3.4 m out),
# top lever 2501011 (-27, 65, -239) facing +x, bottom lever 2501012 (-18.6, 31, -236) facing -x.
CH_TOP_H = "-26.0:65.1:-239.0:0:native:pin=2"
CH_TOP_G = "-26.0:65.1:-237.2:0:native:pin=2"
CH_BOT_H = "-19.6:31.1:-236.0:0:native:pin=2"
CH_BOT_G = "-19.6:31.1:-237.8:0:native:pin=2"
CH_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]
CH_RIDERS = ("guest", "guest", "host")


def phase_ch(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_flag=%s/%s" % (CH_CAR, CH_POS, CH_BUSY)
    nat = lambda xyz: xyz  # the landings carry their own warp options
    arrived = lambda anim: "?obj=%s:%s" % (CH_CAR, anim)
    host = [common,
            "host_move=%s@4%%m25^1" % nat(CH_TOP_H),                               # at the top landing
            "host_press=0@1%%m25!?%s=1" % CH_POS,                                   # (A) over here
            "host_move=%s@3%%m25?%s=0" % (CH_TOP_IN, CH_POS),                      # (C) once (B) is over here
            "host_move=%s@0.3%%m25%s" % (nat(CH_BOT_H), arrived(0)),              # out at the arrival
            "host_press=0@3%%m25!?%s=0" % CH_BUSY]                                # (C) over here
    g1 = [common,
          "guest_move=%s@6%%m25" % nat(CH_TOP_G),                                  # at the top landing
          "guest_move=%s@3%%m25?near=%s:1.5" % (CH_TOP_IN, CH_TOP_H.split(":0:")[0]),  # (A) down
          "guest_move=%s@0.3%%m25%s" % (nat(CH_BOT_G), arrived(0)),               # out at the arrival
          "guest_move=%s@3%%m25?%s=0" % (CH_BOT_IN, CH_BUSY),                      # (B) up
          "guest_move=%s@2%%m25%s" % (nat(CH_TOP_G), arrived(10)),                # 2 s in the car, then out
          "guest_press=0@4%%m25!?%s=0" % CH_BUSY]                                  # (B) over here
    return dict(
        name="ch", seconds=300, count=2, headless=0,
        # 12500000: the first-arrival cutscene seen (run 5: the host sat 40 s in it, its copies paused)
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_PROGRESS_TEST_CLEAR=%s,%s" % (CH_POS, CH_BUSY),
             "BB_PARTY_PROGRESS_TEST_SET=12500000",
             "BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12500076,12500077"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=["--watch", r"object %s stopped at anim (0|10) now=arrive@0.2,2" % CH_CAR] + RIDE_BURST(CH_CAR, "(0|10)", "(1|11)"),
        crash=[], until=CH_UNTIL,
    )


# Phase rh (liftfix, ~3 min, 2 players, group `research_lift`, at 60 FPS): the Research Hall lift 3501105
# (DLC m35, 25 m, y 1583 -> 1558; down 13501113: car 1, 2, end 3 once every player is out; up 13501112:
# 5, 6, end 7; 13501116 = 1: the car at the top). A player's report at 60 FPS: the host rode it down from
# the top alone (the guest 40 m away at the lamp) and went through the car and the shaft's floor (HP 0
# 11 m under it). Fixture: the lift's first start done (13501118), the car at the top. (A) the host down
# alone, the guest at the lamp; (B) host and guest up together; (C) both down together. Riders step out
# on the arrival. After each ride both games show the same flags and car end; every rider on the car all
# the way (ride bit), no drop faster than a lift, no jump, nobody dies; the rider guard's put-backs are
# counted (liftrules::RiderTick: "dropped through the moving car").
RH_CAR, RH_POS, RH_BUSY = "3501105", "13501116", "13504110"
RH_TOP_IN = "-344.56:1582.6:-792.84"       # region 3502106 (the down ride's trigger)
RH_BOT_IN = "-344.56:1558.1:-792.84"       # region 3502107 (the up ride's trigger)
RH_TOP_BESIDE = "-345.9:1583.3:-793.0"     # on the car beside the trigger (the report's rider spots)
RH_BOT_BESIDE = "-345.9:1558.2:-793.0"
RH_TOP_OUT = "-357.2:1583.1:-794.1:0:native:pin=2"   # the top landing (the report's host stood there)
RH_TOP_OUT2 = "-357.6:1583.1:-790.2:0:native:pin=2"
RH_BOT_OUT = "-332.8:1557.8:-822.2:0:native:pin=2"   # the bottom floor toward the lamp
RH_LAMP = "-318.7:1553.0:-824.2:0:native:pin=2"      # lamp 3502950
RH_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]


def phase_rh(only):
    # liftfix runs 11-15: the car left upward again in the frame its rider stepped out of the bottom box
    # (the Research Hall lifts clear their busy flag in that frame: liftrules::BusyDefer) and a host on
    # the platform's edge or stepping back fell into the 12 m pit (the report's death at y 1546). Each
    # exit is followed by a wait in which the car must stay; then the next ride. (A) the host down
    # alone, the guest at the lamp; (B) the host up alone; (C) host and guest down; (D) host and guest up.
    common = "god,log_pos=0.5,log_objanim=%s,log_flag=%s/%s/13501117" % (RH_CAR, RH_POS, RH_BUSY)
    arrived = lambda anim: "?obj=%s:%s" % (RH_CAR, anim)
    top_out = RH_TOP_OUT
    bot_out = RH_BOT_OUT   # run 17: (-341, -796) beside the car has no floor (the host fell out of the world)
    host = [common,
            "host_move=%s@4%%m35^1" % top_out,                                     # at the top landing
            "host_move=%s@3%%m35" % RH_TOP_IN,                                     # (A) down alone
            "host_move=%s@2%%m35%s" % (bot_out, arrived(2)),                        # 2 s in, out; the car stays
            "host_move=%s@6%%m35" % RH_BOT_IN,                                     # (B) 6 s later back in: up
            "host_move=%s@2%%m35%s" % (RH_TOP_OUT2, arrived(6)),                    # out at the top
            "host_move=%s@2%%m35?near=%s:1.0" % (RH_TOP_IN, ":".join(RH_TOP_BESIDE.split(":")[:3])),  # (C) both
            "host_move=%s@2%%m35%s" % (bot_out, arrived(2)),                        # out at the bottom
            "host_move=%s@6%%m35?near=%s:1.0" % (RH_BOT_IN, ":".join(RH_BOT_BESIDE.split(":")[:3])),  # (D) both
            "host_move=%s@2%%m35%s" % (top_out, arrived(6)),
            "host_press=0@4%%m35!?%s=0" % RH_BUSY]
    g1 = [common,
          "guest_move=%s@6%%m35" % RH_LAMP,                                         # at the lamp, 40 m away
          "guest_move=%s@1%%m35?near=-357.6:1583.1:-790.2:1.0" % RH_TOP_BESIDE,     # (C) on the car at the top
          "guest_move=%s@2%%m35%s" % (RH_LAMP, arrived(2)),                         # out at the bottom (lamp)
          "guest_move=%s@3%%m35?near=-332.8:1557.8:-822.2:1.5" % RH_BOT_BESIDE,     # (D) on the car at the bottom
          "guest_move=%s@2%%m35%s" % (RH_TOP_OUT2, arrived(6)),                    # out at the top
          "guest_press=0@4%%m35!?%s=0" % RH_BUSY]
    return dict(
        name="rh", seconds=360, count=2, headless=0, fps=60,
        env=["BB_PARTY_TEST_COND_TIMEOUT=45", "BB_PARTY_TEST_START_LAMP=3502950", "BB_PARTY_PROGRESS_TEST_SET=13501118,%s,13501117" % RH_POS,
             "BB_PARTY_PROGRESS_TEST_CLEAR=%s" % RH_BUSY, "BB_PARTY_EMEVD_TRACE=1",
             "BB_PARTY_EMEVD_TRACE_ALL=13501111,13501112,13501113"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=["--watch", r"object %s stopped at anim [26] now=arrive@0.3,2" % RH_CAR] + RIDE_BURST(RH_CAR, "[15]", "[37]"),
        crash=[], until=RH_UNTIL,
    )


# Phase rs (liftfix diagnosis, ~1.5 min, the host alone, group `research_solo`, 60 FPS): the Research
# Hall lift with nobody else in the session (BB_PARTY_DIRECTOR_TEST_SOLO). Party runs 11 / 12: when the
# host left the car at the bottom the car left upward at once, empty. Alone: the same ride and exit;
# does the car stay down (the departure is the party's) or leave too (the game's or the port's)?
def phase_rs(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_flag=%s/%s/13501117" % (RH_CAR, RH_POS, RH_BUSY)
    host = [common,
            "host_move=%s@4%%m35" % RH_TOP_OUT,                                    # at the top landing
            "host_move=%s@3%%m35" % RH_TOP_IN,                                     # into the car: down
            "host_move=%s@2%%m35?obj=%s:2" % (RH_BOT_OUT, RH_CAR),  # out 2 s after the arrival
            "host_move=%s@6%%m35" % RH_BOT_IN,                                     # 6 s later back in: up
            "host_move=%s@2%%m35?obj=%s:6" % (RH_TOP_OUT, RH_CAR),                  # out at the top
            "host_press=0@4%m35!"]
    return dict(
        name="rs", seconds=200, count=1, headless=0, fps=int(os.environ.get("BB_RS_FPS", "60")),
        env=["BB_PARTY_TEST_COND_TIMEOUT=45", "BB_PARTY_TEST_START_LAMP=3502950", "BB_PARTY_PROGRESS_TEST_SET=13501118,%s,13501117" % RH_POS,
             "BB_PARTY_PROGRESS_TEST_CLEAR=%s" % RH_BUSY, "BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_DIRECTOR_TEST_SOLO=1",
             "BB_PARTY_EMEVD_TRACE_ALL=13501111,13501112,13501113"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host)],
        watch=RIDE_BURST(RH_CAR, "[15]", "[37]"),
        crash=[], until=[r"0:travel step (\d+)/\1: "],
    )


def check_rs(v, L):
    # every ride start (anim 1 down / 5 up playing), not only the ended ones: run 15's up departure
    # started before the down ride's end animation
    starts = []
    for t, l in L.all(0, r"test: object %s anim [15] playing 1" % RH_CAR):
        if not starts or t - starts[-1][0] > 3.0:
            starts.append((t, "up" if " anim 5 " in l else "down"))
    v.step("rs two rides (down, then up with the host): the car stays down after the host left it",
           [d for _t, d in starts] == ["down", "up"],
           "%d ride starts: %s" % (len(starts), ", ".join("%s at +%.1f s" % (d, t) for t, d in starts)))
    f = falls(L, 0, rate=16.0)
    v.step("rs no fall (vertical speed under 16 m/s)", not f, ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "none")
    jump_steps(v, "rs", L, [[dict(t=t, t_end=t + 14) for t, _d in starts]], (-345.0, -792.5), ["down", "up"])
    none_of(v, L, "rs nobody died", r"local death: HP 0", 0, 1e9, (0,))
    check_common(v, L, "rs")


# Phase fl (fallsafe, ~3 min, 2 players, group `fall`): a guest's fall out of the world. At the Great
# Bridge lamp guest 1 walks 3 m onto the bridge (the game's last safe ground spot), then is put just
# under the bridge's floor: it falls into the void to the game's kill plane (about y -1500, ~30 s;
# its fall recovery is off for this, BB_PARTY_FALL_RECOVERY=0). After its held death it must get up at
# a lamp of the loaded map (its own last lamp, or the host's) with no loading screen (never the
# Hunter's Dream) and stay standing there; its bloodstain must be on its last safe floor (user decision:
# the walk back stays the penalty, the stain stays reachable).
FL_UNTIL = r"1:travel step (\d+)/\1: guest_press"
FL_HOST_UNTIL = r"0:travel step (\d+)/\1: host_press"


def phase_fl(only):
    # the host's own fall after the guest's (its god aid only for the guest's part: none here)
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2412952@15",
                     "host_stick=-128.0:61.0:0.7:20@80%m24_01!",   # after the guest is up again
                     "host_press=0@2%m24_01!",
                     "host_move=-131.0:-27.9:58.0@1%m24_01",       # under the floor: out of the world
                     "host_press=0@45%m24_01!", "host_press=0@5%m24_01!"])
    g1 = ",".join(["log_pos", "echoes=4000",
                   "guest_stick=-127.0:62.0:0.7:20@10%m24_01!",   # onto the bridge: the safe ground
                   "guest_press=0@2%m24_01!",
                   "guest_move=-134.6:-27.8:54.7@1%m24_01",       # under the floor: out of the world
                   "guest_press=0@3%m24_01!", "guest_press=0@5%m24_01!"])
    return dict(
        name="fl", seconds=360, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_FALL_RECOVERY=0"], ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", "guest: died from a fall=fallrevive@0.5,3", "--watch", "local death: HP 0=hp0@1,5"],
        crash=[], until=[FL_UNTIL, FL_HOST_UNTIL], until_hold=8,
    )


# Phase fd (fallcam, ~3 min, 2 players, group `fall_deep`): a guest's real fall death far from its lamp
# (a player's report: an 18 m fall beside Djura's tower, up at the host's lamp 158 m away with no load,
# then the camera stuck on the sky and the own chr never shown again). Guest 1's own last lamp is the
# Central Yharnam lamp 2412953 (~95 m from the Great Bridge); it walks 3 m onto the bridge (its last
# safe ground), then is put ~32 m above it and lands: the game's own fall death (its fall-death camera
# latch WorldChrMan +0x68 on, which fl's kill plane never sets). After its held death it gets up at that
# lamp in place: the latch released, the camera behind its chr (within 12 m), drawn where it stands,
# and it walks off the lamp under the player's stick (the camera-relative controls work).
FD_UNTIL = r"1:travel step (\d+)/\1: guest_stick"


def phase_fd(only):
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2412952@15"])
    g1 = ",".join(["log_pos=0.5", "echoes=4000",
                   "guest_lastlamp=2412953@8%m24_01",             # its own last lamp: ~95 m from the bridge
                   "guest_stick=-127.0:62.0:0.7:20@2%m24_01!",    # onto the bridge: the safe ground
                   "guest_press=0@2%m24_01!",
                   "guest_move=-127.0:5.0:62.0@1%m24_01",         # ~32 m above the bridge's floor
                   "guest_press=0@3%m24_01!", "guest_press=0@5%m24_01!",   # up again at the lamp (alive)
                   "guest_stick=-41.0:79.0:0.7:15@2%m24_01!"])    # then off the lamp: ~6 m, camera-relative
    return dict(
        name="fd", seconds=360, count=2, headless=0, pads=(0, 1),
        env=[], ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", "guest: died from a fall=fallrevive@0.5,3", "--watch", r"revive view \(guest=view@0.2,3"],
        crash=[], until=[FD_UNTIL], until_hold=8,
    )


# Phase fdj (fallcam, ~4 min, 2 players, group `fall_deep`): the report's own spot. The host travels to
# the Old Yharnam lamp 2302950 (126.4, -65.2, 36.0); guest 1 is put on the floor beside Djura's tower
# where the player stood (3.7, -53.8, -63.2) and walks 4 m out in one direction after another (-x,
# +x, +z, -z; back on the spot before each) until one leads over the edge into the shaft (the
# player: HP 0 in the air at -72.2 after an 18 m drop, the body at -78.8, the game's fall-death state
# 04..40 / 8800 and its fall-death camera on). It must get up at the host's lamp ~150 m away with no
# load, the fall-death camera released and the camera on its chr.
FDJ_UNTIL = r"1:travel step (\d+)/\1: guest_press"
FDJ_TOP = "3.7:-53.3:-63.2"


def phase_fdj(only):
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2302950@15"])
    g1 = ["log_pos=0.5", "echoes=4000"]
    first = True
    for tx, tz in ((-0.3, -63.2), (7.7, -63.2), (3.7, -59.2), (3.7, -67.2)):
        # back on the floor (alive: after a fall's revive at the lamp too), then 4 m out
        g1 += ["guest_press=0@%d%%m23_00!" % (8 if first else 2), "guest_move=%s@1%%m23_00" % FDJ_TOP,
               "guest_stick=%.1f:%.1f:0.5:8@2%%m23_00!" % (tx, tz), "guest_press=0@3%m23_00!"]
        first = False
    g1 += ["guest_press=0@5%m23_00!"]
    return dict(
        name="fdj", seconds=420, count=2, headless=0, pads=(0, 1),
        env=[], ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=["--watch", "guest: died from a fall=fallrevive@0.5,3", "--watch", r"revive view \(guest=view@0.2,3",
               "--watch", "local death: HP 0=hp0@0.5"],
        crash=[], until=[FDJ_UNTIL], until_hold=8,
    )


# Phase ld (fallcam 3, ~4 min, 2 players, group `ladder`): a death on a ladder (a player's report: "dying
# on a ladder makes it so you can only walk forward"). The party travels to the Cathedral Ward lamp
# 2402950; the host stands at the foot of the ladder 2401024 (its "Ascend Ladder" prompt: dlast), takes
# it with Cross and dies on it: the die step `host_die:ladder` fires only once the chr's ladder state is
# set (it waits up to 15 s, else no death). Later guest 1 does the same. Each is revived in place at the
# lamp (~75 m away, no load); its ladder state must be gone and a stick walk to four points 3 m around
# the lamp (east, north, west, south) must move it toward each one, sideways and backwards too.
LD_LAMP = (17.58, 102.65)
LD_WALK = [(3.0, 0.0), (0.0, 3.0), (-3.0, 0.0), (0.0, -3.0)]   # east, north, west, south of the lamp
LD_UNTIL = [r"0:travel step (\d+)/\1: host_stick", r"1:travel step (\d+)/\1: guest_stick"]


def ld_walks(who):
    return ["%s_stick=%.2f:%.2f:0.5:10@%d%%m24_00!" % (who, LD_LAMP[0] + dx, LD_LAMP[1] + dz, 3 if k == 0 else 1)
            for k, (dx, dz) in enumerate(LD_WALK)]


def ld_climb(who, first):
    return ["%s_obj=2401024:1.0:180@%d%%m24_00" % (who, first),  # the ladder's foot ("Ascend Ladder")
            "%s_press=0x4000@1%%m24_00!" % who,                     # Cross: onto the ladder
            "%s_die:ladder@2%%m24_00" % who,                        # dies on it (only while on it)
            "%s_press=0@3%%m24_00!" % who]                          # up again at the lamp


def phase_ld(only):
    common = "log_pos=0.5,watch_text=Ladder"
    host = ",".join([common, "host_warp=2402950@15"] + ld_climb("host", 10) + ld_walks("host"))
    g1 = ",".join([common, "guest_press=0@10%m24_00!", "guest_press=0@10%m24_00!", "guest_press=0@10%m24_00!",
                   "guest_press=0@10%m24_00!"]                  # the host's turn first
                  + ld_climb("guest", 2) + ld_walks("guest"))
    return dict(
        name="ld", seconds=420, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_B2_REVIVE_DIFF=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"die:ladder: on the ladder=onladder@0.1", "--watch", "local death: HP 0=hp0@0.5",
               "--watch", r"revive view \(=view@0.2,3"],
        crash=[], until=LD_UNTIL, until_hold=4,
    )


# Phase mk (boldmark, ~6 min, 3 players): a guest's Hunter's Mark and Bold Hunter's Mark (a player's
# report: the Bold Mark crashed the host's game right after the guests were back). The party travels to
# the Cathedral Ward lamp 2402950 (m24_00); guest 1 (rendered) has that lamp as its own last lamp and
# answers "Travel with the party?" with alone (BB_PARTY_TRAVEL_CHOICE). (1) the Bold Mark in the host's
# map: the guest's own loading screen, the host stays. Then from the Hunter's Dream (guest 1 goes there
# alone from the lamp: a rest, the host's world reloads and guest 2 follows): (2) the Hunter's Mark,
# (3) the Bold Mark, each a re-join into the host's world (a Mark is no rest); (4) the lamp 2402950
# itself from the Dream (a headstone's travel: a rest the re-join carries, the host's rest load for it
# and guest 2 following: the report's host log). After each everyone is back (the host rewinds its
# member tasks for the arrivals); no crash, no device lost.
# The host's Marks come first (hostmark; the host answers "Travel with the party?" by
# BB_PARTY_SPLIT_TEST, its last lamp the Cathedral Ward's): (h1) the Bold Mark in its own map, YES:
# the in-world party move, no load for anyone, both guests follow beside it; (h2) the Bold Mark
# again, NO: the host alone in-world, the guests stay, no load; (h3) the Hunter's Mark from the
# Dream (the party went there with it), YES: the party's travel, one load each into m24_00, every
# guest placed beside the host. Guest 1's chain waits for the Dream (its first step there) and
# starts again once the party is back.
MK_LAMP = "2402950"    # Cathedral Ward (m24_00)
MK_DREAM = "2102950"   # the Hunter's Dream lamp
MK_UNTIL = r"1:travel step (\d+)/\1: "  # guest 1's last step
MK_CASES = (("Bold Mark in the host's map", "mark_here"), ("Hunter's Mark from the Dream", "mark"),
            ("Bold Mark from the Dream", "mark"), ("lamp from the Dream (a rest)", "rest"))
# the host's cases: (name, goods, the answer, in its own map)
MK_HOST_CASES = (("h1 host Bold Mark in its map, with the party", 1400, "yes", True),
                 ("h2 host Bold Mark in its map, alone", 1400, "no", True),
                 ("h3 host Hunter's Mark from the Dream, with the party", 100, "yes", False))
MK_OFF_LAMP = "13.3:105.1:1.2:25"  # a walk 5 m off the Cathedral Ward lamp's spot (17.6, 102.7)


def _mk_to_dream():
    """Guest 1 alone to the Dream (a rest: the host's world reloads; guest 2 follows), its last lamp the
    Cathedral Ward's again once there (25 s: everyone is back from the host's rest load)."""
    return ["guest_lamp=%s@20%%m24_00" % MK_DREAM, "guest_lastlamp=%s@25%%m21_00" % MK_LAMP]


def phase_mk(only):
    host = ",".join(["god", "host_warp=%s@15" % MK_LAMP,
                     "host_lastlamp=%s@3%%m24_00^2" % MK_LAMP,             # both guests in
                     "host_stick=%s@1%%m24_00!" % MK_OFF_LAMP,             # off the lamp, walked
                     "host_use=1400@2%m24_00",                             # (h1) YES
                     "host_stick=%s@6%%m24_00!" % MK_OFF_LAMP,
                     "host_use=1400@2%m24_00",                             # (h2) NO
                     "host_warp=%s@10%%m24_00" % MK_DREAM,                 # the party to the Dream
                     "host_lastlamp=%s@5%%m21_00^2" % MK_LAMP,
                     "host_use=100@2%m21_00"])                             # (h3) YES: the party back
    g1 = ",".join(["god",
                   "guest_lastlamp=%s@1%%m24_00" % MK_LAMP,               # the party in the Cathedral Ward, then
                   "guest_lastlamp=%s@1%%m21_00" % MK_LAMP,               # in the Dream with the host: its cases done
                   "guest_lastlamp=%s@12%%m24_00" % MK_LAMP,
                   "guest_use=1400@2%m24_00",                     # (1) the Bold Mark in the host's map
                   ] + _mk_to_dream() + ["guest_use=100@2%m21_00"]   # (2) the Hunter's Mark from the Dream
                  + _mk_to_dream() + ["guest_use=1400@2%m21_00"]     # (3) the Bold Mark from the Dream
                  + _mk_to_dream() + ["guest_lamp=%s@2%%m21_00" % MK_LAMP,  # (4) the lamp from the Dream
                                      "guest_press=0@25%m24_00!"])  # back in the host's world: the end
    return dict(
        name="mk", seconds=600, pad=True,
        env=[],
        watch=["--grey", "--watch", r"test: guest_use goods=mark@2,8", "--watch", r"a guest arrived; \d+ member task=back@1,4",
               "--watch", r"host: party move to lamp \d+ inside the loaded map=h1@1,4",
               "--watch", r"lamp \d+ \(host\): .*-> travel alone=h2@1,4",
               "--watch", r"test: host_use goods 100 =h3@30,36"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_SPLIT_TEST=" + ",".join(c[2] for c in MK_HOST_CASES),
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[],
        until=[MK_UNTIL],
    )


# Phase kd (kidnap071, ~4 min, 3 players): the Snatcher kidnap (quest_audit.md item 1, P7). The party
# travels to the Cathedral Ward lamp 2402950 (m24_00). Guest 1 first dies with the kidnap trigger 9420
# set on its own game (what 12400780 would set; a guest's chr type is 1, so the real event never sets
# it there): a normal party death, no Gaol. Then the host's grab-death (9420 set as 12400780 sets it at
# the death, then the kill): common 9421 moves the respawn point to the Hypogean Gaol (2802959, m28)
# and the death must be the PARTY's respawn load there, not the host's revive or a lone load: every
# game in m28_00, the guests beside the host, the waking scene 28000010 on the host and never as a
# live mirror on a guest that is not in m28 (the player report: the host taken alone, the guests
# shown the host's cutscene in the Cathedral Ward).
# kidnapall (0.7.3, "Anyone can be abducted", on by default): before the host's grab, guest 1 is
# grabbed (guest_die:kidnap: what the guest's copy of 12400780 does at the grab, then its death):
# the host's 9420 and the PARTY's travel to the Gaol (one load each), guest 1 follows once its death
# is taken; the waking scene 28000010 then plays on every game (the guests' after their arrival in
# the cell) and every game gets its +1 Insight; guest 1's own save never holds 9420. The host then
# clears 9421 / 9423 (the kidnap undone for the regression), the party goes back to the Ward and the
# host's own grab follows as before (its waking scene on every game too).
KD_LAMP = "2402950"
KD_UNTIL = r"0:travel step (\d+)/\1: "  # the host's last step (awake in the cell, ~20 s after the load)
KD_READ = "9420,9421,9423"


def phase_kd(only):
    host = ",".join(["host_warp=%s@15" % KD_LAMP,
                     # in the cell after guest 1's grab, every scene over: the kidnap undone (test) for the
                     # host's grab next (a flag step keeps the chain armed; a press would wait for a join)
                     "host_flag=9423:0@45%m28_00",
                     "host_flag=9421:0@1",
                     "host_warp=%s@3" % KD_LAMP,          # the party back to the Cathedral Ward
                     "host_flag=9420@30%m24_00",          # the grab-death's trigger
                     "host_die@1%m24_00",
                     "host_press=0@25%m28_00!"])          # awake in the cell: the end
    g1 = ",".join(["guest_flag=9420@12%m24_00", "guest_die@1%m24_00",  # 9420 on a guest: a plain death
                   "guest_die:kidnap@40%m24_00"])                      # a Snatcher's grab of guest 1
    return dict(
        name="kd", seconds=600,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + KD_READ],
        watch=["--watch", r"cutscene 28000010=wake@1,4,8", "--watch", r"remo 2800[01]010 mode=gwake@1,4,8,14",
               "--watch", r"kidnap: a Snatcher grabbed this guest=grab@0.5,3", "--watch",
               r"kidnap: the host's respawn point is the Gaol=gaol@0.5,3"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[],
        until=[KD_UNTIL],
    )


# Phase kw (kidnap071, ~5 min, 3 players): the other scripted warps into another map (quest_audit.md
# "Scripted warps into another map"). (1) The report's broken scene directly: the host plays the Gaol
# waking scene 28000010 (BB_PARTY_STORY_TEST host:) while the party stands elsewhere: no guest may
# mirror it (`dropped: its map is not loaded`). (2) The DLC entry: from the Cathedral Ward the host's
# story warp to the Hunter's Nightmare 3402959 (what 12405263 runs after the Amygdala's grab scene
# 24000000, host only): the party goes (one load each, m34_00). (3) The Yahar'gul Amygdala grab
# 12800431 (no client guard: any player holding the Tonsil Stone): guest 1's story warp to the
# Lecture Building 3202958: the party goes (m32_00). The grab attacks themselves are not scripted
# here (an Amygdala's grab hit is the trigger): the steps run the warps as the events do.
KW_UNTIL = r"1:travel step (\d+)/\1: "  # guest 1's last step (in the Lecture Building)


def phase_kw(only):
    host = ",".join(["god", "host_warp=2402950@25",            # the Cathedral Ward (m24_00)
                     "host_lamp=3402959:story@15",             # the DLC grab's warp
                     "host_warp=2802950@25"])                  # Yahar'gul (m28_00)
    g1 = ",".join(["god", "guest_lamp=3202958:story@15%m28_00",  # the Tonsil Stone grab's warp
                   "guest_press=0@20%m32_00!"])
    return dict(
        name="kw", seconds=480,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_STORY_TEST_DELAY=8"],
        watch=["--watch", r"28000010|dropped: its map=mirror@1,4"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "0:BB_PARTY_STORY_TEST=host:28000010",
              "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[],
        until=[KW_UNTIL],
    )


# Phase cl (chalamp, ~5 min, 2 players): the lamps of a Chalice Dungeon in a party (a tester's report:
# the lamps could be lit but did nothing; a guest's death sent it to its own lamp in the Dream and it
# could not get back in; the host's death left it at a spot it could not leave). The host Continues
# inside the Pthumeru dungeon (fixture chalice_pthumeru_inside: layer 1 boss dead, in its boss room);
# guest 1 (the default save) is summoned in by the quick join. (1) The host lights the boss room's
# lamp (object 2901952, return point 2902952). (2) Guest 1 dies: up again beside the host, no load,
# still in the dungeon. (3) The host dies: up again at the lamp it lit (not the template spot 80 m
# away). (4) The host uses that lamp: "Return to the Hunter's Dream", the party travels (both in
# m21_00). (5) From the Dream, the altar's awaken warp to that lamp (host_lamp, what the altar's
# EMEVD runs): the host enters alone, guest 1 is summoned back into the dungeon (a forced trip: home
# by itself, then the summon). (6) Guest 1 CHOOSES to leave: its lamp to the Dream answered "alone"
# (user decision, option b): it stays out (the notice, no send-home) until (7) Party > Rejoin, which
# takes it home first and the summon brings it into the dungeon again. (8) Guest 1 starts from
# chalice_hintertomb_ritual (its own Hintertomb dungeon in slot 1, a different ChannelId): in the
# host's dungeon its game's current dungeon (FrpgNetMan +0xa88, what its signs and messages carry)
# is the host's, never its own; then it leaves, and the altar's awaken warp takes it into its own
# dungeon (its own ChannelId again). Its save keeps its own slot-1 dungeon (checked by the smoke).
CL_FIXTURE = Path(r"E:\games\bbport\test_saves\chalice_pthumeru_inside\savedata")
CL_DEFAULT = Path(r"E:\games\bbport\test_saves\savedata")
CL_GUEST_FIXTURE = Path(r"E:\games\bbport\test_saves\chalice_hintertomb_ritual\savedata")
CL_HOST_CH = "0x3f22b458bd799"   # the host's Pthumeru dungeon (fixture slot 1)
CL_GUEST_CH = "0xdb68ed64129b"   # guest 1's own Hintertomb dungeon (fixture slot 1)
CL_OWN_ENTRY = "2902950"         # the altar's first awaken point (the current slot's dungeon)
CL_LAMP_OBJ = "2901952"   # the layer 1 boss room's lamp object (its return point 2902952)
CL_LAMP = "2902952"
CL_UNTIL = r"0:travel step (\d+)/\1: "  # the host's last step (back in the dungeon, guest 1 summoned)
CL_UNTIL_G = r"1:travel step (\d+)/\1: "  # guest 1's last step (back in the dungeon after its Rejoin)
CL_DREAM = "2102954"  # the Dream's altar spot a dungeon lamp's "Return to the Hunter's Dream" warps to (slot 0)


def _cl_saves(root: Path, src: Path, inst: int = 0):
    """inst<inst>'s test save (instances.py copies it to savedata_party with --fresh-saves)."""
    dst = root / ("inst%d" % inst) / "user" / "savedata"
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst)


def prep_chalamp(root: Path):
    if not CL_FIXTURE.is_dir():
        sys.exit("chalamp: the fixture %s is missing (docs/party/test_saves.md)" % CL_FIXTURE)
    _cl_saves(root, CL_FIXTURE)
    print("[smoke] chalamp: inst0 starts from %s" % CL_FIXTURE, flush=True)
    if CL_GUEST_FIXTURE.is_dir():
        _cl_saves(root, CL_GUEST_FIXTURE, 1)
        print("[smoke] chalamp: inst1 starts from %s" % CL_GUEST_FIXTURE, flush=True)


def post_chalamp(root: Path):
    _cl_saves(root, CL_DEFAULT)  # the other phases' host starts from the default save again
    _cl_saves(root, CL_DEFAULT, 1)


def _cl_lamp_use():
    """At the lit lamp: Cross (the first press after the stand only brings its prompt up, live cl3 / cl4),
    Cross (the Dream question), Left (YES; it opens on NO), Cross."""
    return ["host_obj=%s:1.2:0@2%%m29" % CL_LAMP_OBJ, "host_press=0x4000@4%m29!", "host_press=0x4000@3%m29!",
            "host_press=0x80@2%m29!", "host_press=0x4000@1%m29!"]


def phase_cl(only):
    host = ",".join(["log_pos", "watch_text=amp/Dream/Hunter",
                     "host_obj=%s:1.2:0@12%%m29" % CL_LAMP_OBJ,    # (1) light the boss room's lamp
                     "host_press=0x4000@2%m29!",
                     "host_press=0@30%m29!",                       # (2) guest 1's death meanwhile
                     "host_move=-60:31.4:-90.8@2%m29",             # off the lamp, then (3) the death
                     "host_die@3%m29",
                     "host_press=0@15%m29!",
                     ] + _cl_lamp_use() + [                        # (4) the lamp: to the Dream
                     "host_lamp=%s@15%%m21" % CL_LAMP,             # (5) the altar's warp back in
                     "host_press=0@10%m29!"])                      # guest 1 summoned back in: the end
    g1 = ",".join(["guest_die@25%m29",
                   "guest_lamp=%s@15#2%%m29" % CL_DREAM,          # (6) back in (join 2): alone to the Dream
                   "guest_rejoin:now@25%m21",                     # (7) stayed out 25 s: Party > Rejoin
                   "guest_press=0@10%m29!",                       # summoned into the dungeon
                   "guest_leave@3%m29",                           # (8) home, then into its own dungeon
                   "guest_lamp=%s@15%%m21&" % CL_OWN_ENTRY,      # "&": out of the party, its own world
                   "guest_press=0@15%m29!&"])                     # in its own dungeon: the end
    watch = []
    for rx, tag in ((r"lamp spot: Chalice Dungeon lamp", "lit"), (r"up again beside the nearest living member", "gup"),
                    (r"host revive #\d+: after", "hrev"), (r"Chalice Dungeon lamp: answered 0", "lamp"),
                    (r"map .* -> m21_00", "dream"), (r"travel step \d+/\d+: host_lamp", "back")):
        watch += ["--watch", "%s=%s@1,4" % (rx, tag)]
    return dict(
        name="cl", seconds=420, count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[],
        until=[CL_UNTIL, CL_UNTIL_G],
    )


# Phase lp (lamprule, ~3 min, 2 players, group `lamps`; user decision 2026-10-21: "host state saves
# lamps, anybody can light them", Guests bring their lamps off by default). Fixture: the host's save
# without Old Yharnam's lamp (2302950: the guest's has it) and both saves without Central Yharnam's
# 2412951; the host starts at the Cleric Beast's lamp (m24_01). The guest joins: its Old Yharnam
# lamp stays out of the host's world (and the host's save); it walks to the unlit 2412951, lights it
# (the host lights it in its world and saves; the guest's own save gets it), and its lamp travel list
# there shows the host's world's lamps only (no 2302950). Then both saves on disk (copies).
LP_OLD = ("2302950", ["12307810", "12307811", "72300200"])   # Old Yharnam: lit in the guest's save only
LP_NEW = ("2412951", ["12417830", "12417831", "72410201"])   # Central Yharnam: lit by the guest in the run
# 0.7.2 Research Hall report: the host's last lamp is the Tomb of Oedon (LP_HOST_LAMP); it stands 8 m
# from the unlit Central Yharnam lamp when the guest joins from its Dream (an A5 cross-map join: the
# arrival must be a lamp lit in the host's world, never the unlit one nearest the host), and dies
# once the guest lit that lamp: it must get up at its own last lamp, not the guest's.
LP_HOST_LAMP = "2412953"
# 1.2 m behind lamp 2412951's object (run 7's spot 8 m off dropped the host 10 m into a niche); the
# host beside the lamp a guest lights must still keep its own last lamp
LP_HOST_NEAR = "-196.8:-28.6:64.2"
LP_UNTIL = [r"1:test: listonly: warp menu closed", r"0:host death taken: .*revive at lamp",
            r"0:save requested \(.*a lamp a guest lit in the host's world",
            r"1:written into the own save \(shared lamps; lamps.* %s[\s\S]*save requested \(.*shared lamps from "
            r"the host's world" % LP_NEW[0]]


def phase_lp(only):
    read = ",".join(LP_OLD[1] + LP_NEW[1])
    return dict(
        name="lp", seconds=300, count=3, headless=1,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + read, "BB_PARTY_GUESTS_BRING_LAMPS=0", "BB_PARTY_SHARE_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + LP_HOST_LAMP, "0:BB_PARTY_DIRECTOR_TEST_SOLO=1",
              "0:BB_PARTY_TEST_INVINCIBLE=1",
              "0:BB_PARTY_DIRECTOR_TEST=" + ",".join([
                  "host_move=%s@3%%m24_01" % LP_HOST_NEAR,          # beside the unlit lamp, before the join
                  "host_die@6%%m24_01?%s=1" % LP_NEW[1][2]]),       # once the guest lit it
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LP_OLD[1] + LP_NEW[1]),
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LP_NEW[1]),
              "1:BB_LAMP_TRAVEL_TEST=members,lamp=%s,listonly" % LP_NEW[0],
              # guest 2 (headless): Continue into its own world, then the A5 cross-map join
              "2:BB_PARTY_TITLE_JOIN=0", "2:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LP_NEW[1])],
        watch=["--watch", r"lamps: the own save's lamps the host's world lacks stay out=lpjoin@3",
               "--watch", r"lamp: slot \d+ lit a lamp in the host's world=lplit@1,5",
               "--watch", r"test: warp menu up \(listonly\)=lplist@1"],
        start_when=[r"1/0:travel step \d+/\d+: host_move", r"2/0:travel step \d+/\d+: host_move"],
        crash=[], until=LP_UNTIL, until_hold=8,
    )


# Phase bf (wipeboss, ~3 min, 3 players, group `fight_load`): loads into the host's world while the
# host's boss fight goes on. The host walks into Father Gascoigne's first-fight region (intro, then
# the fight; invincible, it fights on). Guest 1 at the Tomb of Oedon lamp uses a Hunter's Mark (an
# alone trip to its last lamp, the same lamp: a re-join with the host's snapshot, no rest) - the
# only load path into a running fight (a rest resets it, a summon waits for it, a death in it is a
# limbo); after it the fight must still run for it (its host-entered flag 1, its own guest flag 0,
# the host's fight not reset), and its walk through the fog must join the fight. Guest 2 dies
# outside the fog meanwhile: with the fight on it lies in its limbo (no load of its own).
BF_UNTIL = [r"1:guest_use goods 100[\s\S]*went through boss fog", r"0:boss fog: slot 1 went in",
            r"2:guest: dead in a boss fight|travels alone \(death\)"]


def phase_bf(only):
    host = ",".join(["host_warp=2412953@8^2",                               # the party to the Tomb of Oedon
                     "host_move=%s@3%%m24_01" % GASCOIGNE_TRIGGER])         # the host starts the fight (intro)
    g1 = ",".join(["guest_lastlamp=2412953@4%m24_01~",                      # 4 s after the intro: its last lamp here
                   "guest_use=100@3%m24_01",                                # Hunter's Mark: the re-join load
                   "guest_press=0x0@5%m24_01!",                             # back, alive (HP logged)
                   "guest_obj=2411810@3%m24_01",                            # at the fog
                   "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!"])  # through it
    g2 = "guest_die@8%m24_01~"                                               # outside the fog, the fight on
    return dict(
        name="bf", seconds=300, until_hold=8,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[],
        until=BF_UNTIL,
    )



# Phase rt (rotrules, 0.7.2, ~6 min, 2 players, group `rot`): rot on (BB_PARTY_ROT=1) and what else
# cleanses it (party_rot_rules.h). Guest 1 is frail (BB_PARTY_ROT_TEST_FRAIL: any hit leaves 1 HP, so
# the next enemy hit kills it: a death with a killer, the game's own death path) and picks its stains
# up by itself (BB_PARTY_STAIN_TEST). BB_PARTY_TEST_GUARD: from each revive until its next die / chr step
# the guest is invincible (rel2 0.7.2: revived onto its stain, 2410202 killed it a second time before
# the host's kill step; every later death was one off).
#   1  echoes rule off: killed by one of the enemies 2410247 / 2410202 (Central Yharnam, m24_01; run 1:
#      2410202 struck first), its stain picked up (no change), up at RT_SIGNAL; the host kills both
#      (the kill record relays them): -1 "your killer is dead"
#   2  echoes rule on: a scripted death (no killer), its bloodstain picked up: -1 "Blood Echoes retrieved"
#   3  killer and echoes rules off (host_rule, live): killed at 2410261's group, which it then kills
#      itself, its stain picked up: no change
#   4  a scripted death, its stain picked up: no change (2+ stacks)
#   5  the host kills Father Gascoigne: 0 "Rot cleansed: boss defeated"
#   6  boss rule off (10 s after the kill: its defeat events come ~3 s after the step); a scripted
#      death; the party travels to the Cleric Beast, the host kills it: no change
# The host waits for the guest by its position (`?near=`), the guest for the boss by its defeat flag.
RT_LAMP = "-36.8:-40.75:84.3"          # the Tomb of Oedon lamp's revive spot (2412953)
RT_SIGNAL = "-41.5:-40.5:79.5"         # 6 m off it (the host's walk in s3): the guest is ready
RT_ENEMIES = "-157.7:-41.7:16.5:14"    # m24_01's enemy group 130 m away (2410261 stands there)
RT_UNTIL = r"1:rot: a boss died .*nothing off \(rot on; cleanse boss off"


def _rt_killed_by(entity):
    """Guest 1 at an enemy, R1 to draw it, ~20 s for its two hits (frail), then up at the lamp."""
    return (["guest_chr=%s@3%%m24_01" % entity] + ["guest_press=0x800@1.5%m24_01!"] * 3 +
            ["guest_press=0@8%m24_01", "guest_press=0@8%m24_01"])


def phase_rt(only):
    host = ",".join([
        "god",
        "host_warp=2412953@20",                                     # the Tomb of Oedon lamp (party travel)
        # 1's neighbours 2410249 / 2410250 off first (full smoke 0.7.2: they killed the frail guest instead
        # of 2410247 / 2410202, then again at each stain the stain test warped it back onto: a death loop)
        "host_kill=2410249@3%m24_01", "host_kill=2410250@1%m24_01",
        "host_press=0@8%m24_01!", "host_press=0@8%m24_01!",         # the guest leaves the lamp
        "host_kill=2410247@3%%m24_01?near=%s:2" % RT_SIGNAL,          # 1: its killer, once it is up again
        "host_kill=2410202@1%m24_01",
        "host_rule=party_rot_cleanse_echoes:1@2%m24_01",              # 2
        "host_rule=party_rot_cleanse_killer:0@1%%m24_01?near=%s" % RT_ENEMIES,  # 3: at its next enemy
        "host_rule=party_rot_cleanse_echoes:0@1%m24_01",
        "host_move=%s@2%%m24_01?near=%s:2" % (S3_TRIGGER, RT_SIGNAL),  # 5: Gascoigne's fight
        "host_kill=2410810@6%m24_01~",                                # after its intro
        "host_rule=party_rot_cleanse_boss:0@10%m24_01",              # 6
        "host_warp=2412952@2%%m24_01?near=%s:2" % RT_LAMP,             # the guest up from its death
        "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,              # the Cleric Beast's fight
        "host_kill=2410800@10%m24_01",
        "host_press=0@8%m24_01",
    ])
    g1 = ",".join(
        _rt_killed_by(2410247) +                                      # 1
        ["guest_move=%s@3%%m24_01" % RT_SIGNAL,                       # up again: the host kills its killer
         "guest_press=0@8%m24_01!",
         "guest_die@6%m24_01",                                        # 2: no killer, a stain
         "guest_press=0@8%m24_01!", "guest_press=0@8%m24_01!"] +      # its stain picked up
        _rt_killed_by(2410261) +                                      # 3 (rules off by then)
        ["guest_kill=2410261@4%m24_01", "guest_kill=2410260@1%m24_01",  # it kills its killer itself
         "guest_die@10%m24_01",                                       # 4 (its stain 3 picked up first)
         "guest_press=0@8%m24_01!",
         "guest_move=%s@6%%m24_01" % RT_SIGNAL,                       # ready: the host's boss fight
         "guest_die@14%m24_01?12411800=1",                            # 6: after Gascoigne's defeat
         "guest_move=%s@3%%m24_01" % RT_LAMP])                        # up: the stain test put it on its stain
                                                                      # (RT_SIGNAL); the host waits at the lamp
    return dict(
        name="rt", seconds=600, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR + "," + CLERIC_CLEAR, "BB_PARTY_ROT=1",
             "BB_PARTY_ROT_CLEANSE_BOSS=1", "BB_PARTY_ROT_CLEANSE_KILLER=1", "BB_PARTY_ROT_CLEANSE_ECHOES=0"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_ROT_TEST_FRAIL=1", "1:BB_PARTY_TEST_GUARD=1", "1:BB_PARTY_STAIN_TEST=2", "1:BB_PARTY_ROT_KIND=0,1,3,0,1,3"],
        watch=["--watch", "notice: Rot=rotnotice@0.5,2", "--watch", "rot: .*notice: Rot=rotclean@0.5,2",
               "--watch", "rot: .*nothing off=rotkept@0.5"],
        crash=[], until=[RT_UNTIL], until_hold=4,
    )


# Phase br (laurence, ~6 min, 2 players, group `boss_rooms`): boss rooms open until the real fight
# start, and the co-op wall behind Ludwig (party_bossfog_rules.h FirstFights / ConfinementWalls;
# player reports 2026-10-22). Default test save (owns the DLC; Laurence and Ludwig alive; their
# fixture flags cleared). The party travels to the Nightmare Grand Cathedral lamp 3402953, which
# stands in Laurence's altar region 3402855: without Laurence's Skull nothing starts. The guest walks
# out through his fog 3401850 and back in (a real walk by the stick: a closed fog blocks it), then
# stands inside (BR_SIGNAL, the scene's own warp spot 3402856); the host is given the skull there:
# the first fight starts (scene 34000010, the bar on both), the host kills Laurence. The party
# travels to Ludwig's lamp 3402952, the host walks into his first-fight region (scene 34000020) and
# kills him; then host and guest each walk through the wall 3401801 at the back of his arena and
# back (13400010: it stayed up after his death with a guest in the world).
BR_LAURENCE_CLEAR = "13401850,13401851,13401853,13404850,13404851,13404852,13404858,13404859,13404860,13404861"
BR_LUDWIG_CLEAR = "13401800,13401801,13404800,13404801,13404802,13404808,13404809,13404810,13404811,9471,13400010"
BR_SIGNAL = "-434.55:1535.71:-264.25"   # region 3402856 (the scene's warp spot, inside the cathedral)
BR_LUDWIG = "-407.93:1503.5:-721.36"     # region 3402805 origin + 0.5 m (Ludwig's first fight)
BR_WALK = "1.0:150"                      # arrive within 1 m, up to 150 stick steps
BR_UNTIL = [r"1:travel step (\d+)/\1: guest_stick[\s\S]*approach: (arrived|BLOCKED|GAVE UP)",
            r"0:travel step (\d+)/\1: host_stick[\s\S]*approach: (arrived|BLOCKED|GAVE UP)"]


def phase_br(only):
    # The reward item box after each kill holds the stick (run 1: both screens showed Beast's Embrace
    # + Guidance with an OK row through the walks): Cross twice closes it. R3 (0x4) after each
    # `_obj` step turns the camera behind the player, so the walk's screenshots face the doorway.
    # The steps at Ludwig's back wall carry no map condition: it stands on the m34 / m35 border, and
    # the walk itself changes the map (run 1: the host's next step waited for m34_00 for good).
    host = ",".join([
        "god", "goods=4014:0",                                       # no Laurence's Skull yet
        "host_warp=3402953@8^1",                                     # the party to the cathedral lamp
        "host_use=4014@2%%m34_00?near=%s:2" % BR_SIGNAL,              # the guest walked out and in: the skull
        "host_kill=3400850@8%m34_00~",                               # Laurence, 8 s after his scene
        "host_press=0x4000@8%m34_00!", "host_press=0x4000@2%m34_00!",  # OK on the reward box
        "host_warp=3402952@6%m34_00",                                # the party to Ludwig's lamp
        "host_move=%s@8%%m34_00" % BR_LUDWIG,                         # his first fight (two parts, scene)
        "host_kill=3400800@8%m34_00~",
        "host_press=0x4000@8!", "host_press=0x4000@2!",              # OK on the reward box
        "host_obj=3401801@6!",                                       # the wall behind Ludwig's arena
        "host_press=0x4@1!", "host_press=0@1!",                      # R3: the camera behind the player
        "host_stick=obj:3401801:4:180:%s@2!" % BR_WALK,              # through it
        "host_stick=obj:3401801:4:0:%s@2!" % BR_WALK,                # and back
    ])
    g1 = ",".join([
        "god",
        "guest_obj=3401850@10%m34_00",                               # at Laurence's fog (inside, by the lamp)
        "guest_press=0x4@1%m34_00!", "guest_press=0@1%m34_00!",       # R3
        "guest_stick=obj:3401850:4:180:%s@2%%m34_00" % BR_WALK,       # through it
        "guest_stick=obj:3401850:4:0:%s@2%%m34_00" % BR_WALK,         # and back
        "guest_move=%s@2%%m34_00" % BR_SIGNAL,                        # inside: the host's skull now
        "guest_press=0x4000@10%m34_00?13401850=1", "guest_press=0x4000@2%m34_00!",  # Laurence's reward box
        "guest_press=0x4000@10?9471=1", "guest_press=0x4000@2!",      # Ludwig's
        "guest_obj=3401801@4!",                                      # at the wall behind him
        "guest_press=0x4@1!", "guest_press=0@1!",                    # R3
        "guest_stick=obj:3401801:4:180:%s@2!" % BR_WALK,
        "guest_stick=obj:3401801:4:0:%s@2!" % BR_WALK,
    ])
    return dict(
        name="br", seconds=720, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LAURENCE_CLEAR + "," + BR_LUDWIG_CLEAR],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: (?:host|guest)_stick: object 340(?:1850|1801) found=brdoor@0.5,4"],
        crash=[], until=BR_UNTIL, until_hold=6,
    )


# Phase sm (cwscene, ~5 min, 2 players, group `scene_mirror`): the host's cutscene plays live on a
# guest standing beside it (player report 2026-10-22).
#   1  Father Gascoigne's intro race: the guest stands where the player's guest stood (-51.7, 69.1),
#      the host walks into the trigger region 2412815 4-5 m from it; both games run event 12411802
#      at once: the guest's own copy of the intro is held and the host's copy plays there live
#      (within 1 s of the host's), never a second time. The host then kills Gascoigne.
#   2  The door from the Tomb of Oedon into Oedon Chapel (door 2401210, m24_00, event 12400750,
#      scene 24000020 with the evening): the host opens it from the m24_01 side, the guest 3 m behind
#      it, still in m24_01; the mirror plays there within 1 s (the scene's block m24_00 is loaded /
#      the host is near), and the guest's walk into m24_00 afterwards plays no catch-up replay.
SM_DOOR = (30.04, -9.2, 95.57)          # MSB part 2401210 (yaw -60): its back side is m24_01
SM_GUEST_RACE = "-51.7:-40.8:69.1"      # the player's guest at the intro (cwscene.log 1868)
SM_UNTIL = r"1:map m24_01_00_00 -> m24_00_00_00"


def phase_sm(only):
    door = "%.2f:%.2f:%.2f" % SM_DOOR
    host = ",".join([
        "god",
        "host_warp=2412953@8^1",                                        # the party at the Tomb of Oedon
        "host_move=%s@3%%m24_01?near=%s:1.5" % (GASCOIGNE_TRIGGER, SM_GUEST_RACE),  # 1: the intro, guest 4.6 m off
        "host_kill=2410810@6%m24_01~",                                   # Gascoigne dead after it
        "host_move=33.2:-10.0:93.7:pin=3@10%m24_01",                     # 2: up at the door (m24_01 side)
        "host_obj=2401210:1.2:180@4%m24_01",
        "host_press=0x4000@4%m24_01!",                                   # OK on Gascoigne's key notice (run 1)
        "host_press=0x4000@3%m24_01!",                                   # "Open Door": scene 24000020
    ])
    g1 = ",".join([
        "god",
        "guest_move=%s@10%%m24_01?near=%s:4" % (SM_GUEST_RACE, RT_LAMP),  # the party at the lamp: beside the trigger
        "guest_obj=2401210:4.2:180@1%%m24_01?near=%s:2" % door,           # 3 m behind the host at the door
        "guest_move=27.4:-9.2:97.1@4%m24_01~",                           # after the scene: through the door (m24_00)
    ])
    return dict(
        name="sm", seconds=480, count=2, headless=0,
        # Only the host skips its scenes (5 s): the guest's live mirror must end with the host's
        # skip (the player-facing rule), not by a skip of its own.
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR + ",12400750,9800,9801,9802",
             "BB_PARTY_TEST_INVINCIBLE=1", "BB_TEST_REMO_SKIP_AFTER=0"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "0:BB_TEST_REMO_SKIP_AFTER=5"],
        watch=["--watch", r"remo playing=smremo@0.5,1.5,4", "--watch", r"cutscene 24000020=smdoor@0.5,1.5,4",
               "--watch", r"map m24_01_00_00 -> m24_00_00_00=smwalk@2,8"],
        crash=[], until=[SM_UNTIL], until_hold=15,
    )


# Phase da (dreamstone, ~3 min, 2 players, group `dream_apart`; a player's report: when the host
# left the Hunter's Dream alone the guest could not use any headstone and was stuck there). The host
# starts in its Dream (start lamp 2102950), the guest is summoned in; the host uses headstone t210000
# and travels alone (1st Floor Sickroom). The guest's game runs its leave state (the load-wait /
# action-button hold), split travel drops its send-home, and the hold must be lifted; 10 s after the
# host is in another area the guest uses headstone t210000 itself, travels alone and arrives (the
# same lamp as the host's: a re-join into the host's area).
# (after the host's alone travel: the guest's start-of-run re-join into the Dream is a "travels alone (lamp)")
# Also ends on a failure: the fixture's FAILED (no prompt / no menu), a travel refused or closed.
DA_UNTIL = [r"1:the host travels alone[\s\S]*(guest: travels alone \(headstone\)[\s\S]*(arrived in the host world|map - -> m2[2-9])"
            r"|test: levelup: FAILED|travel alone \(headstone\) refused|travelling alone is not possible|nothing runs"
            r"|the question is closed|no party request possible)"]


def phase_da(only):
    return dict(
        name="da", seconds=240, count=2, headless=0,
        env=["BB_PARTY_GUEST_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_GUESTSVC_TEST=hostheadstone;hsdelay=12;keys=cross,cross,left,cross,cross",
              "0:BB_PARTY_TRAVEL_CHOICE=alone", "1:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_GUESTSVC_TEST=guestheadstone;needprompt;hsapart;hsdelay=10;keys=cross,cross,cross,cross",
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"test: levelup: before Cross=daprompt@0.2,1",
               "--watch", r"test: levelup: no talk list: the menu was requested=dahs@0.5,2",
               "--watch", r"the host travels alone=dahost@1,8",
               # 0.7.3: the host's screen while the guest leaves the Dream alone (no loading screen there)
               "--watch", r"guest: travels alone \(headstone\)=daleave@1,4,8"],
        crash=[], until=DA_UNTIL, until_hold=10,
    )


# Phase nf (npcfights, ~5 min, 2 players, group `npc_fights`; ERSC parity D11): scripted NPC
# hunter fights with the whole party (user decision 2026-10-22: the NPC is there for everyone,
# anyone can hurt it, its death and the quest step count for all, like a boss).
#   1  Tomb of Oedon (m24_01), Eileen's quest: her state 1363 (she waits there, hidden) with Henryk
#      (2410901) on the graves. The host stands at Henryk (his fight brings Eileen in as the
#      player's ally: 12410811, team 28, quest state 1365); the guest in front of him hits him (R1:
#      his HP falls on the host's game) and deals the killing blow on its own game (guest_kill: HP 0
#      there only); the host's game sees him die and sets the quest flags (12410804: 1367;
#      12410810's own flag), which reach the guest's own save (read at the next load).
#   2  Cainhurst (m25, its first lamp 2502950; Logarius dead), Alfred hostile (state
#      1349): his init event (12500050) re-teams him (27) on the host's game only (1003[6] "end if
#      client"). A: the host's rule party_npc_fights OFF: the guest's copy keeps him friendly (26,
#      the "before"). B: the rule ON and a party round trip to the Tomb and back: the guest's copy
#      runs as the host's, team 27 on both games. Read from the chr dumps (400 m): the party stays at
#      the lamp, about 290 m from him (the Queen's Chamber lamp 2502951 put both players into an
#      undrawn void in this save, Alfred falling through it, so there is no fight with him here).
NF_HENRYK, NF_EILEEN, NF_ALFRED = 2410901, 2410900, 2500790
NF_FLAGS = "1349/1363/1365/1366/1367/12410810"
NF_LOGARIUS_DEAD = "12501800,12501802,12501803"  # Logarius dead: the queen's chamber (Alfred) is open
# the host's last step (its load after the Cainhurst checks) and its world up
NF_UNTIL = r"0:test: travel step 8/8: host_warp[\s\S]*test read \(load mode 0"


def phase_nf(only):
    host = ",".join([
        "god", "log_flag=" + NF_FLAGS,
        "host_warp=2412953@8^1",                          # 1: the party at the Tomb of Oedon lamp
        "host_chr=%d:4@8%%m24_01" % NF_HENRYK,            # in front of Henryk: his fight starts
        "host_rule=party_npc_fights:0@2%m24_01",          # 2A: the rule off (the "before")
        "host_warp=2502950@8%m24_01?12410810=1",          # Henryk dead: the party to Cainhurst
        "host_rule=party_npc_fights:1@15%m25",            # 2B: the rule on
        "host_warp=2412953@2%m25",                        # a round trip: the guest's init runs again
        "host_warp=2502950@8%m24_01",
        "host_warp=2412953@15%m25",                       # the last load
    ])
    g1 = ",".join(["god", "log_flag=" + NF_FLAGS,
                   "guest_chr=%d:1.6@14%%m24_01" % NF_HENRYK,        # 1: in front of Henryk
                   "guest_press=0x4@1%m24_01!", "guest_press=0x4:lock@1%m24_01!"]
                  + ["guest_press=0x800@1.2%m24_01!"] * 8          # R1: the guest's hits
                  + ["guest_kill=%d:hp@2%%m24_01" % NF_HENRYK])     # its killing blow (HP 0 on its game)
    clear = ("1340,1341,1342,1343,1344,1345,1346,1347,1348,1350,1351,1352,1353,1354,1355,1356,1357,1358,1359,"
             "1360,1361,1362,1364,1365,1366,1367,1368,1369,1700,1701,1702,1703,1704,1705,12410810,5912")
    return dict(
        name="nf", seconds=480, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=1363,1349," + NF_LOGARIUS_DEAD,
             "BB_PARTY_PROGRESS_TEST_READ=1349,1363,1365,1367,12410810", "BB_PARTY_CHR_DUMP=400",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: host_chr=nfhost@1,4", "--watch", r"test: guest_chr=nfguest@1,4",
               "--watch", r"guest_press 0x800=nfhit@0.5", "--watch", r"test: guest_kill=nfkill@1,4",
               "--watch", r"guest copy of event 12500050 runs=nfrule@1,6"],
        crash=[], until=[NF_UNTIL], until_hold=12,
    )


# Phase cv (covenants, 0.7.3, ~3 min, 2 players, groups `covenant` / `valtr_rune`; user decision
# 2026-10-22: covenant outcomes are per player). The host's save: Alfred killed Queen Annalise (1042
# the flesh, Alfred 1348 in the castle, 72500328 her revival flag) and Valtr is dead (1790). The
# guest's save: Annalise on her throne (1040), Alfred at the chapel (1344), Valtr alive (1800), and
# the Cleric Beast not killed (12411700: the shared-progress control, the host's kill must reach it).
# After the join the guest's game sets Valtr's rune trigger 72700321 as his League talk does (the
# talk itself is not driven), then leaves (Party > Leave): at home its own world's values are logged
# (BB_PARTY_PROGRESS_TEST_READ) and both saves are read from copies after the run.
CV_HOST_SET = ["1042", "1348", "1790", "72500328"]
CV_GUEST_SET = ["1040", "1344", "1800"]
CV_CONTROL = "12411700"
CV_RUNE_TRIGGER, CV_RUNE_LOT, CV_RUNE_FLAG = "72700321", "43120", "6676"
CV_READ = CV_HOST_SET + CV_GUEST_SET + [CV_CONTROL, CV_RUNE_FLAG]
CV_UNTIL = [r"1:guest_leave: Party > Leave pressed[\s\S]*test read \(load mode 0"]


def phase_cv(only):
    return dict(
        name="cv", seconds=240, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(CV_READ), "BB_PARTY_TEST_COND_TIMEOUT=90"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_PROGRESS_TEST_SET=" + ",".join(CV_HOST_SET),
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(CV_GUEST_SET + [CV_RUNE_FLAG]),
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join([
                  "god",
                  "guest_flag=%s@8" % CV_RUNE_TRIGGER,   # what joining the League sets (Valtr's talk)
                  "guest_leave@12"]),                    # home: the own world's values, the rune at home
              "1:BB_PARTY_PROGRESS_TEST_SET=" + ",".join(CV_GUEST_SET),
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(CV_HOST_SET + [CV_CONTROL, CV_RUNE_FLAG])],
        watch=["--watch", r"covenant outcomes per player: \d+ covenant flag=cvjoin@2,6",
               "--watch", r"own talk gift trigger %s=cvrune@1,4" % CV_RUNE_TRIGGER,
               "--watch", r"guest_leave: Party > Leave pressed=cvleave@4,10"],
        crash=[], until=CV_UNTIL, until_hold=8,
    )


# Phase ie (itemsevery, 0.7.3, ~3 min, 2 players, group `items_every`; a player's report: starting
# weapons, Iosefka's vial and the Old Hunter Bell reached one player only; docs/party/items.md 10.8).
#   Bell: the host's 6670 is cleared at its first world, so the join's full list has no lot 10050
#   (a guest takes each flag's grant once per session: a full-list skip would also drop the later
#   award, run r5). The guest's bell (goods 4312) is taken away 3 s into its first world and its flag
#   6670 cleared after that (guest_flag, the personal block: its own pool); the host's test award of
#   lot 10050 (BB_PARTY_ITEMS_HOST_TEST, 20 s after the join; the host's own bell taken away too, its
#   fit check) gives the host its bell and reaches the guest: the bell's item box on both.
#   Messengers: the host's own picks cleared (12101020 + its stock flags 72101000-72101002) at its
#   first world; two lamp warps reload the Dream, so event 12101020 runs again with them cleared; the
#   host stands 1.5 m from messenger 2100215 (they rise, 12105050). The guest still has its picks: its
#   copy of the event ends, and before 0.7.3 that ended the host's copy too (0x31, run r4: the host's
#   12101020 set 2.8 s after the rise with none of its picks). 12101020 must stay 0 on the host.
IE_UNTIL = [r"0:test: travel step (\d+)/\1: [\s\S]{3000}", r"1:lot 10050 "]


def phase_ie(only):
    host = ",".join(["god", "goods=4312:0", "log_flag=12101020/12105050/72101000/72101001/72101002",
                     "watch_text=Saw Cleaver/Hunter Axe",
                     "host_warp=2412951@35^1", "host_warp=2102961@10",
                     "host_chr=2100215:1.5@6%m21_00"])
    g1 = ",".join(["god", "goods=4312:0", "log_flag=6670/12101020", "guest_flag=6670:0@12"])
    return dict(
        name="ie", seconds=300, count=2, headless=0,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_PROGRESS_TEST_CLEAR=12101020,72101000,72101001,72101002,6670",
              "0:BB_PARTY_ITEMS_HOST_TEST=10050",
              "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_PROGRESS_TEST_READ=6670,12101020,12101021",
              "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"gave #\d+ lot 10050=iebell@1,3",
               "--watch", r"flags 12101020=0 12105050=1=ierise@1,3,6",
               "--watch", r"text shows=ietext@0.3",
               "--watch", r"flags 12101020=1=iedone@0.5,3"],
        crash=[], until=IE_UNTIL, until_hold=15,
    )


# Phase nfa (npcfights Alfred, ~5 min, 2 players, groups `npc_fights` / `npc_alfred`; D11 live proof):
# Alfred in the Queen's Chamber (m25), friendly at the start (his state 1348 in both saves), the
# guest standing next to him when he turns hostile (an in-map transition, 12500053), then the guest's
# hits and killing blow.
#   The fixture: Logarius dead (12501800/02/03), the castle's display event 12500810 done (its map
#   parts 2506000-2506073: the Queen's Chamber; without it they stay disabled and the chamber lamp
#   2502951 is an undrawn void: nf runs 7/9), the arrival scene 12500000 seen (no carriage remo).
#   1  The party at the chamber lamp; the host 1.8 m in front of Alfred, the guest 2 m beside him.
#   2  The host's R1s once the guest stands at Alfred (?near): his counter 12500052 (3 hits by the
#      local player) -> 12500053 on the host: team 27, 1349. The guest's copy of 12500053 (its
#      1003[6] passed by the rule) fires with the host's flag (run 1: 0.9 s later) and switches him
#      to team 23 on its game (27 is no foe for a phantom: party_npcfight_rules.h).
#   3  The guest in front of him locks on, R1s (his HP falls on both games), HP 0 on its game
#      (guest_kill :hp); the host's 12500054 sees him die: 1350 on the host. Alfred's states
#      1340-1359 are covenant outcomes (per player, 0.7.3): the guest's own save keeps its 1348.
#   4  The host to the Tomb (the guest's next load reads its own save).
NFA_POS = "127:130:-463.8"  # Alfred in the Queen's Chamber (nf run 7, before he fell through the void)
NFA_FLAGS = "1348/1349/1350/1351/12500053/12500054"
NFA_FIXTURE_SET = "1348,12501800,12501802,12501803,12500810,12500000"
# the guest's killing blow, then its next load (the host's warp to the Tomb after 1350, or the
# condition's timeout)
NFA_UNTIL = r"1:test: guest_kill[\s\S]*test read \(load mode 1"


def phase_nfa(only):
    host = ",".join([
        "god", "log_flag=" + NFA_FLAGS,
        "host_warp=2502951@8^1",                              # 1: the party at the Queen's Chamber lamp
        "host_chr=%d:1.8@10%%m25^1" % NF_ALFRED,              # in front of Alfred (the guest back in)
        # 2: the host's R1s once the guest stands at Alfred
        "host_press=0x800@3%%m25!?near=%s:4" % NFA_POS]
        + ["host_press=0x800@1.2%m25!"] * 2
        + ["host_warp=2412953@10%m25?1350=1"])                 # 4: Alfred dead: the guest's next load
    g1 = ",".join(["god", "log_flag=" + NFA_FLAGS,
                   "guest_chr=%d:2:90@12%%m25" % NF_ALFRED,     # 1: beside Alfred (friendly)
                   "guest_press=0x4:lock@1%m25!",              # no lock-on on a friendly chr (the "before")
                   "guest_chr=%d:1.6@14%%m25" % NF_ALFRED]      # 3: in front of him after the switch
                  + ["guest_press=0x4:lock@1%m25!"] * 3
                  + ["guest_press=0x800@1.5%m25!"] * 8
                  + ["guest_kill=%d:hp@2%%m25" % NF_ALFRED])
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    return dict(
        name="nfa", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_PROGRESS_TEST_READ=1348,1349,1350,12500810", "BB_PARTY_CHR_DUMP=40",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: host_chr %d=nfaarrive@0.5,3" % NF_ALFRED,
               "--watch", r"test: host_press 0x800=nfaprovoke@0.6",
               "--watch", r"test: guest_press 0x800=nfahit@0.5",
               "--watch", r"test: guest_kill: =nfakill@1,5"],
        crash=[], until=[NFA_UNTIL], until_hold=8,
    )


# Phase eh (echodup, ~5 min, 2 players, group `echo_hold`): Blood Echoes an enemy took are paid once
# (player report for 0.7.3: 15k from the glowing-eyed enemy and 15k again from the bloodstain the
# marker pointed to). Guest 1 (15000 echoes, frail) is killed by 2410247 beside it and gets up at
# the lamp; the game gives its stain to that enemy (glowing eyes, the party stain hidden). Its walk
# onto the stain spot then pays nothing; its kill of the enemy pays the echoes once and removes the
# party stain; a walk to the enemy's body (the stain spot) pays nothing again.
EH_UNTIL = r"1:travel step (\d+)/\1: guest_move"


def phase_eh(only):
    host = ",".join([
        "god",
        "host_warp=2412953@20",                                     # the Tomb of Oedon lamp (party travel)
        # its neighbours (rt: a death loop; eh1: 2410202 killed the guest instead and took the echoes)
        "host_kill=2410249@3%m24_01", "host_kill=2410250@1%m24_01", "host_kill=2410202@1%m24_01",
    ] + ["host_press=0@20%m24_01!"] * 12)
    g1 = ",".join(
        ["echoes=15000"] + _rt_killed_by(2410247) +                  # killed beside it: the stain there
        ["guest_move=%s@3%%m24_01" % RT_LAMP,                        # up at the lamp (invincible from here)
         "guest_press=0@20%m24_01!",                                 # the enemy takes the echoes
         "guest_chr=2410247@2%m24_01",                               # back beside it (the lamp is 142 m off)
         "guest_stick=stain:0.6:20@2%m24_01!", "guest_press=0x4000@2%m24_01!", "guest_press=0@6%m24_01!",
         "guest_kill=2410247@2%m24_01", "guest_press=0@10%m24_01!",  # its kill: the echoes once
         "guest_chr=2410247@2%m24_01", "guest_press=0x4000@2%m24_01!", "guest_press=0@6%m24_01!",
         "guest_move=%s@4%%m24_01" % RT_LAMP])
    return dict(
        name="eh", seconds=420, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_ROT_TEST_FRAIL=1", "1:BB_PARTY_TEST_GUARD=1", "1:BB_PARTY_TEST_INVINCIBLE=1",
              "1:BB_PARTY_STAIN_MARKER=1"],
        watch=["--watch", "party stain: an enemy took=ehheld@1,1", "--watch", "party stain: the game paid=ehpaid@1,1",
               "--watch", "party stain: recovered=ehparty@1,1"],
        crash=[], until=[EH_UNTIL], until_hold=8,
    )


def check_eh(v, L):
    """Phase eh: an enemy that took a guest's echoes pays them once; the party stain never pays them too."""
    need(v, L, "eh join: 2 players", 0, r"cooperators 0 -> 1")
    d = need(v, L, "eh guest killed by an enemy", 1, r"death \d+: killed by an enemy \(chr [0-9a-f]+, entity 24102\d\d")
    rec = L.find(1, r"guest: bloodstain recorded: echoes \d+ -> 0, stain \d+ echoes", d or 0)
    v.step("eh its bloodstain holds the echoes", rec is not None, rec[1][:160] if rec else "no record line")
    held = need(v, L, "eh the enemy takes them (glowing eyes; the party stain hidden)", 1,
                r"party stain: an enemy took its \d+ Blood Echoes", d or 0)
    need(v, L, "eh its kill pays them (the game), the party stain goes", 1,
         r"party stain: the game paid its \d+ Blood Echoes", held or 0)
    pays = L.all(1, r"party stain: recovered \d+ Blood Echoes|party stain: the game paid its \d+ Blood Echoes")
    v.step("eh the echoes are paid exactly once", len(pays) == 1,
           "; ".join("+%.1f s %s" % (t, l[:110]) for t, l in pays) or "never paid")
    check_common(v, L, "eh")


# Phase clg (chalicelamp, ~6 min, 2 players): two guest reports in a Chalice Dungeon. The host Continues
# in the Pthumeru dungeon's layer 1 boss room (chalice_pthumeru_inside), guest 1 is
# summoned in (from chalice_hintertomb_ritual when present, as in cl). (1) The host lights the boss room's lamp 2901952 (its last lamp 2902952, whose template
# spot in the party's table is 80 m off). (2) Guest 1 is warped 31 m above the floor and dies from the
# fall: it must get up beside the host, never at the host's lamp's template spot (the report: "teleported
# above the dungeon"). (3) Both ride the post-boss lift 2901004 down (31.5 -> 10.5): the guest's game
# learns the car from the ride, no fall. (4) Guest 1 lights the bottom floor's lamp 2901953 and uses it:
# "Return to the Hunter's Dream" (the report: nothing happened), then YES to the party travel: the party
# goes to the Dream.
CLG_LAMP_OBJ2 = "2901953"   # the lift's bottom floor lamp object (return point 2902953)
CLG_UNTIL = r"0:map .* -> m21_00"
CLG_UNTIL_G = r"1:travel step (\d+)/: "  # guest 1's last step: 40 s in the Dream (r2: its earlier m21 ended the run)


def phase_clg(only):
    host = ",".join(["log_pos", "god",
                     "host_obj=%s:1.2:0@12%%m29" % CL_LAMP_OBJ,     # (1) light the boss room's lamp
                     "host_press=0x4000@2%m29!",
                     "host_move=-49.4:30.6:-91.0@4%m29!",          # beside the lamp: where the guest gets up
                     "host_press=0@45%m29!",                       # (2) the guest's fall meanwhile
                     "host_move=-65.0:31.6:-91.0@1%m29!?near=-64.6:31.6:-90.4:1.5",  # (3) onto the car
                     "host_press=0@60%m29!"])                      # (4) the guest's lamp: the party travels
    g1 = ",".join(["log_pos=0.5",
                   "guest_move=-46.0:62.0:-92.0@25%m29!",         # (2) 31 m above the boss room's floor
                   "guest_move=-64.6:31.6:-90.4@25%m29!",         # (3) onto the lift's car
                   "guest_obj=%s:1.2:0@14%%m29!" % CLG_LAMP_OBJ2,  # (4) the bottom lamp: light it
                   "guest_press=0x4000@2%m29!",
                   "guest_obj=%s:1.2:0@9%%m29!" % CLG_LAMP_OBJ2,   # use it: the Dream question (r1: at the
                   "guest_press=0x4000@4%m29!",                    # first Cross), YES (it opens on NO), OK
                   "guest_press=0x80@3%m29!", "guest_press=0x4000@2%m29!",
                   "guest_press=0@40%m21!"])
    watch = []
    for rx, tag in ((r"died from a fall", "gfall"), (r"up again beside the nearest living member", "gup"),
                    (r"Chalice Dungeon lift car", "car"), (r"lamp flag 729001", "dream"), (r"map .* -> m21_00", "m21")):
        watch += ["--watch", "%s=%s@1,4" % (rx, tag)]
    return dict(
        name="clg", seconds=480, count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[],
        until=[CLG_UNTIL, CLG_UNTIL_G],
    )



PHASES = {"s1": phase_s1, "s2": phase_s2, "rules": phase_rules, "sc": phase_sc, "summonfail": phase_summonfail,
          "s3": phase_s3, "sk": phase_sk, "ga": phase_ga, "gl": phase_gl, "lf": phase_lf, "mk": phase_mk, "kd": phase_kd, "kw": phase_kw, "fl": phase_fl, "fd": phase_fd, "fdj": phase_fdj, "ld": phase_ld, "cl": phase_cl, "clg": phase_clg, "el": phase_el, "lt": phase_lt, "bf": phase_bf, "ch": phase_ch, "rh": phase_rh, "rs": phase_rs, "lp": phase_lp, "rt": phase_rt, "br": phase_br, "sm": phase_sm, "da": phase_da, "cv": phase_cv, "ie": phase_ie, "tv": phase_tv, "nf": phase_nf, "nfa": phase_nfa, "eh": phase_eh}
# --only groups (a smaller run for repeat cycles): the phase each needs and the verdict rows it keeps
GROUPS = {"join": "s1", "leave": "s1", "death": "s1", "travel": "s1", "boss": "s1", "host_death": "s1",
          "wipe": "s1", "wipe_late": "s1", "kill": "s2", "arena": "s2", "itembox": "s2", "rejoin_kill": "s2", "c6": "s2", "host_crash": "s2",
          "host_leave": "s2", "keyitems": "s2", "rules": "rules", "savecopy": "sc", "summonfail": "summonfail",
          "host_revive": "s3", "streamkill": "sk", "gather": "ga", "gather_ground": "gl", "lifts": "lf", "ladder": "lf,ld", "mark": "mk", "kidnap": "kd", "story_warp": "kw", "fall": "fl", "fall_deep": "fd,fdj", "chalamp": "cl", "chalice_guest": "clg", "elevator": "el", "lifts_together": "lt", "fight_load": "bf", "cainhurst_lift": "ch", "research_lift": "rh", "research_solo": "rs", "lamps": "lp",
          "rot": "rt", "boss_rooms": "br", "scene_mirror": "sm", "dream_apart": "da", "covenant": "cv",
          "valtr_rune": "cv", "items_every": "ie", "travel_vote": "tv", "npc_fights": "nf,nfa", "npc_alfred": "nfa", "echo_hold": "eh"}
GROUP_ROWS = {"join": r"join", "leave": r"s1 (Leave|Rejoin)", "death": r"guest death|bloodstain|revived",
              "travel": r"travel", "boss": r"Gascoigne|intro", "host_death": r"host death|limbo",
              "wipe": r"wipe", "wipe_late": r"late wipe", "kill": r"s2 boss kill|party stays", "arena": r"arena", "itembox": r"item box|reward on the summoned",
              "rejoin_kill": r"s2 (Leave after|Rejoin|no kick)", "c6": r"guest crash",
              "host_crash": r"host crash", "host_leave": r"host Leave", "keyitems": r"key item", "rules": r"^rules",
              "savecopy": r"^sc ", "summonfail": r"^summonfail",
              "host_revive": r"^s3 ", "streamkill": r"^sk ", "gather": r"^ga ", "gather_ground": r"^gl ",
              "lifts": r"^lf (join|lift|guest's lift|host)", "ladder": r"^lf (join|ladder)|^ld ", "mark": r"^mk ", "kidnap": r"^kd ", "story_warp": r"^kw ", "fall": r"^fl ", "fall_deep": r"^fd ", "chalamp": r"^cl ", "chalice_guest": r"^clg ", "elevator": r"^el ", "lifts_together": r"^lt ", "fight_load": r"^bf ", "cainhurst_lift": r"^ch ", "research_lift": r"^rh ", "research_solo": r"^rs ", "lamps": r"^lp ",
              "rot": r"^rt ", "boss_rooms": r"^br ", "scene_mirror": r"^sm ", "dream_apart": r"^da ",
              "covenant": r"^cv(:| join| host's fixture| rule| covenant| shared| host's save| guest's save)",
              "valtr_rune": r"^cv(:| join| rule| rune)", "items_every": r"^ie ", "travel_vote": r"^tv ", "npc_fights": r"^nfa? ", "npc_alfred": r"^nfa ", "echo_hold": r"^eh "}


# --only: the log lines that end each group's checks (instances.py --until "I:RX"); a phase with
# --only ends once the chosen groups' lines are all seen, so the steps after them never run.
GROUP_UNTIL = {
    "join": ["0:cooperators [01] -> 2"],
    "leave": ["1:guest_rejoin: Party > Rejoin pressed", "0:cooperators 1 -> 2"],
    "death": ["1:guest: respawned at the host's lamp|guest: died alone: revived|revived at"],
    "travel": ["1:map - -> m24_01", "2:map - -> m24_01"],
    "boss": ["1:cutscene 24010010"],
    "host_death": ["0:spectating: following", "1:Party HUD: slot 0 spectating"],
    "wipe": [WIPE_UNTIL],
    # guest 1 back from the late wipe's respawn load (then the hold: no revive of its own after it)
    # and guest 2's walk through Gascoigne's fog 30 s later: the fight opens on the host
    "wipe_late": [r"1:own chr at HP 0: told the host[\s\S]*arrived in the host world \(flow 4\)",
                  r"2:own chr at HP 0: told the host[\s\S]*went through boss fog",
                  r"0:a wipe; the[\s\S]*boss fog: slot \d+ went in first"],
    "kill": ["2:host boss kill", "0:K10: boss-clear leave suppressed"],
    "arena": [r"2:test: guest_move to \(-136\.5"],
    "itembox": ["1:item notice closed|lot 50000010 .*(?:not )?given (?:now )?while summoned|owed key item: lot 50000010",
                "1:" + STORAGE_GIVE],
    # guest 1 back in the host's world after the Rejoin, then ~10 s more log (the no-kick check);
    # the press alone ended the run before the join (kickfix k2)
    "rejoin_kill": [r"1:guest_rejoin: Party > Rejoin pressed[\s\S]*apply gate open: summoned[\s\S]{20000}"],
    "keyitems": [r"1:key goods held: .*load mode 0\)"],  # guest 1 back in its own world after the Leave
    "rules": [r"0:mark=rules_h_ffoff[\s\S]{200}", "1:test menu: done"],
    # guest 2 in the host's world, then ~10 s more log (the screenshots of it in the host's world)
    "summonfail": [r"0:cooperators 0 -> 1"],
    "streamkill": [SK_UNTIL],
    "gather": GA_UNTIL,
    "gather_ground": GL_UNTIL,
    # the host back at the lamp (the lift's block streamed in, its init run) and guest 1's chain done
    "lifts": [LF_UNTIL],
    "fall": [FL_UNTIL, FL_HOST_UNTIL],
    "elevator": EL_UNTIL,
    "lifts_together": LT_UNTIL,
    "cainhurst_lift": CH_UNTIL,
    "research_lift": RH_UNTIL,
    "research_solo": [r"0:travel step (\d+)/\1: "],
    "lamps": LP_UNTIL,
    "mark": [MK_UNTIL],
    # the host awake in the cell (its last step) and both guests in m28_00
    "kidnap": [KD_UNTIL, "1:map .*-> m28_00", "2:map .*-> m28_00"],
    "story_warp": [KW_UNTIL],
    # the host back in the dungeon from the Dream, ~40 s after (guest 1 summoned back in)
    "chalamp": [CL_UNTIL, CL_UNTIL_G],
    "chalice_guest": [CLG_UNTIL, CLG_UNTIL_G],
    "fight_load": BF_UNTIL,
    "rot": [RT_UNTIL],
    "echo_hold": [EH_UNTIL],
    "boss_rooms": BR_UNTIL,
    "scene_mirror": [SM_UNTIL],
    "dream_apart": DA_UNTIL,
    "npc_fights": [NF_UNTIL],
    "npc_alfred": [NFA_UNTIL],
    "covenant": CV_UNTIL,
    "valtr_rune": CV_UNTIL,
    "items_every": IE_UNTIL,
    "travel_vote": TV_UNTIL,
}
# --only: seconds the run goes on after the group's lines (screenshots of the state they reach)
GROUP_HOLD = {"host_death": 10, "wipe_late": 10}  # wipe_late: the fight opened by the walk-in on every screen  # the spectate on every screen (the guests' panels: "Spectating")
NEEDS_CRASH = {"c6", "host_crash", "host_leave"}  # s2 groups that need the crash plan and its full chain


def narrow_phase(ph, only):
    """--only: end the phase on the chosen groups' lines; drop the s2 crash plan nobody asked for."""
    mine = [g for g in only if ph["name"] in GROUPS[g].split(",")]
    if not mine or NEEDS_CRASH & set(mine):
        return ph
    ph = dict(ph, until=[u for g in mine for u in GROUP_UNTIL.get(g, [])] or ph["until"], crash=[])
    hold = max([GROUP_HOLD.get(g, 0) for g in mine] + [ph.get("until_hold", 0)])
    if hold:
        ph["until_hold"] = hold
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
        if ph.get("ini_preset") and not ini.exists():
            ini.parent.mkdir(parents=True, exist_ok=True)
            ini.write_text("".join("%s=%s\n" % kv for kv in ph["ini_preset"].items()), encoding="utf-8")
        elif ph.get("ini_reset") and ini.exists():
            keep = [l for l in ini.read_text(encoding="utf-8", errors="replace").splitlines()
                    if l.partition("=")[0].strip() not in ph["ini_reset"]]
            # ini_preset: values an older version left (rules: 0.7.1's party_rot=1, its migration)
            keep += ["%s=%s" % kv for kv in ph.get("ini_preset", {}).items()]
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
           *(["--fps", str(ph["fps"])] if ph.get("fps") else []),
           "--headless", str(ph.get("headless", 1)), "--party"] + (["--fresh-saves"] if ph.get("fresh", True) else []) + [
           "--timestamps", "--idle", "0", "--seconds", str(ph["seconds"]),
           "--root", str(root), "--build", str(a.build), "--game", str(a.game),
           "--label", "smoke %s" % ph["name"]] + ph.get("args", [])
    if getattr(a, "real_net", False):
        cmd += ["--real-net"]  # the real network instead of loopback (instances.py --real-net)
    # Speed (user decision 2026-10-22): every cutscene is watched 5 s, then skipped by the game's own
    # skip / end path (its end actions still run); a phase that needs whole scenes sets full_scenes.
    if not ph.get("full_scenes") and not any(e.startswith("BB_TEST_REMO_SKIP_AFTER=") for e in ph["env"]):
        cmd += ["--env", "BB_TEST_REMO_SKIP_AFTER=5"]
    for e in ph["env"]:
        cmd += ["--env", e.replace("{root}", str(root))]
    for e in ph["ienv"]:
        cmd += ["--ienv", e.replace("{hold}", hold).replace("{root}", str(root))]
    for w in ph.get("start_when", []):
        cmd += ["--start-when", w]
    for i in (ph.get("pads") or ((0,) if ph.get("pad") else ())):
        # the stick walks (host_stick / guest_stick) write the in-process pad script
        (root / f"pad{i}.txt").write_text("lx=128 ly=128\n")
        cmd += ["--ienv", f"{i}:BB_PAD_FILE=" + str(root / f"pad{i}.txt")]
    if ph.get("until_hold"):
        cmd += ["--until-hold", str(ph["until_hold"])]
    for c in ph["crash"]:
        cmd += ["--crash-plan", c]
    for u in ph.get("until", []):
        cmd += ["--until", u]
    # BB_MP_PARENT_PID: this harness; instances.py (and the reaper, by the slot's "parent") end the
    # games once it is gone (a killed smoke left its phase's games running to their cap).
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"), BB_MP_PARENT_PID=str(os.getpid()))
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
    if ph.get("post"):
        ph["post"](root)


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
        if self.keep and not any(k.search(name) for k in self.keep) and not name.endswith(("no crash / assert", "no 'Session lost'", "no boot stall (BB_BOOT_WATCHDOG)")):
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
    none_of(v, L, tag + ": no boot stall (BB_BOOT_WATCHDOG)", r"\[WATCHDOG\] Boot stalled|asking for a boot snapshot")


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
        sp = need(v, L, "s1 host limbo + spectate", 0, r"spectating: following", hd)
        if sp is not None:
            # The guests' party panels: "Spectating" with the host's body spot, not the carried chr's
            # "0.2 m" (its VITALS flag kVitalsSpectating).
            need(v, L, "s1 host limbo: guest 1's panel shows the host spectating", 1,
                 r"Party HUD: slot 0 spectating", sp - 2)
    need(v, L, "s1 wipe: the party respawns together", 0, WIPE_RX, hd or 0)
    if WIPE_LATE_RUN:
        check_wipe_late(v, L, hd)  # the wipe_late chain (no Leave, guest 1 down last)
    check_common(v, L, "s1")


WIPE_LATE_RUN = False  # --only wipe_late: s1 ran the late-death wipe chain
GASCOIGNE_ARENA = (-48.76, -40.92, 72.44)  # the intro's trigger box, in the arena


def check_wipe_late(v, L, hd):
    """Late-death wipe (player report 2026-10-21): guest 1 is the last one down, long after the host
    (its scripted death at 40 s, or Gascoigne's hit before it: run1 36 s); one respawn load for
    everyone, its bloodstain in the arena, no revive of its own after the load."""
    g2 = need(v, L, "s1 late wipe: guest 2 down", 2, r"own chr at HP 0: told the host", hd or 0)
    g1 = need(v, L, "s1 late wipe: guest 1 down last", 1, r"own chr at HP 0: told the host", (g2 or hd or 0) + 0.5)
    if g1 is None or hd is None:
        return
    v.step("s1 late wipe: guest 1 down >= 20 s after the host", g1 - hd >= 20, "%.1f s after" % (g1 - hd))
    arr = need(v, L, "s1 late wipe: guest 1 arrives with the respawn load", 1, r"arrived in the host world \(flow 4\)", g1)
    end = arr if arr is not None else 1e9
    wipes = L.all(0, WIPE_RX, hd, end)
    v.step("s1 late wipe: one wipe (one respawn load)", len(wipes) == 1, "%d wipe line(s)" % len(wipes))
    w = wipes[0][0] if wipes else g1
    v.step("s1 late wipe: the wipe at guest 1's death", w >= g1 - 1, "wipe +%.1f s, guest 1 down +%.1f s" % (w, g1))
    need(v, L, "s1 late wipe: guest 1 waits for its death, then follows dead", 1,
         r"follows it dead to the host's lamp with the party", g1 - 1, end)
    st = L.find(1, r"party stain \(death in the host's world\): \d+ Blood Echoes at", g1 - 1, end)
    if st:
        m = re.search(r"at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", st[1])
        p = [float(x) for x in m.groups()] if m else [1e9] * 3
        d = sum((a - b) ** 2 for a, b in zip(p, GASCOIGNE_ARENA)) ** 0.5
        v.step("s1 late wipe: guest 1's bloodstain where it died", d < 30, "%.1f m from the arena: %s" % (d, st[1][:120]))
    else:
        v.step("s1 late wipe: guest 1's bloodstain where it died", False, "no party stain line after +%.1f s" % g1)
    if arr is None:
        return
    loads = L.all(1, r"Loading: begins|loading screen up", g1 - 1, end)
    v.step("s1 late wipe: guest 1 took one load", 1 <= len(loads) <= 2, "%d load line(s) after its death" % len(loads))
    need(v, L, "s1 late wipe: guest 1 up beside the host", 1, r"placed beside the host after the death follow", arr - 1)
    need(v, L, "s1 late wipe: guest 1 alive after the load", 1, r"guest: HP [1-9]\d* after the load", g1, arr + 1)
    none_of(v, L, "s1 late wipe: no revive of its own after the load", r"died alone: revived|revived at its own last lamp|"
            r"died alone \(no host travel|guest death redirect runs|own death in the host's world held", arr, arr + 15, insts=(1,))
    need(v, L, "s1 late wipe: host re-join done", 0, r"host: re-join done", arr - 2)
    check_wipe_boss(v, L, w, arr)


# A boss fight's start on any game (the health bar, the host opening it for a guest, a guest's walk-in)
BOSS_START = (r"boss health bar shown|host: boss fog: slot \d+ went in|guest: went through boss fog|"
              r"host's first-fight event \d+ starts now")


def check_wipe_boss(v, L, w, arr):
    """wipeboss (0.7.2 known issue 2): after the wipe's respawn load every game has the boss as the
    host's load reset it - no fight at the lamp for 30 s while nobody goes in - and a real walk
    through the fog afterwards still opens the fight for everyone."""
    back = L.find(0, r"loading screen gone", w)
    t0 = back[0] if back else arr
    for i in (0, 1, 2):
        need(v, L, "s1 late wipe boss: boss reset by the load on inst%d" % i, i, r"boss health bar: reset \(a load\)", w - 1)
    walk = L.find(2, r"travel step \d+/\d+: guest_obj", arr)  # guest 2 (headless) walks in
    t1 = walk[0] if walk else 1e9
    v.step("s1 late wipe boss: 30 s at the lamp before the walk-in", walk is not None and t1 - arr >= 30,
           "walk +%.1f s, guest 1 back +%.1f s" % (t1, arr) if walk else "no guest_obj step after +%.1f s" % arr)
    none_of(v, L, "s1 late wipe boss: no fight starts at the lamp (bar, fog, intro)", BOSS_START, t0, t1)
    none_of(v, L, "s1 late wipe boss: no stale fog flag held back", r"already 1 when this stay began", arr - 15, t1, insts=(1, 2))
    if walk is None:
        return
    went = need(v, L, "s1 late wipe boss: guest 2 walks through the fog", 2, r"guest: went through boss fog", t1)
    if went is None:
        return
    need(v, L, "s1 late wipe boss: the host opens the fight from it", 0, r"boss fog: slot \d+ went in first", went - 0.5)
    for i in (0, 1, 2):
        need(v, L, "s1 late wipe boss: the fight is on for inst%d (bar)" % i, i, r"boss health bar shown", went - 0.5)

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


def check_key_saves(v):
    """Each guest's party save on disk (save_decode.py): no key item in two stacks (the saved and the
    summoned key section are both written; items.md 10.6 / 10.7)."""
    sys.path.insert(0, str(REPO / "tools" / "party"))
    import save_decode  # noqa: E402
    seen, bad = [], []
    for i in (1, 2):
        for f in sorted((Path(RUN_ROOT[0]) / f"inst{i}" / "user" / "savedata_party").rglob("userdata00[0-9][0-9]")):
            try:
                sv = save_decode.Save.read(f)
            except (OSError, ValueError):
                continue
            ks = sv.key_sections()
            if ks is None:
                continue
            dup = sv.duplicate_keys()
            seen.append("inst%d %s: %d saved + %d summoned" % (i, f.name, len(ks[0]), len(ks[1])))
            if dup:
                bad.append("inst%d %s: %s" % (i, f.name, ",".join(map(str, dup))))
    v.step("s2 key items: one stack each in the guests' saves on disk", bool(seen) and not bad,
           ("DUPLICATE " + "; ".join(bad)) if bad else "; ".join(seen[:4]) or "no guest save decoded")


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
    # Key items (items.md 10.5, 10.7). The test save used Amelia's Gold Pendant (4001): 0.6.2 - 0.6.9
    # gave it back at every own-world load. After the travel the host holds its keys, and so does a
    # summoned guest (0.7.1: the own key section is shown at every host-world load); at home the
    # save's keys are there, one stack each, in memory and on disk.
    none_of(v, L, "s2 key item: a used-up item is not given back", r"(catch-up|owed lot|given again).*goods 4001")
    none_of(v, L, "s2 key item: no duplicate stack", r"DUPLICATE STACK")
    if tr is not None:
        need(v, L, "s2 key items after the travel (host)", 0, r"key goods held: .*4000 x1,(?: 4001 x0,)?.*map m24_01", tr)
        need(v, L, "s2 key items: the own key section shown on the summoned guest", 1,
             r"summoned load: the own key section is shown \([1-9]", tr)
        need(v, L, "s2 key items held by the summoned guest after the travel", 1,
             r"key goods held: .*4000 x1,.*4114 x1,.*map m24_01.*load mode 1", tr)
        # The menu walk (phase_s2 files): the Key Items tab of the summoned guest lists its own keys.
        need(v, L, "s2 key items: the summoned guest's Key Items tab lists its own keys", 1,
             r'text shows: "Oedon Tomb Key"', tr)
    check_key_saves(v)
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
        # The restarted host's copies of the guests: built, then shown by the game when their data
        # arrives (the draw bit with it), never by the port before it (the safety net firing early).
        # chr +0xC0 is not checked: it keeps the creation spot on every join, not the drawn position.
        need(v, L, "s2 host crash: a guest's chr gets its data, shown by the game", 0,
             r"its member's data after [\d.]+ s, drawn by the game .*crash-restarted", h[0])
        none_of(v, L, "s2 host crash: no guest shown by the safety net", r"shown as InGameStart does \(safety net", h[0], insts=(0,))
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
        if back or page == "Party rules: PvP & lamps":
            b = need(v, L, "rules host: %s set back (bbport.ini %s)" % (page, ini_back), 0,
                     r"Settings: bbport.ini %s$" % re.escape(ini_back), t)
    # The host's rule on the guest: its Party rules: PvP & lamps page shows the host's friendly fire (on then)
    ff = L.find(1, r"Game menu: page \d+ rows \(\d+\): Friendly fire / PvP=")
    v.step("rules guest: sees the host's friendly fire (On, read-only)", ff and "Friendly fire / PvP=On" in ff[1],
           ff[1][:200] if ff else "no PvP & lamps rows line on the guest")
    # 0.7.2: and the host's Guests bring their lamps (off by default)
    v.step("rules guest: sees the host's Guests bring their lamps (Off, read-only)",
           ff and "Guests bring their lamps=Off" in ff[1], ff[1][:300] if ff else "no PvP & lamps rows line on the guest")
    # 0.7.1: the guest's Party rules: World page shows the host's rules (read-only lines, the host's values)
    sw = L.find(1, r"Game menu: page \d+ rows \(\d+\): Enemies stay dead=")
    v.step("rules guest: sees the host's Party rules: World rules (read-only)",
           sw and "Host can gather=On" in sw[1] and "NPCs as in host's world=On" in sw[1]
           and "Party votes on travel=Off" in sw[1] and "Covenants per player=On" in sw[1],  # 0.7.3 defaults
           sw[1][:250] if sw else "no Party rules: World rows line on the guest")
    # guestrules (0.7.3): the enemy scaling is the host's: read-only lines with the host's values on
    # the guest ("150 %", the host's default), no slider position (a host's row shows "5")
    es = L.find(1, r"Game menu: page \d+ rows \(\d+\): Boss health=")
    v.step("rules guest: sees the host's Party rules: Enemies (read-only values)",
           bool(es) and "Boss health=150 %" in es[1] and "Boss damage=+10 %" in es[1],
           es[1][:250] if es else "no Party rules: Enemies rows line on the guest")
    none_of(v, L, "rules guest: never writes the host's scaling to its bbport.ini",
            r"Settings: bbport.ini party_(boss|enemy)_", insts=(1,))
    # 0.7.2: a 0.7.1 bbport.ini (party_rot=1, no settings_rev) has rot off once, on both games
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rules %s: 0.7.1 bbport.ini migrated (rot off once)" % who, i,
             r"Settings: bbport.ini settings_rev 0 -> 1 \(rot off by default")
    rot_rows = L.find(0, r"Game menu: page \d+ rows \(\d+\): Rot on death=")
    v.step("rules host: Rot page shows rot off after the migration", bool(rot_rows) and "Rot on death=Off" in rot_rows[1],
           rot_rows[1][:200] if rot_rows else "no Rot rows line on the host")
    none_of(v, L, "rules: no menu walk stuck", r"STUCK", insts=(0, 1))
    check_common(v, L, "rules")


def check_summonfail(v, L):
    f = need(v, L, "summonfail: guest 1's join fails (its party port is blocked)", 1, SF_FAILED)
    if f is None:
        return
    ask = need(v, L, "summonfail: guest 2 asks for a direct summon", 2, r"direct summon: asked the host for its room", f)
    if ask is not None:
        # guest 2 comes in by the direct summon or by its sign (the host's sign summon of it can win)
        hit = L.find(0, r"direct summon: slot 2 \(.*\) gets this host's room", ask)
        sign = None if hit else L.find(2, r"chalice: summon message type 1", ask)
        served = hit[0] if hit else None
        v.step("summonfail: the host summons guest 2 (direct or by its sign)", hit or sign,
               ("+%.1f s inst%d: %s" % ((hit or sign)[0], 0 if hit else 2, (hit or sign)[1][:150])) if hit or sign
               else "neither a direct summon served nor a sign summon message on inst2 after +%.0f s" % ask)
        if served is not None:
            v.step("summonfail: served within 30 s of guest 2's first ask", served - ask < 30,
                   "%.1f s (limit 30)" % (served - ask))
        j = need(v, L, "summonfail: guest 2 in the host's world (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1", ask)
        if j is not None:
            end = max([t for t, _ in L.all(0, r".", j)] or [j])
            v.step("summonfail: the run lasted %d s past guest 2's join" % SF_STAY, end - j >= SF_STAY,
                   "%.0f s" % (end - j))
            left = L.find(0, r"cooperators 1 -> 0", j, j + SF_STAY) or L.find(2, r"BeginLeaveSession|session client .* -> leaving", j, j + SF_STAY)
            v.step("summonfail: guest 2 stays in the host's world for %d s" % SF_STAY, not left,
                   "left at +%.1f s: %s" % (left[0], left[1][:150]) if left else "no leave")
            unr = L.find(0, r"slot 1 \(.*\) (refused \(its last join failed|reports a failure: its UDP is not heard)", f - 5)
            v.step("summonfail: the unreachable guest 1 is held back", unr, unr[1][:150] if unr else "no hold line")
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
             r"enemy census: (?:digest \d+ matches the host's .*after the host's revive|.*the same as the host's again"
             r"|loaded differently .*\(players apart\), no alive / dead difference)", t, t + 25)
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



def lf_flags(L, i, after=0.0, before=1e9):
    """The last `test: flags` line of instance i in [after, before): {id: value}."""
    rows = L.all(i, r"test: flags ", after, before)
    if not rows:
        return None
    return {int(a): int(b) for a, b in re.findall(r" (\d+)=(-?\d)", rows[-1][1])}


def check_lf(v, L):
    need(v, L, "lf join: 3 players", 0, r"cooperators [01] -> 2")
    far = need(v, L, "lf host far away", 0, r"test: host_move to \(-158\.10")
    gone = L.find(0, r"test: object %s not loaded" % LF_LIFT, far or 0) if far else None
    v.step("lf host: the lift's block streamed out there", gone, "+%.1f s" % gone[0] if gone else
           "the lift stayed loaded on the host (the case is not tested)")
    ladder_end = need(v, L, "lf ladder: guest 1 pulled the lever (its ObjAct kick)", 1,
                      r"ObjAct kick \(local player\).*kick event 12410206")
    ride = need(v, L, "lf guest's lift ride (its own copy of the lift's up event)", 1,
                r"test: flags 12400827=0 12400828=1", far or 0)
    # The ride's end goes up from the first guest whose copy finished it: guest 2's copy (at the lamp, the
    # car far away) ends at once, guest 1's when it leaves the car
    over = None
    for i in (1, 2):
        hit = L.find(i, r"guest: lift %s ride over \(flag 12400827 = 1.*(to the host|has that end already)" % LF_LIFT,
                     ride or 0)
        over = hit[0] if hit and (over is None or hit[0] < over) else over
    v.step("lf guest's lift ride over: sent to the host", over is not None,
           "+%.1f s" % over if over is not None else "no guest sent the ride's end")
    if over is not None:
        need(v, L, "lf host's world took the ride", 0,
             r"lift %s 12400827=1\((applied: the lift's block is not loaded here|same here)\)" % LF_LIFT, over - 1)
    back = need(v, L, "lf host back at the lamp", 0, r"test: host_move to \(-36\.80", over or 0)
    if back is not None:
        # The block's init (12400823): 2005[7] anim 0 = the car at the top (12400827 = 1), 30 = at the bottom
        car = L.find(0, r"test: object %s anim (?!-1 )\d+ " % LF_LIFT, back)
        v.step("lf host's car where the guest left it (init anim 0, at the top)", car and " anim 0 " in car[1],
               ("+%.1f s inst0: %s" % (car[0], car[1][:120])) if car else "no car state after the return")
    end = 1e9
    last = {i: lf_flags(L, i, 0, end) for i in (0, 1, 2)}
    for key, name in ((12400827, "position"), (12400828, "busy")):
        vals = [last[i].get(key) if last[i] else None for i in (0, 1, 2)]
        v.step("lf lift %s flag %d equal on all games at the end" % (name, key), len(set(vals)) == 1 and None not in vals,
               "host %s, guest 1 %s, guest 2 %s" % tuple(vals))
    pull = ladder_end
    if pull is not None:
        need(v, L, "lf ladder: the host's game got the kick", 0, r"ObjAct kick \(another player\).*kick event 12410206",
             pull - 1, pull + 10)
        for i in (0, 1, 2):
            need(v, L, "lf ladder down on game %d (12410285 = 1)" % i, i, r"test: flags .*12410285=1", pull, pull + 15)
        for i in (0, 1):
            need(v, L, "lf ladder down on game %d (its anim 2)" % i, i, r"test: object 2411204 anim 2 ", pull, pull + 20)
    # liftfix: the guest rode the car up: at its top (y ~3) at the arrival, on the car all the way
    arr = L.find(1, r"object %s stopped at anim 32 now" % LF_LIFT)
    pos = L.all(1, POS_RX.pattern, arr[0] - 1.0, arr[0] + 0.1) if arr else []
    y = float(POS_RX.search(pos[-1][1]).group(2)) if pos else None
    v.step("lf guest's lift ride: guest 1 at the top on the car's arrival", y is not None and y > -5.0,
           ("y %.1f at +%.1f s" % (y, arr[0])) if pos else "no arrival seen")
    # liftfix: the ride itself: no drop faster than a lift (god mode would hide a fall down the shaft)
    # from the approach beside the car on (a fall there must show too)
    into = L.find(1, r"guest_move to \(30\.11, -45\.60")
    v.step("lf lift: guest 1 reached the car's landing", into is not None, "+%.1f s" % into[0] if into else "no approach")
    for i, who in ((0, "host"), (1, "guest 1")):
        f = falls(L, i, into[0] if into else 0.0)
        v.step("lf lift: %s no fall (vertical speed under 8 m/s)" % who, not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "fastest drop within a lift's speed")
    # the warp check on guest 1's ride (the car's anims 31, 32 up to its end 7)
    st = L.find(1, r"test: object %s anim 31 playing 1" % LF_LIFT)
    en = L.find(1, r"test: object %s anim 7 playing 0" % LF_LIFT, st[0] if st else 0) if st else None
    j = jumps(L, 1, st[0], en[0], (24.28, 194.17)) if st and en else None
    v.step("lf lift: guest 1 no jump on the car", j == [], ("; ".join("+%.1f s %.1f m" % x for x in j[:4])) if j else
           ("no jump over 2 m while on the car" if j == [] else "no ride seen"))
    none_of(v, L, "lf lift: nobody died", r"local death: HP 0", 0, 1e9, (0, 1, 2))
    check_common(v, L, "lf")


def check_sk(v, L):
    need(v, L, "sk join: 3 players", 0, r"cooperators [01] -> 2")
    kill = need(v, L, "sk host kills the neighbour map's enemy %s" % SK_ENEMY, 0,
                r"test: host_kill: entity %s HP \d+ -> 0" % SK_ENEMY)
    if kill is None:
        check_common(v, L, "sk")
        return
    away = need(v, L, "sk host 130 m away", 0, r"test: host_move to \(-158\.10", kill)
    if away is not None:
        v.step("sk host left within 5 s of the kill (before any census)", away - kill < 5, "%.1f s" % (away - kill))
        gone = need(v, L, "sk host far: %s not loaded (its map streamed out)" % SK_ENEMY, 0,
                    r"test: host_kill: no target \(entity %s\)" % SK_ENEMY, away)
    for i in (1, 2):
        need(v, L, "sk guest %d: %s dead there too (the kill record)" % (i, SK_ENEMY), i,
             r"kill record: entity %s alive here, killed by the party 3 s ago: put to death|kill seen: entity %s died here"
             % (SK_ENEMY, SK_ENEMY), kill, kill + 20)
    back = need(v, L, "sk host back at the lamp", 0, r"test: host_move to \(-36\.80", away or kill)
    if back is None:
        check_common(v, L, "sk")
        return
    rk = need(v, L, "sk host: %s back alive after the stream-in, put to death again" % SK_ENEMY, 0,
              r"kill record: entity %s alive again after its map streamed in .*: put to death again" % SK_ENEMY, back)
    if rk is not None:
        need(v, L, "sk host: no drop from the second death", 0, r"drops: .*no drop \(put to death again by the kill record", rk,
             rk + 5)
        # The director logs every change of the own Echoes ("Echoes 258 -> 516"); none may follow it
        ech = L.all(0, r"\] Echoes \d+ -> \d+", rk - 0.1, rk + 6)
        v.step("sk host: no echoes from the second death", not ech, "none in 6 s" if not ech else ech[0][1][:160])
    end = need(v, L, "sk host: %s still dead at the end (no target)" % SK_ENEMY, 0,
               r"test: host_kill: no target \(entity %s\)" % SK_ENEMY, (rk or back) + 15)
    for i in (1, 2):
        need(v, L, "sk guest %d census matches the host's after the return" % i, i,
             r"enemy census: digest \d+ matches the host's", rk or back, (end or 1e9) + 1)
    none_of(v, L, "sk: no census difference", r"enemy census: DIFFERS", kill, 1e9)
    check_common(v, L, "sk")


def _ga_first(L, rx, after=0.0):
    """The first match on either guest: (inst, t, line) or None."""
    hits = [(i,) + h for i in (1, 2) for h in [L.find(i, rx, after)] if h]
    return min(hits, key=lambda h: h[1]) if hits else None


def _ga_need(v, L, name, rx, after=0.0):
    h = _ga_first(L, rx, after)
    v.step(name, h, ("+%.1f s inst%d: %s" % (h[1], h[0], h[2][:150])) if h else "missing on both guests: /%s/" % rx)
    return h


def check_ga(v, L):
    """Rounds and guests as the run's timing gives them (which guest is far / apart when the host presses
    depends on the loads): each case must show up once, in any round, on either guest."""
    need(v, L, "ga join: 3 players", 0, r"cooperators [01] -> 2")
    need(v, L, "ga hotkey: touchpad + L3 gathers", 0, r"Gather the party: touchpad \+ L3 pressed")
    r1 = need(v, L, "ga round 1: the host asks 2 guests", 0, r"round 1: Gather the party: 2 guest")
    if r1 is None:
        check_common(v, L, "ga")
        return
    far = _ga_first(L, r"guest: YES: route inworld \(.*, ([\d.]+) m\)")
    d = float(re.search(r"([\d.]+) m\)$", far[2]).group(1)) if far else 0.0
    v.step("ga same map, > 100 m: YES -> in-world warp", far and d > 100.0,
           "inst%d +%.1f s: %.0f m" % (far[0], far[1], d) if far else "no in-world route on either guest")
    if far:
        w = L.find(far[0], r"gather: put beside the host .* no load", far[1])
        v.step("ga in-world: put beside the host, no load", w, w[1][:150] if w else "missing")
        none_of(v, L, "ga in-world: no loading screen", r"guest: (host snapshot applied|travels alone)", far[1],
                far[1] + 5, (far[0],))
    tr = _ga_need(v, L, "ga another map: YES -> travel", r"guest: round \d+: YES -> travel")
    if tr:
        need(v, L, "ga another map: arrived beside the host (one load)", tr[0], GA_ARRIVED, tr[1])
    need(v, L, "ga round 1: summary", 0, r"round 1 over .*Gather: ", r1)
    need(v, L, "ga cooldown: a press within 30 s refused", 0, r"Gather the party refused: too soon", r1)
    _ga_need(v, L, "ga refusal: a guest in a menu", r"guest: round \d+ refused: in a menu")
    _ga_need(v, L, "ga a guest answers No", r"guest: round \d+: NO")
    need(v, L, "ga host summary names the No and the menu", 0, r"round \d+ over .*said no.*in a menu")
    r2 = L.find(0, r"round 2: Gather the party", r1)
    v.step("ga rounds at least 30 s apart", r2 and r2[0] - r1 >= 30.0, "%.1f s" % (r2[0] - r1) if r2 else "no round 2")
    need(v, L, "ga round 3 over", 0, r"round 3 over", r1)
    check_common(v, L, "ga")

def check_gl(v, L):
    need(v, L, "gl join: 2 players", 0, r"cooperators 0 -> 1")
    r1 = need(v, L, "gl round 1: the host gathers at the ladder's foot", 0, r"round 1: Gather the party: 1 guest")
    if r1 is None:
        check_common(v, L, "gl")
        return
    yes = need(v, L, "gl guest: YES -> in-world warp", 1, r"guest: YES: route inworld", r1)
    if yes is not None:
        put = need(v, L, "gl guest: put beside the host (test: 8 m too high)", 1, r"gather: put beside the host .*test: lifted",
                   yes)
        fell = need(v, L, "gl guest: the landing watch sees the fall", 1, r"guest: FELL ([4-9]|1\d)\.\d m", yes, yes + 6)
        if fell is not None:
            need(v, L, "gl guest: back where it stood (in-world, no load)", 1,
                 r"in-world warp \(gather: no ground beside the host, back\)", fell, fell + 1)
            need(v, L, "gl guest: then the travel (one loading screen)", 1, r"guest: travels alone \(gather\)", fell, fell + 3)
            arr = need(v, L, "gl guest: arrived in the host's map", 1, r"guest: arrived in the host's map", fell, fell + 60)
            if arr is not None:
                need(v, L, "gl guest: stands beside the host after the travel", 1,
                     r"guest: stands beside the host [\d.]+ s after the warp \([0-3]\.\d m below", arr, arr + 8)
        none_of(v, L, "gl guest: no death", r"local death: HP 0", r1, 1e9, (1,))
    lad = need(v, L, "gl host on the ladder: gather refused", 0, r"Gather the party refused: on a ladder or in the air .*ladder state [0-9]",
               r1 + 30)
    if lad is not None:
        none_of(v, L, "gl no question on the guest for the refused gather", r"guest: round 2: the host gathers", lad, lad + 10,
                (1,))
    check_common(v, L, "gl")


def check_fl(v, L):
    need(v, L, "fl join: 2 players", 0, r"cooperators 0 -> 1")
    hp0 = need(v, L, "fl guest fell out of the world (HP 0 in the void)", 1,
               r"local death: HP 0 at \([-\d.]+, -(1\d{3}|[2-9]\d\d)\.")
    if hp0 is None:
        check_common(v, L, "fl")
        return
    up = need(v, L, "fl guest up at a lamp of the loaded map, no load", 1,
              r"guest: died from a fall .*up at (its own|the host's) last lamp", hp0)
    if up is not None:
        ys = [float(re.search(r"area 0x[0-9a-f]+ \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", l).group(2))
              for _, l in L.all(1, r"test: pos area 0x[0-9a-f]+ \(", up + 5, up + 30)]
        again = L.find(1, r"local death: HP 0", up + 0.5, up + 30)
        v.step("fl guest standing at the lamp afterwards (alive, on the floor)", bool(ys) and min(ys) > -100 and not again,
               "height %s%s" % (["%.1f" % y for y in ys[:3]], "; HP 0 again: " + again[1][:80] if again else ""))
        none_of(v, L, "fl no loading screen after the death", r"Party B2: load started", hp0, up + 20, (1,))
        none_of(v, L, "fl guest stays in the host's world (no session leave)", r"session client \(6, sub 0\) ->", hp0,
                up + 20, (1,))
        none_of(v, L, "fl never the Hunter's Dream", r"map - -> m21_", hp0, up + 30, (1,))
        xyz = r"\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
        st = L.find(1, r"guest: bloodstain recorded: .*stain object at " + xyz, hp0, up + 1)
        sf = re.search(r"its bloodstain on its last safe floor " + xyz, L.find(1, r"guest: died from a fall", hp0)[1])
        ok, d = False, -1.0
        if st and sf:
            so = re.search(r"stain object at " + xyz, st[1])
            d = sum((float(a) - float(b)) ** 2 for a, b in zip(so.groups(), sf.groups())) ** 0.5
            ok = d < 1.0 and float(so.group(2)) > -100
        v.step("fl bloodstain on the last safe floor", ok,
               ("%.1f m from it: %s" % (d, st[1][:120])) if st else "no stain record (echoes 0?)")
        check_revive_view(v, L, "fl guest", 1, "guest revive", up)
    # The host's own fall out of the world (after the guest's)
    h0 = need(v, L, "fl host fell out of the world (HP 0 in the void)", 0,
              r"local death: HP 0 at \([-\d.]+, -(1\d{3}|[2-9]\d\d)\.", (up or hp0))
    if h0 is not None:
        hup = need(v, L, "fl host up at its lamp, no load", 0, r"host death: a fall: up at its lamp", h0)
        if hup is not None:
            hp = [int(m.group(1)) for _, l in L.all(0, r"test: host_press 0x0; own HP (\d+)/", hup + 2, hup + 60)
                  for m in [re.search(r"own HP (\d+)/", l)]]
            v.step("fl host alive after it (no second death)", bool(hp) and min(hp) > 0, "HP %s" % hp[:3])
            none_of(v, L, "fl host: no second HP 0 after the revive", r"local death: HP 0", hup + 1, hup + 15, (0,))
            none_of(v, L, "fl host keeps its party (no session leave)", r"session host \(3, sub \d\) -> leaving", h0,
                    hup + 20, (0,))
            none_of(v, L, "fl host: no loading screen, never the Dream", r"Party B2: load started|map - -> m21_", h0,
                    hup + 20, (0,))
            xyz = r"\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
            sf = re.search(r"last safe ground spot " + xyz, L.find(0, r"local death: HP 0", h0 - 0.1)[1])
            st = L.all(0, r"own bloodstain (recorded|replaced): .*stain object at " + xyz, h0, hup + 5)
            ok, d = False, -1.0
            if st and sf:
                so = re.search(r"stain object at " + xyz, st[-1][1])
                d = sum((float(a) - float(b)) ** 2 for a, b in zip(so.groups(), sf.groups())) ** 0.5
                ok = d < 1.0
            v.step("fl host bloodstain on the last safe floor", ok, ("%.1f m from it" % d) if st else "no stain record")
            check_revive_view(v, L, "fl host", 0, "host revive", hup)
    check_common(v, L, "fl")


REVIVE_VIEW = re.compile(r"revive view \((?P<what>[^,]+), (?P<t>[\d.]+) s after\): camera \([^)]*\) (?P<cam>[-\w.]+) m "
                         r"from the chr \((?P<chr>[^)]*)\).*?; (?:drawn|ChrCtrl \+0xC0) \([^)]*\) (?P<drawn>[-\w.]+) m off(?: \(logged only\))?; "
                         r"fall-death camera (?P<latch>\d+), (?:ladder state -?\d+, )?hp (?P<hp>-?\d+): (?P<verdict>.*)$")


def check_revive_view(v, L, tag, i, what, after):
    """fallcam: the revive's 3 s view line (ReviveViewTick): the render camera within 12 m of the chr
    and the game's fall-death camera off."""
    hit = L.find(i, r"revive view \(%s, [2-9]\.\d s after\)" % re.escape(what), after, after + 40)
    m = REVIVE_VIEW.search(hit[1]) if hit else None
    if not m:
        v.step(tag + " camera on its chr after the revive", False, "no 3 s 'revive view (%s' line on inst%d" % (what, i))
        return
    cam, drawn, latch = float(m.group("cam")), float(m.group("drawn")), int(m.group("latch"))
    v.step(tag + " camera on its chr after the revive (within 12 m, the fall-death camera off)",
           latch == 0 and cam == cam and cam <= 12.0, "camera %.1f m from the chr, fall-death camera %d: %s" %
           (cam, latch, m.group("verdict")[:80]))
    # fallcam 2: level as after a load, not the fall camera's steep look down (fdj r3: -0.78 .. -0.98)
    look = re.search(r"looking \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", hit[1])
    ly = float(look.group(2)) if look else float("nan")
    v.step(tag + " camera level after the revive (view down at most 0.4)", ly == ly and ly >= -0.4,
           look.group(0) if look else "no look direction")
    # Logged only (fl run 1: ChrCtrl +0xC0 is a rotation row, not the drawn position); the screenshots
    # show whether the chr is drawn at its spot.
    v.step(tag + " ChrCtrl +0xC0 logged (not judged)", True,
           "%.2f m off the physics position (chr %s)" % (drawn, m.group("chr")))


def check_fall_deep(v, L, tag, hp0_rx, walk):
    """fd / fdj: a guest's real fall death, up at a far lamp of the loaded map with no load. The
    fall-death camera rows are real checks only when the game had its fall-death camera on at the
    revive (the safe-spot fields line: WorldChrMan +0x68 1); a fall the game took as plain landing
    damage (fd run r2) never sets it: then they are info rows."""
    xyz = r"\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    hp0 = need(v, L, tag + " guest died from a fall (not the kill plane)", 1, hp0_rx)
    if hp0 is None:
        check_common(v, L, tag)
        return
    up = need(v, L, tag + " guest up at a lamp of the loaded map, no load", 1,
              r"guest: died from a fall .*up at (its own|the host's) last lamp", hp0)
    if up is not None:
        line = L.find(1, r"guest: died from a fall", hp0)[1]
        at = re.search(r"HP 0 at " + xyz, line)
        lamp = re.search(r"last lamp \d+ " + xyz, line)
        d = sum((float(a) - float(b)) ** 2 for a, b in zip(at.groups(), lamp.groups())) ** 0.5 if at and lamp else -1.0
        v.step(tag + " the lamp is far from the fall (at least 40 m)", d >= 40.0, "%.1f m: %s" % (d, line[:140]))
        fields = L.find(1, r"guest revive: the game's safe-spot revive fields: WorldChrMan \+0x68 (\d)", up, up + 5)
        on = bool(fields) and re.search(r"\+0x68 (\d)", fields[1]).group(1) == "1"
        rel = L.find(1, r"guest revive: the game's fall-death camera released \(WorldChrMan \+0x68 1 -> 0\)", up, up + 5)
        if on:
            v.step(tag + " the game's fall-death camera was on at the revive (the report's case)", True, fields[1][:150])
            v.step(tag + " fall-death camera released by the revive", bool(rel),
                   ("+%.1f s: %s" % (rel[0], rel[1][:120])) if rel else "no release line after the +0x68 1 revive")
        else:
            v.step(tag + " the game's fall-death camera at the revive (info: not on, the case not reproduced)", True,
                   fields[1][:150] if fields else "no safe-spot fields line")
            v.step(tag + " fall-death camera release (info: nothing to release)", True,
                   "released" if rel else "no release line (the camera was not on)")
        check_revive_view(v, L, tag + " guest", 1, "guest revive", up)
        # until the chain's next move (fdj puts it back on the ledge for the next direction)
        nxt = L.find(1, r"travel step \d+/\d+: guest_move", up)
        again = L.find(1, r"local death: HP 0", up + 0.5, min(up + 8, nxt[0]) if nxt else up + 8)
        v.step(tag + " guest alive after it (no second death within 8 s)", not again, again[1][:100] if again else "none")
        none_of(v, L, tag + " no loading screen after the death", r"Party B2: load started", hp0, up + 20, (1,))
        none_of(v, L, tag + " guest stays in the host's world (no session leave)", r"session client \(6, sub 0\) ->", hp0,
                up + 20, (1,))
        none_of(v, L, tag + " never the Hunter's Dream", r"map - -> m21_", hp0, up + 30, (1,))
        if walk:
            # the walk off the lamp: the player's stick moves it (camera-relative controls)
            st = L.find(1, r"travel step \d+/\d+: guest_stick", up)
            ps = [tuple(float(x) for x in POS_RX.search(l).groups()) for _, l in L.all(1, POS_RX.pattern, st[0], st[0] + 20)] if st else []
            moved = max((sum((a - b) ** 2 for a, b in zip(q, ps[0])) ** 0.5 for q in ps), default=0.0) if ps else 0.0
            v.step(tag + " guest walks off the lamp after the revive (at least 2 m)", moved >= 2.0,
                   "%.1f m in %d samples" % (moved, len(ps)) if st else "no guest_stick step after the revive")
    check_common(v, L, tag)


def check_fd(v, L):
    check_fall_deep(v, L, "fd", r"local death: HP 0 at \([-\d.]+, -[1-4]\d\.\d, [-\d.]+\), dropped (1[5-9]|[2-9]\d)\.",
                    True)


def check_fdj(v, L):
    # the report's shaft beside Djura's tower (m23_00): HP 0 in the shaft after a drop of 10 m or more
    check_fall_deep(v, L, "fdj", r"local death: HP 0 at \([-\d.]+, -[5-9]\d\.\d, -6\d\.\d\), dropped ([1-9]\d)\.",
                    False)


def check_ld(v, L):
    """fallcam 3: a death on a ladder, revived in place: the ladder state released, then the stick
    walks to four points around the lamp each move the chr toward its point (deathrules WalkedToward)."""
    need(v, L, "ld join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        on = L.find(i, r"%s_die:ladder: on the ladder \(ladder state (\d+)\)" % who)
        v.step("ld %s on the ladder before the die step (its ladder state set)" % who, bool(on),
               on[1][:120] if on else (L.find(i, r"%s_die:ladder: NOT on a ladder" % who) or (0, "no ladder line"))[1][:120])
        d = L.find(i, r"travel step \d+/\d+: %s_die" % who)
        if not on or not d:
            v.step("ld %s died on the ladder (the die step)" % who, False, "not on the ladder: no death taken")
            continue
        hp0 = L.find(i, r"local death: HP 0 .*ladder state (-?\d+)", d[0] - 1)
        st = int(re.search(r"ladder state (-?\d+)", hp0[1]).group(1)) if hp0 else None
        v.step("ld %s died on the ladder (ladder state at HP 0)" % who, st is not None and st >= 0,
               ("state %s: %s" % (st, hp0[1][:110])) if hp0 else "no HP 0 line")
        up = L.find(i, r"%s revive #\d+: after" % who, d[0])
        v.step("ld %s revived in place (no load)" % who, bool(up), up[1][:120] if up else "no revive")
        if not up:
            continue
        none_of(v, L, "ld %s: no loading screen" % who, r"Party B2: load started", d[0], up[0] + 20, (i,))
        rel = L.find(i, r"%s revive: the chr's ladder state released" % who, d[0], up[0] + 1)
        if st is not None and st >= 0:
            v.step("ld %s ladder state released by the revive" % who, bool(rel), rel[1][:140] if rel else "no release line")
        view = L.find(i, r"revive view \(%s revive, [2-9]\.\d s after\).*ladder state (-?\d+)" % who, up[0], up[0] + 15)
        vs = int(re.search(r"ladder state (-?\d+)", view[1]).group(1)) if view else None
        v.step("ld %s no ladder state 3 s after the revive" % who, vs == -1, ("state %s" % vs) if view else "no view line")
        walks = L.all(i, r"test: %s_stick: walk to \(([-\d.]+), ([-\d.]+)\).*pos \(([-\d.]+), [-\d.]+, ([-\d.]+)\)" % who,
                      up[0])
        names = ["east", "north", "west", "south"]
        for k, (t, line) in enumerate(walks[:4]):
            m = re.search(r"walk to \(([-\d.]+), ([-\d.]+)\).*pos \(([-\d.]+), [-\d.]+, ([-\d.]+)\)", line)
            tx, tz, sx, sz = (float(g) for g in m.groups())
            nxt = walks[k + 1][0] if k + 1 < len(walks) else t + 8
            ps = [POS_RX.search(l) for _, l in L.all(i, POS_RX.pattern, t, nxt)]
            if not ps:
                v.step("ld %s walks %s after the revive" % (who, names[k]), False, "no position samples")
                continue
            ex, ez = float(ps[-1].group(1)), float(ps[-1].group(3))
            dx, dz, wx, wz = ex - sx, ez - sz, tx - sx, tz - sz
            ln = (wx * wx + wz * wz) ** 0.5
            along = (dx * wx + dz * wz) / ln if ln > 0 else 0.0
            v.step("ld %s walks %s after the revive (at least 1.5 m toward it)" % (who, names[k]), along >= 1.5,
                   "%.1f m toward (%.1f, %.1f) from (%.1f, %.1f); moved (%.1f, %.1f)" % (along, tx, tz, sx, sz, dx, dz))
        if len(walks) < 4:
            v.step("ld %s all four walks ran" % who, False, "%d walk(s)" % len(walks))
    check_common(v, L, "ld")


WALL = re.compile(r"\[(\d\d):(\d\d):(\d\d(?:\.\d+)?)\] ")
POS_RX = re.compile(r"test: pos area 0x[0-9a-f]+ \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")


def wall(line):
    """The wall-clock seconds of a Party line ([hh:mm:ss.mmm], the same clock on every instance)."""
    m = WALL.search(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3)) if m else None


def falls(L, i, after=0.0, before=1e9, rate=8.0):
    """Position samples of instance i dropping faster than `rate` m/s (no lift does: ~5 m/s), except
    across a test step (a warp): [(t, vy, line)]."""
    steps = [t for t, _ in L.all(i, r"travel step \d+/\d+: ", after, before)]
    pos = [(t, float(POS_RX.search(l).group(2))) for t, l in L.all(i, POS_RX.pattern, after, before)]
    out = []
    for (t0, y0), (t1, y1) in zip(pos, pos[1:]):
        if t1 <= t0 or any(t0 <= s <= t1 for s in steps):
            continue
        vy = (y1 - y0) / (t1 - t0)
        if vy < -rate:
            out.append((t1, vy))
    return out


def jumps(L, i, t0, t1, axis, radius=1.5, horiz=2.0, rate=16.0):
    """liftfix (the warp check): position samples of instance i between t0 and t1 while its chr is on the car
    (within `radius` m of the car's axis `axis` = (x, z)): pairs that moved more than `horiz` m sideways or
    more than a lift's rate allows up or down (+ `horiz` m), except across a test step: [(t, metres)]."""
    steps = [t for t, _ in L.all(i, r"travel step \d+/\d+: ", t0, t1)]
    pos = []
    for t, line in L.all(i, POS_RX.pattern, t0, t1):
        m = POS_RX.search(line)
        x, y, z = float(m.group(1)), float(m.group(2)), float(m.group(3))
        if ((x - axis[0]) ** 2 + (z - axis[1]) ** 2) ** 0.5 <= radius:
            pos.append((t, x, y, z))
    out = []
    for (ta, xa, ya, za), (tb, xb, yb, zb) in zip(pos, pos[1:]):
        if tb <= ta or any(ta <= s <= tb for s in steps):
            continue
        h = ((xb - xa) ** 2 + (zb - za) ** 2) ** 0.5
        if h > horiz or abs(yb - ya) > rate * (tb - ta) + horiz:
            out.append((tb, max(h, abs(yb - ya))))
    return out


def jump_steps(v, tag, L, rides_by_game, axis, names):
    """One warp-check row per ride and game: the rider and anyone else on the car."""
    for i, rides in enumerate(rides_by_game):
        for k, r in enumerate(rides[:len(names)]):
            if r.get("t") is None:
                continue
            t1 = r.get("t_end") or (r["t"] + 60)
            j = jumps(L, i, r["t"], t1, axis[k] if isinstance(axis[0], tuple) else axis)
            v.step("%s %s: no jump on the car (inst%d)" % (tag, names[k], i), not j,
                   ("; ".join("+%.1f s %.1f m" % x for x in j[:4])) if j else "no jump over 2 m while on the car")


def el_rides(L, i):
    """The library lift's rides on instance i from its car's animations: [{start, up, end, end_anim}]
    (wall clock); a ride starts with anim 6 (down) / 8 (up) and ends when its end anim 9 / 7 stops."""
    rides = []
    for t, line in L.all(i, r"test: object %s anim -?\d+ playing" % EL_CAR):
        m = re.search(r"anim (-?\d+) playing (\d)", line)
        anim, playing, w = int(m.group(1)), m.group(2) == "1", wall(line)
        if anim in (6, 8) and playing and (not rides or rides[-1]["end"] is not None or rides[-1]["up"] != (anim == 8)):
            if rides and rides[-1]["end"] is None:
                rides[-1]["end"], rides[-1]["end_anim"] = w, -1  # a new ride before the old one's end
            rides.append(dict(start=w, t=t, up=anim == 8, end=None, end_anim=None, t_end=None))
        elif anim in (7, 9) and not playing and rides and rides[-1]["end"] is None:
            rides[-1].update(end=w, end_anim=anim, t_end=t)
    return rides


def flag_at(L, i, w):
    """Instance i's 12601253 at wall-clock time w (its last `test: flags` line before it)."""
    val = None
    for _t, line in L.all(i, r"test: flags %s=" % EL_POS):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = int(re.search(r"%s=(-?\d)" % EL_POS, line).group(1))
    return val


def car_anim_at(L, i, w):
    val = None
    for _t, line in L.all(i, r"test: object %s anim -?\d+ playing" % EL_CAR):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = int(re.search(r"anim (-?\d+)", line).group(1))
    return val


RIDE_BIT = 0x0200000000  # PlayerIns +0x1E5 bit 0x02: the chr stands on a moving object (deathrules::kRideFlag1e1)


def rides_with_car(L, i, t0, t1, axis=(119.43, -18.52)):
    """Instance i's chr in the car (within 0.6 m of its axis) between t0 and t1: (samples, samples with the
    ride bit off - the chr is not on the car: falling, or left behind)."""
    n = off = 0
    for _t, line in L.all(i, POS_RX.pattern, t0, t1):
        m = POS_RX.search(line)
        f = re.search(r" f ([0-9a-f]{10})", line)
        if not f or ((float(m.group(1)) - axis[0]) ** 2 + (float(m.group(3)) - axis[1]) ** 2) ** 0.5 > 0.6:
            continue
        n += 1
        off += 0 if int(f.group(1), 16) & RIDE_BIT else 1
    return n, off


def check_el(v, L):
    need(v, L, "el join: 2 players", 0, r"cooperators 0 -> 1")
    names = ["A guest alone down", "B host alone up", "C guest alone down, 2 s in the car",
             "D host alone down in the second cage"]
    rides = [el_rides(L, i) for i in (0, 1)]
    v.step("el rides: %d on each game (no ride without a step into the car)" % len(names),
           len(rides[0]) == len(names) and len(rides[1]) == len(names), "host %d, guest %d" % (len(rides[0]), len(rides[1])))
    for k, name in enumerate(names):
        rh = rides[0][k] if k < len(rides[0]) else None
        rg = rides[1][k] if k < len(rides[1]) else None
        if not rh or not rg:
            v.step("el %s: ran on both games" % name, False, "host %s, guest %s" % (bool(rh), bool(rg)))
            continue
        want_up = k % 2 == 1  # (D): the second cage down is the first cage up (anims 8, 5; end 7, pos 1)
        v.step("el %s: one ride on both games (the game's own sync)" % name,
               rh["up"] == want_up and rg["up"] == want_up and abs(rh["start"] - rg["start"]) < 1.0,
               "start host %.2f / guest %.2f, %s" % (rh["start"] % 3600, rg["start"] % 3600, "up" if rh["up"] else "down"))
        if rh["end"] is None or rg["end"] is None:
            v.step("el %s: ended on both games" % name, False, "host end %s, guest end %s" % (rh["end"], rg["end"]))
            continue
        # Settled: 2 s after the later game's end the flag and the car agree everywhere.
        w = max(rh["end"], rg["end"]) + 2.0
        fh, fg = flag_at(L, 0, w), flag_at(L, 1, w)
        ah, ag = car_anim_at(L, 0, w), car_anim_at(L, 1, w)
        end = 1 if want_up else 0
        v.step("el %s: same flag and car end on both games" % name,
               fh == fg == end and ah == ag == (7 if want_up else 9),
               "flag host %s / guest %s (want %d), car anim host %s / guest %s, ends %.1f s apart"
               % (fh, fg, end, ah, ag, abs(rh["end"] - rg["end"])))
        i = 0 if EL_RIDERS[k] == "host" else 1
        r = rides[i][k]
        n, off = rides_with_car(L, i, r["t"] + 0.6, r["t_end"] - 0.6, EL_AXES[k]) if r["t_end"] else (0, 0)
        v.step("el %s: the %s rode in the car all the way" % (name, EL_RIDERS[k]), n >= 10 and off == 0,
               "%d samples in the car, %d off it" % (n, off))
    for i, who in ((0, "host"), (1, "guest")):
        f = falls(L, i)
        v.step("el %s: no fall (vertical speed under 8 m/s)" % who, not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "fastest drop within a lift's speed")
    jump_steps(v, "el", L, rides, EL_AXES, names)
    none_of(v, L, "el no fall recovery on the lift", r"recovery: falling", 0, 1e9, (0, 1))
    none_of(v, L, "el nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    # liftfix: the library lift's ride end became the all-players check once per map load on every game
    # (its four 3[2] "outside" checks: four lines), and later runs dispatch 3[7] (the EMEVD trace)
    for i, who in ((0, "host"), (1, "guest")):
        rw = L.all(i, r"lift ride end: event 12601352 waits until every player is out")
        regions = sorted(set(re.findall(r"out of region (\d+)", "".join(l for _t, l in rw))))
        later = L.all(i, r"EMEVD event 12601352 \[2[5-8]\] 3\[07\]")
        v.step("el %s: the lift's ride end checks every player (rewritten once per region)" % who,
               len(rw) >= 4 and len(rw) % 4 == 0 and regions == ["2602241", "2602242", "2602243", "2602244"] and len(later) >= 4,
               "%d rewrites (%s), %d later 3[07] dispatches" % (len(rw), ",".join(regions), len(later)))
    none_of(v, L, "el no flag put back after this game's own end",
            r"lift %s: flag %s = \d from the host's world during this game's own ride \(car anim [79]\)" % (EL_CAR, EL_POS),
            0, 1e9, (0, 1))
    check_common(v, L, "el")


def lt_rides(L, i):
    """Central Yharnam's lift on instance i: [{start, up, end, t, t_end}] (down: anims 4, 1, end 5; up: 6, 3,
    end 7 once every player is out)."""
    rides = []
    for t, line in L.all(i, r"test: object %s anim -?\d+ playing" % LT_CAR):
        m = re.search(r"anim (-?\d+) playing (\d)", line)
        anim, playing, w = int(m.group(1)), m.group(2) == "1", wall(line)
        if anim in (4, 6) and playing and (not rides or rides[-1]["end"] is not None):
            rides.append(dict(start=w, t=t, up=anim == 6, end=None, t_end=None, t_arr=None))
        elif anim in (1, 3) and not playing and rides and rides[-1]["end"] is None:
            rides[-1]["t_arr"] = rides[-1]["t_arr"] or t
        elif anim in (5, 7) and not playing and rides and rides[-1]["end"] is None:
            rides[-1].update(end=w, t_end=t)
    return rides


def lt_flags_at(L, i, w):
    val = None
    for _t, line in L.all(i, r"test: flags %s=" % LT_POS):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = tuple(int(x) for x in re.findall(r"=(-?\d)", line)[:2])
    return val


def check_lt(v, L):
    need(v, L, "lt join: 2 players", 0, r"cooperators 0 -> 1")
    rides = [lt_rides(L, i) for i in (0, 1)]
    v.step("lt rides: 2 on each game", len(rides[0]) == 2 and len(rides[1]) == 2,
           "host %d, guest %d" % (len(rides[0]), len(rides[1])))
    for k, name in enumerate(["A together down", "B together up"]):
        rh = rides[0][k] if k < len(rides[0]) else None
        rg = rides[1][k] if k < len(rides[1]) else None
        if not rh or not rg or rh["end"] is None or rg["end"] is None:
            v.step("lt %s: ran and ended on both games" % name, False, "host %s, guest %s" % (rh, rg))
            continue
        up = k == 1
        v.step("lt %s: one ride on both games" % name, rh["up"] == up and rg["up"] == up and
               abs(rh["start"] - rg["start"]) < 1.0, "start host %.2f / guest %.2f" % (rh["start"] % 3600, rg["start"] % 3600))
        w = max(rh["end"], rg["end"]) + 0.5  # the next ride may start ~1 s after
        fh, fg = lt_flags_at(L, 0, w), lt_flags_at(L, 1, w)
        want = (1 if up else 0, 0)
        ah, ag = car_anim_lt(L, 0, w), car_anim_lt(L, 1, w)
        v.step("lt %s: same flags and car end on both games" % name, fh == fg == want and ah == ag == (7 if up else 5),
               "pos/busy host %s / guest %s (want %s), car anim host %s / guest %s" % (fh, fg, want, ah, ag))
        for i, who in ((0, "host"), (1, "guest")):
            r = rides[i][k]
            arr = r["t_arr"]
            pos = L.all(i, POS_RX.pattern, arr - 1.0, arr + 0.05) if arr else []
            y = float(POS_RX.search(pos[-1][1]).group(2)) if pos else None
            far = (y is not None) and (y > -46.0 if up else y < -56.0)
            n = off = 0
            for _t, line in L.all(i, POS_RX.pattern, r["t"] + 0.6, (arr or r["t"]) - 0.3):
                m = POS_RX.search(line)
                f = re.search(r" f ([0-9a-f]{10})", line)
                if f and ((float(m.group(1)) + 128.72) ** 2 + (float(m.group(3)) + 9.73) ** 2) ** 0.5 < 1.5:
                    n += 1
                    off += 0 if int(f.group(1), 16) & RIDE_BIT else 1
            v.step("lt %s: the %s rode along to the %s" % (name, who, "top" if up else "bottom"), far and n >= 3 and off == 0,
                   "y %s at the arrival, %d samples on the car, %d off it" % ("%.1f" % y if y is not None else "-", n, off))
    for i, who in ((0, "host"), (1, "guest")):
        f = falls(L, i)
        v.step("lt %s: no fall (vertical speed under 8 m/s)" % who, not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "fastest drop within a lift's speed")
    jump_steps(v, "lt", L, rides, (-128.72, -9.73), ["A together down", "B together up"])
    none_of(v, L, "lt nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    none_of(v, L, "lt no fall recovery", r"recovery: falling", 0, 1e9, (0, 1))
    check_common(v, L, "lt")


def car_anim_lt(L, i, w):
    val = None
    for _t, line in L.all(i, r"test: object %s anim -?\d+ playing" % LT_CAR):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = int(re.search(r"anim (-?\d+)", line).group(1))
    return val


def ch_rides(L, i):
    """Cainhurst's lift on instance i: [{start, up, end, t, t_arr, t_end}] (down 0 -> end 1, up 10 -> end 11)."""
    rides = []
    for t, line in L.all(i, r"test: object %s anim -?\d+ playing" % CH_CAR):
        m = re.search(r"anim (-?\d+) playing (\d)", line)
        anim, playing, w = int(m.group(1)), m.group(2) == "1", wall(line)
        if anim in (0, 10) and playing and (not rides or rides[-1]["end"] is not None):
            rides.append(dict(start=w, t=t, up=anim == 10, end=None, t_end=None, t_arr=None))
        elif anim in (0, 10) and not playing and rides and rides[-1]["end"] is None:
            rides[-1]["t_arr"] = rides[-1]["t_arr"] or t
        elif anim in (1, 11) and not playing and rides and rides[-1]["end"] is None:
            rides[-1].update(end=w, t_end=t)
    return rides


def ch_at(L, i, w):
    """(pos, busy) and the car's animation on instance i at wall time w."""
    flags = anim = None
    for _t, line in L.all(i, r"test: flags %s=" % CH_POS):
        if wall(line) is not None and wall(line) <= w:
            flags = tuple(int(x) for x in re.findall(r"=(-?\d)", line)[:2])
    for _t, line in L.all(i, r"test: object %s anim -?\d+ playing" % CH_CAR):
        if wall(line) is not None and wall(line) <= w:
            anim = int(re.search(r"anim (-?\d+)", line).group(1))
    return flags, anim


def check_ch(v, L):
    need(v, L, "ch join: 2 players", 0, r"cooperators 0 -> 1")
    names = ["A guest alone down", "B guest alone up, 2 s in the car", "C host alone down"]
    rides = [ch_rides(L, i) for i in (0, 1)]
    v.step("ch rides: 3 on each game (no ride without a step into the car)", len(rides[0]) == 3 and len(rides[1]) == 3,
           "host %d, guest %d" % (len(rides[0]), len(rides[1])))
    for k, name in enumerate(names):
        rh = rides[0][k] if k < len(rides[0]) else None
        rg = rides[1][k] if k < len(rides[1]) else None
        if not rh or not rg or rh["end"] is None or rg["end"] is None:
            v.step("ch %s: ran and ended on both games" % name, False, "host %s, guest %s" % (rh, rg))
            continue
        up = k == 1
        v.step("ch %s: one ride on both games (the game's own sync)" % name, rh["up"] == up and rg["up"] == up and
               abs(rh["start"] - rg["start"]) < 1.0, "start host %.2f / guest %.2f" % (rh["start"] % 3600, rg["start"] % 3600))
        w = max(rh["end"], rg["end"]) + 0.5
        (fh, ah), (fg, ag) = ch_at(L, 0, w), ch_at(L, 1, w)
        want = (0 if up else 1, 0)  # 12500074: 1 = the car at the bottom
        v.step("ch %s: same flags and car end on both games" % name, fh == fg == want and ah == ag == (11 if up else 1),
               "pos/busy host %s / guest %s (want %s), car anim host %s / guest %s, ends %.1f s apart"
               % (fh, fg, want, ah, ag, abs(rh["end"] - rg["end"])))
        i = 0 if CH_RIDERS[k] == "host" else 1
        r = rides[i][k]
        arr = r["t_arr"]
        pos = L.all(i, POS_RX.pattern, arr - 1.0, arr + 0.05) if arr else []
        y = float(POS_RX.search(pos[-1][1]).group(2)) if pos else None
        far = y is not None and (y > 63.0 if up else y < 33.0)
        n = off = 0
        for _t, line in L.all(i, POS_RX.pattern, r["t"] + 0.6, (arr or r["t"]) - 0.3):
            m = POS_RX.search(line)
            f = re.search(r" f ([0-9a-f]{10})", line)
            if f and ((float(m.group(1)) + 22.78) ** 2 + (float(m.group(3)) + 237.74) ** 2) ** 0.5 < 1.2:
                n += 1
                off += 0 if int(f.group(1), 16) & RIDE_BIT else 1
        # the other player stays off the car: its height does not follow the ride, no ride bit
        o = 1 - i
        ro = rides[o][k]
        ys, bits = [], 0
        for _t, line in L.all(o, POS_RX.pattern, ro["t"] + 0.6, (ro["t_arr"] or ro["t"]) - 0.3):
            ys.append(float(POS_RX.search(line).group(2)))
            fo = re.search(r" f ([0-9a-f]{10})", line)
            bits += 1 if fo and int(fo.group(1), 16) & RIDE_BIT else 0
        v.step("ch %s: the %s stayed off the car" % (name, "host" if o == 0 else "guest"),
               len(ys) >= 3 and max(ys) - min(ys) < 1.0 and bits == 0,
               "%d samples, height %s, %d with the ride bit" % (len(ys), ("%.1f..%.1f" % (min(ys), max(ys))) if ys else "-", bits))
        v.step("ch %s: the %s rode in the car to the %s" % (name, CH_RIDERS[k], "top" if up else "bottom"),
               far and n >= 5 and off == 0,
               "y %s at the arrival, %d samples in the car, %d off it" % ("%.1f" % y if y is not None else "-", n, off))
    for i, who in ((0, "host"), (1, "guest")):
        # from the first warp to the lift on: the approach counts too
        start = L.find(i, r"_move to \(-(19|2\d)\.")
        f = falls(L, i, start[0] - 0.1 if start else 0.0, rate=16.0)  # this car peaks at 12.7 m/s (run 5)
        v.step("ch %s: no fall from the approach on (vertical speed under 16 m/s)" % who, start is not None and not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else
               ("fastest drop within a lift's speed" if start else "never reached the lift"))
        rw = L.all(i, r"lift ride end: event 1250007[67] waits until every player is out")
        later = L.all(i, r"EMEVD event 1250007[67] \[12\] 3\[07\]")
        v.step("ch %s: the lift's ride end checks every player (rewritten once per event)" % who,
               len(rw) == 2 and len(later) >= 1, "%d rewrites, %d later 3[07] dispatches" % (len(rw), len(later)))
    jump_steps(v, "ch", L, rides, (-22.78, -237.74), names)
    none_of(v, L, "ch nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    none_of(v, L, "ch no fall recovery", r"recovery: falling", 0, 1e9, (0, 1))
    check_common(v, L, "ch")


def rh_rides(L, i):
    """The Research Hall lift on instance i: [{start, up, end, t, t_arr, t_end}] (down 1, 2, end 3; up 5, 6, end 7)."""
    rides = []
    for t, line in L.all(i, r"test: object %s anim -?\d+ playing" % RH_CAR):
        m = re.search(r"anim (-?\d+) playing (\d)", line)
        anim, playing, w = int(m.group(1)), m.group(2) == "1", wall(line)
        # a far game's car (not active: the guest 40 m away) runs its copy's animations at once, never
        # "playing": a ride starts with anim 1 / 5 either way
        if anim in (1, 5) and (not rides or rides[-1]["end"] is not None):
            rides.append(dict(start=w, t=t, up=anim == 5, end=None, t_end=None, t_arr=None))
        elif anim in (2, 6) and not playing and rides and rides[-1]["end"] is None:
            rides[-1]["t_arr"] = rides[-1]["t_arr"] or t
        elif anim in (3, 7) and not playing and rides and rides[-1]["end"] is None:
            rides[-1].update(end=w, t_end=t)
    return rides


def check_rh(v, L):
    need(v, L, "rh join: 2 players", 0, r"cooperators 0 -> 1")
    names = ["A host alone down", "B host alone up", "C host and guest down", "D host and guest up"]
    riders = [(0,), (0,), (0, 1), (0, 1)]
    rides = [rh_rides(L, i) for i in (0, 1)]
    v.step("rh rides: 4 on each game", len(rides[0]) == 4 and len(rides[1]) == 4,
           "host %d, guest %d" % (len(rides[0]), len(rides[1])))
    # every ride start (the car's ride animation 1 / 5 begins): 4, none without a step into the car
    starts = []
    for t, l in L.all(0, r"test: object %s anim [15] playing 1" % RH_CAR):
        if not starts or t - starts[-1] > 3.0:
            starts.append(t)
    v.step("rh the car never leaves on its own (4 ride starts on the host)", len(starts) == 4,
           "%d starts at %s" % (len(starts), ", ".join("+%.1f s" % t for t in starts)))
    v.step("rh the busy flag clears after the end animation",
           bool(L.all(0, r"lift ride end: event 1350111[23] clears busy")),
           "%d lines" % len(L.all(0, r"lift ride end: event 1350111[23] clears busy")))
    for k, name in enumerate(names):
        rh_ = rides[0][k] if k < len(rides[0]) else None
        rg = rides[1][k] if k < len(rides[1]) else None
        if not rh_ or not rg or rh_["end"] is None or rg["end"] is None:
            v.step("rh %s: ran and ended on both games" % name, False, "host %s, guest %s" % (rh_, rg))
            continue
        up = k in (1, 3)
        v.step("rh %s: one ride on both games" % name, rh_["up"] == up and rg["up"] == up and
               abs(rh_["start"] - rg["start"]) < 1.0, "start host %.2f / guest %.2f" % (rh_["start"] % 3600, rg["start"] % 3600))
        w = max(rh_["end"], rg["end"]) + 0.5
        vals = []
        for i in (0, 1):
            fl = an = None
            for _t, line in L.all(i, r"test: flags %s=" % RH_POS):
                if wall(line) is not None and wall(line) <= w:
                    fl = tuple(int(x) for x in re.findall(r"=(-?\d)", line)[:2])
            for _t, line in L.all(i, r"test: object %s anim -?\d+ playing" % RH_CAR):
                if wall(line) is not None and wall(line) <= w:
                    an = int(re.search(r"anim (-?\d+)", line).group(1))
            vals.append((fl, an))
        want = ((1 if up else 0, 0), 7 if up else 3)
        v.step("rh %s: same flags and car end on both games" % name, vals[0] == vals[1] == want,
               "host %s / guest %s (want %s)" % (vals[0], vals[1], want))
        for i in riders[k]:
            r = rides[i][k]
            arr = r["t_arr"]
            pos = L.all(i, POS_RX.pattern, arr - 1.0, arr + 0.05) if arr else []
            y = float(POS_RX.search(pos[-1][1]).group(2)) if pos else None
            far = y is not None and (y > 1582.0 if up else 1557.0 < y < 1560.0)
            n = off = 0
            for _t, line in L.all(i, POS_RX.pattern, r["t"] + 0.6, (arr or r["t"]) - 0.3):
                m = POS_RX.search(line)
                f = re.search(r" f ([0-9a-f]{10})", line)
                if f and ((float(m.group(1)) + 345.0) ** 2 + (float(m.group(3)) + 792.5) ** 2) ** 0.5 < 2.5:
                    n += 1
                    off += 0 if int(f.group(1), 16) & RIDE_BIT else 1
            who = "host" if i == 0 else "guest"
            v.step("rh %s: the %s rode in the car to the %s" % (name, who, "top" if up else "bottom"),
                   far and n >= 5 and off == 0,
                   "y %s at the arrival, %d samples on the car, %d off it" % ("%.1f" % y if y is not None else "-", n, off))
    jump_steps(v, "rh", L, rides, (-345.0, -792.5), names)
    for i, who in ((0, "host"), (1, "guest")):
        f = falls(L, i, rate=16.0)
        v.step("rh %s: no fall (vertical speed under 16 m/s)" % who, not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "fastest drop within a lift's speed")
        g = L.all(i, r"lift ride: the local chr dropped through the moving car")
        v.step("rh %s: rider guard put-backs (a drop through the car, logged)" % who, True,
               ("%d: " % len(g)) + "; ".join(l[:140] for _t, l in g[:3]) if g else "none needed")
    none_of(v, L, "rh no step condition timed out", r"condition timed out", 0, 1e9, (0, 1))
    none_of(v, L, "rh nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    none_of(v, L, "rh no fall recovery", r"recovery: falling", 0, 1e9, (0, 1))
    check_common(v, L, "rh")


def check_mk_host(v, L, guests_from):
    """The host's Marks (MK_HOST_CASES): the question and its answer, then per the travel rules: in
    its own map no load for anyone (YES: both guests follow in-world; NO: they stay); from another
    map (YES) one load each into m24_00, every guest placed beside the host."""
    uses = L.all(0, r"test: host_use goods (100|1400) ")
    for k, (name, goods, answer, here) in enumerate(MK_HOST_CASES):
        u = uses[k] if k < len(uses) else None
        v.step("mk %s: used (host)" % name, u and ("goods %d " % goods) in u[1],
               ("+%.1f s: %s" % (u[0], u[1][:150])) if u else "case never ran")
        if not u:
            continue
        t = u[0]
        # in its own map: until the host's next step (the next walk or the travel to the Dream)
        nxt = L.find(0, r"travel step \d+/\d+: host_(stick|warp)", t)
        end = nxt[0] if here and nxt else t + 60
        if guests_from > t:
            end = min(end, guests_from)
        need(v, L, "mk %s: \"Travel with the party?\" asked" % name, 0,
             r"lamp %s: asking \"Travel with the party\?\"" % MK_LAMP, t, end)
        need(v, L, "mk %s: answered %s" % (name, answer), 0,
             r"lamp %s \(host\): .*-> travel %s" % (MK_LAMP, "with the party" if answer == "yes" else "alone"), t, end)
        if here:
            if answer == "yes":
                need(v, L, "mk %s: the host's in-world party move" % name, 0,
                     r"host: party move to lamp %s inside the loaded map: no load" % MK_LAMP, t, end)
                for g in (1, 2):
                    f = need(v, L, "mk %s: guest %d follows in-world" % (name, g), g,
                             r"guest: the host moved to lamp %s inside its map \(no load\); following" % MK_LAMP, t, end)
                    need(v, L, "mk %s: guest %d warped beside it, no load" % (name, g), g,
                         r"in-world warp \(the host's party move, lamp %s\): .*no load" % MK_LAMP, f or t, end)
            else:
                need(v, L, "mk %s: the host's own in-world warp" % name, 0, r"in-world warp \(own move, lamp %s\): .*no load" % MK_LAMP, t, end)
                none_of(v, L, "mk %s: the guests stay" % name, r"guest: (the host moved to lamp|followed the host)|in-world warp \(the host's party move",
                        t, end, (1, 2))
            none_of(v, L, "mk %s: no load for anyone" % name, r"map \S+ -> ", t, end)
        else:
            for i in (0, 1, 2):
                loads = L.all(i, r"map - -> ", t, end)
                v.step("mk %s: inst%d one load into m24_00" % (name, i),
                       len(loads) == 1 and "m24_00" in loads[0][1],
                       "; ".join("+%.1f s %s" % (x[0], x[1][:60]) for x in loads) or "no load")
            for g in (1, 2):
                need(v, L, "mk %s: guest %d placed beside the host" % (name, g), g,
                     r"guest: (arrived by the host's own travel|arrived in the host world)", t, end)
            need(v, L, "mk %s: host: the guests arrived" % name, 0,
                 r"a guest arrived; \d+ member task\(s\) rewound", t, end)


def check_mk(v, L):
    need(v, L, "mk join: 3 players", 0, r"cooperators [01] -> 2")
    need(v, L, "mk party at the Cathedral Ward lamp (guest 1)", 1, r"map - -> m24_00")
    g0 = L.find(1, r"test: guest_use goods (100|1400) ")
    check_mk_host(v, L, g0[0] if g0 else 1e9)
    # each case's start on guest 1: its Mark item use, or (4) its lamp step from the Dream
    starts = L.all(1, r"test: guest_use goods (100|1400) |travel step \d+/\d+: guest_lamp.*map m21_00")
    for k, (name, kind) in enumerate(MK_CASES):
        u = starts[k] if k < len(starts) else None
        ok = u and ("applied" in u[1] if kind != "rest" else True)
        v.step("mk %d %s: used (guest 1)" % (k + 1, name), ok,
               ("+%.1f s: %s" % (u[0], u[1][:150])) if u else "case %d never ran" % (k + 1))
        if not u:
            continue
        end = starts[k + 1][0] if k + 1 < len(starts) else 1e9
        back = need(v, L, "mk %d guest 1 back in the host's map" % (k + 1), 1, r"map - -> m24_00", u[0], end)
        if kind == "rest":
            rest = need(v, L, "mk %d host: the rest load the re-join carries" % (k + 1), 0,
                        r"slot \d rested at lamp %s" % MK_LAMP, u[0] - 1, end)
            need(v, L, "mk %d guest 2 followed the rest load" % (k + 1), 2, r"map - -> m24_00", rest or u[0], end)
            need(v, L, "mk %d host: arrivals, member tasks rewound" % (k + 1), 0,
                 r"a guest arrived; \d+ member task\(s\) rewound", rest or u[0], end)
        else:
            if kind == "mark_here":
                none_of(v, L, "mk %d host stays (no rest load for a Mark)" % (k + 1), r"rested at lamp %s" % MK_LAMP,
                        u[0], back or end, (0,))
            need(v, L, "mk %d host: guest 1 arrived" % (k + 1), 0, r"host: slot \d arrived", u[0], end)
    none_of(v, L, "mk no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "mk")


def check_kd(v, L):
    need(v, L, "kd join: 3 players", 0, r"cooperators [01] -> 2")
    need(v, L, "kd party at the Cathedral Ward lamp (guest 1)", 1, r"map - -> m24_00")
    gd = need(v, L, "kd guest 1 death with 9420 set (scripted)", 1, r"travel step \d+/\d+: guest_die")
    if gd is not None:
        need(v, L, "kd guest 1: a normal party death (revived)", 1,
             r"guest: respawned at the host's lamp|guest: died alone: revived|revived at", gd)
    # kidnapall: guest 1's grab takes the party to the Gaol (Anyone can be abducted, on by default)
    ga = need(v, L, "kd guest 1 grabbed (scripted)", 1, r"kidnap: a Snatcher grabbed this guest", gd or 0)
    if gd is not None and ga is not None:
        none_of(v, L, "kd guest 1's plain death: nobody to the Gaol", r"map .*-> m28_00", gd, ga)
    hd = need(v, L, "kd host grab-death", 0, HOST_DIED, ga or 0)
    if ga is not None:
        gs = need(v, L, "kd host: guest 1's grab starts the party's kidnap", 0,
                  r"kidnap: slot \d+ was grabbed by a Snatcher .*: start", ga - 5)
        gt = need(v, L, "kd host: 9421 moved the respawn point, the party's travel", 0,
                  r"kidnap: the host's respawn point is the Gaol", gs if gs is not None else ga - 5)
        need(v, L, "kd guest 1: follows once its death is taken", 1,
             r"the party's kidnap travel to the Gaol .*follows once its death is taken", ga)
        end = hd if hd is not None else 1e9
        g28 = {}
        for i in (0, 1, 2):
            g28[i] = need(v, L, "kd (guest grab) inst %d in the Gaol (m28_00)" % i, i, r"map .*-> m28_00", ga, end)
        none_of(v, L, "kd (guest grab): no host death / revive", r"host death taken|host death: ", ga, end - 1,
                insts=(0,))
        need(v, L, "kd (guest grab) host: the waking scene 28000010", 0, r"remo 2800[01]010|cutscene 28000010",
             g28[0] if g28[0] is not None else ga, end)
        need(v, L, "kd (guest grab) host: +1 Insight from 9422", 0, r"event 9422 gave the host 1 Insight", ga, end)
        for i in (1, 2):
            if g28[i] is not None:
                need(v, L, "kd (guest grab) guest %d: the waking scene in the cell" % i, i, r"remo 2800[01]010 mode",
                     g28[i], end)
                need(v, L, "kd (guest grab) guest %d: +1 Insight (9422)" % i, i,
                     r"event 9422, 1 Insight\): \+1 here", ga, end)
        need(v, L, "kd guest 1's own save: no kidnap trigger after the grab", 1,
             r"test read \(load mode \d, own save / visible\): 9420=0/", ga)
        none_of(v, L, "kd the guests' own saves never hold 9420",
                r"test read \(load mode \d, own save / visible\): 9420=1/", insts=(1, 2))
        if gt is None:
            check_common(v, L, "kd")
            return
    if hd is None:
        check_common(v, L, "kd")
        return
    need(v, L, "kd host: the kidnap is the party's respawn load", 0,
         r"host death: .*the respawn load for the whole party at lamp 2802959", hd - 15)
    none_of(v, L, "kd host: no revive / lone load", r"host death taken|the host loads it alone", hd - 15, insts=(0,))
    m0 = need(v, L, "kd host in the Gaol (m28_00)", 0, r"map .*-> m28_00", hd)
    need(v, L, "kd host: the waking scene 28000010", 0, r"28000010", hd)
    for i in (1, 2):
        mi = need(v, L, "kd guest %d in the Gaol (m28_00)" % i, i, r"map .*-> m28_00", hd)
        if mi is not None:
            need(v, L, "kd guest %d beside the host" % i, i, r"placed beside the host", mi)
            # kidnapall: the waking scene for every member (user decision 2026-10-22); a guest that saw
            # it at guest 1's grab earlier in this run keeps only its flags now (seen once, as in the game)
            if L.find(i, r"remo 2800[01]010 mode", 0, hd) is None:
                need(v, L, "kd guest %d: the waking scene in the cell" % i, i, r"remo 2800[01]010 mode", mi)
            else:
                v.step("kd guest %d: the waking scene seen at the guest grab (not again)" % i,
                       L.find(i, r"remo 2800[01]010 mode", mi) is None, "")
        # a live mirror of the waking scene on a guest not (yet) in m28 is the report's broken scene
        none_of(v, L, "kd guest %d: no live mirror of 28000010 outside m28" % i,
                r"live mirror: remo 2800(0|1)010", hd, mi if mi is not None else 1e9, insts=(i,))
    none_of(v, L, "kd no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "kd")


def check_kw(v, L):
    need(v, L, "kw join: 3 players", 0, r"cooperators [01] -> 2")
    sc = need(v, L, "kw host: the Gaol scene played outside m28 (test)", 0, r"remo 2800(0|1)010 mode")
    for i in (1, 2):
        if sc is not None:
            need(v, L, "kw guest %d: no mirror of a map it has not loaded" % i, i,
                 r"live mirror of remo 2800(0|1)010 dropped: its map is not loaded", sc - 1)
        # its missed-scene replay may play once the guest stands in m28 (the scene's map), never before
        m28 = L.find(i, r"map .*-> m28_00")
        none_of(v, L, "kw guest %d: the Gaol scene not played outside m28" % i, r"remo 2800(0|1)010 mode",
                0, m28[0] if m28 else 1e9, insts=(i,))
    dl = need(v, L, "kw DLC entry: the host's story warp 3402959", 0, r"test: host_lamp 3402959 as a story warp")
    if dl is not None:
        for i in (0, 1, 2):
            need(v, L, "kw DLC entry: inst%d in the Hunter's Nightmare" % i, i, r"map .*-> m34_00", dl)
    am = need(v, L, "kw Amygdala grab: guest 1's story warp 3202958", 1, r"test: guest_lamp 3202958 as a story warp")
    if am is not None:
        for i in (0, 1, 2):
            need(v, L, "kw Amygdala grab: inst%d in the Lecture Building" % i, i, r"map .*-> m32_00", am)
    none_of(v, L, "kw no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "kw")


def check_cl(v, L):
    need(v, L, "cl join: guest 1 summoned into the dungeon", 1, r"map .* -> m29_00")
    need(v, L, "cl host lit the boss room's lamp (its spot seen)", 0,
         r"lamp spot: Chalice Dungeon lamp %s lit / used" % CL_LAMP)
    gd = need(v, L, "cl guest 1 death", 1, r"travel step \d+/\d+: guest_die")
    if gd:
        need(v, L, "cl guest 1 up beside the host, no load", 1, r"up again beside the nearest living member", gd)
        none_of(v, L, "cl guest 1 never to its own lamp outside",
                r"travels alone to its own last lamp|died alone: revived, then", gd, 1e9, (1,))
    hd = need(v, L, "cl host death", 0, r"travel step \d+/\d+: host_die")
    if hd:
        need(v, L, "cl host revives at the lit lamp's spot", 0,
             r"host death taken: .*revive at lamp %s \(-4\d\.\d, 3[01]\.\d" % CL_LAMP, hd)
        none_of(v, L, "cl host: no template spot for a dungeon lamp", r"lamp spot: lamp 29\d+ from the game data",
                hd, 1e9, (0,))
    need(v, L, "cl host's dungeon lamp opens (talk answered)", 0,
         r"talk t2901\d\d asks IsMultiplayerInProgress \(Chalice Dungeon lamp: answered 0")
    dream = need(v, L, "cl host: the lamp takes it to the Dream", 0, r"map .* -> m21_00", hd or 0)
    if dream:
        need(v, L, "cl guest 1 to the Dream with the party", 1, r"map .* -> m21_00", dream - 5)
        back = need(v, L, "cl host back in the dungeon at the lamp", 0, r"map .* -> m29_00", dream)
        if back:
            g_back = need(v, L, "cl guest 1 summoned back into the dungeon", 1, r"map .* -> m29_00", back)
            if g_back:
                need(v, L, "cl guest 1 forced out: sent home by itself", 1, r"chalice: this guest is apart in", back - 5,
                     g_back)
                chose = need(v, L, "cl guest 1 chose to leave (alone to the Dream)", 1, r"chalice: the guest's own choice",
                             g_back)
                if chose:
                    need(v, L, "cl guest 1 stays out with the notice", 1, r"chose to leave the host's Chalice Dungeon", chose)
                    press = need(v, L, "cl guest 1 Party > Rejoin pressed", 1, r"Party > Rejoin pressed now", chose)
                    none_of(v, L, "cl guest 1 not sent home before its Rejoin", r"chalice: this guest is apart in", chose,
                            press or 1e9, (1,))
                    if press:
                        need(v, L, "cl Rejoin: home first", 1, r"chalice: Party > Rejoin while apart", press - 1)
                        need(v, L, "cl guest 1 back in the dungeon after Rejoin", 1, r"map .* -> m29_00", press)
    # (8) Guest 1's current dungeon (FrpgNetMan +0xa88) in the host's dungeon: the host's, never its own.
    first = need(v, L, "cl join: guest 1 summoned into the dungeon (for +0xa88)", 1, r"map .* -> m29_00")
    if first and CL_GUEST_FIXTURE.is_dir():
        need(v, L, "cl guest 1's current dungeon is the host's (+0xa88 retargeted)", 1,
             r"chalice: in the host's dungeon: the game's current dungeon \(FrpgNetMan \+0xa88\) is the quick-joined slot 0",
             first - 5)
        need(v, L, "cl guest 1's +0xa88 holds the host's dungeon", 1,
             r"chalice: FrpgNetMan ChannelId %s .*the host's dungeon" % CL_HOST_CH, first - 5)
        none_of(v, L, "cl guest 1's +0xa88 never its own dungeon in the host's",
                r"chalice: FrpgNetMan ChannelId .*NOT the host's dungeon", 0, 1e9, (1,))
        left = need(v, L, "cl guest 1 left (Party > Leave)", 1, r"guest_leave: Party > Leave pressed", first)
        if left:
            own = need(v, L, "cl guest 1 into its own dungeon from the altar", 1, r"travel step \d+/\d+: guest_lamp",
                       left)
            if own:
                need(v, L, "cl guest 1 in its own dungeon (m29)", 1, r"map .* -> m29_00", own)
                need(v, L, "cl guest 1's +0xa88 its own dungeon again", 1, r"chalice: FrpgNetMan ChannelId %s " % CL_GUEST_CH, own)
    # A re-summon into the dungeon whose layer boss is dead never pays it again (cl6 / cl8: PREY
    # SLAUGHTERED, +1995 echoes and +1 Insight at every re-summon).
    none_of(v, L, "cl no second paid kill of 2900100 after a re-summon",
            r"boss clear on this game \(BlockClear2\): boss 2900100|kill seen: entity 2900100 3\.5 s later: .*\(\+[1-9]\d* since\)",
            0, 1e9, (1,))
    none_of(v, L, "cl no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "cl")


def check_clg(v, L):
    need(v, L, "clg join: guest 1 in the host's dungeon", 1, r"map .* -> m29_00")
    need(v, L, "clg host lit the boss room's lamp", 0, r"lamp spot: Chalice Dungeon lamp %s lit / used" % CL_LAMP)
    fall = need(v, L, "clg guest 1 died from the fall", 1, r"local death: HP 0 at")
    if fall:
        need(v, L, "clg guest 1 up beside the host (no lamp spot from the table)", 1,
             r"up again beside the nearest living member", fall)
        none_of(v, L, "clg guest 1 never at a dungeon lamp's table spot", r"up at the host's last lamp 29|"
                r"up at its own last lamp 29", fall, 1e9, (1,))
        none_of(v, L, "clg guest 1 no second fall death after the revive", r"local death: HP 0 at", fall + 1, fall + 20, (1,))
    car = need(v, L, "clg guest 1's game learns the dungeon's lift car", 1, r"Chalice Dungeon lift car 2901004 seen riding")
    if car:
        none_of(v, L, "clg no death on the lift ride", r"local death: HP 0 at", car, car + 15, (0, 1))
    lamp = need(v, L, "clg guest 1's dungeon lamp: Return to the Dream chosen", 1, r"lamp flag 7290010\d set by this game")
    if lamp:
        warp = need(v, L, "clg guest 1's lamp warp runs (12907020 on the guest)", 1,
                    r"lamp / headstone warp 21029|lamp 21029\d\d|warp to 21029\d\d", lamp)
        if warp:
            d = need(v, L, "clg the party to the Dream (host)", 0, r"map .* -> m21_00", lamp)
            need(v, L, "clg the party to the Dream (guest 1)", 1, r"map .* -> m21_00", lamp)
    none_of(v, L, "clg no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "clg")


def lp_save_flags(d: Path, i, ids):
    """Instance i's party save slot 0 (userdata0000): copied into the phase's output first, the copy
    decoded (save_decode.py). {id: bool}, or None without a readable save."""
    sys.path.insert(0, str(REPO / "tools" / "party"))
    import save_decode  # noqa: E402
    src = sorted((Path(RUN_ROOT[0]) / f"inst{i}" / "user" / "savedata_party").rglob("userdata0000"))
    if not src:
        return None
    copy = d / "saves" / f"inst{i}_userdata0000"
    copy.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src[0], copy)
    try:
        sv = save_decode.Save.read(copy)
    except (OSError, ValueError):
        return None
    return {f: bool(sv.flag(int(f))) for f in ids}


def check_lp(v, L, d: Path):
    j = need(v, L, "lp join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "lp guest's own lamps stay out of the host's world", 1,
         r"lamps: the own save's lamps the host's world lacks stay out of it .*lamps.* %s" % LP_OLD[0], j or 0)
    none_of(v, L, "lp host takes none of the guest's own lamps",
            r"own save has lamps this world lacked|lamps: \d+ flag\(s\) of lamps lit in the own save", insts=(0, 1))
    # The arrival: a lamp lit in the host's world (its last lamp), never the unlit one beside it.
    arr = L.find(2, r"arrival lamp in m24_01_00_00: (\d+)")
    v.step("lp guest's arrival: a lamp lit in the host's world", bool(arr) and ("arrival lamp in m24_01_00_00: %s " % LP_HOST_LAMP) in arr[1] + " ",
           arr[1][:200] if arr else "no A5 arrival line (joined without a cross-map load?)")
    press = L.find(1, r"test: Cross at the lamp #1")
    # lamprule run 7: the title-joined guest fell through the map (placed before the floor loaded).
    none_of(v, L, "lp no guest falls out of the world after its join", r"local death: HP 0 at", 0,
            press[0] if press else 1e9, insts=(1, 2))
    none_of(v, L, "lp nothing lit for the guest before its own lamp use",
            r"lamp: slot \d+ lit a lamp in the host's world", 0, press[0] if press else 1e9, insts=(0,))
    lit = need(v, L, "lp guest lights a lamp in the host's world", 1,
               r"lamp: the guest lit a lamp of the host's world \(overlay:.*%s" % LP_NEW[1][2])
    need(v, L, "lp host lights it in its world", 0, r"lamp: slot \d+ lit a lamp in the host's world: set.*%s" % LP_NEW[1][2],
         lit or 0)
    need(v, L, "lp host saves it", 0, r"save requested \(.*a lamp a guest lit in the host's world", lit or 0)
    need(v, L, "lp the guest's own save gets it", 1,
         r"written into the own save \(shared lamps; lamps.* %s" % LP_NEW[0], lit or 0)
    need(v, L, "lp guest's lamp travel list opens", 1, r"test: warp menu up \(listonly\)")
    hit = L.find(1, r"test: warp menu up \(listonly\): \d+ entries:")
    entries = re.findall(r"\b(\d{7})\b", hit[1].split("entries:", 1)[1]) if hit else []
    v.step("lp guest's list: only the host's world's lamps", hit and LP_OLD[0] not in entries and "2412953" in entries,
           "entries %s" % " ".join(entries) if hit else "no list")
    # Every lamp lit in the host's world (its save at the end: lit flag 1AAB7810+20n set, name /
    # blocked flag 7000AABn clear, as lamp_travel_rules.h BuildList) is in the guest's list, but the
    # one it stands at ("lamp N (...): the warp menu").
    src = (REPO / "gpu" / "shim" / "party" / "lamp_travel_rules.h").read_text(encoding="utf-8")
    table = src.split("kLamps[] = {", 1)[1].split("};", 1)[0]
    lamps = [(int(a), int(n)) for a, n in re.findall(r"\{(\d{3}), (\d), \w+\}", table)]
    lit_f = {(a, n): str(10000000 + a * 10000 + 7810 + 20 * n) for a, n in lamps}
    name_f = {(a, n): str(70000000 + a * 10 + n) for a, n in lamps}
    hw = lp_save_flags(d, 0, sorted(set(lit_f.values()) | set(name_f.values())))
    here = L.find(1, r"Lamp travel: lamp (\d+) \(.*the warp menu instead")
    here_id = re.search(r"lamp (\d+) ", here[1]).group(1) if here else ""
    want = sorted(str(a * 10000 + 2950 + n) for a, n in lamps
                  if hw and hw[lit_f[(a, n)]] and not hw[name_f[(a, n)]]) if hw else []
    missing = [w for w in want if w != here_id and w not in entries]
    v.step("lp guest's list: made at the lamp it lit", here_id == LP_NEW[0], "here %s" % (here_id or "?"))
    v.step("lp guest's list: every lamp lit in the host's world", bool(hit and hw) and not missing,
           "here %s; host world lit %s; missing %s" % (here_id, " ".join(want), " ".join(missing) or "none"))
    host = lp_save_flags(d, 0, LP_OLD[1] + LP_NEW[1])
    guest = lp_save_flags(d, 1, LP_OLD[1] + LP_NEW[1])
    fmt = lambda m: " ".join("%s=%d" % (k, m[k]) for k in m) if m else "no save"
    v.step("lp host's save: the guest's old lamp unlit", host is not None and not any(host[f] for f in LP_OLD[1]), fmt(host))
    v.step("lp host's save: the lamp the guest lit", host is not None and all(host[f] for f in LP_NEW[1]), fmt(host))
    v.step("lp guest's save: its own old lamp kept", guest is not None and all(guest[f] for f in LP_OLD[1]), fmt(guest))
    v.step("lp guest's save: the lamp it lit", guest is not None and all(guest[f] for f in LP_NEW[1]), fmt(guest))
    # The host dies after the guest lit the lamp: up at its own last lamp, never the guest's.
    rev = L.find(0, r"host death taken: .*revive at lamp (\d+)")
    v.step("lp host gets up at its own last lamp", bool(rev) and ("revive at lamp %s " % LP_HOST_LAMP) in rev[1],
           rev[1][:200] if rev else "no host death line")
    check_common(v, L, "lp")


def check_bf(v, L):
    """Loads into the host's world while its boss fight goes on (phase bf)."""
    need(v, L, "bf join: 3 players", 0, r"cooperators [01] -> 2")
    hm = need(v, L, "bf host walks into the fight's region", 0, r"travel step \d+/\d+: host_move")
    on = need(v, L, "bf the host's fight is on (bar)", 0, r"boss health bar shown", hm or 0)
    if on is None:
        return
    mark = need(v, L, "bf guest 1 uses a Hunter's Mark in the fight", 1, r"guest_use goods 100", on)
    if mark is not None:
        need(v, L, "bf guest 1: a re-join with the host's snapshot", 1,
             r"travels alone \(hunters_mark\) to .*a re-join with the host's snapshot", mark - 1)
        need(v, L, "bf host re-sends the snapshot (fight on)", 0, r"slot 1 travels alone into this area", mark - 1)
        arr = need(v, L, "bf guest 1 back in the host's world", 1, r"arrived in the host world \(flow 4\)", mark)
        if arr is not None:
            need(v, L, "bf guest 1 sees the fight running (host-entered 1, its own fog flag 0)", 1,
                 r"boss fogs of this block: .*\[12414811: read 1 intro 1 host 1 dead 0", arr - 0.5, arr + 10)
            none_of(v, L, "bf guest 1: no stale fog flag", r"already 1 when this stay began", mark, insts=(1,))
            bar = L.find(1, r"boss health bar shown", mark + 0.5)  # after the load's reset
            v.step("bf guest 1's boss bar after its load (shown for every member in the map while the fight runs)",
                   True, ("+%.1f s (back +%.1f s)" % (bar[0], arr)) if bar else "not shown before its walk-in")
            walk = L.find(1, r"travel step \d+/\d+: guest_obj", arr)
            went = need(v, L, "bf guest 1 walks through the fog", 1, r"guest: went through boss fog", walk[0] if walk else arr)
            if went is not None:
                none_of(v, L, "bf guest 1 did not report before its walk", r"guest: went through boss fog", mark,
                        walk[0] if walk else went - 0.1, insts=(1,))
                need(v, L, "bf the host: guest 1 joins the running fight (nothing to open)", 0,
                     r"boss fog: slot 1 went in \(flag 12414801\); nothing to open \(host in 1", went - 0.5)
                need(v, L, "bf guest 1 in the fight (bar)", 1, r"boss health bar shown", mark + 0.5)
        none_of(v, L, "bf the host's fight is never reset", r"boss health bar: reset|host: rest load|a wipe; the",
                on, insts=(0,))
    d2 = need(v, L, "bf guest 2 dies outside the fog", 2, r"travel step \d+/\d+: guest_die", on)
    if d2 is not None:
        need(v, L, "bf guest 2: its limbo (the fight is on: no load of its own)", 2, r"guest: dead in a boss fight", d2)
        none_of(v, L, "bf guest 2: no respawn load in the fight", r"travels alone \(death\)|loading screen up", d2, insts=(2,))
    check_common(v, L, "bf")


def check_rt(v, L):
    """Phase rt: rot cleansed by the killer's death, the bloodstain, a boss; each rule off: no change."""
    need(v, L, "rt join: 2 players", 0, r"cooperators 0 -> 1")

    d1 = need(v, L, "rt 1 guest killed by an enemy (its killer known)", 1,
              r"death 1: killed by an enemy \(chr [0-9a-f]+, entity 24102(47|02)")
    k1 = None
    if d1 is not None:
        ent = re.search(r"entity (\d+)", L.find(1, r"death 1: killed by an enemy", d1 - 0.1)[1]).group(1)
        need(v, L, "rt 1 a stack (health)", 1, r"death 1; rot on, 1 stack\(s\) \(this one: health\)", d1 - 1)
        k1 = need(v, L, "rt 1 the host kills it", 0, r"travel step \d+/\d+: host_kill entity %s" % ent, d1)
        c1 = need(v, L, "rt 1 killer dead: -1 (notice)", 1,
                  # [1-5]: the stain test can put the frail guest back onto its stain beside the living
                  # killer, a second death by it (release smoke 0.7.2: "Rot -2: your killer is dead")
                  r"rot: an enemy died \(entity %s, .*\): [1-5] stack\(s\) off, \d+ left; notice: Rot -[1-5]: your killer is dead"
                  % ent, d1)
        if c1 is not None and k1 is not None:
            v.step("rt 1 the cleanse came from the host's kill", c1 >= k1 - 1 and c1 - k1 < 15,
                   "+%.1f s after the host's kill" % (c1 - k1))
    d2 = need(v, L, "rt 2 a scripted death (no killer), a stain", 1,
              r"death \d+: killed by nobody", k1 or 0)
    if d2 is not None:
        need(v, L, "rt 2 stack +1", 1, r"death \d+; rot on, 1 stack\(s\)", d2 - 1)
        need(v, L, "rt 2 bloodstain picked up", 1, r"party stain: recovered \d+ Blood Echoes|own bloodstain taken and its Blood Echoes back", d2)
        need(v, L, "rt 2 echoes back: -1 (notice)", 1,
             r"rot: the bloodstain was picked up .*: 1 stack\(s\) off, 0 left; notice: Rot -1: Blood Echoes retrieved", d2)
    off = need(v, L, "rt 3 the host turns the killer and echoes rules off", 0,
               r"test: host_rule: party_rot_cleanse_echoes 1 -> 0")
    if off is not None:
        need(v, L, "rt 3 the guest gets the host's rules (cleanses off)", 1,
             r'the host\'s rules: .*"rot_cleanse_killer":false.*"rot_cleanse_echoes":false|'
             r'the host\'s rules: .*"rot_cleanse_echoes":false', off - 1)
    d3 = need(v, L, "rt 3 guest killed by enemy 2410261 / 2410260", 1,
              r"death \d+: killed by an enemy \(chr [0-9a-f]+, entity 241026[01]",
              d2 or 0)
    if d3 is not None:
        k3 = need(v, L, "rt 3 its killer dead, rule off: no change", 1,
                  r"rot: an enemy died \(entity 241026[01], .*\): nothing off \(rot on; cleanse boss on, killer off", d3)
        none_of(v, L, "rt 3 no stack off while the rules are off", r"rot: .*stack\(s\) off", d3,
                L.find(1, r"rot: a boss died", d3)[0] if L.find(1, r"rot: a boss died", d3) else 1e9, (1,))
        need(v, L, "rt 3 stain picked up, rule off: no change", 1,
             r"rot: the bloodstain was picked up .*nothing off \(rot on; cleanse boss on, killer off, echoes off", d3)
    d4 = need(v, L, "rt 4 a death: 2+ stacks", 1, r"death \d+; rot on, [2-5] stack\(s\)", d3 or 0)
    b = need(v, L, "rt 5 boss killed: all rot cleansed (notice)", 1,
             r"rot: a boss died .*: [2-5] stack\(s\) off, 0 left; notice: Rot cleansed: boss defeated", d4 or 0)
    if b is not None:
        need(v, L, "rt 5 the host killed Gascoigne", 0, r"travel step \d+/\d+: host_kill entity 2410810", (d4 or 0))
        d6 = need(v, L, "rt 6 a death after the boss: 1 stack", 1, r"death \d+; rot on, 1 stack\(s\)", b)
        if d6 is not None:
            need(v, L, "rt 6 the host turns the boss rule off", 0, r"test: host_rule: party_rot_cleanse_boss 1 -> 0", b - 30)
            need(v, L, "rt 6 Cleric Beast killed, boss rule off: no change", 1,
                 r"rot: a boss died \(entity 2410800.*nothing off \(rot on; cleanse boss off.*; [1-5] stack\(s\)\)", d6)
    # The party HUD's rot line follows (System > Party / HUD read party::status::SetRot)
    none_of(v, L, "rt no rot file errors", r"rot: .*(could not|cannot) (write|read)", 0, 1e9, (0, 1))
    check_common(v, L, "rt")



def _br_walks(L, i, kind, after, before=1e9):
    """The (start, result) of each `kind` (guest_stick / host_stick) walk started in [after, before)."""
    out = []
    for t, _line in L.all(i, r"travel step \d+/\d+: " + kind, after, before):
        out.append((t, L.find(i, r"test: approach: (arrived|BLOCKED|GAVE UP)", t)))
    return out


def _br_walk_rows(v, L, i, who, kind, whats, after, before=1e9):
    ws = _br_walks(L, i, kind, after, before)
    for k, what in enumerate(whats):
        w = ws[k] if k < len(ws) else None
        ok = bool(w and w[1] and "arrived" in w[1][1])
        v.step("br %s walks %s" % (who, what), ok,
               ("+%.1f s: %s" % (w[1][0], w[1][1][:120])) if w and w[1] else "walk %d not run / no result" % (k + 1))


def check_br(v, L):
    """Phase br: Laurence's room open until the skull starts his fight; the wall behind Ludwig gone."""
    need(v, L, "br join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "br %s: the boss-room rule (Laurence's room open on a client)" % who, i,
             r"EMEVD rule \(built in, boss rooms\): skip 13404850:1003:5@2 \(on a client\)")
        need(v, L, "br %s: the wall rule behind Ludwig" % who, i, r"skip 13400010:2005:3@7")
        need(v, L, "br %s: Ludwig's wall goes at his death" % who, i,
             r"EMEVD rule \(built in, boss rooms\): skip 13400010:0:0@11")
    arr = need(v, L, "br guest in the Nightmare (m34_00)", 1, r"map .*-> m34_00")
    if arr is None:
        return check_common(v, L, "br")
    skull = L.find(0, r"travel step \d+/\d+: host_use", arr)
    t_skull = skull[0] if skull else 1e9
    _br_walk_rows(v, L, 1, "guest", "guest_stick", ("out through Laurence's fog (no fight yet)",
                                                     "back in through it (no fight yet)"), arr, t_skull)
    none_of(v, L, "br no fight before the skull (no fog report, no host visit, no bar)",
            r"went through boss fog \(event 13404851|boss fog: slot \d+ went in|first fight \(guest event 13404851|"
            r"Boss fight started|boss health bar shown", arr, t_skull, insts=(0, 1))
    if skull is None:
        v.step("br the host is given Laurence's Skull", False, "host_use not run")
        return check_common(v, L, "br")
    sc = need(v, L, "br the skull at the altar starts the fight (scene 34000010)", 0, r"cutscene 34000010", t_skull)
    if sc is not None:
        for i, who in ((0, "host"), (1, "guest")):
            need(v, L, "br Laurence's bar on the %s" % who, i, r"boss health bar shown", sc)
    kill = need(v, L, "br Laurence killed", 0, r"boss defeat, event 13401850", t_skull)
    lud = L.find(0, r"travel step \d+/\d+: host_move", kill or t_skull)
    if lud is None:
        v.step("br the host walks into Ludwig's fight", False, "host_move not run")
        return check_common(v, L, "br")
    need(v, L, "br Ludwig's fight (bar)", 0, r"boss health bar shown: .*event 13404802", lud[0])
    dead = need(v, L, "br Ludwig killed (9471)", 0, r"boss defeat, event 13401800", lud[0])
    if dead is not None:
        for i, who, kind in ((0, "host", "host_stick"), (1, "guest", "guest_stick")):
            _br_walk_rows(v, L, i, who, kind, ("through the wall behind Ludwig", "back through it"), dead)
    check_common(v, L, "br")

def _remo_starts(L, i, after=0.0, before=1e9):
    return [t for t, _ in L.all(i, r"Party story: remo playing", after, before)]


def check_sm(v, L):
    """Phase sm: the host's intro / door scene live on the guest beside it, once."""
    need(v, L, "sm join: 2 players", 0, r"cooperators 0 -> 1")
    # 1: Gascoigne's intro race
    hm = need(v, L, "sm host walks into Gascoigne's intro region (guest 4-5 m off)", 0, r"travel step \d+/\d+: host_move", 0)
    t_kill = L.find(0, r"travel step \d+/\d+: host_kill", hm or 0)
    end1 = t_kill[0] if t_kill else 1e9
    if hm is not None:
        h = _remo_starts(L, 0, hm, end1)
        g = _remo_starts(L, 1, hm - 1, end1)
        v.step("sm intro: host plays 24010010", h, ("+%.1f s" % h[0]) if h else "no remo on the host")
        need(v, L, "sm intro: the guest plays the host's copy (live mirror)", 1, r"live mirror: remo 2401(0|1)010", hm - 1, end1)
        held = L.find(1, r"own 2002\[\d\] \(boss intro 24010010", hm - 1, end1)
        v.step("sm intro: guest's own copy held for the host's (when its event ran too)", True,
               ("held +%.1f s" % held[0]) if held else "its own event did not run (no race this time)")
        ok = bool(h and g and abs(g[0] - h[0]) <= 1.0)
        v.step("sm intro: guest's scene within 1 s of the host's", ok,
               "host +%.1f guest +%.1f" % (h[0], g[0]) if h and g else "host %s guest %s" % (h, g))
        v.step("sm intro: one play on the guest", len(g) == 1, "%d remo start(s) on the guest" % len(g))
        none_of(v, L, "sm intro: no own copy after a wait", r"held own boss intro|this player triggered and watched it itself",
                hm, end1, insts=(1,))
        if L.find(0, r"test: remo 2401(0|1)010 skipped after", hm, end1):
            need(v, L, "sm intro: the host's skip ends the guest's mirror", 1,
                 r"the host's cutscene ended: the live mirror of remo \d+ ends here too", hm, end1)
    # 2: the Cathedral Ward door
    presses = L.all(0, r"travel step \d+/\d+: host_press", end1 if end1 < 1e9 else 0)
    pr = presses[-1] if presses else None  # the last one opens the door (the first closes the key notice)
    v.step("sm door: host opens the door (m24_01 side)", pr, ("+%.1f s" % pr[0]) if pr else "no press")
    if pr:
        hs = need(v, L, "sm door: host plays 24000020", 0, r"host #\d+ cutscene 24000020", pr[0] - 1)
        h = _remo_starts(L, 0, pr[0] - 1)
        g = _remo_starts(L, 1, pr[0] - 1)
        need(v, L, "sm door: guest takes it as the live mirror", 1, r"cutscene 24000020 .*taken \(phantom: live mirror\)", pr[0] - 1)
        need(v, L, "sm door: the mirror plays in m24_01 (block loaded / host near)", 1,
             r"live mirror of remo 24000020 plays here though this guest is in m24_01", pr[0] - 1)
        none_of(v, L, "sm door: never dropped for its map", r"live mirror of remo 24000020 dropped", pr[0] - 1, insts=(1,))
        ok = bool(h and g and abs(g[0] - h[0]) <= 1.0)
        v.step("sm door: guest's scene within 1 s of the host's", ok,
               "host +%.1f guest +%.1f" % (h[0], g[0]) if h and g else "host %s guest %s" % (h, g))
        if L.find(0, r"test: remo 2400(0|1)020 skipped after", pr[0] - 1):
            need(v, L, "sm door: the host's skip ends the guest's mirror", 1,
                 r"the host's cutscene ended: the live mirror of remo \d+ ends here too", pr[0] - 1)
            need(v, L, "sm door: the scene's time of day after the skip", 1, r"time_of_day 1 .*taken", pr[0] - 1)
        walk = need(v, L, "sm door: guest walks into m24_00 after it", 1, r"map m24_01_00_00 -> m24_00_00_00", pr[0])
        if walk is not None:
            none_of(v, L, "sm door: no catch-up replay of the scene", r"replay: remo 2400(0|1)020", pr[0], insts=(1,))
            late = _remo_starts(L, 1, walk - 0.5)
            v.step("sm door: no second play after the walk", not late, "none" if not late else "+%.1f s" % late[0])
    check_common(v, L, "sm")


def check_da(v, L):
    need(v, L, "da join: 2 players", 0, r"cooperators 0 -> 1")
    left = need(v, L, "da host leaves the Dream alone", 1, r"guest: the host travels alone \(.*\); staying here")
    if left is None:
        check_common(v, L, "da")
        return
    need(v, L, "da guest's send-home dropped (split)", 1, r"\(OnLeave_Limit\) dropped: split travel", left)
    lift = need(v, L, "da guest's action-button hold lifted", 1, r"the leave state's action-button hold lifted", left)
    hs = need(v, L, "da guest uses a headstone (fixture started)", 1, r"test: guest headstone: starting", left)
    pr = L.find(1, r"test: levelup: before Cross", hs or left)
    v.step("da (info) headstone prompt: see the daprompt screenshots", True,
           pr[1][:150] if pr else "no 'before Cross' line")
    need(v, L, "da guest's Cross opens the headstone's travel list", 1,
         r"test: levelup: no talk list: the menu was requested", hs or left)
    flag = need(v, L, "da guest's headstone choice (its lamp flag)", 1,
                r"lamp flag \d+ set by this game|lamp \d+ \(guest\): (held for the choice|BB_PARTY_TRAVEL_CHOICE)",
                hs or left)
    fb = L.find(1, r"no warp followed", flag or left)
    v.step("da (info) the host's destination: warp run by the used-up-event fallback", True,
           fb[1][:150] if fb else "no fallback line (the event warped itself)")
    go = need(v, L, "da guest travels from the Dream", 1, r"guest: travels alone \(headstone\)", flag or left)
    if go is not None:
        need(v, L, "da guest arrives (not stuck in the Dream)", 1, r"arrived in the host world|map - -> m2[2-9]", go)
    # 0.7.3 (user decision 2026-10-22: only the host's own rest resets the world): the guest's trip
    # out of the Dream is a plain travel - no rest request, no rest load or loading screen for the host.
    need(v, L, "da guest's trip from the Dream is no rest", 1,
         r"leaves the Hunter's Dream alone: a plain travel, no rest load", flag or left)
    none_of(v, L, "da guest sends no rest request", r"the host's world gets a rest load|rest rides|\"rest\":",
            flag or left, insts=(1,))
    none_of(v, L, "da host: no rest load for the guest's trip",
            r"rested at lamp|a rest load for this world|re-join waits for the rest load|rest load could not start",
            flag or left, insts=(0,))
    none_of(v, L, "da host: no loading screen while the guest leaves", r"map (m\d\d_\d\d_\d\d_\d\d|-) -> m",
            flag or left, insts=(0,))
    if lift is not None and hs is not None:
        v.step("da hold lifted before the guest's headstone use", lift <= hs, "lift +%.1f s, use +%.1f s" % (lift, hs))
    check_common(v, L, "da")


def nf_hp(L, i, entity, after=0.0, before=1e9):
    """[(t, hp, team)] of `entity` in instance i's chr dumps."""
    out = []
    for t, line in L.all(i, r"chr dump: entity %d " % entity, after, before):
        m = re.search(r"team (-?\d+) .*HP (-?\d+)/", line)
        if m:
            out.append((t, int(m.group(2)), int(m.group(1))))
    return out


def check_nf(v, L):
    need(v, L, "nf join: 2 players", 0, r"cooperators 0 -> 1")
    # 1  Henryk and Eileen at the Tomb of Oedon
    at = need(v, L, "nf host at Henryk (his fight)", 0, r"test: host_chr %d" % NF_HENRYK, 0)
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "nf %s's game shows Henryk" % who, i, r"chr dump: entity %d .*HP [1-9]" % NF_HENRYK, at or 0)
    need(v, L, "nf Eileen joins the fight (her ally state 1365, host)", 0, r"test: flags .*1365=1", 0)
    hit = L.find(1, r"test: guest_press 0x800", at or 0)
    kill = L.find(1, r"test: guest_kill: NPC entity %d" % NF_HENRYK, at or 0)
    if hit and kill:
        hp = [h for t, h, _ in nf_hp(L, 0, NF_HENRYK, hit[0] - 1, kill[0])]
        v.step("nf the guest's R1s hurt Henryk on the host's game", bool(hp) and min(hp) < 1716 and min(hp) > 0,
               "host HP %s" % hp[:8])
        need(v, L, "nf Henryk dies on the host after the guest's killing blow", 0,
             r"test: flags .*12410810=1", kill[0] - 1, kill[0] + 30)
        need(v, L, "nf Henryk's quest step on the host (1367)", 0, r"test: flags .*1367=1", kill[0] - 1)
        need(v, L, "nf the quest step in the guest's own save (next load)", 1,
             r"test read \(load mode 1, own save / visible\):[^\n]*1367=1/1", kill[0])
    else:
        v.step("nf the guest hits and kills Henryk", False, "no guest R1 / kill step")
    # 2  Alfred in Cainhurst: the rule off (A), then on (B, after a round trip)
    off = L.find(0, r"test: host_rule: party_npc_fights 1 -> 0", 0)
    on = L.find(0, r"test: host_rule: party_npc_fights 0 -> 1", off[0] if off else 0)
    if not off or not on:
        v.step("nf the host's rule off, then on", False, "%s / %s" % (bool(off), bool(on)))
        check_common(v, L, "nf")
        return
    host_a = nf_hp(L, 0, NF_ALFRED, off[0], on[0])
    guest_a = nf_hp(L, 1, NF_ALFRED, off[0], on[0])
    v.step("nf (rule off) Alfred hostile on the host's game (team 27)", any(tm == 27 for _, _, tm in host_a),
           "teams %s" % sorted({tm for _, _, tm in host_a}))
    v.step("nf (rule off, the vanilla gap) Alfred friendly on the guest's game", bool(guest_a) and
           all(tm != 27 for _, _, tm in guest_a), "teams %s" % sorted({tm for _, _, tm in guest_a}))
    rule = need(v, L, "nf (rule on) the guest's copy of Alfred's init runs as the host's", 1,
                r"guest copy of event 12500050 runs as the host's", on[0])
    guest_b = nf_hp(L, 1, NF_ALFRED, rule or on[0])
    # a guest's copy switches him to 23 (a foe for a phantom; 27 is none: party_npcfight_rules.h)
    v.step("nf (rule on) Alfred hostile on the guest's game too (team 23)", any(tm == 23 for _, _, tm in guest_b),
           "teams %s" % sorted({tm for _, _, tm in guest_b}))
    none_of(v, L, "nf no flag write from a guest copy reaches the host as a world action",
            r"world action sent to the host[^\n]*\b(134[0-9]|1350)\b", 0, insts=(1,))
    check_common(v, L, "nf")


def check_nfa(v, L):
    need(v, L, "nfa join: 2 players", 0, r"cooperators 0 -> 1")
    at = need(v, L, "nfa host at Alfred (Queen's Chamber)", 0, r"test: host_chr %d" % NF_ALFRED)
    gat = need(v, L, "nfa guest beside Alfred", 1, r"test: guest_chr %d" % NF_ALFRED)
    for i, who in ((0, "host"), (1, "guest")):
        pos = [(t, ln) for t, ln in L.all(i, r"chr dump: entity %d " % NF_ALFRED, at or 0)]
        ys = [float(m.group(1)) for _, ln in pos for m in [re.search(r"at \(-?[\d.]+, (-?[\d.]+),", ln)] if m]
        v.step("nfa the chamber is drawn on the %s's game (Alfred stands, y > 100)" % who, bool(ys) and min(ys) > 100,
               "y %s" % sorted({round(y) for y in ys})[:6])
    prov = L.find(0, r"test: host_press 0x800", at or 0)
    if not (at and gat and prov):
        v.step("nfa the host's R1s ran", False, "host_chr %s guest_chr %s press %s" % (at, gat, bool(prov)))
        check_common(v, L, "nfa")
        return
    before_h = nf_hp(L, 0, NF_ALFRED, 0, prov[0])
    before_g = nf_hp(L, 1, NF_ALFRED, 0, prov[0])
    v.step("nfa (before) Alfred friendly on both games (team 26)",
           bool(before_h) and bool(before_g) and all(tm == 26 for _, _, tm in before_h + before_g),
           "host teams %s, guest teams %s" % (sorted({tm for _, _, tm in before_h}), sorted({tm for _, _, tm in before_g})))
    lock0 = L.find(1, r"test: guest_press 0x4; .*lock-on (\d)", gat, prov[0])
    v.step("nfa (info) the guest's lock-on on the friendly Alfred", True, lock0[1][:120] if lock0 else "no press line")
    # the host's own switch: its quest state 1349 (12500053's flag write) and team 27 on its game
    hflag = L.find(0, r"test: flags .*1349=1", prov[0] - 1)
    hteam = [(t, tm) for t, _, tm in nf_hp(L, 0, NF_ALFRED, prov[0] - 1) if tm == 27]
    v.step("nfa the host's R1s turn Alfred hostile on the host (1349 / team 27)", bool(hflag or hteam),
           "1349 at %s, team 27 at %s" % ("+%.1f" % hflag[0] if hflag else "-", "+%.1f" % hteam[0][0] if hteam else "-"))
    # the guest's copy: team 23 (a foe for a phantom; 27 is none: party_npcfight_rules.h)
    gteam = [(t, tm) for t, _, tm in nf_hp(L, 1, NF_ALFRED, prov[0] - 1) if tm in (23, 27)]
    t27 = gteam[0][0] if gteam else None
    gnear = L.find(1, r"test: guest_chr %d" % NF_ALFRED, 0, t27 or 1e9)
    hsw = min([x for x in (hflag[0] if hflag else None, hteam[0][0] if hteam else None) if x is not None] or [0])
    v.step("nfa in-map switch: Alfred hostile on the guest's game while it stands there (team 23)",
           bool(gteam) and bool(gnear) and gteam[0][1] == 23,
           "team %d at +%.1f s (the host's switch +%.1f s, the guest at him +%.1f s)" % (gteam[0][1], t27, hsw, gnear[0])
           if gteam and gnear else "guest teams after the host's R1s %s" %
           sorted({tm for _, _, tm in nf_hp(L, 1, NF_ALFRED, prov[0] - 1)}))
    need(v, L, "nfa the guest's copy of 12500053 runs as the host's", 1, r"guest copy of event 12500053 runs as the host's")
    need(v, L, "nfa the guest's copy switches him to a foe it can hit (27 -> 23)", 1,
         r"event 12500053's team switch at 7 \(entity -?\d+\): team 27 -> 23")
    t0 = t27 or prov[0]
    gkill = L.find(1, r"test: guest_kill: .*entity %d" % NF_ALFRED, t0)
    hits = L.all(1, r"test: guest_press 0x800", t0, gkill[0] if gkill else 1e9)
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already",
                  t0, gkill[0] if gkill else 1e9)
    v.step("nfa the guest locks on to the hostile Alfred", bool(lock), lock[1][:140] if lock else "no lock-on")
    if hits and gkill:
        h0 = [h for _, h, _ in nf_hp(L, 0, NF_ALFRED, 0, hits[0][0])]
        start = h0[-1] if h0 else 3100
        presses = [m.group(1) for _, ln in hits for m in [re.search(r"target %d \(NPC, team -?\d+\) HP (-?\d+)" % NF_ALFRED, ln)] if m]
        v.step("nfa (info) Alfred's HP on the guest's game at each guest R1", True, "%s" % presses)
        hh = [h for _, h, _ in nf_hp(L, 0, NF_ALFRED, hits[0][0] - 0.5, gkill[0])]
        gh = [h for _, h, _ in nf_hp(L, 1, NF_ALFRED, hits[0][0] - 0.5, gkill[0])]
        v.step("nfa the guest's R1s hurt Alfred on the host's game", bool(hh) and 0 < min(hh) < start,
               "from %d: host HP %s" % (start, hh[:10]))
        v.step("nfa ... and on the guest's game", bool(gh) and 0 < min(gh) < start, "guest HP %s" % gh[:10])
        need(v, L, "nfa Alfred dies on the host after the guest's killing blow (1350)", 0,
             r"test: flags .*1350=1", gkill[0] - 1, gkill[0] + 30)
        drop = L.find(0, r"shared drop roll .*entity %d" % NF_ALFRED, gkill[0] - 1)
        v.step("nfa (info) his drop roll on the host", True, drop[1][:140] if drop else "none")
        rd = L.find(1, r"test read \(load mode 1, own save / visible\):[^\n]*1350=", gkill[0])
        v.step("nfa (info) the guest's own save at its next load (Alfred's states: covenant outcomes, per player)",
               bool(rd), rd[1][:160] if rd else "no read")
    else:
        v.step("nfa the guest hits and kills Alfred", False, "R1s %d, kill %s" % (len(hits), bool(gkill)))
    none_of(v, L, "nfa no flag write from a guest copy reaches the host as a world action",
            r"world action sent to the host[^\n]*\b(134[0-9]|135[0-9])\b", 0, insts=(1,))
    check_common(v, L, "nfa")


def check_cv(v, L, d: Path):
    """Phase cv: covenant outcomes per player; Valtr's rune for a guest (0.7.3)."""
    j = need(v, L, "cv join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "cv host's fixture: Annalise dead by Alfred, Valtr dead (host's save)", 0,
         r"test: set in the own save .*1042", 0)
    need(v, L, "cv rule on (host's setting)", 0, r"covenant outcomes per player: on")
    need(v, L, "cv rule on (guest: the host's rule)", 1, r"covenant outcomes per player: on")
    snap = need(v, L, "cv covenant: the host's covenant flags not copied into the guest's save", 1,
                r"covenant outcomes per player: \d+ covenant flag\(s\) set in the host's world not copied.*\b1042\b")
    # In the host's world: the guest sees the host's state (visible), its own save its own.
    rd = L.find(1, r"test read \(load mode 1, own save / visible\): ", j or 0)
    vals = dict(re.findall(r"\b(\d+)=([0-9?]/[0-9?])", rd[1])) if rd else {}
    v.step("cv covenant: in the host's world the guest sees the host's state (1042 visible)",
           vals.get("1042", "").endswith("/1") and vals.get("1040", "").endswith("/0"),
           rd[1][:220] if rd else "no load mode 1 test read on the guest")
    none_of(v, L, "cv covenant: no covenant flag sent up as a world action",
            r"world action sent to the host.*\b(10[45]\d|13[45]\d|179\d|180\d|725003\d\d|727003\d\d)\b", j or 0)
    lv = need(v, L, "cv covenant: the guest leaves (Party > Leave)", 1, r"guest_leave: Party > Leave pressed", j or 0)
    home = L.find(1, r"test read \(load mode 0, own save / visible\): ", lv or 1e9)
    hv = dict(re.findall(r"\b(\d+)=([0-9?])/", home[1])) if home else {}
    v.step("cv covenant: at home the guest's world has Annalise alive, its Alfred and Valtr as before",
           bool(hv) and hv.get("1040") == "1" and hv.get("1042") == "0" and hv.get("1344") == "1" and
           hv.get("1348") == "0" and hv.get("1800") == "1" and hv.get("1790") == "0",
           home[1][:220] if home else "no load mode 0 test read after the Leave")
    v.step("cv shared: the host's boss kill reached the guest's own world", bool(hv) and hv.get(CV_CONTROL) == "1",
           "%s=%s" % (CV_CONTROL, hv.get(CV_CONTROL, "?")))
    ids = CV_HOST_SET + CV_GUEST_SET + [CV_CONTROL, CV_RUNE_FLAG]
    host = lp_save_flags(d, 0, ids)
    guest = lp_save_flags(d, 1, ids)
    fmt = lambda m: " ".join("%s=%d" % (k, m[k]) for k in m) if m else "no save"
    v.step("cv host's save: its own covenant outcomes kept", host is not None and all(host[f] for f in CV_HOST_SET),
           fmt(host))
    v.step("cv guest's save: Annalise alive, its Alfred and Valtr unchanged (per player)",
           guest is not None and all(guest[f] for f in CV_GUEST_SET) and not any(guest[f] for f in CV_HOST_SET),
           fmt(guest))
    v.step("cv shared: the guest's save has the host's boss kill (the sync ran and was saved)",
           guest is not None and guest[CV_CONTROL], fmt(guest))
    # Valtr's Impurity rune
    tr = need(v, L, "cv rune: the guest's League talk trigger set (test step)", 1,
              r"test: guest_flag %s: \d -> 1" % CV_RUNE_TRIGGER, j or 0)
    need(v, L, "cv rune: given to the guest itself (own grant)", 1,
         r"own talk gift trigger %s .*lot %s .*given to this guest" % (CV_RUNE_TRIGGER, CV_RUNE_LOT), tr or j or 0)
    need(v, L, "cv rune: given into the guest's inventory", 1,
         r"gave #\d+ lot %s \(flag %s, own\).*flag %s is set" % (CV_RUNE_LOT, CV_RUNE_FLAG, CV_RUNE_FLAG),
         tr or j or 0)
    none_of(v, L, "cv rune: the trigger never goes to the host", r"world action sent to the host.*%s" % CV_RUNE_TRIGGER, insts=(0, 1))
    none_of(v, L, "cv rune: the host gets no rune", r"award lot %s\b" % CV_RUNE_LOT, insts=(0,))
    v.step("cv rune: in the guest's save (its own flag %s), not the host's" % CV_RUNE_FLAG,
           guest is not None and host is not None and guest[CV_RUNE_FLAG] and not host[CV_RUNE_FLAG],
           "guest %s host %s" % (fmt(guest and {CV_RUNE_FLAG: guest[CV_RUNE_FLAG]}),
                                 fmt(host and {CV_RUNE_FLAG: host[CV_RUNE_FLAG]})))
    check_common(v, L, "cv")


def check_ie(v, L):
    """Phase ie: early items for every player (0.7.3)."""
    j = need(v, L, "ie join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "ie bell: the host's award of lot 10050 goes to the party", 0,
         r"host award #\d+ lot 10050 \(flag 6670, award\) queued for the party", j or 0)
    need(v, L, "ie bell: the guest gets the Old Hunter Bell (item box)", 1,
         r"gave #\d+ lot 10050 .*goods 4312 x1 \(held 0 -> 1", j or 0)
    rise = need(v, L, "ie messengers: the host's own messengers rise (its picks cleared)", 0,
                r"test: flags 12101020=0 12105050=1", j or 0)
    # Which messenger event the guest's game ends (and sends) varies (r5: 12101026, the bell's).
    need(v, L, "ie messengers: another game's end of a gift messenger event not applied", 0,
         r"Messengers: event 121010(2[0-8])'s state from another player not applied", j or 0)
    done = L.find(0, r"test: flags 12101020=1", rise or 0) if rise is not None else None
    picked = L.find(0, r"test: flags .*7210100[012]=1", rise or 0) if rise is not None else None
    v.step("ie messengers: the host's stay up until its own pick",
           rise is not None and (done is None or (picked is not None and picked[0] <= done[0])),
           ("done %s; own pick %s" % (done[1][:120] if done else "never", picked[1][:120] if picked else "none")))
    check_common(v, L, "ie")


CHECKS = {"s1": lambda v, L, d: check_s1(v, L), "s2": check_s2, "rules": lambda v, L, d: check_rules(v, L),
          "sc": check_sc, "summonfail": lambda v, L, d: check_summonfail(v, L),
          "s3": lambda v, L, d: check_s3(v, L), "sk": lambda v, L, d: check_sk(v, L),
          "ga": lambda v, L, d: check_ga(v, L), "gl": lambda v, L, d: check_gl(v, L), "lf": lambda v, L, d: check_lf(v, L), "el": lambda v, L, d: check_el(v, L), "lt": lambda v, L, d: check_lt(v, L), "ch": lambda v, L, d: check_ch(v, L), "rh": lambda v, L, d: check_rh(v, L), "rs": lambda v, L, d: check_rs(v, L),
          "mk": lambda v, L, d: check_mk(v, L), "kd": lambda v, L, d: check_kd(v, L), "kw": lambda v, L, d: check_kw(v, L),
          "fl": lambda v, L, d: check_fl(v, L), "fd": lambda v, L, d: check_fd(v, L), "fdj": lambda v, L, d: check_fdj(v, L), "ld": lambda v, L, d: check_ld(v, L),
          "cl": lambda v, L, d: check_cl(v, L), "clg": lambda v, L, d: check_clg(v, L), "bf": lambda v, L, d: check_bf(v, L), "lp": check_lp, "br": lambda v, L, d: check_br(v, L), "sm": lambda v, L, d: check_sm(v, L), "da": lambda v, L, d: check_da(v, L), "cv": check_cv, "ie": lambda v, L, d: check_ie(v, L), "nf": lambda v, L, d: check_nf(v, L), "nfa": lambda v, L, d: check_nfa(v, L),
          "rt": lambda v, L, d: check_rt(v, L), "tv": lambda v, L, d: check_tv(v, L),
          "eh": lambda v, L, d: check_eh(v, L)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "smoke" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default="s1,s2,rules,sc,summonfail,s3,sk,ga,gl,lf,ld,mk,kd,kw,fl,fdj,cl,clg,el,lt,ch,rh,rs,bf,lp,rt,eh,br,sm,da,cv,tv,nf,nfa,ie")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--real-net", action="store_true",
                    help="the phases on the real network (instances.py --real-net: public STUN, UPnP, the Internet code)")
    ap.add_argument("--only", default="", help="comma list of step groups (a smaller run): " + ",".join(GROUPS))
    a = ap.parse_args()
    RUN_ROOT[0] = a.root
    only = {g.strip() for g in a.only.split(",") if g.strip()}
    global WIPE_LATE_RUN
    WIPE_LATE_RUN = "wipe_late" in only
    bad = only - set(GROUPS)
    if bad:
        sys.exit("unknown --only group(s): " + ",".join(sorted(bad)))
    if only:
        a.phase = ",".join(p for p in ("s1", "s2", "rules", "sc", "summonfail", "s3", "sk", "ga", "gl", "lf", "mk", "kd", "kw", "fl", "fd", "fdj", "ld", "cl", "clg", "el", "lt", "bf", "ch",
                                     "lp", "rt", "br", "sm", "da", "cv", "ie", "rh", "rs", "tv", "nf", "nfa", "eh")
                           if p in {n for g in only for n in GROUPS[g].split(",")})
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
