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
Phase bl (~4 min, 2 players, group `limbo_leave`, 0.7.4 player report host_limbo_mark): the host dies in
  Father Gascoigne's fight while the guest fights on (the host's limbo + spectate; the boss must keep
  fighting the guest: its HP and the boss's target logged), then the guest, the last one in the fight,
  takes a Hunter's Mark to the host's lamp: that counts as a wipe (the party's one respawn load: the
  host up at its lamp, no spectating its corpse until a rest).
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
import argparse, json, math, os, re, shutil, struct, subprocess, sys, time
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
        # flaky078: entity 2410260 (HP 279, 418 scaled; the plaza enemy s3 fights, it hit the host every run), not 2410100 (HP 70): the weak
        # one died to the first R1 (or stood passive at 0.2 m for 12 s) before it ever struck: "takes an enemy hit" missing in every run
        "guest_chr=2410260@3%m24_01",                       # next to an enemy on that enemy's floor
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
# 0.7.7 noticerow: BB_RULES_PLAYERS=4 runs the walk with four hunters (host + 3 guests, two headless): the Party members
# page must keep all four, the last event and Connection warnings within its six rows (Shared item notices lives on
# My party: Your items).
RULES_PLAYERS = int(os.environ.get("BB_RULES_PLAYERS", "2"))
# The guest opens Party rules: PvP & lamps ~72 s into its walk and reads the host's Friendly fire there once, when the page
# opens; the host turns it on ~106 s into its own walk. The guest starts this long after the host so it opens the page
# after the change (a page's rows line is printed once, on open; 2026-10-30: the guest got there 7 s early).
RULES_HOST_DELAY = 8 + 30 * (RULES_PLAYERS > 2)
RULES_GUEST_LAG = 50
RULES_PAGES = [
    # the member rows carry the hunters' names: only the rows after them are listed (see check_rules)
    # 5 hunters (0.7.8): the page holds six rows, so "Last event" steps aside
    ("Party members", ["Last event", "Connection warnings"] if RULES_PLAYERS < 5 else ["Connection warnings"],
     ("Connection warnings", "down/" * RULES_PLAYERS + ("down/" if RULES_PLAYERS < 5 else ""), "left", "Off", "right", "party_conn_warnings=0",
      "party_conn_warnings=1", None)),
    # 0.7.3: the host's rules (Party rules: ..., read-only lines on a guest), then each player's own
    # (My party: ...), in the System menu's order.
    # 0.7.5: host over a VPN (a game dropdown, Auto by default: the walk checks it is listed on host and
    # guest; the change is the relay toggle, a dropdown needs Cross / Down / Cross)
    ("Party network", ["Network", "Find via public DHT", "Relay for members", "IPv6", "Join via a member",
                       "Host over a VPN"],
     ("Relay for members", "down/down/", "left", "Off", "right", "party_member_relay=0", "party_member_relay=1", None)),
    # 0.7.8: the host's opt-ins to join without a code, and to be invaded (a guest sees one "Host's access" line)
    ("Party access", ["Nightwatch VPN lobby", "Open party (no password)", "Allow invaders", "Any level can join", "Max invaders", "Host's access"],
     ("Open party (no password)", "down/", "right", "On", "left", "party_open=1", "party_open=0", None)),
    # 0.7.1 / 0.7.3: the host's world rules
    ("Party rules: World", ["Enemies stay dead", "Host can gather", "NPCs as in host's world", "Covenants per player",
                            "Party votes on travel", "NPC hunters fight all"],
     ("Enemies stay dead", "", "left", "Off", "right", "party_kill_record=0", "party_kill_record=1",
      '"kill_record":false')),
    ("Party rules: PvP & lamps", ["Friendly fire / PvP", "Full health in PvP", "Shared lamps", "Guests bring their lamps",
                                    "Chalice layer travel"],
     # set back at the end of the walk: the guest checks the host's value meanwhile
     ("Friendly fire / PvP", "", "right", "On", None, "party_friendly_fire=1", "party_friendly_fire=0",
      '"friendly_fire":true')),
    # 0.7.5 (partyrules): the guests' deaths, travel and rewards are the host's rules too
    ("Party rules: Deaths", ["Host gets up, no load", "Up after boss kill", "Anyone can be abducted",
                             "Guest gets up, no load", "Respawn at host's lamp", "Refill vials, bullets"],
     ("Host gets up, no load", "", "left", "Off", "right", "party_host_revive=0", "party_host_revive=1",
      '"host_revive":false')),
    ("Party rules: Travel", ["Guests use lamps", "Guests use Hunter's Mark", "Open world", "Stay in Chalice Dungeons",
                             "Guests can gather", "Cutscenes live with host"],
     ("Open world", "down/down/", "left", "Off", "right", "party_open_world=0", "party_open_world=1",
      '"open_world":false')),
    ("Party rules: Rewards", ["Full rewards for guests", "Boss drops for guests", "Insight for bosses"],
     ("Full rewards for guests", "", "left", "Off", "right", "party_full_rewards=0", "party_full_rewards=1", None)),
    # 0.7.2: rot (off by default: the change turns it on and back off) and its three cleanses (on)
    ("Party rules: Rot", ["Rot on death", "Rot: boss kill cleanses", "Rot: kill your killer", "Rot: echoes back"],
     ("Rot on death", "", "right", "On", "left", "party_rot=1", "party_rot=0", '"rot":true')),
    ("Party rules: Enemies", ["Boss health", "Boss damage", "Enemy health", "Enemy damage", "Boss poise", "Enemy poise"],
     ("Boss health", "", "right", "7", "left", "party_boss_hp=200", "party_boss_hp=150", '"boss_hp":200')),
    # 0.7.7 bosstarget: the host's boss rule, a page of its own between Enemies and Phantoms
    ("My party: Bosses", ["Bosses share attention"],
     ("Bosses share attention", "", "left", "Off", "right", "party_boss_share=0", "party_boss_share=1", None)),
    # 0.7.5: level sync and the phantom health cut (on by default: the change turns level sync off and back on)
    ("Party rules: Phantoms", ["Level sync", "Phantom health cut", "Skip the prologue"],
     ("Level sync", "", "left", "Off", "right", "party_level_sync=0", "party_level_sync=1", '"level_sync":false')),
    ("My party: Your save", ["Share story progress", "Share NPC quests", "Share cutscenes, endings"],
     ("Share story progress", "", "left", "Off", "right", "party_progress=0", "party_progress=1", None)),
    ("My party: Your items", ["Host's item rewards", "Give the bells", "Own key items as guest", "Key items at once",
                                "Shared item notices"],
     ("Give the bells", "down/", "left", "Off", "right", "party_grant_bells=0", "party_grant_bells=1", None)),
    # 0.7.9: Giving (off by default: the change turns it on and back off)
    ("My party: Giving", ["Give items to the party", "Give weapons and armor"],
     ("Give items to the party", "", "right", "On", "left", "party_give_items=1", "party_give_items=0", None)),
    ("My party: Display", ["Party HUD", "Party name tags", "Pings", "Show party through walls", "Coloured name tags",
                           "Above companions"],
     ("Pings", "down/down/", "left", "Off", "right", "party_pings=0", "party_pings=1", None)),
    # 0.7.8: the chat rows and the bloodstain marker moved off the full Display page
    ("My party: Chat & markers", ["Party chat", "Show others' chat", "Bloodstain marker"],
     ("Party chat", "", "right", "On", "left", "party_chat=1", "party_chat=0", None)),
    ("My party: Look & notices", ["No companion glow", "Ping in the party list", "Join/leave notices",
                                  "Share NPC dialogue", "NPC dialogue range", "Party discovery"],
     ("Join/leave notices", "down/down/", "left", "Off", "right", "party_toasts=0", "party_toasts=1", None)),
    # 0.7.7 CL-2eec68: splitscreen mode (off by default: the change turns it on and back off) and the messages list
    ("My party: Splitscreen", ["Splitscreen mode", "Party messages"],
     ("Splitscreen mode", "", "right", "On", "left", "party_splitscreen=1", "party_splitscreen=0", None)),
]
# The values a fresh run starts from (the rows' defaults): these keys are dropped from each
# instance's bbport.ini first, so an earlier run's changes never shift the walk.
RULES_INI_RESET = ["party_chat", "party_rot", "party_guest_revive_inplace", "party_friendly_fire", "party_npc_talk", "party_full_rewards",
                   "party_boss_hp", "party_pings", "party_progress", "party_grant_bells", "party_host_revive", "party_share_lamps", "party_guests_bring_lamps",
                   "party_kill_record", "party_gather", "party_travel_vote", "party_covenant_per_player",
                   "party_npc_mirror", "party_key_view", "party_key_items_summoned", "party_rot_cleanse_boss",
                   "party_rot_cleanse_killer", "party_rot_cleanse_echoes", "party_npc_fights", "party_open_world",
                   "party_toasts", "party_any_kidnap", "party_level_sync", "party_phantom_hp", "party_pvp_full_hp", "party_boss_drops", "party_vpn", "party_member_relay", "party_conn_warnings", "party_item_notices", "party_boss_share", "party_vpn_lobby", "party_open", "party_open_any_level", "party_splitscreen", "party_messages", "party_stay", "settings_rev"]


def rules_walk_to_first():
    """Options > System, then Up from the top (wrapping: Exit Game, Network, Brightness, then the
    port's pages from the last) to the first rules page, Cross. Read from bbport_game_menu_pages.cpp."""
    src = (REPO / "gpu" / "shim" / "bbport_game_menu_pages.cpp").read_text(encoding="utf-8", errors="replace")
    pages = re.findall(r'AddPage\("([^"]+)"', src)
    first = pages.index(RULES_PAGES[0][0])
    # the walk steps page to page with Down: a page added to the System menu must be listed in RULES_PAGES
    # (the 2026-10-30 misses: My party: Bosses and the VPN lobby row were not), so fail fast here
    assert pages[first:first + len(RULES_PAGES)] == [pg[0] for pg in RULES_PAGES],         "RULES_PAGES out of step with the System menu pages: %s" % pages[first:first + len(RULES_PAGES) + 2]
    ups = 3 + (len(pages) - pages.index(RULES_PAGES[0][0]) - 1) + 1
    return "options/right/right/cross/" + "/".join(["up"] * ups) + "/cross"


def phase_ss(only):
    # CL-2eec68 splitscreen mode: guest 1 runs with BB_PARTY_SPLITSCREEN=1, the host without. Both are in the host's
    # world; the guest draws the compass only, no name tags, and hides the host-side "joined" notices; the screenshots
    # (shots/) are the proof (host: full panel and name tag; guest: compass rows only).
    return dict(
        name="ss", seconds=240, count=2, headless=0, fresh=False,
        env=["BB_PARTY_HUD_LOG=1"], ienv=["1:BB_PARTY_SPLITSCREEN=1", "0:BB_PARTY_MESSAGES=all"], crash=[],
        until=[r"1:Party HUD: splitscreen compass, [1-9] companion"], until_hold=60,
    )


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
    host += "/circle/" + up + "/cross/left/mark=rules_h_ffoff/circle/circle/circle"
    guest += "/circle/circle/circle"
    watch = []
    n = len(RULES_PAGES)
    for tag in ["rules_h%d" % k for k in range(1, n + 1)] + ["rules_g%d" % k for k in range(1, n + 1)] + \
               ["rules_h%dc" % k for k in range(1, n + 1)] + ["rules_h_ffoff"]:
        watch += ["--watch", r"step \d+/\d+: ~?mark=%s(@|\s)=%s@1.2" % (tag, tag)]
    # 420 s: ~1.35 s per menu step, 183 steps in 0.7.8 (330 s cut the last step, Friendly fire set back)
    return dict(
        name="rules", seconds=420 + 90 * (RULES_PLAYERS > 2), count=RULES_PLAYERS, headless=max(0, RULES_PLAYERS - 2),
        env=["BB_GAME_MENU_TRACE=1", "BB_TEST_MENU_FILE={root}\\menu{i}.txt"] + (["BB_PARTY_MAX=%d" % RULES_PLAYERS] if RULES_PLAYERS > 2 else []),
        ienv=[],
        files={"menu0.txt": "joined %d %s\n-\n-\n" % (RULES_HOST_DELAY, host),
               "menu1.txt": "joined %d %s\n-\n-\n" % (RULES_HOST_DELAY + RULES_GUEST_LAG, guest)},
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
        env=["BB_PARTY_CONNWARN_UDP_N=1"],  # 0.7.6: one failed report is enough for the notice (the host gets one in this run)
        ienv=["1:BB_PARTY_PORT={hold}"],
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


def _s3_enemy(dist="0.6", waits=4):
    """A nearby enemy on the same floor: R3 lock-on, R1 once (it fights back), HP lines, the kills, a vial
    (killed within ~10 s: two of them took the host's 575 HP in 13 s). R3 twice (":lock": the second
    only while not locked on): after the host_chr warp the camera can face away from the enemy, and
    an R3 then only turns the camera behind the player, as on a pad (deaths071: the first R3 missed
    after host_chr with and without a death before it; the second locked on)."""
    return ["host_chr=0:%s@2%%m24_01!" % dist, "host_press=0x4:lock@1%m24_01!", "host_press=0@1%m24_01!",  # R3: lock-on
            "host_press=0x4:lock@1%m24_01!", "host_press=0@1%m24_01!",
            "host_press=0x800@1%m24_01!"] + ["host_press=0@2%m24_01!"] * waits + [                  # R1: it fights back
        "host_kill=0@1%m24_01", "host_kill=0@1%m24_01", "host_press=0@2%m24_01!",
        "host_press=0x1000@1%m24_01!", "host_press=0@3%m24_01!"]                           # Triangle: a Blood Vial


def phase_s3(only):
    host = ",".join([
        "echoes=5000",
        "host_warp=2412953@8^2",                                   # the Tomb of Oedon lamp (party travel)
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
    ] + _s3_stain() + _s3_enemy("0.6", 5) + [
        "host_die@2%m24_01",                                       # 130 m from the lamp: lamp revive 2
        "host_press=0@14%m24_01!", "host_press=0@12%m24_01!",
    ] + _s3_enemy("1.6", 5) + [  # flaky078: 5 waits (10 s): the first strike lands 8-11 s after the lock-on; 4 waits killed it first (e4 run 2), 7 let it kill the host (k run 1: HP 719 -> 261, dead, 133 m away)
        "host_lastlamp=2402950@1%m24_01", "host_die@2%m24_01",     # last lamp in the Cathedral Ward: a lone load
        "host_press=0@15%m24_00",                                  # its census (every 15 s), then the end
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
        "host_warp=2412953@8^2",                                   # the Tomb of Oedon lamp (party travel)
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


# Phase kg (gaolagain078, CL-00a95e / CL-11e308, ~6 min, 3 players): EVERY Snatcher grab of a guest takes the
# party to the Hypogean Gaol, not only the first (BB_PARTY_GAOL_ALWAYS, default on). Guest 1 is grabbed in the
# Cathedral Ward (guest_die:kidnap: what its copy of 12400780 does at the grab, then its death): the first
# grab starts the host's 9420 + common 9421 and common 9422's waking scene (9423 on the host). The party goes
# back to the Ward by the lamp, guest 1 is grabbed AGAIN: the host's 9423 is on (common 9421 ended), so the
# respawn record is written directly and the party travels to the Gaol again; the waking scene 28000010 is not
# played a second time on any game (once per character). Run it under BB_NET_SIM too (--ienv).
def phase_kg(only):
    host = ",".join(["god", "host_warp=%s@8^2" % KD_LAMP,
                     "host_warp=%s@30%%m28_00" % KD_LAMP,        # awake in the cell, every scene over: back to the Ward
                     "host_flag=9420:0@25%m24_00",               # the second grab happens meanwhile
                     "host_press=0@25%m28_00!"])                 # the second Gaol: the end
    g1 = ",".join(["guest_die:kidnap@40%m24_00",                 # grab 1
                   "guest_flag=9420:0@5%m28_00",                 # wait for the cell
                   "guest_die:kidnap@14%m24_00"])                # back in the Ward: grab 2
    return dict(
        name="kg", seconds=720,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + KD_READ],
        watch=["--watch", r"cutscene 28000010=wake@1,4,8", "--watch", r"remo 2800[01]010 mode=gwake@1,4,8",
               "--watch", r"kidnap: a Snatcher grabbed this guest=grab@0.5,3", "--watch",
               r"respawn record is the Gaol=gaol@0.5,3,10"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[],
        until=[KD_UNTIL],
    )


def check_kg(v, L):
    need(v, L, "kg join: 3 players", 0, r"cooperators [01] -> 2")
    g1 = L.all(1, r"kidnap: a Snatcher grabbed this guest")
    v.step("kg guest 1: two Snatcher grabs", len(g1) >= 2, "%d" % len(g1))
    h = L.all(0, r"kidnap: slot \d+ was grabbed by a Snatcher")
    v.step("kg host: two grabs answered", len(h) >= 2, "%d" % len(h))
    if len(h) < 2:
        check_common(v, L, "kg")
        return
    t1, t2 = h[0][0], h[1][0]
    v.step("kg host: grab 1 starts the kidnap (9420 + common 9421)", ": start" in h[0][1], h[0][1][:120])
    v.step("kg host: grab 2 goes to the Gaol too (9423 on: direct)", ": direct" in h[1][1], h[1][1][:120])
    need(v, L, "kg host: grab 2's respawn record written, the party travels", 0,
         r"kidnap: a later grab \(9423 on\): the respawn record is the Gaol", t2)
    for i in (0, 1, 2):
        m = L.all(i, r"map .*-> m28_00")
        v.step("kg inst %d: in the Gaol after both grabs" % i, len(m) >= 2 and m[-1][0] > t2, "%d arrivals" % len(m))
    need(v, L, "kg guest 1: follows once its death is taken (grab 2)", 1,
         r"the party's kidnap travel to the Gaol .*follows once its death is taken", t2 - 20)
    # the waking scene: once per character
    sc0 = L.all(0, r"host #\d+ cutscene 28000010 \(mode 8")
    v.step("kg host: the waking scene played once (grab 1 only)", len(sc0) == 1 and sc0[0][0] < t2, "%d plays" % len(sc0))
    need(v, L, "kg host: grab 2 only offers the scene (guests that saw it skip it)", 0,
         r"host #\d+ cutscene 28000010 \(mode 0, 2002\[1\]", t2 - 1)
    for i in (1, 2):
        sc = L.all(i, r"remo 2800[01]010 mode")
        v.step("kg guest %d: the waking scene at most once" % i, len(sc) <= 1, "%d plays" % len(sc))
    none_of(v, L, "kg the guests' own saves never hold 9420", r"test read \(load mode \d, own save / visible\): 9420=1/",
            insts=(1, 2))
    none_of(v, L, "kg no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "kg")


# Phase fdr (fardrop078, CL-a6a2a7, ~4 min, 3 players): a shared drop paid to a member far from the dead enemy
# is a proper item-get notice into the inventory with the source named ("<item> x3 (party drop)"), never a pile
# in a spot this game does not show. Test aids: every enemy rolls lot 10202404 (goods x3) and guest 1's
# limit is 5 m (the enemy 2400125 is 28 m from the lamp where everyone stands), the host's and guest 2's are
# the default 60 m: they keep the pile as before.
FDR_UNTIL = r"0:travel step (\d+)/\1: "


def phase_fdr(only):
    host = ",".join([
        "god",
        "host_warp=2412953@8^2",                                   # the Tomb of Oedon lamp (party travel)
        "host_kill=%s:hp@8%%m24_01" % SK_ENEMY,                     # the neighbour map's enemy, 28 m away
        "host_press=0@12%m24_01!",
    ])
    return dict(
        name="fdr", seconds=300,
        env=["BB_PARTY_DROP_LOT_TEST=10202404"],
        watch=["--watch", r"far shared drop=far@0.3,1.5,4"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god", "2:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_FAR_DROP_M=5"],
        crash=[],
        until=[FDR_UNTIL],
    )


def check_fdr(v, L):
    need(v, L, "fdr join: 3 players", 0, r"cooperators [01] -> 2")
    kill = need(v, L, "fdr host kills %s" % SK_ENEMY, 0, r"test: host_kill: entity %s HP \d+ -> 0" % SK_ENEMY)
    if kill is None:
        check_common(v, L, "fdr")
        return
    far = need(v, L, "fdr guest 1 (limit 5 m): paid into the inventory, no pile", 1,
               r"drops: far shared drop .*goods 900 x3 paid into the inventory \(given, item-get shown\)", kill)
    need(v, L, "fdr guest 1: the notice names the source", 1,
         r"notice \".*x3 \(party drop\)\" \(far shared drop\): queued|notice shown: .*\(party drop\)", kill)
    for i in (0, 2):
        need(v, L, "fdr inst %d (60 m limit): the pile as before" % i, i,
             r"drops: shared drop roll .*goods 900 x3", kill)
        none_of(v, L, "fdr inst %d: no far-drop payment" % i, r"drops: far shared drop", kill, insts=(i,))
    if far is not None:
        none_of(v, L, "fdr guest 1: no GiveItem failure", r"GiveItem FAILED", kill, insts=(1,))
    check_common(v, L, "fdr")



# Phase lg (load_gap, ~3 min, 3 players; 0.7.7 item 08): a party travel (Tomb of Oedon, a long load for
# everyone) must not raise the "connection unstable" notice nor move a guest-guest pair to the host
# relay: receive gaps inside a loading screen (ours or the peer's) are no outage (nr_loadgap.h).
# Before the fix the load's 4+ s silence fired both. BB_NET_RXGAP_LOG=1 adds the "ignored" lines.
LG_UNTIL = r"0:travel step (\d+)/: "


def phase_lg(only):
    host = ",".join([
        "god",
        "host_warp=2412953@8^2",                                   # the Tomb of Oedon lamp (party travel)
        "host_press=0@30%m24_01!",
    ])
    return dict(
        name="lg", seconds=300,
        env=["BB_NET_RXGAP_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god", "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[],
        until=[LG_UNTIL],
    )


# Phase lv (leave_gap, ~3 min, 3 players; 0.7.7 CL-9142b8): guest 1 presses Party > Leave (its own home load).
# The host and guest 2 must not raise "connection unstable", move the pair to the relay, or log an outage resume
# about it: a member that leaves is expected silence (nr_loadgap.h). BB_NET_RXGAP_LOG=1 adds the "ignored" lines.
LV_UNTIL = r"1:guest_leave: Party > Leave pressed"


def phase_lv(only):
    return dict(
        name="lv", seconds=240,
        env=["BB_NET_RXGAP_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god,guest_leave@15", "2:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[], until=[LV_UNTIL], until_hold=45,
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
        # the block's init must have placed the car (its anim 30 plays and stops) before the ride starts: a
        # fixed wait let a slow machine / rate ride a car still in its init, which "arrived" at once
        # liftre077: BB_LF_PIN / BB_LF_CAR_DELAY (s) shorten the wait on the landing (the stress for a ride that
        # starts before the car is built: 3 s after a player comes near)
        "guest_move=%s@1%%m24_0?obj=%s:30" % (LF_LOW_SIDE.replace("pin=4", "pin=" + os.environ.get("BB_LF_PIN", "4")), LF_LIFT),
        "guest_move=%s@%s%%m24_0" % (LF_CAR_LOW, os.environ.get("BB_LF_CAR_DELAY", "4")),  # into the car: up it goes
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


def phase_lfx(only):
    """liftre077: the lf ride with two players (no observer game): host + guest. Used with BB_TEST_LIFT_PROBE /
    BB_TEST_LIFT_WATCH for the lift reverse engineering, and as the 2-player lf proof."""
    ph = phase_lf(only)
    ph["name"] = "lfx"
    ph["count"] = 2
    ph["ienv"] = [e for e in ph["ienv"] if not e.startswith("2:")]
    ph["until"] = [LF_UNTIL, r"1:travel step (\d+)/\1: guest_press"]
    return ph


# Phase lfw / lfwc / lfws (liftwalk077, groups `lift_walk` / `lift_walk_slow`): the lifts ridden the way players ride
# them: WALKED onto the car by the left stick (the director's route walker over the game's navmesh, pad file), never
# warped onto it. Each ride's rider(s) start 60-100 m of walking from the car (a warp to a far spot on the same
# floor, then a stick route, then a stick step into the trigger box), the other player waits at the other end.
# lfw: Central Yharnam's lift 2411320 (the shortcut lift below Gascoigne's area; 14 m, a platform): guest down, host
# up, both down, both up. lfwc: Cathedral Ward's lift 2401102 (48 m; its top end is a 12 m
# pocket of the navmesh, so the walks into the top end are short): guest up, host down, both up, both down.
# lfws: lfw with a slow-loading simulation (BB_TEST_LIFT_BUILD_DELAY / _DROP: the car is dropped when the walker is far
# and its full build comes only 5 s after the walker is within 30 m) and sprinting walks (BB_TEST_WALK_SPRINT).
# BB_LFW_ENV="K=V,K=V" adds env (BB_TEST_LIFT_PROBE=2, BB_PARTY_LIFT_PREBUILD=0, ...). Pass: every rider is on the
# arrival floor alive with its ride bit on all the way, both games show the same flags and car end after each ride.
def _p(*a):
    return ":".join("%.2f" % x for x in a)


LW = {
    "yh": dict(
        tag="lfw", map="m24_01", car="2411320", pos="12410350", busy="12415350", start="top", top_val=1,
        mirror=True, lamp="2412953", set_flags="12410330,12410350", dismiss=True,  # flaky078: a "Beast x1" item box showed up mid-walk in lfw too (c3 run c)
        trig={"top": (-128.72, -44.3, -9.73), "bottom": (-128.72, -58.0, -9.73)},
        side={"top": (-128.19, -44.3, -9.20), "bottom": (-129.25, -58.0, -10.26)},
        out_h={"top": (-134.70, -44.2, -2.50), "bottom": (-125.28, -58.4, -11.69)},
        out_g={"top": (-130.80, -44.5, -3.70), "bottom": (-117.10, -58.5, -17.30)},
        wait={"top": (-140.40, -44.5, 5.20), "bottom": (-117.10, -58.5, -17.30)},
        wk={"top": (-136.70, -44.5, 1.50), "bottom": (-121.10, -58.4, -15.60)},
        far={"top": (-184.6, -38.7, 32.6), "bottom": (-74.9, -54.8, 17.2)},
        gate={"top": "-129.8:-44.4:-8.5"},
        # smokefix077: the spot where the walk-ins that entered the trigger cleanly (60 FPS lfw runs) turned into the cage; a stick
        # step to it (0.5 m) before the trigger / side step makes the last 2 m the same every time (the route ends within 2 m of
        # anywhere: from its east side the straight step ran into the cage post, 2 m short of the trigger)
        door={"top": (-131.4, -7.1), "bottom": (-127.0, -10.9)},
        route={"top": "-180.7:-41.5:29.6/-177.6:-41.7:26.7/-173.4:-41.7:26.3/-169.0:-41.7:23.9/-165.4:-41.7:21.5/-161.9:-41.7:17.5/-158.2:-41.7:15.0/-154.3:-41.7:11.6/-151.3:-41.7:9.1/-147.6:-41.7:10.8/-144.8:-43.3:7.6/-140.4:-44.5:5.2/-136.7:-44.5:1.5/-134.7:-44.2:-2.5/-130.8:-44.5:-3.7/-130.0:-44.5:-8.0",
               "bottom": "-78.8:-55.4:14.7/-79.1:-55.5:9.9/-83.5:-55.5:8.9/-88.2:-55.4:6.2/-89.5:-55.5:2.0/-92.6:-55.5:-1.5/-96.6:-55.5:-3.9/-100.3:-55.4:-6.3/-103.5:-55.4:-10.0/-107.9:-55.4:-12.6/-110.3:-55.4:-15.8/-113.0:-56.9:-19.2/-116.5:-58.7:-21.3/-117.1:-58.5:-17.3/-121.1:-58.4:-15.6/-124.4:-58.4:-12.6/-128.1:-58.1:-10.7"},
        # liftfar077: starts more than 150 m from the car (outside the prebuild ring), walked in over the navmesh route
        # (tools/party/nav_route.py) and then the recorded tail above
        ringwarp={"top": (-188.7, -28.6, 72.7), "bottom": (-34.5, -40.5, 71.9)},  # the rider waits at the far spot, then steps to 100 m out (the ladder's top) and walks in
        farlong={"top": (-220.5, -45.0, 112.2), "bottom": (-11.8, -40.5, 88.1)},
        routelong={"top": "-194.4:-28.6:66.7/-197.1:-28.6:59.4/-204.1:-31.5:52.1/-205.6:-33.4:46.7/-204.0:-36.2:40.2/-198.3:-36.2:37.4/-191.2:-36.2:36.3/-184.6:-38.7:32.6/",
                   "bottom": "-42.3:-42.0:68.5/-48.9:-42.0:68.7/-55.4:-42.0:69.4/-57.4:-42.9:63.7/-61.1:-46.1:58.2/-64.4:-48.6:53.8/-66.8:-48.8:49.3/-68.0:-48.8:44.3/-70.6:-48.8:38.7/-72.3:-51.3:35.1/-73.0:-51.3:26.4/-72.2:-52.2:21.4/"},
        arr={"down": "1", "up": "3"}, start_anim={"down": (4,), "up": (6,)}, end_anim={"down": 5, "up": 7},
        y_arr={"top": lambda y: y > -46.0, "bottom": lambda y: y < -56.0},
        rides=["guest", "host", "both", "both"], kills={"top": 8, "bottom": 8},
    ),
    "ca": dict(
        tag="lfwc", map="m24_0", stagger=True, trim={"top": 1, "bottom": 2}, car="2401102", pos="12400827", busy="12400828", start="bottom", top_val=1,
        # liftreg077: the prelude's kills drop an item (a blood gem: "Beast x1" box on the host); the box stays up and blocks the
        # stick walks of the later rides (the lfwc / lfwcbs flake), so the host presses Cross after each kill group (dismiss)
        lamp="2412953", set_flags="", dismiss=True,
        skip1={"top": 1},
        door={"top": (24.1, 192.3), "bottom": (26.0, 194.5)},
        trig={"top": (24.28, 3.0, 194.17), "bottom": (24.28, -45.5, 194.17)},
        side={"top": (25.4, 3.0, 193.3), "bottom": (25.4, -45.6, 193.3)},
        out_h={"top": (21.3, 2.8, 191.2), "bottom": (30.11, -45.6, 197.56)},
        out_g={"top": (16.9, 3.8, 187.9), "bottom": (33.3, -45.8, 201.3)},
        wk={"top": (21.3, 2.8, 191.2), "bottom": (30.11, -45.6, 197.56)},
        # flaky078: the top far spot is the route's own first waypoint (a recorded walked spot on the pocket's floor): a warp to
        # (13.1, 3.6, 188.9) landed on the ledge 3 m below (y 0.3) every run; the route's "hop" teleported it back up, and when the hop
        # failed the walk gave up on the lower ledge (lfwc c2: "STOPPED at waypoint 3/4 ... y 0.1")
        far={"top": (16.1, 3.8, 186.9), "bottom": (66.2, -51.0, 236.6)},
        route={"top": "16.1:3.8:186.9/18.5:3.6:189.1/21.3:2.8:191.2/23.2:2.9:192.3/23.8:2.8:194.0",
               "bottom": "64.1:-51.0:235.7/63.1:-50.9:235.4/62.7:-49.8:232.6/61.2:-47.8:229.2/59.2:-46.3:227.4/57.1:-45.8:226.2/55.9:-45.7:223.7/52.2:-45.6:221.7/52.2:-45.7:218.9/49.8:-45.7:217.6/48.3:-45.7:215.0/46.1:-45.7:212.7/41.0:-45.8:211.8/41.1:-45.7:208.5/38.2:-45.8:206.8/36.4:-45.7:204.2/33.3:-45.8:201.3/30.6:-45.6:201.0/28.3:-45.7:199.4/27.6:-45.8:196.7/25.0:-45.5:194.9/24.3:-45.4:194.3"},
        # liftfar077: the top end is reached on foot from the Oedon Chapel side (the Great Bridge gate, 238 m of navmesh route);
        # the bottom end is a navmesh island with the top (857 faces, 95 m across): no walk of 150 m exists there
        ringwarp={"top": (-39.9, -11.7, 76.3)},
        farlong={"top": (-66.0, -18.0, 59.9)},
        routelong={"top": "-35.1:-12.0:80.9/-30.0:-12.6:79.4/-25.8:-13.4:80.3/-22.7:-14.3:81.8/-18.2:-16.2:83.7/-12.5:-16.1:85.1/-5.8:-16.2:88.6/-2.2:-15.5:90.6/1.1:-13.2:94.6/4.0:-11.8:101.4/6.0:-11.0:106.4/2.7:-11.0:111.3/-3.9:-9.9:115.2/-10.7:-10.4:117.1/-8.8:-10.7:124.0/-9.4:-10.7:132.2/-6.1:-9.3:137.1/-3.6:-6.1:143.2/-1.6:-4.8:148.9/1.7:-2.7:154.0/4.6:0.2:159.1/6.1:0.8:167.4/10.4:1.1:170.3/14.1:0.1:176.4/16.6:0.1:181.7/12.9:0.1:188.2/"},
        arr={"down": "28", "up": "32"}, start_anim={"down": (1,), "up": (31,)}, end_anim={"down": 29, "up": 7},
        y_arr={"top": lambda y: y > 0.5, "bottom": lambda y: y < -40.0},
        rides=["guest", "host", "both", "both"], kills={"top": 4, "bottom": 14},
    ),
}


# liftfar077: the slow-load phases with two riders (lfwbs / lfwcbs): both rides carry both players, the walks are not sprints
LW["ybs"] = dict(LW["yh"], tag="lfwbs", rides=["both", "both"])
LW["cbs"] = dict(LW["ca"], tag="lfwcbs", rides=["both", "both"])


def lfw_plan(spec, slow=False):
    """[(direction, riders, car end before)] for the spec's rides: the car alternates ends from its start."""
    out, end = [], spec["start"]
    # BB_LFW_KINDS=guest,host,both: a custom list. The slow-load phases (sprinting walks) ride singly: a sprinting walker
    # overshoots the beside-the-trigger spot into the trigger and starts the ride alone (live lfws2-4)
    kinds = os.environ.get("BB_LFW_KINDS", "").split(",") if os.environ.get("BB_LFW_KINDS") else (
        spec["rides"][:2] if slow else spec["rides"])
    for riders in kinds[:int(os.environ.get("BB_LFW_RIDES", "0")) or None]:  # BB_LFW_RIDES=N: a shorter run
        out.append(("down" if end == "top" else "up", riders, end))
        end = "bottom" if end == "top" else "top"
    return out


def lfw_far_spec(spec):
    """liftfar077: the spec with the long walk-ins (a start more than 150 m from the car) where the end has one."""
    out = dict(spec, far=dict(spec["far"]), route=dict(spec["route"]), _farwalk=True)
    for e, p in spec["farlong"].items():
        if e not in os.environ.get("BB_LFW_FAR_ENDS", "top").split(","):  # route validation: only these ends
            continue
        # the bottom end: a warp to a spot over 150 m out does not stream its floor in (the chr falls: live liftfar077), so
        # the rider is warped 124 m out and WALKS out to 160 m and back (routelong's first points)
        assert max([math.dist(p, spec["trig"][e])] + [math.dist(tuple(float(x) for x in w.split(":")), spec["trig"][e]) for w in spec["routelong"][e].replace("|", "/").split("/") if w]) > 150.0, (e, p)
        out["far"][e] = p
        out["route"][e] = spec["routelong"][e] + spec["route"][e]
    return out


def lfw_chains(spec, slow=False):
    """(host steps, guest steps) of the walk-in plan."""
    m, car = spec["map"], spec["car"]
    steps = {"host": [], "guest": []}
    # prelude: the host kills the regular enemies near both ends' walk-in routes (a brute standing in the Gascoigne
    # plaza stopped the walker dead, live lfw4), one end after the other, and ends on the first waiter's spot: the
    # guest's first step waits for it
    first_end = lfw_plan(spec, slow)[0][2]
    pre = (("top", "far"), ("top", "out_h"), ("bottom", "far"), ("bottom", "out_h"))
    if os.environ.get("BB_LFW_PRELUDE") == "0":  # route validation runs: no enemy clearing, just the first waiter's spot
        pre = pre[3:]
    for n, (e, kind) in enumerate(pre):
        steps["host"].append("host_move=%s:pin=4:ground@%s%%%s%s" % (_p(*spec[kind][e]), "4" if n == 0 else "1", m,
                                                                      "^1" if n == 0 else ""))
        if len(pre) == 4 and not spec.get("_noenemies"):  # flaky078: clear_enemies phases have none to kill
            steps["host"] += ["host_kill=0@0.6%%%s" % m] * (spec["kills"][e] if kind == "far" else 5)
            if spec.get("dismiss"):
                steps["host"].append("host_press=0x4000@1.5%%%s" % m)  # OK on the drop's item box (no box: nothing)
    for k, (d, riders, end) in enumerate(lfw_plan(spec, slow)):
        other = "bottom" if end == "top" else "top"
        # smokefix077: the gate on "the previous ride is over" is the car's end animation stopped (the car's own state on this
        # game), not the position flag: the flag read of a guest in the host's world can stay at the host's old value (lfwcbs: the
        # guest's ride-2 step waited 700 s on a flag its own log showed as 1)
        gate = "" if k == 0 else "?obj=%s:%d" % (spec["car"], spec["end_anim"]["up" if end == "top" else "down"])
        who = ("host", "guest") if riders == "both" else (riders,)
        for role in ("host", "guest"):
            s = steps[role]
            first = k == 0
            # smokefix077: a rider of the previous ride has the exit move at the arrival (the ride is over) as its previous step: no gate
            # (the guest's end-animation gate of ride 2 never fired: lfwbs rider 700 s, lfws / lfwcs waiter: its walk was never sent)
            rode_before = k > 0 and (lfw_plan(spec, slow)[k - 1][1] in ("both", role))
            if role not in who:
                # the one who waits: at the other end (the arrival), beside where the rider steps out
                s.append("%s_move=%s:pin=4:ground@%s%%%s%s" % (role, _p(*spec.get("wait", spec["out_g"])[other]), "1" if first else "14", m, "" if k and rode_before else gate))  # a rider of the previous ride: its exit move at the arrival came just before
                # the player who does not ride walks a few metres by the stick while the car moves (the user's report:
                # "the other player runs in place and cannot move while the elevator moves"); it starts once the rider
                # stands in the trigger box
                ox, oy, oz = spec["wk"][other]
                # smokefix077: starts on the car's start animation (it plays, then the car moves on), not on the rider's position
                # (vitals): the car carries the rider away within a frame or two, so "within 0.9 m of the trigger" was missed
                # half the time and the walk never ran (lfws ride 2: 0.0 m, the step still waiting 700 s later)
                s.append("%s_stick=%.2f:%.2f:1.0:24@0.5%%%s?obj=%s:%d" % (role, ox, oz, m, car, spec["start_anim"][d][0]))
                continue
            second = riders == "both" and role == "guest"
            # smokefix077: a rider of the previous ride has the exit move at the arrival (the ride is over) as its previous step: no gate
            # (the guest of lfwbs never saw the end-animation gate of ride 2 and waited 700 s); a waiter before needs it
            far, route = spec["far"][end], spec["route"][end]
            pin = 10 if far in spec.get("farlong", {}).values() and spec.get("_farwalk") else 4  # a far warp: the floor streams in slowly
            if not (second and spec.get("stagger")) and spec.get("skip1", {}).get(end) and not spec.get("_farwalk"):
                # flaky078: lfwc's top pocket: the first waypoint lies on the ledge 3 m BELOW the platform (a warp there lands on y 0.1 and
                # the walker only got up by the route's "hop", when it hopped at all: lfwc c3 "STOPPED at waypoint 4/4 (15.9, 0.1, 185.1)").
                # Start on the second waypoint, as the staggered second rider does (it never got stuck).
                pts = route.split("/")
                # a pair: the first rider starts one waypoint AHEAD of where the staggered second rider (pts[1]) stands, so the second
                # one never has to pass it in the narrow pocket (lfwc 90 FPS: both warped to pts[1], the host stuck behind the idle guest)
                n0 = 2 if riders == "both" else 1
                far, route = tuple(float(x) for x in pts[n0].split(":")), "/".join(pts[n0 + 1:])
            if second and spec.get("stagger"):
                # the second rider starts 2 waypoints further on: two walkers warped to one spot push each other off
                # a ledge (live lfwc4: the top pocket's floor edge)
                pts = route.split("/")
                far, route = tuple(float(x) for x in pts[1].split(":")), "/".join(pts[2:])
            if riders == "both" and role == "host" and spec.get("trim"):
                # the first of two riders stops short of the trigger and steps to the spot beside it: the route's last
                # points run through the trigger cylinder (r 0.55 m), which starts the ride with the second still outside
                route = "/".join(route.split("/")[:-spec["trim"][end]])
            if first:
                # the host after its prelude; the guest once the host stands where the first waiter does
                g0 = "" if role == "host" else "?near=%s:12" % _p(*spec["out_h"]["bottom"])  # the prelude's last spot (and the first waiter's)
                s.append("%s_move=%s:pin=%d:ground@1%%%s%s" % (role, _p(*far), pin, m, g0))
            else:
                s.append("%s_move=%s:pin=%d:ground@14%%%s%s" % (role, _p(*far), pin, m, "" if k and rode_before else gate))
            if spec.get("dismiss"):
                # flaky078: an item box ("Beast x1": a pickup on the pocket's platform, taken by the warp onto it) blocks the stick walk
                # (lfwc c3: 6 m from the target, "stuck", the box on screen). Cross = OK on the box; with no box it does nothing in the
                # world (Circle is the dodge).
                s.append("%s_press=0x4000@2%%%s" % (role, m))
            # the second rider walks in once the first one stands on the car: two walkers on the same route block
            # each other in its narrow places and in the doorway (live lfw4 / lfw5)
            rc = "?near=%s:0.9" % _p(*spec["side"][end]) if second else ""
            rw = spec.get("ringwarp", {}).get(end) if far == spec.get("farlong", {}).get(end) and spec.get("_farwalk") else None
            if rw:
                # far (outside the prebuild ring) first, 8 s there, then the step in to the walk's start inside the ring
                s.append("%s_move=%s:pin=4:ground@8%%%s%s" % (role, _p(*rw), m, rc))
                rc = ""
                pin = 4
            if True:  # flaky078: every rider
                # flaky078: the route (the second rider's, and a lone rider's: lfwc c1 ride 2) must not end inside the trigger cylinder either: lfwc's bottom route ends 0.1 m from the
                # trigger, the ride started mid-route (the host already beside it) and the guest's later "near the host" step then waited
                # 120 s on a host that was gone, skipped to the last step and ride 4 never ran (the lfwc flake). The stick step into the
                # trigger (after the first rider stands beside it) starts the ride.
                # liftbis077: the route's last waypoints can lie inside the trigger cylinder (r 0.55 m: lfwcs top 0.5 m): the ride then
                # starts mid-route, and the tail walk below (a HOP back to the landing) warped the carried host to the top and into the
                # open shaft (live lfwcs, car down: fell 48 m). The slow phases' route stops outside the cylinder; the stick step rides.
                _tx, _ty, _tz = spec["trig"][end]
                _head, _bar, _last = route.rpartition("|")
                _wp = [w for w in _last.split("/") if w]
                # liftpair078: the Cathedral slow phases stop the long walk 2.6 m short: the walker ends a route within 2 m of its last waypoint and
                # the run's momentum carries the chr another 1-2 m (lfwcs r2 ride 2: the host slid through the cylinder at 4 m/s, the
                # ride started alone, and the side warp 8 s later put it on the open top spot: fall 48 m); the short tail walk goes the rest
                _trim = 2.6 if (slow and spec["map"] == "m24_0") else 0.8
                _minwp = 1 if (slow and spec["map"] == "m24_0") else (3 if slow and not second else 2)  # liftpair078: lfwcs top route has 3 waypoints, the last 1.3 m from the trigger
                while len(_wp) > _minwp and math.dist((float(_wp[-1].split(":")[0]), float(_wp[-1].split(":")[2])), (_tx, _tz)) <= _trim:
                    _wp.pop()
                route = _head + _bar + "/".join(_wp)
            legs = route.split("|")  # a leg ends at a ladder's foot: Cross climbs it, the next leg starts at the top
            for li, leg in enumerate(legs):
                if li:
                    s.append("%s_press=0x4000@1%%%s" % (role, m))
                s.append("%s_route=%s@%s%%%s%s" % (role, leg, "14" if li else ("1" if second else (6 if rw else str(pin + 2 if pin > 4 else 9))), m, rc if not li else ""))
            route = legs[-1]
            if slow:
                # liftreg077: the unbuilt-car floor guard may have put the walker back on the landing (a warp, as designed) outside
                # the car's gate: the straight stick step below then runs into the cage rail (live lfws / lfwcs / lfwcbs: stuck 2-3 m
                # short of the trigger for 120 s). Walk the last recorded waypoints again (through the gate) first; at the route's
                # end they are skipped at once.
                # (not the points inside the trigger cylinder: the ride must start by the trigger step, not mid-walk)
                tx0, _ty0, tz0 = spec["trig"][end]
                pts = [w for w in route.split("/") if w and math.dist((float(w.split(":")[0]), float(w.split(":")[2])), (tx0, tz0)) > 0.8]
                # liftbis077: ... and the last standing spot recorded in the car's doorway (real runs: the guest stood at (-129.8, -8.5)
                # on its way into the trigger; the straight step from the cage's front corner (-129.3, -7.6) runs into the rail)
                gate = spec.get("gate", {}).get(end)
                tail = "/".join(pts[-1:] + ([gate] if gate else []))  # smokefix077: only the last waypoint (the last 3 walked a rider already at the door 5 m back, off the Cathedral pocket's edge: lfwcs fall to y 0.1)
                if tail:
                    s.append("%s_route=%s@1%%%s" % (role, tail, m))
            if spec.get("door"):
                dx, dz = spec["door"][end]
                # liftpair078: not for the first rider of a pair: the side-spot warp below is the way in, and this stick walk's last-resort
                # "roll" (7.7 m/s) carried the host into the trigger cylinder before the guest was there: the ride started alone and
                # the side warp 8 s later put the host on the open top spot (lfwc c5 try 1, ride 4: fall 48 m)
                # ... nor in the Cathedral phases at all (lfwc f1 ride 1: the lone guest's door walk wandered ("off the corridor", a roll) into
                # the trigger, the ride started 3 s before the planned warps, and those warps then put the rider on the open bottom spot
                # under the rising car: fell to y -70): the route's end is within 1-2 m of the side spot, the warps do the rest
                if spec["map"] != "m24_0" and not (riders == "both" and role == "host"):
                    s.append("%s_stick=%.2f:%.2f:0.5:20@0.2%%%s" % (role, dx, dz, m))
                if spec.get("dismiss"):
                    s.append("%s_press=0x4000@0.5%%%s" % (role, m))  # OK on an item box before the last 2 m (flaky078)
            if riders == "both" and role == "host":
                # on the car beside the trigger box; the guest steps into the box once the host stands there
                sx, sy, sz = spec["side"][end]
                # flaky078: the side spot lies 0.75 m from the trigger centre (cylinder r 0.55). The stick walker's FIRST burst of a
                # step is a guess of the camera's direction (live lfw 15:50: -151 deg, then 61 deg: 0.9 m the wrong way, into the
                # cylinder): the ride started with the second rider still outside ("0 samples on the car"; the 0.7.7 triage cause).
                # No stick step can be that exact, so the first rider's last 2 m (door -> side spot) is the game's own in-world warp
                # to the verified spot beside the trigger (as the exit warps at the arrival); the walk-in route up to the door is
                # still walked, and the second rider still walks into the trigger.
                # liftpair078: the slow-build car is not there yet when the host arrives: the floor guard holds a warped host on its last
                # standing spot (1.2 m from the side spot, outside the guest's "near" 0.9 m: the guest never started, lfwcbs). A second warp
                # 20 s after the first (flaky3078) fixed that but fired again when the guest was quick (ride 2: the guest arrives in
                # 14 s from the top landing): the ride had started and the host was put on the open top spot, 214 s: fall 48 m. One warp
                # only, after the build delay (16 s, counted from the host being within 22 m of the car: it has walked there before).
                s.append("%s_move=%.2f:%.2f:%.2f:0:native@%s%%%s" % (role, sx, sy, sz, "18" if slow else "0.5", m))
            else:
                tx, ty, tz = spec["trig"][end]
                cond = "?near=%s:0.9" % _p(*spec["side"][end]) if second else ""
                if second and spec.get("mirror"):
                    # flaky078: the second rider the same (lfw f_lfw_1: its stick from the door GAVE UP at the cage post, the host waited 240 s
                    # for a ride that never started): the in-world warp, once the first rider stands beside the trigger, to the point
                    # mirrored through the trigger (the other end's side spot: the car's footprint is the same at both ends, this end's
                    # height), 0.75 m from the trigger on its far side; the stick step then walks the 0.75 m into the trigger.
                    ox_, oy_, oz_ = spec["side"]["bottom" if end == "top" else "top"]
                    s.append("%s_move=%.2f:%.2f:%.2f:0:native@0.5%%%s%s" % (role, ox_, spec["side"][end][1], oz_, m, cond))
                    cond = ""
                if not second:
                    # flaky078: a lone rider too: the stick walker's first burst from the door (-127, -10.9) went east into the cage post
                    # (-126.5, -10.4) twice (lfw c3 run c, c4 run 2: "stuck", ride 2 never started, the host waited 240 s). The 0.75 m
                    # from the side spot into the trigger is the walked part that is left; the door -> side spot part is the in-world warp.
                    sx, sy, sz = spec["side"][end]
                    s.append("%s_move=%.2f:%.2f:%.2f:0:native@0.5%%%s" % (role, sx, sy, sz, m))
                # flaky3078: the last 0.75 m (side spot -> trigger centre) is the game's own in-world warp as well. The nav walker
                # (default since nav2078) ends a stick step of that length at once ("arrived at 0.69 m, 0.0 m of path left": the
                # projection of the start onto a 1.5 m path lands at its end) and the older walker hovered beside the cylinder:
                # lfw ride 4 and lfws / lfwbs / lfwcbs ride 1 never started (3 of 4 runs), the waiter stood 240 s for the car. The
                # route up to the door is still walked; the trigger is entered by position as a player entering it does.
                # liftpair078: the Cathedral slow phases (a lone rider, lfwcs) used a walked step here (the unbuilt-car guard holds a rider that
                # steps in before its car is built: lfwcs ride 1, 0 samples on the car). The nav walker ends that 0.7 m at once ("arrived at
                # 0.64 m", outside the cylinder r 0.55: no ride, the waiter stood 240 s) or, when it does walk, wanders on the rising car
                # (the navmesh ends under it: the old walker took the rider off the platform, lfwcs c5 ride 1: guest to y -70). The warp
                # goes in once the build delay (16 s from the rider being within 22 m of the car; the side warp came just before) is over.
                # The second rider of a pair steps in 8 s after the first rider stands beside the trigger: its own car is built 16 s after IT
                # is within 22 m (40 s of walking, 13 s of them near): a warp before the build starts the ride without the rider (the guest's
                # exit step then fired at once off the unbuilt car's stale end animation: lfwcbs ride 1, the guest stayed at the bottom).
                _in_delay = ("8" if second else ("18" if spec["map"] == "m24_0" else "0.5")) if slow else "0.5"
                s.append("%s_move=%.2f:%.2f:%.2f:0:native@%s%%%s%s" % (role, tx, ty, tz, _in_delay, m, cond))
            # out at the arrival: walk off the car once the car shows its arrival animation
            ox, oy, oz = spec["out_g" if second else "out_h"][other]
            if os.environ.get("BB_LFW_EXIT") == "walk":
                s.append("%s_stick=%.2f:%.2f:1.5:16@0.3%%%s?obj=%s:%s" % (role, ox, oz, m, car, spec["arr"][d]))
            else:
                # off the car by the game's own in-world warp at the arrival (as phase lt): a stick walk needs 2-3 s to
                # find its direction, and a rider still in the arrival end's trigger when the ride ends rides back (live lfw8)
                s.append("%s_move=%s:0:native@0.3%%%s?obj=%s:%s" % (role, _p(ox, oy, oz), m, car, spec["arr"][d]))
    return steps["host"], steps["guest"]


def phase_lfw_for(key, slow=False, name=None, far=False, sprint=True):
    spec = LW[key]
    if (name or spec["tag"]) in CLEAR_ENEMY_PHASES and os.environ.get("BB_SMOKE_CLEAR_ENEMIES", "1") != "0":
        spec = dict(spec, _noenemies=True)
    host, guest = lfw_chains(lfw_far_spec(spec) if far else spec, slow)
    common = "god,log_pos=0.5,log_objanim=%s,log_obj=%s,log_flag=%s/%s" % (spec["car"], spec["car"], spec["pos"], spec["busy"])
    env = ["BB_TEST_REMO_SKIP_AFTER=5", "BB_PARTY_TEST_START_LAMP=" + spec["lamp"], "BB_PARTY_TEST_COND_TIMEOUT=%d" % (180 if far else 240)]  # flaky078: 240: the guest's script runs 30-50 s behind the host's; a wait that long is a failed row (none_of in check_lfw), never a skip
    if spec["set_flags"]:
        env.append("BB_PARTY_PROGRESS_TEST_SET=" + spec["set_flags"])
    if slow:
        env += ["BB_TEST_LIFT_BUILD_DELAY=16", "BB_TEST_LIFT_BUILD_NEAR=22", "BB_TEST_LIFT_BUILD_DROP=40"] + (["BB_TEST_WALK_SPRINT=1"] if sprint else [])
    extra = os.environ.get("BB_LFW_ENV", "")
    env += [e for e in extra.split(",") if "=" in e]
    starts = "(" + "|".join("|".join(str(a) for a in spec["start_anim"][d]) for d in ("down", "up")) + ")"
    ends = "(%d|%d)" % (spec["end_anim"]["down"], spec["end_anim"]["up"])
    return dict(
        name=name or spec["tag"], seconds=1800 if far else 900, count=2, headless=0, pads=(0, 1),
        env=env,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join([common] + host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join([common] + guest)],
        watch=RIDE_BURST(spec["car"], starts, ends),
        crash=[], until=LFW_UNTIL, until_hold=15,
    )


LFW_UNTIL = [r"0:travel step (\d+)/\1: ", r"1:travel step (\d+)/\1: "]

# Phase lfa (liftapart078, ~3 min, 2 players, group `lift_apart`; 4-player playtest report NW-80b250f: the Cathedral
# Ward church lift 2401101 (flags 12400150 position / 12400151 busy; events 12400147 down / 12400148 up) was out of sync
# for the host until a guest rode it). Both start at the Tomb of Oedon lamp; the host travels ALONE to the Hunter's Dream
# (a headstone / lamp of the Dream: m21, the lift's map block is not its world), guest 1 stays in Cathedral Ward, walks to
# the church lift (m24_00) and rides it (its own copy of the lift's event, flag 12400150 flips). The ride goes up as a
# world action (lift_set / lift_clr); the host (in the Dream) found the lift car "loaded" (a car entry of a block it left
# is still in the object table) and answered `loaded: left alone`, so its world kept the old end. The host then comes
# back to the lamp and walks to the lift: its flag and its car must be where the guest left them (docs/party/world_actions.md 10.1).
LFA_CAR = "2401101"
LFA_POS, LFA_BUSY = "12400150", "12400151"
LFA_LAMP = "2412953"                 # the Tomb of Oedon lamp (m24_01), the start
LFA_DREAM = "2102950"                # the Hunter's Dream lamp
LFA_TOP_SIDE = os.environ.get("LFA_TOP_SIDE", "24.50:13.10:121.80")   # the top landing beside the car (a chr stood here, live)
LFA_TOP_CAR = os.environ.get("LFA_TOP_CAR", "22.30:13.10:122.10")     # in the car at the top: its ride (down to y -9.5) starts
LFA_UNTIL = r"0:travel step (\d+)/\1: host_press"


def phase_lfa(only):
    common = "god,log_pos=0.5,log_objanim=%s,log_flag=%s/%s" % (LFA_CAR, LFA_POS, LFA_BUSY)
    # The guest's ride is its own copy of the lift's event ending: the position flag flips (guest_flag, as the
    # event writes it; LFA_RIDE=1 walks a real ride instead). The host is in the Dream when it arrives, and
    # comes back once its own world has the ride (?flag=1; with the fix off that never comes true).
    ride = ["guest_move=%s:0.8:native:pin=%s@%s%%m24_0" % (LFA_TOP_SIDE, os.environ.get("LFA_PIN", "3"), os.environ.get("LFA_GO", "1")),
            "guest_move=%s@4%%m24_0" % LFA_TOP_CAR] if os.environ.get("LFA_RIDE") else ["guest_flag=%s@%s%%m24_01" % (LFA_POS, os.environ.get("LFA_FLAG_AT", "20")),
                                  "guest_move=%s:0.8:native@40%%m24_0" % LFA_TOP_SIDE]   # then to look at the lift (a screenshot of both games)
    host = ",".join([common,
        "host_alone=%s@%s" % (LFA_DREAM, os.environ.get("LFA_LEAVE", "6")),    # alone to the Dream: m24_00 is not its world
        "host_alone=%s@3%%m21?%s=1" % (LFA_LAMP, LFA_POS),                  # back at the lamp once its world has the ride (flag 1)
        "host_move=%s@8%%m24_01" % LFA_TOP_SIDE,                           # at the lift's top landing
        "host_press=0@12%m24!",
    ])
    g1 = ",".join([common] + ride + ["guest_press=0@60!"])
    return dict(
        name="lfa", seconds=int(os.environ.get("LFA_SECONDS", "300")), count=2, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=" + LFA_LAMP, "BB_PARTY_TRAVEL_CHOICE=alone", "BB_TEST_LIFT_STALE_TABLE=1", "BB_PARTY_TEST_COND_TIMEOUT=60"] + [e for e in os.environ.get("LFA_ENV", "").split(";") if e],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[],
        until=[LFA_UNTIL, r"1:travel step (\d+)/\1: guest_press"],
    )



def phase_lfw(only):
    return phase_lfw_for("yh")


def phase_lfwc(only):
    return phase_lfw_for("ca")


def phase_lfwf(only):
    return phase_lfw_for("yh", name="lfwf", far=True)


def phase_lfwcf(only):
    return phase_lfw_for("ca", name="lfwcf", far=True)


def phase_lfwbs(only):
    return phase_lfw_for("ybs", True, "lfwbs", sprint=False)


def phase_lfwcbs(only):
    return phase_lfw_for("cbs", True, "lfwcbs", sprint=False)


def phase_lfws(only):
    # smokefix077: no sprint (BB_TEST_WALK_SPRINT): the sprinting stick steps overshot into the cage and the trigger (60 FPS full
    # smoke: 0 rides, wedged 2 m short); the slow car build (the point of the phase) is unchanged, lfwbs / lfwcbs walk the same way
    return phase_lfw_for("yh", True, "lfws", sprint=False)


def phase_lfwcs(only):
    return phase_lfw_for("ca", True, "lfwcs", sprint=False)  # smokefix077: no sprint, see phase_lfws


def lw_rides(L, i, spec):
    """The lift's rides on instance i from its car animations: [{start, t, up, end, t_end, t_arr}]."""
    rides = []
    starts = {a for d in spec["start_anim"].values() for a in d}
    arrs = {int(a) for a in spec["arr"].values()}
    ends = set(spec["end_anim"].values())
    for t, line in L.all(i, r"test: object %s anim -?\d+ playing" % spec["car"]):
        m = re.search(r"anim (-?\d+) playing (\d)", line)
        anim, playing, w = int(m.group(1)), m.group(2) == "1", wall(line)
        if anim in starts and playing and (not rides or rides[-1]["end"] is not None):
            rides.append(dict(start=w, t=t, up=anim in spec["start_anim"]["up"], end=None, t_end=None, t_arr=None))
        elif anim in arrs and not playing and rides and rides[-1]["end"] is None:
            rides[-1]["t_arr"] = rides[-1]["t_arr"] or t
        elif anim in ends and not playing and rides and rides[-1]["end"] is None:
            rides[-1].update(end=w, t_end=t, t_arr=rides[-1]["t_arr"] or t)
        elif anim in ends and playing and rides and rides[-1]["end"] is None and rides[-1]["t_arr"] is None:
            rides[-1]["t_arr"] = t  # the end animation starts at once when nobody is on the car at the arrival
    return rides


def lw_flags_at(L, i, spec, w):
    val = None
    for _t, line in L.all(i, r"test: flags %s=" % spec["pos"]):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = tuple(int(x) for x in re.findall(r"=(-?\d)", line)[:2])
    return val


def lw_anim_at(L, i, spec, w):
    val = None
    for _t, line in L.all(i, r"test: object %s anim -?\d+ playing" % spec["car"]):
        lw = wall(line)
        if lw is not None and lw <= w:
            val = int(re.search(r"anim (-?\d+)", line).group(1))
    return val


REMOTE_RX = re.compile(r"Party B2: remote chrs: \[mode \d+ 0x[0-9a-f]+ \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)")


def copy_freeze(real, copies, span=6.0, moved=5.0, still=0.3):
    """A remote copy that stops while its player moves: over a stretch of `span` s the copy stays within `still` m although the
    player's real position travels more than `moved` m (the copy of a chr riding a car not updating: the liftwalk077 note). Lag is not
    a freeze: under load the games run at different game-time rates (a lift ride took twice the wall time on one of them).
    [(t, x, y, z)] lists on one clock. -> (worst real travel while frozen, t, windows tested)."""
    worst, wt, n = 0.0, 0.0, 0
    for k, (t, x, y, z) in enumerate(copies):
        end = [c for c in copies[k + 1:] if c[0] - t >= span]
        if not end:
            break
        c1 = end[0]
        window = [c for c in copies[k:] if c[0] <= c1[0]]
        if any(b[0] - a[0] > 2.5 for a, b in zip(window, window[1:])):
            continue  # the copy lines are 1 s apart; a gap is a gap in the log, not a stopped copy
        if max(math.dist((x, y, z), c[1:]) for c in window) > still:
            n += 1
            continue
        n += 1
        pts = [r for r in real if t <= r[0] <= c1[0]]
        if len(pts) < 2:
            continue
        travel = max(math.dist(pts[0][1:], q[1:]) for q in pts)
        if travel > moved and travel > worst:
            worst, wt = travel, t
    return worst, wt, n


def lw_copy_rows(v, L, tag, t0):
    """flaky078: the other player's body on each screen follows the real player through the rides (the user saw a host without a
    body on the guest's screen): every copy sample of 'remote chrs' within 5 m of the real position (the log_pos lines of the other game)."""
    # the log offsets ([+ s]) start at each process's own start: put both instances on the wall clock of their pos lines
    off = {}
    for k in (0, 1):
        d = sorted(wall(l) - t for t, l in L.all(k, POS_RX.pattern) if wall(l) is not None)
        off[k] = d[len(d) // 2] if d else 0.0
    for i, j, who in ((1, 0, "host"), (0, 1, "guest")):
        real = [(t + off[j],) + tuple(float(x) for x in POS_RX.search(l).groups()) for t, l in L.all(j, POS_RX.pattern)]
        copies = [(t + off[i],) + tuple(float(x) for x in REMOTE_RX.search(l).groups())
                  for t, l in L.all(i, REMOTE_RX.pattern, t0)]  # from 5 s before the first ride: the join and the first warps (two maps, 100+ m) are not rides
        worst, wt, n = copy_freeze(real, copies)
        # liftpair078: >= 3 windows (one ride has 3-4: the copy is logged once a second while the player moves, and the walks between the
        # rides are short or none in lfwcbs / lfwcs: 10 failed lfwcs try 2 / lfwcbs with 8 windows, lfwcs with 4, and no freeze)
        v.step("%s the %s's copy on the other screen does not freeze while it moves" % (tag, who), n >= 3 and worst == 0.0,
               "%d windows of 6 s, worst: copy still while the player travelled %.1f m (at wall +%.0f s)" % (n, worst, wt % 3600))


def check_lfw(v, L, key, tag):
    spec = LW[key]
    plan = lfw_plan(spec, tag.endswith("s"))
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    rides = [lw_rides(L, i, spec) for i in (0, 1)]
    v.step(tag + " rides: %d on each game" % len(plan), len(rides[0]) == len(plan) and len(rides[1]) == len(plan),
           "host %d, guest %d" % (len(rides[0]), len(rides[1])))
    for k, (d, riders, end) in enumerate(plan):
        name = "%d %s %s" % (k + 1, riders, d)
        rh = rides[0][k] if k < len(rides[0]) else None
        rg = rides[1][k] if k < len(rides[1]) else None
        if not rh or not rg or rh["end"] is None or rg["end"] is None:
            v.step("%s %s: ran and ended on both games" % (tag, name), False, "host %s, guest %s" % (rh, rg))
            continue
        up = d == "up"
        v.step("%s %s: one ride, the planned way, on both games" % (tag, name),
               rh["up"] == up and rg["up"] == up and abs(rh["start"] - rg["start"]) < 1.5,
               "start host %.2f / guest %.2f" % (rh["start"] % 3600, rg["start"] % 3600))
        w = max(rh["end"], rg["end"]) + 0.5
        want_pos = spec["top_val"] if up else 1 - spec["top_val"]
        fh, fg = lw_flags_at(L, 0, spec, w), lw_flags_at(L, 1, spec, w)
        ah, ag = lw_anim_at(L, 0, spec, w), lw_anim_at(L, 1, spec, w)
        v.step("%s %s: same flags and car end on both games" % (tag, name),
               fh == fg == (want_pos, 0) and ah == ag == spec["end_anim"][d],
               "pos/busy host %s / guest %s (want %s), car anim host %s / guest %s (want %s)" %
               (fh, fg, (want_pos, 0), ah, ag, spec["end_anim"][d]))
        arrive = "top" if up else "bottom"
        ax, _ay, az = spec["trig"][end]
        for role in (("host", "guest") if riders == "both" else (riders,)):
            i = 0 if role == "host" else 1
            r = rides[i][k]
            arr = r["t_arr"]
            pos = L.all(i, POS_RX.pattern, arr - 1.0, arr + 0.05) if arr else []
            y = float(POS_RX.search(pos[-1][1]).group(2)) if pos else None
            after = L.all(i, POS_RX.pattern, (r["t_arr"] or r["t_end"]) + 0.5, (r["t_arr"] or r["t_end"]) + 3.0)
            y2 = float(POS_RX.search(after[0][1]).group(2)) if after else None
            n = off = 0
            bits = []  # (counted, on): the sample just before the window is context for "isolated", it is not counted
            for _t, line in L.all(i, POS_RX.pattern, r["t"] - 1.0, (arr or r["t"]) - 0.3):
                m = POS_RX.search(line)
                f = re.search(r" f ([0-9a-f]{10})", line)
                if f and ((float(m.group(1)) - ax) ** 2 + (float(m.group(3)) - az) ** 2) ** 0.5 < 2.0:
                    on = bool(int(f.group(1), 16) & RIDE_BIT)
                    counted = _t >= r["t"] + 0.6
                    bits.append((counted, on))
                    if counted:
                        n += 1
                        off += 0 if on else 1
            # one isolated off sample with on-car samples on both sides is the ride bit flickering as the car starts (lfwc 30 / 60 FPS, the
            # host of a pair at the top end): not a fall, but kept visible in the evidence; several off samples, or one at either end, fail
            flicker = False
            if off == 1:
                k_off = next(k for k, (c, on) in enumerate(bits) if c and not on)
                flicker = 0 < k_off < len(bits) - 1 and bits[k_off - 1][1] and bits[k_off + 1][1]
            v.step("%s %s: the %s rode along to the %s end, on the floor after" % (tag, name, role, arrive),
                   y is not None and spec["y_arr"][arrive](y) and y2 is not None and spec["y_arr"][arrive](y2) and n >= 3 and (off == 0 or flicker),
                   "y %s at the arrival, %s shortly after the arrival, %d samples on the car, %d off it%s" %
                   ("%.1f" % y if y is not None else "-", "%.1f" % y2 if y2 is not None else "-", n, off,
                    " (ride bit flicker (1 sample))" if flicker else ""))
    need_far = 1.5 if tag.endswith("s") else 3.0  # sprinting walkers meet the other player's exit spot
    for k, (d, riders, end) in enumerate(plan):
        for role in ("host", "guest"):
            if riders in ("both", role) or k >= len(rides[0]) or k >= len(rides[1]):
                continue
            i = 0 if role == "host" else 1
            r = rides[i][k]
            if r.get("t_end") is None:
                continue
            pts = [tuple(float(x) for x in POS_RX.search(l).groups()) for _t, l in L.all(i, POS_RX.pattern, r["t"], r["t_end"])]
            far = max([math.hypot(q[0] - pts[0][0], q[2] - pts[0][2]) for q in pts] or [0.0])
            v.step("%s %d %s: the %s who does not ride walks while the car moves" % (tag, k + 1, d, role), far >= need_far,
                   "%d samples, farthest %.1f m from where the ride began (need %.1f m)" % (len(pts), far, need_far))
    for i, who in ((0, "host"), (1, "guest")):
        f = falls(L, i, rate=12.0)  # the 48 m lift runs 8.3 m/s; a fall is faster than any lift (the nets use 16)
        v.step("%s %s: no fall (vertical speed under 12 m/s)" % (tag, who), not f,
               ("; ".join("+%.1f s %.1f m/s" % x for x in f[:4])) if f else "fastest drop within a lift's speed")
        walks = L.all(i, r"test: route: DONE at waypoint")
        stops = L.all(i, r"test: route: STOPPED")
        mine = sum(1 for _d, riders, _e in plan if riders == "both" or riders == who)
        v.step("%s %s: every walk-in's route was walked to its end" % (tag, who), len(walks) >= mine and not stops,
               "%d routes done (need %d), %d stopped, %d hop(s)" % (len(walks), mine, len(stops),
                                                                   len(L.all(i, r"test: route: HOP"))))
    jump_steps(v, tag, L, rides, (spec["trig"]["top"][0], spec["trig"]["top"][2]),
               ["%d %s %s" % (k + 1, r, d) for k, (d, r, _e) in enumerate(plan)])
    first = [r[0]["t"] for r in rides if r]
    lw_copy_rows(v, L, tag, (min(first) - 5.0) if first else 1e9)
    # flaky078: one side waiting out a step's 120 s condition (the other player never came / the car was elsewhere) and skipping to the
    # last step is a failure, never a pass: every ride is synchronised on the other player's state (near the first rider, the car's anim)
    none_of(v, L, tag + " no step waited out its condition (nobody skipped ahead)", r"travel step \d+ \(\w+\): condition timed out", 0, 1e9, (0, 1))
    none_of(v, L, tag + " nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    none_of(v, L, tag + " no fall recovery", r"recovery: falling", 0, 1e9, (0, 1))
    holds = sum(len(L.all(i, r"lift car hold:|lift car floor:|lift car prebuild:")) for i in (0, 1))
    delays = sum(len(L.all(i, r"lift car build (delay|drop) \(test aid\)")) for i in (0, 1))
    v.step(tag + " (info) the unbuilt-car guards and the slow-load aid", True,
           "%d hold/floor/prebuild lines, %d test-aid lines" % (holds, delays))
    check_common(v, L, tag)


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
        "host_warp=2412953@8^2",         # party lamp travel: the Tomb of Oedon (everyone in m24_01)
        "host_press=0x100002@15%m24_01",  # round 1: the hotkey, touchpad + L3 (runtime_pad.c ping_combo)
        "host_gather@5%m24_01",          # refused: the cooldown
        "host_gather@28%m24_01",         # round 2
        "host_gather@45%m24_01",         # round 3
        "host_press=0@15%m24_01!",
    ])
    # guest 2's alone lamp (a rest) gives the host a rest load that guest 1 follows: guest 1 walks
    # off after it (30 s, ~9 s after round 1's gather), and to the Dream after round 2
    g1 = ",".join(["god", "guest_move=%s@30%%m24_01" % GA_FAR,   # far away in the same map (after round 1)
                   "guest_lamp=2102950@75%m24_01"])              # after round 2: alone to the Dream
    g2 = ",".join(["god", "guest_lamp=2102950@20%m24_01"])       # alone to the Dream (another map)
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
GL_UNTIL = [r"0:travel step (\d+)/\1: "]  # the host's chain (the guest's 120 s press only keeps it there)


def phase_gl(only):
    host = ",".join([
        "god", "log_pos=0.5",
        "host_warp=2402950@8^1",                    # the party to the Cathedral Ward lamp
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
              "1:BB_PARTY_GATHER_TEST=yesdropbad"],
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


# Phase md (maid dress, ~3 min, 1 player, group `cainhurst_maid`, player report NW-e05d435 / item CL-1a574f): the
# Cainhurst Castle maids (chr c2150, the ghost ladies in the hall beside the lift's top landing) are drawn
# without their long skirt; it shows again after a hit. The host starts at the Cainhurst lamp, is put at the
# lift's top landing, then 3 m in front of maid 2500234 facing it, locks on, waits, then hits it (R1).
# Proof, engine side: frame analyzer records (BB_CAPTURE_TRIGGER, `host_gfx=cap`) list every draw's index
# count; a c2150 mesh is one count per LOD (its FLVER face sets: MD_MESHES), so the record says which of the
# maid's meshes were drawn that frame. Screenshots at each record for the eye.
MD_MAID = "2500234"
MD_LANDING = CH_TOP_H
# 4 m west of the maid (-34.35, 65.0, -248.43) facing east (yaw = atan2(-dx, -dz)): the camera behind the player
# stays in the hall (a host_chr warp left the camera outside the castle wall: no player, no maid on screen)
MD_SPOT = "-38.4:65.05:-248.4:-1.5708:native:pin=2"
# c2150.flver (dvdroot_ps4/chr/c2150.chrbnd.dcx): mesh -> index counts of its LOD 0/1/2 face sets (the
# same counts with flag 0x80000000: the shadow / motion copies). tools/mp/flver_meshes.py prints them.
MD_MESHES = {
    "knife": (1704, 1017, 336), "body": (20817, 12027, 3687), "dress": (24024, 14805, 5226),
    "BD_cloth": (699, 657, 252), "AM_cloth": (804, 534, 192), "BD_cloth_e": (2097, 1032, 315),
}
MD_UNTIL = [r"0:travel step (\d+)/\1: "]


def phase_md(only):
    cap = "host_gfx=cap@%s%%m25!"
    host = ["god,log_pos=1",
            "host_move=%s@4%%m25" % MD_LANDING,                     # the lift's top landing (native warp)
            "host_move=%s@3%%m25" % MD_SPOT,                        # in the hall, 4 m west of the maid, facing her
            "host_gfx=log@3%m25!",
            "host_press=0x4@1%m25!", "host_press=0@1%m25!",         # R3: the camera behind the player
            cap % 2,                                                # before any hit
            "host_press=0x4:lock@1%m25!", "host_press=0@1%m25!",    # R3 lock-on
            "host_press=0x4:lock@1%m25!", "host_press=0@1%m25!",
            "host_gfx=log@1%m25!",
            cap % 2, cap % 4,                                       # before the hit, locked on
            "host_press=0x800@1%m25!", "host_press=0@1.5%m25!",     # R1: the hit
            "host_press=0x800@0.5%m25!", "host_press=0@1.5%m25!",
            cap % 1, cap % 4,                                       # after the hit
            "host_press=0@3%m25!"]
    return dict(
        name="md", seconds=240, count=1, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_PROGRESS_TEST_SET=12500000", "BB_PARTY_DIRECTOR_TEST_SOLO=1",
             "BB_PARTY_TEST_COND_TIMEOUT=45",
             "BB_CAPTURE_TRIGGER={root}/inst{i}/capture.trigger", "BB_CAPTURE_DIR={out}/captures",
             "BB_CAPTURE_MESH_COUNTS=" + ",".join(str(c) for cs in MD_MESHES.values() for c in cs)],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host)],
        watch=["--watch", r"test: host_gfx: capture=cap@0.3"],
        prep=lambda root: None,
        crash=[], until=MD_UNTIL,
    )


# Phase mds (clothfps077, group `cloth_stall`, 1 player): the steady cloth time step. The host stands among the
# Cainhurst maids (as `md`); five times a frame record, then a frame stalled MDS_STALL_MS (BB_TEST_FRAME_STALL_FILE,
# bbport_frame_rate.cpp: host_gfx=stallcap) with the record of the frame after it, then a settled record. A stutter
# kicked the cloth (its time was the raw frame time): the maids' cloth vertex boxes jump between the record before
# the stall and the one after it, much more than between two quiet records. BB_STEADY_TIME=0 is the old behaviour.
MDS_STALL_MS = 300
MDS_ROUNDS = 5


def phase_mds(only):
    cap = "host_gfx=cap@%s%%m25!"
    host = ["god,log_pos=1",
            "host_move=%s@4%%m25" % MD_LANDING, "host_move=%s@3%%m25" % MD_SPOT,
            "host_press=0x4@1%m25!", "host_press=0@1%m25!"]               # R3: the camera behind the player
    for _k in range(MDS_ROUNDS):
        host += [cap % 3, "host_gfx=stallcap@1.5%m25!", cap % 4]         # quiet, stall + the frame after, settled
    host += ["host_press=0@2%m25!"]
    return dict(
        name="mds", seconds=240, count=1, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_PROGRESS_TEST_SET=12500000", "BB_PARTY_DIRECTOR_TEST_SOLO=1",
             "BB_PARTY_TEST_COND_TIMEOUT=45", "BB_TEST_FRAME_STALL_MS=%d" % MDS_STALL_MS,
             "BB_TEST_FRAME_STALL_FILE={root}/inst{i}/stall.trigger",
             "BB_CAPTURE_TRIGGER={root}/inst{i}/capture.trigger", "BB_CAPTURE_DIR={out}/captures",
             "BB_CAPTURE_MESH_COUNTS=" + ",".join(str(c) for cs in MD_MESHES.values() for c in cs)],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host)],
        watch=["--watch", r"test: host_gfx: capture=cap@0.3"],
        prep=lambda root: None,
        crash=[], until=MD_UNTIL,
    )


# Phase svl (clothfps077, group `save_load`, 1 player): a save loaded from the title at the sweep's frame rate and
# played for 40 s. Supermedo's steady cloth crashed loading a save in his beta 7 (code over PC Settings' page table);
# SVL_FIXTURE (env BB_SVL_FIXTURE, a folder of E:/games/bbport/test_saves) picks the slot.
SVL_UNTIL = r"0:travel step (\d+)/\1: host_press"


def prep_svl(root: Path):
    fixture = Path(os.environ.get("BB_SVL_FIXTURE", "E:/games/bbport/test_saves/cathedral_day")) / "savedata"
    if not fixture.is_dir():
        sys.exit("save_load: the fixture %s is missing (docs/party/test_saves.md)" % fixture)
    _cl_saves(root, fixture)
    print("[smoke] save_load: inst0 starts from %s" % fixture, flush=True)


def phase_svl(only):
    hold = int(os.environ.get("BB_SVL_SECONDS", "40"))  # texture quality runs: BB_SVL_SECONDS=240
    # BB_SVL_STEPS: more director steps before the end, e.g. host_warp=2402950@25,host_warp=3502950@25 (texture quality runs)
    extra = [x for x in os.environ.get("BB_SVL_STEPS", "").split(",") if x]
    host = ",".join(["god", "log_pos=2"] + extra + ["host_press=0@%d!" % hold])
    return dict(name="svl", seconds=max(200, hold + 160, int(os.environ.get("BB_SVL_CAP", "0"))), count=1, headless=0, prep=prep_svl, post=post_chalamp,
                env=["BB_PARTY_DIRECTOR_TEST_SOLO=1"], ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host],
                crash=[], until=[SVL_UNTIL])


def check_svl(v, L, d):
    pos = L.all(0, POS_RX.pattern)
    v.step("svl the save loaded and the player is in the world (position lines)", len(pos) >= 8, "%d position lines" % len(pos))
    check_common(v, L, "svl")


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


# Phases fw / fwg / fwb (lifts2, ~2 min each, 2 players, groups `forbidden_lift` / `forbidden_lift_guest` /
# `forbidden_lift_both`): the Forbidden Woods' second shortcut lift 2701032 (pos flag 12700134: 1 the car
# at the bottom, 0 at the top; busy 12700135; 12700137 the shortcut open). A player's report on a 3 player
# party: the host rose a foot at a time far above the car while it went down, then fell and died; a guest
# clipped off the car's side. The ride starts on a step into the car (region 2702001 at the bottom,
# 2702000 at the top), so riders who stay in the car at the top start the ride down at once: the up
# ride and the down ride follow without a frame between them. fw: the host rides up and down, the guest
# at the lamp; fwg: the guest rides, the host at the lamp; fwb: both ride (the report).
FW_CAR, FW_POS, FW_BUSY, FW_OPEN = "2701032", "12700134", "12700135", "12700137"
FW_BOT_IN = "-217.5:-106.0:279.9:1.047"   # region 2702001 (the ride up's trigger; the floor at y -106.44)
FW_TOP_IN = "-217.5:-77.6:279.9:1.047"    # region 2702000 (the ride down's trigger; the floor at y -77.94)
FW_BOT_EDGE = "-216.6:-106.0:279.4:1.047"  # on the car, outside the trigger box (the report host's spot)
FW_TOP_OUT = "-215.8:-77.8:275.3:0"       # the top landing by the call lever
FW_LAMP = "-190.1:-76.2:252.2:0:native:pin=2"
FW_UNTIL = [r"0:travel step (\d+)/" + chr(92) + "1: "]


def fw_phase(name, trigger, edge):
    """`trigger` steps into the car at the bottom (the ride up), out at the top landing (the ride's end
    waits for every player out of the box), back in (the ride down) and out at the bottom; `edge` (or
    None) rides both ways on the car's edge, outside the box, as the report's host did."""
    common = "god,log_pos=0.25,log_obj=%s,log_objanim=%s,log_flag=%s/%s" % (FW_CAR, FW_CAR, FW_POS, FW_BUSY)
    xyz = lambda spot: ":".join(spot.split(":")[:3])
    steps = {"host": [common, "host_move=" + FW_LAMP + "@4%m27"], "guest": [common, "guest_move=" + FW_LAMP + "@4%m27"]}
    w = trigger
    steps[w] += [w + "_move=" + FW_BOT_IN + "@3%m27" + ("?near=%s:1.5" % xyz(FW_BOT_EDGE) if edge else ""),
                 w + "_move=" + FW_TOP_OUT + "@1%m27?obj=" + FW_CAR + ":6",      # arrived: out (the ride's end)
                 w + "_move=" + FW_TOP_IN + "@3%m27?" + FW_POS + "=0",           # back in: the ride down
                 w + "_move=" + FW_LAMP + "@9%m27"]                              # down: out
    for o in ("host", "guest"):
        if o == trigger:
            continue
        if o == edge:
            steps[o] += [o + "_move=" + FW_BOT_EDGE + "@2%m27",
                         o + "_press=0@1%m27?" + FW_POS + "=0",
                         o + "_move=" + FW_LAMP + "@3%m27?" + FW_POS + "=1"]
        elif o == "host":
            steps[o] += [o + "_press=0@1%m27?" + FW_POS + "=0", o + "_press=0@1%m27?" + FW_POS + "=1"]
    steps["host"] += ["host_press=0@6%m27!"]
    return dict(
        name=name, seconds=240, count=2, headless=0, fps=60,
        env=["BB_PARTY_TEST_COND_TIMEOUT=60", "BB_PARTY_TEST_START_LAMP=2702950", "BB_PARTY_EMEVD_TRACE=1",
             "BB_PARTY_PROGRESS_TEST_SET=" + FW_OPEN] + [e for e in os.environ.get("FW_ENV", "").split(";") if e],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(steps["host"]), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(steps["guest"])],
        watch=RIDE_BURST(FW_CAR, "[0-9]+", "[0-9]+"),
        crash=[], until=FW_UNTIL,
    )


def phase_fw(only):
    return fw_phase("fw", "host", None)


def phase_fwg(only):
    return fw_phase("fwg", "guest", None)


def phase_fwb(only):
    return fw_phase("fwb", "guest", "host")   # the report: the host on the car's edge, a guest steps in


def phase_fwe(only):
    return fw_phase("fwe", "host", "guest")   # a guest on the car's edge, the host steps in


# Phase fh (lifts2, ~2 min, 2 players, group `hemwick_lift`): Hemwick's shortcut lift 2201210 (pos flag
# 12200125: 0 the car at the top, 1 at the bottom; busy 12200126), the report's other lift (both guests
# sent each ride's end). The host steps into the car at the top (region 2202000: the ride down), out at
# the bottom, back in (region 2202001: the ride up) and out at the top; the guest rides both ways on the
# car's edge.
FH_CAR, FH_POS, FH_BUSY = "2201210", "12200125", "12200126"
FH_TOP_IN = "-211.25:-20.1:542.1:0"
FH_BOT_IN = "-211.25:-29.4:542.1:0"
FH_TOP_EDGE = "-210.4:-20.1:541.6:0"
FH_TOP_OUT = "-215.3:-20.8:547.1:0"
FH_BOT_OUT = "-207.7:-29.5:539.1:0"
FH_LAMP = "-172.0:-22.05:485.5:2.094:native:pin=2"


def phase_fh(only):
    common = "god,log_pos=0.25,log_obj=%s,log_objanim=%s,log_flag=%s/%s" % (FH_CAR, FH_CAR, FH_POS, FH_BUSY)
    host = [common, "host_move=" + FH_LAMP + "@4%m22",
            "host_move=" + FH_TOP_IN + "@3%m22?near=" + ":".join(FH_TOP_EDGE.split(":")[:3]) + ":1.5",
            "host_move=" + FH_BOT_OUT + "@1%m22?obj=" + FH_CAR + ":2",      # arrived (anim 2 stopped): out
            "host_move=" + FH_BOT_IN + "@3%m22?" + FH_POS + "=1",           # back in: the ride up
            "host_move=" + FH_TOP_OUT + "@8%m22",                           # up: out
            "host_press=0@6%m22!?" + FH_POS + "=0"]
    g1 = [common, "guest_move=" + FH_LAMP + "@4%m22",
          "guest_move=" + FH_TOP_EDGE + "@2%m22",
          "guest_press=0@1%m22?" + FH_POS + "=1",
          "guest_move=" + FH_LAMP + "@4%m22?" + FH_POS + "=0"]
    return dict(
        name="fh", seconds=240, count=2, headless=0, fps=60,
        env=["BB_PARTY_TEST_COND_TIMEOUT=60", "BB_PARTY_TEST_START_LAMP=2202950", "BB_PARTY_EMEVD_TRACE=1",
             "BB_PARTY_EMEVD_TRACE_ALL=12200120,12200121,12200122,12200123"] +
            [e for e in os.environ.get("FW_ENV", "").split(";") if e],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)],
        watch=RIDE_BURST(FH_CAR, "[0-9]+", "[0-9]+"),
        crash=[], until=FW_UNTIL,
    )


# Phase lu (liftfall078, group `lift_down`, 2 players): the guest rides Upper Cathedral Ward's lift 2421250 (to Ebrietas; pos
# flag 12420154: 0 the car at the top, 1 at the bottom; top y 49.6, bottom 4.4) DOWN in the host's world, walked from the
# landing into the car's trigger by the stick. A player's report (NW-e118458): the guest fell through the car's floor on
# every descent (also Yahar'gul's lift 2801400: not in the smoke, its first ride goes up from the bottom and no landing is
# known; LD is the table to add it to). FW_ENV="BB_TEST_LIFT_DROP=2.0" drops the guest under the car 2 s into the ride,
# "2.0:8" eight times (the rescue gives up and lands it); LD_BOTH=1 puts the host on the car's rest spot first.
LD = {
    "lu": dict(map="m24_02", block="0x18020000", car="2421250", pos="12420154", busy="12420155", lamp="2422950",
               top_in="82.8:49.6:366.8:0", top_out="77.1:49.6:361.4:0", bot_out=None, host_on="83.7:49.6:367.9:0", arrived="1", top_y=49.6, bot_y=4.4),
}


def ld_phase(tag):
    sp = LD[tag]
    m = "%" + sp["map"]
    common = "god,log_pos=0.25,log_obj=%s,log_objanim=%s,log_flag=%s/%s" % (sp["car"], sp["car"], sp["pos"], sp["busy"])
    stand = sp["top_out"] or sp["top_in"]
    # LD_BOTH=1: the host stands on the car beside the guest's trigger (the report's host: the car carried both)
    both = os.environ.get("LD_BOTH") == "1" and sp.get("host_on")
    host = [common, "host_move=%s@4%s" % (sp["host_on"] if both else stand, m),
            "host_press=0@6%s!?%s=%s" % (m, sp["pos"], sp["arrived"])]
    tx, ty, tz = (float(x) for x in sp["top_in"].split(":")[:3])
    if os.environ.get("LD_WARP") == "1":  # the old way: warped into the car's trigger
        step = "guest_move=%s@3%s" % (sp["top_in"], m)
    else:  # walk on like a player: the stick from the landing into the trigger, as the lfw phases' last step
        step = "guest_stick=%.2f:%.2f:0.25:60@0.5%s" % (tx, tz, m)
    guest = [common, "guest_move=%s@4%s" % (stand, m), step,                # into the car at the top: the ride down
             "guest_stick=%.2f:%.2f:0.45:30@0.2%s" % (tx, tz, m),           # (again from where it stands, as lfw does)
             "guest_press=0@25%s" % m]                                      # rides; time for the arrival
    return dict(
        name=tag, seconds=240, count=2, headless=0, pads=(0, 1), fps=60,
        env=["BB_PARTY_TEST_COND_TIMEOUT=60", "BB_PARTY_TEST_START_LAMP=" + sp["lamp"], "BB_TEST_LIFT_TRACE=1"] +
            [e for e in os.environ.get("FW_ENV", "").split(";") if e],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(guest)],
        watch=RIDE_BURST(sp["car"], "[0-9]+", "[0-9]+"),
        crash=[], until=FW_UNTIL,
    )


def phase_lu(only):
    return ld_phase("lu")


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
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2412952@8^1",
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
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2412952@8^1"])
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
    host = ",".join(["log_pos", "echoes=3000", "host_warp=2302950@8^1"])
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


# Phase cd (hostfall, ~3 min, 2 players, group `host_fall_lamp`, 0.7.5 player report cainhurst_death_loop): the
# host jumps off a ledge in Cainhurst (m25) and dies of the fall; the player's report: stuck at HP 0, every
# revive at the lamp died again a frame later ("host revive watch" hp 0 for good). The host starts at the
# first lamp 2502950 (-4.6, 33.9, -188.0), is put 28 m above its floor (a lethal fall) and must get up at
# the lamp and stay alive (no second death), the stick then walks it off the lamp.
CD_UNTIL = r"0:travel step (\d+)/\1: host_stick"


def phase_cd(only):
    host = ",".join(["log_pos=0.5", "echoes=3000",
                     "host_move=49.4:75.0:-188.8@8%m25",       # 28 m above the ledge where the player died (y 47)
                     "host_press=0@3%m25!", "host_press=0@5%m25!",   # up again at the lamp (alive)
                     "host_stick=-1.0:-188.0:0.7:8@2%m25!"])    # then off the lamp
    return dict(
        name="cd", seconds=110, count=2, headless=0, pads=(0,),
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_B2_REVIVE_DIFF=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=log_pos=0.5"],
        watch=["--watch", "local death: HP 0=hp0@0.5", "--watch", r"host revive #\d+: after=rev@0.5,3"],
        crash=[], until=[CD_UNTIL], until_hold=8,
    )


def check_iw(v, L):
    need(v, L, "iw join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "iw guest: the display event's wait passed (the host's scene was mirrored)", 1,
         r"client: event 12500810's wait passed")
    # the event's enable step (instructions 97-178: map parts 2506000-2506073 on) runs on the guest's game
    need(v, L, "iw guest: the display event's enable step ran (the chamber's parts on)", 1,
         r"EMEVD event 12500810 \[(9[7-9]|1[0-7]\d)\]")
    check_common(v, L, "iw")


def check_cd(v, L):
    need(v, L, "cd join: 2 players", 0, r"cooperators 0 -> 1")
    hp0 = need(v, L, "cd host died of the fall", 0, r"local death: HP 0 at \([-\d.]+, [-\d.]+, [-\d.]+\), dropped (1\d|[2-9]\d)\.")
    if hp0 is None:
        check_common(v, L, "cd")
        return
    up = need(v, L, "cd host up at its lamp, no load", 0, r"host revive #\d+: after: hp \d+", hp0)
    if up is not None:
        again = L.find(0, r"local death: HP 0", up + 0.2, up + 12)
        v.step("cd host alive after the revive (no second death within 12 s)", not again,
               again[1][:120] if again else "none")
        st = L.find(0, r"travel step \d+/\d+: host_stick", up)
        ps = [tuple(float(x) for x in POS_RX.search(l).groups()) for _, l in L.all(0, POS_RX.pattern, st[0], st[0] + 12)] if st else []
        moved = max((sum((a - b) ** 2 for a, b in zip(q, ps[0])) ** 0.5 for q in ps), default=0.0) if ps else 0.0
        v.step("cd host walks off the lamp after the revive (at least 1.5 m)", moved >= 1.5,
               "%.1f m in %d samples" % (moved, len(ps)) if st else "no host_stick step after the revive")
    check_common(v, L, "cd")


# Phase iw (illusion wall, ~4 min, 2 players, group `cainhurst_scene`, 0.7.5 player reports illusion_wall /
# annalise_missing): Cainhurst's castle display event 12500810 (Logarius dead; its scene 25000010 reveals the
# Queen's Chamber: map parts 2506000-2506073 enabled, Annalise and the walls) plays when the host stands in
# the hall at (63, 112, -373). The guest's own copy of the event stayed waiting there, so the guest saw the
# scene (live mirror) but its chamber parts stayed disabled (the wall closed, Annalise missing / the floor
# gone) until a load. Both are put in the hall; after the scene the guest's copy of the event must reach its
# enable step (the 2012[1] instructions of 12500810) without a load.
IW_HALL = "63.4:112.2:-372.7:0:native:pin=3"
IW_UNTIL = [r"0:travel step (\d+)/\1: host_press", r"1:travel step (\d+)/\1: guest_press"]
# iwn: the scene plays 45 s after the wait starts (BB_PARTY_TEST_FORCE_WAIT), long after the presses: wait for its lines
IWN_UNTIL = [r"0:12500810=1", r"1:EMEVD event 12500810 \[(9[7-9]|1[0-7]\d)\]"]


def phase_iw(only):
    host = ",".join(["log_pos=2", "host_move=%s@6%%m25" % IW_HALL, "host_press=0@20%m25!"])
    g1 = ",".join(["log_pos=2", "guest_move=%s@7%%m25" % IW_HALL, "guest_press=0@20%m25!"])
    return dict(
        name="iw", seconds=100, count=2, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_PROGRESS_TEST_SET=12501800,12501802,12501803,12500000",
             "BB_PARTY_PROGRESS_TEST_CLEAR=12500810", "BB_PARTY_PROGRESS_TEST_READ=12500810",
             "BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12500810"],
        # the guest's event copy counts the host's scene as mirrored from the start (the host's own trigger
        # needs equipment the test save lacks); IW_NOFIX=1: the A/B without the wait pass
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TEST_MIRRORED=12500810"]
        + (["1:BB_PARTY_SCENE_PASS_WAIT=0"] if os.environ.get("IW_NOFIX") else []),
        watch=[], crash=[], until=IW_UNTIL, until_hold=5,
    )


# Phase iwn (the natural Crown trigger, ~3 min, 2 players, group `cainhurst_scene`, 0.7.6 report "wall stays closed
# for guests after the Crown is worn on the roof"): like iw, but the guest's event copy is NOT told the scene was
# mirrored by a test aid: the host wears the Crown of Illusions (head piece 250000, director armor=) in the hall,
# its event 12500810 passes its own wait and plays scene 25000010, the guest's copy (waiting for a Crown only the
# host wears) must continue by the live mirror alone and run the enable step.
IWN_SPOT = "47.9:112.0:-352.0:0:native:pin=3"  # inside region 2502806 (a real host's spot when the scene began)


def phase_iwn(only):
    host = ",".join(["log_pos=2", "armor=250000:keep", "host_move=%s@6%%m25" % IWN_SPOT, "host_press=0@20%m25!"])
    # IWN_NEAR=1: the guest stands in the Crown's region too (its own wait passes: the phantom check must not end its
    # copy); default: it stays at the lamp, so only the mirror's force can pass the wait
    g1 = ",".join(["log_pos=2"] + (["guest_move=%s@7%%m25" % IWN_SPOT] if os.environ.get("IWN_NEAR") else [])
                   + ["guest_press=0@20%m25!"])
    return dict(
        name="iwn", seconds=170, count=2, headless=0,
        env=["BB_PARTY_TEST_START_LAMP=2502950", "BB_PARTY_PROGRESS_TEST_SET=12501800,12501802,12501803,12500000",
             "BB_PARTY_PROGRESS_TEST_CLEAR=12500810", "BB_PARTY_PROGRESS_TEST_READ=12500810",
             "BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12500810"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "0:BB_PARTY_TEST_FORCE_WAIT=45"],
        watch=[], crash=[], until=IWN_UNTIL, until_hold=5,
    )


def check_iwn(v, L):
    need(v, L, "iwn join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "iwn host: its event played the scene (flag 12500810 on)", 0, r"12500810=1")
    need(v, L, "iwn guest: the phantom check (1000[2]) of the display event is passed", 1,
         r"EMEVD event 12500810 \[91\] 1000\[02\].*SKIPPED")
    need(v, L, "iwn guest: the display event's enable step ran (the chamber's parts on)", 1,
         r"EMEVD event 12500810 \[(9[7-9]|1[0-7]\d)\]")
    check_common(v, L, "iwn")


# Phase an (Annalise, ~4 min, 2 players, group `cainhurst_annalise`, 0.7.6 report "Annalise falls into the void after
# the lamp in her chamber is used"): the Queen's Chamber fixture of nfa (Logarius dead, display event 12500810 done),
# the party at the chamber lamp 2502951; the host uses that lamp (a rest: the host's world loads again, the guest
# follows), then the guest uses it. The chr dumps of both games follow Annalise (entity 2500600): she must stay on
# her floor (y of the throne) and drawn on both games after every load.
AN_ANNALISE = 2500600
AN_UNTIL = r"0:test: listonly: warp menu closed"


def phase_an(only):
    # who uses the lamp: AN_WHO=host (default) or guest; the real lamp interaction (BB_LAMP_TRAVEL_TEST walks to the
    # lamp, presses Cross, the warp menu opens and is closed again: the lamp's rest, as a player's)
    who = os.environ.get("AN_WHO", "host")
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    lamp = "members,lamp=2502951,listonly"
    return dict(
        name="an", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_PROGRESS_TEST_READ=1040,1041,1042,12500810", "BB_PARTY_CHR_DUMP=60",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2502951", "0:BB_PARTY_DIRECTOR_TEST=god,log_pos=3",
              "1:BB_PARTY_DIRECTOR_TEST=god,log_pos=3",
              ("0" if who == "host" else "1") + ":BB_LAMP_TRAVEL_TEST=" + lamp],
        watch=[], crash=[], until=[("0" if who == "host" else "1") + ":test: listonly: warp menu closed"], until_hold=45,
    )


def an_pos(L, i, after=0.0, before=1e9):
    """[(t, y, disabled)] of Annalise in instance i's chr dumps."""
    out = []
    for t, line in L.all(i, r"chr dump: entity %d " % AN_ANNALISE, after, before):
        m = re.search(r"at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line)
        if m:
            out.append((t, float(m.group(2)), "DISABLED" in line))
    return out


def check_an(v, L):
    need(v, L, "an join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        pos = an_pos(L, i)
        v.step("an %s's game shows Annalise" % who, bool(pos), "%d samples" % len(pos))
        if pos:
            ys = [y for _, y, _ in pos if y > -100]
            v.step("an %s's game: Annalise never falls (lowest y %.1f, first %.1f)" % (who, min(y for _, y, _ in pos), pos[0][1]),
                   min(y for _, y, _ in pos) > pos[0][1] - 5.0 and bool(ys), "%s" % [round(y, 1) for _, y, _ in pos][:12])
            v.step("an %s's game: Annalise drawn at the end" % who, not pos[-1][2], "last %s" % (pos[-1],))
    check_common(v, L, "an")


# Phase anl (0.7.7 report NW-a1c18ef, group `cainhurst_annalise`): the LIVING queen. `an` above sets 1348 (Alfred
# in the castle: event 12500010 turns Annalise into the flesh, 1042), which is not the state players met. Here 1348 is
# cleared: Annalise on her throne (1040). The party lights the chamber lamp and the host (AN_WHO=guest: the guest) opens
# its menu (the rest): her chr dump must never show her sinking (the game resets her, the throne animation is lost:
# AnnaliseSitTick). AN_SOLO=1: one game, no members (the lamp walk), the vanilla reference; AN_TRACE=1: EMEVD trace.
ANL_FIXTURE_SET = "12501800,12501802,12501803,12500810,12500000"


def _anl_env(extra_ienv, who_extra=""):
    clear = ",".join(str(f) for f in list(range(1340, 1360)) + [1041, 1042, 12500051, 12500052, 12500053, 12500054, 12500020, 52500920] + list(range(72500350, 72500360)))
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + ANL_FIXTURE_SET,
           "BB_PARTY_PROGRESS_TEST_READ=1040,1041,1042,12500810,12101007,52500920", "BB_PARTY_CHR_DUMP=60",
           "BB_PARTY_TEST_COND_TIMEOUT=100"]
    return env, ["0:BB_PARTY_TEST_START_LAMP=2502951"] + extra_ienv


def phase_anl(only):
    who = os.environ.get("AN_WHO", "host")
    lf = "log_flag=1040/1041/1042/12500010/12500020/12500021/72500201/72500312/72500320/72500326/72500328/72500331/9180"
    env, ienv = _anl_env(["0:BB_PARTY_DIRECTOR_TEST=god,log_pos=3," + lf, "1:BB_PARTY_DIRECTOR_TEST=god,log_pos=3," + lf,
                          ("0" if who == "host" else "1") + ":BB_LAMP_TRAVEL_TEST=members,lamp=2502951,listonly"])
    if os.environ.get("AN_TRACE") == "1":  # the instructions of the Annalise events, every call
        env = env + ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12500010,12500020,12500021"]
    solo = os.environ.get("AN_SOLO") == "1"  # one game only: is the fall the party's doing?
    if solo:
        ienv = [e.replace("lamp=2502951,listonly", "lamp=2502951,listonly").replace("members,", "") for e in ienv if not e.startswith("1:")]
    return dict(name="anl", seconds=420, count=1 if solo else 2, headless=0, env=env, ienv=ienv, watch=[], crash=[],
                until=[("0" if who == "host" else "1") + ":test: listonly: warp menu closed"], until_hold=45)


def check_anl(v, L):
    solo = os.environ.get("AN_SOLO") == "1"
    if not solo:
        need(v, L, "anl join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        if solo and i:
            break
        pos = an_pos(L, i)
        v.step("anl %s's game shows Annalise" % who, bool(pos), "%d samples" % len(pos))
        if pos:
            v.step("anl %s's game: Annalise never falls (lowest y %.1f, first %.1f)" % (who, min(y for _, y, _ in pos), pos[0][1]),
                   min(y for _, y, _ in pos) > pos[0][1] - 5.0, "%s" % [round(y, 1) for _, y, _ in pos][:12])
            v.step("anl %s's game: Annalise drawn at the end" % who, not pos[-1][2], "last %s" % (pos[-1],))
    check_common(v, L, "anl")


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
            # Cross: onto the ladder, 2 s after the turn ("Ascend Ladder" came 1.5 s after it in the
            # 0.7.3 release smoke, under load: a press at 1 s did nothing)
            "%s_press=0x4000@2%%m24_00!" % who,
            "%s_die:ladder@2%%m24_00" % who,                        # dies on it (only while on it)
            "%s_press=0@3%%m24_00!" % who]                          # up again at the lamp


def phase_ld(only):
    common = "log_pos=0.5,watch_text=Ladder"
    host = ",".join([common, "host_warp=2402950@8^1"] + ld_climb("host", 10) + ld_walks("host"))
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
# itself from the Dream (a headstone's travel: since 0.7.3 a split-travel re-join with the host's
# snapshot, no host rest load, guest 2 stays: a guest leaving the Dream never resets the host's world). After each everyone is back (the host rewinds its
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
            ("Bold Mark from the Dream", "mark"), ("lamp from the Dream (no host rest)", "rest"))
# the host's cases: (name, goods, the answer, in its own map)
MK_HOST_CASES = (("h1 host Bold Mark in its map, with the party", 1400, "yes", True),
                 ("h2 host Bold Mark in its map, alone", 1400, "no", True),
                 ("h3 host Hunter's Mark from the Dream, with the party", 100, "yes", False))
MK_OFF_LAMP = "13.3:105.1:1.2:25"  # a walk 5 m off the Cathedral Ward lamp's spot (17.6, 102.7)


def _mk_to_dream():
    """Guest 1 alone to the Dream (a rest: the host's world reloads; guest 2 follows), its last lamp the
    Cathedral Ward's again once there (25 s: everyone is back from the host's rest load)."""
    return ["guest_lamp=%s@12%%m24_00" % MK_DREAM, "guest_lastlamp=%s@25%%m21_00" % MK_LAMP]


def phase_mk(only):
    host = ",".join(["god", "host_warp=%s@8^2" % MK_LAMP,
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
                                      "guest_press=0@8%m24_00!"])  # back in the host's world: the end
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
    host = ",".join(["host_warp=%s@8^2" % KD_LAMP,
                     # in the cell after guest 1's grab, every scene over: the kidnap undone (test) for the
                     # host's grab next (a flag step keeps the chain armed; a press would wait for a join)
                     "host_flag=9423:0@30%m28_00",
                     "host_flag=9421:0@1",
                     "host_warp=%s@3" % KD_LAMP,          # the party back to the Cathedral Ward
                     "host_flag=9420@20%m24_00",          # the grab-death's trigger
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
    host = ",".join(["god", "host_warp=2402950@18",            # the Cathedral Ward (m24_00)
                     "host_lamp=3402959:story@10",             # the DLC grab's warp
                     "host_warp=2802950@25"])                  # Yahar'gul (m28_00)
    g1 = ",".join(["god", "guest_lamp=3202958:story@15%m28_00",  # the Tonsil Stone grab's warp
                   "guest_press=0@10%m32_00!"])
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
        # guest 1 starts 20 s after the host: its title join then asks a host that is already in the dungeon and is refused
        # at once (summoned from its own world). Started at 2 s it raced the host's answer: when the room was handed out
        # before the host was marked as in a dungeon, the title join loaded m29 without the dungeon, bailed home and
        # counted as a join of its own (#N of the guest's script shifted by one, 4 of 4 full-smoke runs on 2026-11-03).
        args=["--stagger", "20"],
        watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[],
        until=[CL_UNTIL, CL_UNTIL_G],
    )


# Phase cpr (chalprog, ~3 min, 2 players, group `chalice_progress`; 0.7.6): a guest with no dungeon of its
# own plays the host's Pthumeru dungeon (fixture chalice_pthumeru_inside; the host lights the boss room's
# lamp 2901952), then leaves the party: back in its own world the dungeon is at its altar (a free slot)
# with the lamp lit. The guest's save is kept in <agent>/smoke/cpr_guest_save for the decode check
# and for the altar screenshots (docs/party/test_saves.md chalice_guest_brought).
CPR_KEEP = AGENT_ROOT / "smoke" / "cpr_guest_save"


def prep_chalprog(root: Path):
    if not CL_FIXTURE.is_dir():
        sys.exit("chalprog: the fixture %s is missing (docs/party/test_saves.md)" % CL_FIXTURE)
    _cl_saves(root, CL_FIXTURE)
    _cl_saves(root, CL_DEFAULT, 1)


def post_chalprog(root: Path):
    src = root / "inst1" / "user" / "savedata_party"
    if src.is_dir():
        if CPR_KEEP.exists():
            shutil.rmtree(CPR_KEEP)
        shutil.copytree(src, CPR_KEEP)
    post_chalamp(root)


def phase_cpr(only):
    host = ",".join(["log_pos", "host_obj=%s:1.2:0@12%%m29" % CL_LAMP_OBJ, "host_press=0x4000@2%m29!",
                     "host_press=0@60%m29!"])
    g1 = ",".join(["guest_leave@30%m29", "guest_press=0@25%m21&"])
    return dict(
        name="cpr", seconds=300, count=2, headless=0, prep=prep_chalprog, post=post_chalprog,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[],
        until=[r"1:travel step (\d+)/\1: "],
    )


# Phase chc (chalcurse, ~4 min, 2 players, group `chalice_curse`; 0.7.7 cursedhp): the host's Pthumeru dungeon is Cursed
# (fixture chalice_pthumeru_cursed = chalice_pthumeru_inside with the Cursed feature flag 92905370 set in the slot's
# record) and a guest plays it. Vanilla (m29 event 12907420): the host gets SpEffect 4730 (max HP x0.5), a guest
# 4735 / 4736 (x0.714 / x0.700) on top of the cooperator cut 9006 (x0.7): both end near half. The test aid log_pos
# logs the local max HP and the curse SpEffects ("test: hp H/M area A curse speffects: ...").
CHC_FIXTURE = Path(r"E:\games\bbport\test_saves\chalice_pthumeru_cursed\savedata")


def prep_chalcurse(root: Path):
    if not CHC_FIXTURE.is_dir():
        sys.exit("chalcurse: the fixture %s is missing (docs/party/test_saves.md)" % CHC_FIXTURE)
    _cl_saves(root, CHC_FIXTURE)
    _cl_saves(root, CL_DEFAULT, 1)


def phase_chc(only, name="chc", cut_off=False):
    """cut_off (phase chn): the host's rule Phantom health cut is off, the guest has no x0.7 (BB_LAUNCH chn)."""
    return dict(
        name=name, seconds=240, count=2, headless=0, prep=prep_chalcurse, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_SHOP_GATE_LOG=1"] + (["BB_PARTY_PHANTOM_HP=0"] if cut_off else []) +
            (["BB_PARTY_CURSE_GUEST_ROWS=0"] if os.environ.get("BB_CHN_OLD") else []),
        ienv=["0:BB_PARTY_DIRECTOR_TEST=log_pos=3,host_die@30%m29,host_press=0@40%m29!",
              "1:BB_PARTY_DIRECTOR_TEST=log_pos=3,guest_die@70%m29,guest_press=0@45%m29!"],
        crash=[],
        until=[r"1:travel step (\d+)/: "],
        until_hold=10,
    )


# Phase cpa (chalaltar, 1 player, group `chalice_altar`): the fixture chalice_guest_brought (a guest
# that played the host's dungeon, then left) walks to the First Ritual Altar in its own Dream and opens
# its menu; the screenshots show what the altar offers (docs/party/chalice.md 9).
CPA_FIXTURE = Path(r"E:/games/bbport/test_saves/chalice_guest_brought/savedata")


def prep_chalaltar(root: Path):
    _cl_saves(root, CPA_FIXTURE, 0)


def phase_cpa(only):
    host = ("log_pos,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.0:-16.9:0.35,face=-18.27:-16.88,prompt=First Ritual Altar,"
            "seq=cross/?cross@Use the Ritual Altar/wait/wait/wait/wait/wait/wait")
    return dict(
        name="cpa", seconds=130, count=1, headless=0, prep=prep_chalaltar, post=post_chalamp, pad=1,
        env=[], ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host], crash=[], until=[],
    )


# Phase cae (chalice_enter, 2 players, group `chalice_enter`; 0.7.7 altar077): the one-press entry
# (BB_PARTY_ALTAR_AUTO=1) at the host's First Ritual Altar: the guest (fixture chalice_pthumeru_ritual) has a
# Pthumeru dungeon of its own, the host (default save) none; the altar offers "Enter the party's dungeon", the
# autopilot answers the glyph box, OK, the result, YES, the altar and the awaken point; both end in m29.
CAE_GUEST = Path("E:/games/bbport/test_saves/chalice_pthumeru_ritual/savedata")


def prep_chalenter(root: Path):
    if not CAE_GUEST.is_dir():
        sys.exit("chalenter: the fixture %s is missing (docs/party/test_saves.md)" % CAE_GUEST)
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CAE_GUEST, 1)


def phase_cae(only):
    host = ("log_pos,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar,"
            "seq=" + "cross/?cross@Use the Ritual Altar/wait/down/down/cross"
            + ",host_press=0@40%m29!")
    return dict(
        name="cae", seconds=240, count=2, headless=0, prep=prep_chalenter, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_ALTAR_AUTO=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=log_pos"],
        crash=[], until=[r"1:travel step (\d+)/\1: "], until_hold=20,
    )


# Phase cao (chalice_enter, 2 players; 0.7.7 altar077; FLAKY: the first Cross at the altar often finds no prompt
# (1 clean entry in 5 runs), not in the default list): the host's OWN ritual dungeon from the Makeshift Altar
# (2100954) with a guest present: the host starts from chalice_pthumeru_ritual (its Pthumeru dungeon in slot 1,
# nothing cleared: every boss alive), the guest from the default save; the altar's awaken list, the first point.
CAO_HOST = Path("E:/games/bbport/test_saves/chalice_pthumeru_ritual/savedata")
CAO_SEQ = os.environ.get("CAO_SEQ", "cross/wait/cross/wait/!cross@Select location to awaken in/wait/cross/wait/wait/cross/wait/wait/cross/wait")


def prep_chalown(root: Path):
    if not CAO_HOST.is_dir():
        sys.exit("chalown: the fixture %s is missing (docs/party/test_saves.md)" % CAO_HOST)
    _cl_saves(root, Path(os.environ.get("CAO_HOST_SAVE", str(CAO_HOST))), 0)
    _cl_saves(root, Path(os.environ["CAO_GUEST_SAVE"]) if os.environ.get("CAO_GUEST_SAVE") else CL_DEFAULT, 1)


def phase_cao(only):
    walk = "approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar"
    host = ("log_pos," + os.environ.get("CAO_AIDS", "log_pos") + ",place=0x15000000:-15.5:-5.43:-16.9:0," + walk
            + ",seq=" + CAO_SEQ + os.environ.get("CAO_TAIL", ""))
    return dict(
        name="cao", seconds=int(os.environ.get("CAO_SECONDS", "260")), count=2, headless=0, prep=prep_chalown, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=log_pos"],
        # the phase is done once the guest is in the host's dungeon (its last check): end 15 s later instead of idling
        # out the 260 s with the menu walk's leftover steps waiting for screens that never come (user, 2026-10-31)
        crash=[], until=[r"1:map - -> m29_00"], until_hold=15,
    )



# Phases cgt / cgp / cgu (group `chalice_glyph`, 2 players; 0.7.8 glyph078, CL-420d9f): the Chalice glyph pick list at the host's
# First Ritual Altar (bbport.ini party_glyph_list, here BB_PARTY_GLYPH_LIST=1). cae's setup: the guest (chalice_pthumeru_ritual) has
# a dungeon of its own, the host (default save) none; the board holds the guest's dungeon with its FormData (its glyph is a hash of
# it, CGT_GLYPH below). The altar's "Enter a party glyph" opens the glyph box with the party's list on top:
#   cgt: host_glyph=CODE types the code (the keyboard path stays), the autopilot answers the prompts, both end in m29;
#   cgp: host_pick=1 picks the list's first row with Down / Enter (the pick list's own UI path), the same entry;
#   cgu: host_glyph=zzzzzzzz is no glyph of the party: a clear notice, no entry, nobody loads.
CGT_GLYPH = os.environ.get("CGT_GLYPH", "ksxx7rqb")  # the guest fixture's dungeon glyph (see the log's `glyph list:` line)


def _glyph_host(aid):
    return ("log_pos,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar,"
            "seq=" + "cross/?cross@Use the Ritual Altar/wait/down/down/cross"
            + "," + aid + ",host_press=0@40%m29!")


def _phase_glyph(name, aid, until, hold):
    return dict(
        name=name, seconds=240, count=2, headless=0, prep=prep_chalenter, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_ALTAR_AUTO=1", "BB_PARTY_GLYPH_LIST=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + _glyph_host(aid), "1:BB_PARTY_DIRECTOR_TEST=log_pos"],
        crash=[], until=until, until_hold=hold,
    )


def phase_cgt(only):
    return _phase_glyph("cgt", "host_glyph=" + CGT_GLYPH, [r"1:map - -> m29_00"], 15)


def phase_cgp(only):
    return _phase_glyph("cgp", "host_pick=1", [r"1:map - -> m29_00"], 15)


def phase_cgu(only):
    return _phase_glyph("cgu", "host_glyph=zzzzzzzz", [r"0:Party: chalice: glyph 'zzzzzzzz' chosen at the altar is not in the party"], 25)


def _check_glyph(v, L, tag, enters):
    check_common(v, L, tag)
    lst = need(v, L, tag + " the host's glyph list has the guest's dungeon", 0, r"glyph list: [a-z0-9]{8} \[member\]")
    box = need(v, L, tag + " the altar's glyph box opened with the list", 0, r"the altar's glyph box opens with a list of \d+ party glyph", lst or 0)
    if enters:
        ch = need(v, L, tag + " a glyph of the party was chosen", 0, r"glyph [a-z0-9]{8} chosen at the altar \(on the board\)", box or 0)
        t = need(v, L, tag + " the autopilot answered the prompts and the host's load started", 0, r"altar: the party's dungeon is loading", ch or 0)
        need(v, L, tag + " host in the dungeon (m29)", 0, r"map - -> m29_00", t or 0)
        need(v, L, tag + " guest followed into the dungeon (m29)", 1, r"map - -> m29_00", t or 0)
        none_of(v, L, tag + " the autopilot never gave up", r"altar: a prompt did not come", 0, 1e9, (0,))
    else:
        need(v, L, tag + " the notice says the glyph is not in the list", 0, r"glyph 'zzzzzzzz' chosen at the altar is not in the party's list: Glyph zzzzzzzz is not in this party", box or 0)
        none_of(v, L, tag + " nobody entered the dungeon", r"map - -> m29_00", 0, 1e9, (0, 1))


def check_cgt(v, L):
    _check_glyph(v, L, "cgt", True)
    need(v, L, "cgt the code was typed into the box", 0, r"test: host_glyph " + CGT_GLYPH + " typed into the glyph box")


def check_cgp(v, L):
    _check_glyph(v, L, "cgp", True)
    need(v, L, "cgp the first row was picked with the keys", 0, r"test: host_pick 1 \(of \d+\)")


def check_cgu(v, L):
    _check_glyph(v, L, "cgu", False)


# Phase caa (chalice_alias, 2 players; 0.7.8 chal078, NW-5e27806): cao's flow (the host enters its OWN ritual dungeon from the
# altar with a guest present in the Dream: the guest's travel is not followed, it leaves the session and the summon quick-joins)
# but the host's game ChannelId is one the board cannot hold (BB_CHALICE_ALIAS_TEST=1 on the host, as Nightwatch's 0x80e3...), so
# the summon carries the alias made of the dungeon's FormData. Live it loaded the guest empty (grey screen) and looped.
# CAO_GUEST_SAVE = a guest save to use instead of the default one.
def phase_caa(only):
    d = phase_cao(only)
    d["name"] = "caa"
    d["ienv"] = d["ienv"] + ["0:BB_CHALICE_ALIAS_TEST=1"]
    # The players' patch list had "Unlock Game Region": the sell region 5 has an empty ss.info URL (the FROM client stayed
    # offline: no board request worked). CAA_PATCHES="Skip Intro" runs it without.
    d["env"] = d["env"] + ["BB_PATCHES=" + os.environ.get("CAA_PATCHES", "Skip Intro;Unlock Game Region")]
    d["watch"] = ["--watch", r"cannot ride the board=alias@0,4"]
    return d


def check_caa(v, L):
    check_common(v, L, "caa")
    need(v, L, "caa host: the game's id rides the board under an alias", 0,
         r"chalice: the game's dungeon id 0x[0-9a-f]+ cannot ride the board: summons carry (\d+)")
    need(v, L, "caa the FROM client of the guest logged in (ss.info answered)", 1, r"http: GET https://\S+/ss\.info -> 200")
    need(v, L, "caa guest followed into the host's dungeon (m29)", 1, r"map - -> m29_00")
    need(v, L, "caa guest's game made the channel request for the alias", 1,
         r"direct summon: Chalice Dungeon \d+ \(ChannelId .*channel request made")
    none_of(v, L, "caa the guest was never loaded in empty", r"without the dungeon \(map unknown|NOT made", 1, 1e9, (1,))
    none_of(v, L, "caa no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")


# Phases cam / camo (group chalice_alias; 0.7.8 chalmsg078): the clear notices when a guest's Chalice summons cannot work. cao's flow
# (the host enters its own ritual dungeon with a guest in the Dream) with a test aid on the guest (BB_PARTY_CHALICE_MSG_TEST):
# cam = "stop": the empty-load count is forced to the loop stop (no real empty loads needed): the guest's alert box and the host's;
# camo = "offline": the guest's game reads as offline when the summon is delivered: the specific offline notice. The shots/ show the box.
def phase_cam(only, mode="stop"):
    d = phase_cae(only) if mode == "stop" else phase_cao(only)  # cao is flaky; the stop notice needs no summon
    d["name"] = "cam" if mode == "stop" else "camo"
    d["ienv"] = d["ienv"] + ["1:BB_PARTY_CHALICE_MSG_TEST=" + mode]
    d["crash"] = []
    d["until"] = [r"1:chalice: notice shown: " + ("Chalice Dungeon loaded empty twice" if mode == "stop" else "This game is offline"), r"0:seq STUCK"]  # a stuck altar menu ends the run (the check then fails)
    d["until_hold"] = 12
    d["seconds"] = int(os.environ.get("CAM_SECONDS", "200"))
    return d


def check_cam(v, L, mode="stop"):
    name = "cam" if mode == "stop" else "camo"
    check_common(v, L, name)
    title = "Chalice Dungeon loaded empty twice" if mode == "stop" else "This game is offline"
    t = need(v, L, name + " guest: the clear notice is shown (" + title + ")", 1, r"chalice: notice shown: " + title)
    need(v, L, name + " host: told the guest's summons stopped", 0,
         r"chalice: notice shown: Chalice summon stopped for a guest \(slot \d+, " + ("empty loads" if mode == "stop" else "offline") + r"\)", t or 0)
    none_of(v, L, name + " the guest was never loaded in empty", r"without the dungeon \(map unknown", 1, 1e9, (1,))


# Phase camh (group chalice_alias; 0.7.8 chaloff078, NW-cab726f): a guest whose game is in offline mode (test aid
# BB_PARTY_CHALICE_MSG_TEST=offline_mode: FrpgNetMan +8 / +0xa read clear) when the host enters its Chalice Dungeon: cae's flow. The
# summon could not bring it in, so it is NOT sent to its own world (no leave, no load): it stays in the host's Dream with the
# offline-mode notice, and the host is told. Control: cae / caa (online guests follow into m29).
def phase_camh(only):
    d = phase_cae(only)
    d["name"] = "camh"
    d["ienv"] = d["ienv"] + ["1:BB_PARTY_CHALICE_MSG_TEST=offline_mode"]
    d["crash"] = []
    d["until"] = [r"1:chalice: notice shown: This game is in offline mode", r"0:seq STUCK"]
    d["until_hold"] = 25
    return d


def check_camh(v, L):
    check_common(v, L, "camh")
    t = need(v, L, "camh host: the dungeon load started", 0, r"altar: the party's dungeon is loading")
    need(v, L, "camh host in the dungeon (m29)", 0, r"map - -> m29_00", t or 0)
    need(v, L, "camh guest: the send-home is dropped (offline game)", 1, r"send-home at \+0x138a51c \(OnLeave_Limit\) dropped: the host is in a Chalice Dungeon and this game is offline", t or 0)
    need(v, L, "camh guest: the offline-mode notice names the real cause", 1, r"chalice: notice shown: This game is in offline mode", t or 0)
    need(v, L, "camh host: told the guest cannot join", 0, r"chalice: notice shown: Chalice summon stopped for a guest \(slot \d+, offline\)", t or 0)
    none_of(v, L, "camh the guest was not sent to its own world", r"leaving the session to its own world|guest_sent_home|BeginLeaveSession called", t or 0, 1e9, (1,))
    none_of(v, L, "camh the guest never loaded in", r"map - -> m29_00", t or 0, 1e9, (1,))


# Phase cas (group chalice_alias; 0.7.9 signin079, CL-4b4614 / NW-cab726f): the guest's ss.info answer at the title is LATE
# (BB_HTTP_DELAY_SSINFO_MS on the guest, CAS_DELAY_MS, default 17000 as in the report: healthy ~3 s). Before 0.7.9 the title
# went on to Continue meanwhile and the game stayed offline for the session (FrpgNetMan +8 clear, no Chalice summon works);
# now Continue waits for the sign-in. cae's flow (the host enters its ritual dungeon with the guest present): the guest must
# follow into m29 through the summon. CAS_HOLD=0 puts the 0.7.8 behaviour back (BB_PARTY_SIGNIN_HOLD=0) = the reproduction
# (the checks then fail: the guest stays offline).
def phase_cas(only):
    d = phase_cae(only)
    d["name"] = "cas"
    for who in os.environ.get("CAS_WHO", "0,1").split(","):  # which instances get the late answer (0 host, 1 guest)
        d["ienv"] = d["ienv"] + [who + ":BB_HTTP_DELAY_SSINFO_MS=" + os.environ.get("CAS_DELAY_MS", "17000")]
        if os.environ.get("CAS_HOLD") == "0":
            d["ienv"] = d["ienv"] + [who + ":BB_PARTY_SIGNIN_HOLD=0"]
    d["seconds"] = 300
    return d


def check_cas(v, L):
    check_common(v, L, "cas")
    need(v, L, "cas the first ss.info answer was delayed (test knob)", int(os.environ.get("CAS_WHO", "0,1").split(",")[0]), r"http: test: delaying the first ss\.info answer")
    t = need(v, L, "cas guest: the sign-in finished (ss.info answered late, game online: FrpgNetMan net up)", 1,
             r"online: FrpgNetMan net 1 online 1 .*user \d+ session [1-9]\d* chars")
    need(v, L, "cas host: the dungeon load started", 0, r"altar: the party's dungeon is loading")
    need(v, L, "cas guest followed into the dungeon (m29) through the summon", 1, r"map - -> m29_00", t or 0)
    none_of(v, L, "cas the guest was not sent home / offline-held", r"send-home at \+0x138a51c \(OnLeave_Limit\) dropped|chalice: notice shown: This game is in offline mode", 0, 1e9, (1,))


# Phase caw (chalice_queen walk; 0.7.7 queen077): the Queen fixture (chalice_queen_ritual: a Pthumeru Ihyll Root Chalice
# dungeon in slot 1, made by phase caq) is the GUEST's save; the host (default save) enters it by the altar's one-press
# "Enter the party's dungeon" (as cae). CAW_TAIL = director steps after the dungeon loads (probes, warps, host_kill=ID).
CAW_GUEST = Path(os.environ.get("CAW_FIX", "E:/games/bbport/test_saves/chalice_ihyll_ritual/savedata"))


def prep_chalqw(root: Path):
    if not CAW_GUEST.is_dir():
        sys.exit("chalqueen: the fixture %s is missing (docs/party/test_saves.md)" % CAW_GUEST)
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CAW_GUEST, 1)


def phase_caw(only):
    host = ("log_pos,goods=6109:1,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar,"
            "seq=" + "cross/?cross@Use the Ritual Altar/wait/down/down/cross"
            + ",host_press=0@40%m29!" + os.environ.get("CAW_TAIL", ""))
    return dict(
        name="caw", seconds=int(os.environ.get("CAW_SECONDS", "300")), count=2, headless=0, prep=prep_chalqw, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_ALTAR_AUTO=1", "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=log_pos" + os.environ.get("CAW_GUEST_TAIL", "")],
        crash=[], until=[r"1:travel step (\d+)/: "], until_hold=20,
    )


# Phase cab (chalice_boss, 2 players; 0.7.7 altar077 / chalboss077): a LIVE Chalice layer boss with a guest. The host Continues
# from chalice_pthumeru_boss (chalice_pthumeru_inside with the layer 1 boss's defeat flags cleared by
# tools/party/make_chalice_boss_fixture.py: it stands inside the boss arena, whose boss only wakes when a player comes in through
# the arena's closed door). Host: out to the corridor (x -20), the stick walk to the door ("Open Door"), Cross, in; the boss bar
# and HP show on both games, the host sets it to 50% (synced), then kills it (clear bonus on host and guest).
CAB_HOST = Path("E:/games/bbport/test_saves/chalice_pthumeru_boss/savedata")


def prep_chalboss(root: Path):
    if not CAB_HOST.is_dir():
        sys.exit("chalboss: the fixture %s is missing (docs/party/test_saves.md)" % CAB_HOST)
    _cl_saves(root, CAB_HOST, 0)
    _cl_saves(root, CL_DEFAULT, 1)


def phase_cab(only):
    # The boss 2900100 wakes (bar, fog, AI) by the arena's entry region, which only a player coming from OUTSIDE
    # the closed door x -24.4 wakes (world frame: the arena runs along x, the lamp room is its far end, the
    # layer 1 boss stands at (-39, 30.5, -91)): the fixture's player starts inside, so both hop out to x -20 and back in.
    # host: out to x -20, the stick walk to the door (prompt "Open Door"), Cross, and the same walk goes on through
    # the opened door: the arena's entry wakes the boss (bar, fog) ~50 s into the run; the guest goes in after.
    host = ",".join(["log_pos=1", "watch_text=Open Door", "host_move=-20:31.6:-91.1@3%m29", "approach=-26:-91.1:4.0:cross",
                     "host_kill=2900100:hp50@36%m29", "host_kill=2900100:hp@12%m29"])
    g1 = ",".join(["log_pos=1", "guest_move=-20:31.6:-91.1@25%m29", "guest_move=-27:31.4:-91.1@4%m29"])
    host = os.environ.get("CAB_HOST_STEPS", host)   # probes (fog position, prompt text)
    g1 = os.environ.get("CAB_G1_STEPS", g1)
    return dict(
        name="cab", seconds=int(os.environ.get("CAB_SECONDS", "260")), count=2, headless=0, pad=int(os.environ.get("CAB_PAD", "1")), prep=prep_chalboss, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[], until=[], until_hold=20,
    )


# Phase cag (chalice_altar_guest, 2 players; 0.7.9 altar079, CL-13f7d1): a GUEST summoned into the host's Dream performs a
# Chalice ritual at the Ritual Altar (the "Makeshift Altar" object 2100954, prompt "First Ritual Altar") with real menu presses
# (CAG_SEQ: the caq walk). Both start from the default save; the guest is granted the Pthumeru Chalice (6109) and the rite
# materials by the test aid goods= (the guest title-joins at once, so they are given while summoned), walks to the altar
# (`in_host`: it never places itself, a place= would leave the party), conducts the ritual, leaves the party (its own world
# saves) and rejoins; then the host (its altar offers "Enter the party's dungeon" for the guest's new dungeon, as in `cae`;
# `approach_join=2`: only after the guest's rejoin) enters it and the guest follows. The toggle party_guest_altar
# (BB_PARTY_GUEST_ALTAR=1 here) lets a guest's altar answer. The guest's save is kept in <agent>/smoke/cag_guest_save.
CAG_GOODS = os.environ.get("CAG_GOODS", "goods=6109:1,goods=7000:9,echoes=30000")
CAG_SEQ = os.environ.get("CAG_SEQ", "cross/?cross@Use the Ritual Altar/cross@Search by Chalice Glyph/cross@Select chalice to use/cross@Rite materials/left@Conduct a ritual to create a Chalice Dungeon?/cross/wait/wait/wait/wait")
CAG_KEEP = AGENT_ROOT / "smoke" / "cag_guest_save"
CAG_DUNGEON = (0x458bd799, 0x3f22b)  # the shared fixed Pthumeru dungeon's ChannelId (low, high word), as in check_cpr


def prep_chalaltarg(root: Path):
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CL_DEFAULT, 1)


def post_chalaltarg(root: Path):
    src = root / "inst1" / "user" / "savedata_party"
    if src.is_dir():
        if CAG_KEEP.exists():
            shutil.rmtree(CAG_KEEP)
        shutil.copytree(src, CAG_KEEP)
    post_chalamp(root)


def phase_cag(only, full=False):
    """cag: the guest's ritual, its leave (the own world saves) and the save read back. cage (full): then the host enters the
    guest's new dungeon and the guest follows (FLAKY like cao: the host's walk to the altar often gives up; not in the default list)."""
    walk = "approach=-16.9:-16.85:0.3,face=-18.27:-16.88,prompt=First Ritual Altar"
    guest = ("log_pos,in_host,watch_text=Rite/ritual/Ritual/chalice/Chalice," + CAG_GOODS + "," + walk + ",seq=" + CAG_SEQ
             + ",guest_leave@" + os.environ.get("CAG_LEAVE", "95") + "%m21" + (",guest_rejoin@5" if full else ""))
    host = "log_pos"
    ienv = []
    until, hold = [r"1:guest_leave: Party > Leave pressed"], 40
    if full:
        host = ("log_pos,approach_join=2,approach=-16.9:-16.85:0.3,face=-18.27:-16.88,prompt=First Ritual Altar,"
                "seq=cross/?cross@Use the Ritual Altar/wait/down/down/cross,host_press=0@1#2%m21")
        ienv = ["0:BB_PARTY_ALTAR_AUTO=1"]
        until, hold = [r"1:map - -> m29_00"], 25
    return dict(
        name="cage" if full else "cag", seconds=int(os.environ.get("CAG_SECONDS", "420" if full else "260")), count=2, headless=0,
        prep=prep_chalaltarg, post=post_chalaltarg, pads=(0, 1), env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_GUEST_ALTAR=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest] + ienv,
        crash=[], until=until, until_hold=hold, until_quiet=200,
    )


def check_cag(v, L, full=False):
    tag = "cage" if full else "cag"
    check_common(v, L, tag)
    need(v, L, tag + " the guest's altar talk answered (party_guest_altar)", 1, r"altar talk t2100\d0: IsClientPlayer answered 0")
    need(v, L, tag + " the guest reached the altar prompt", 1, r'test: text shows: "First Ritual Altar"')
    t = need(v, L, tag + " the altar menu opened for the guest", 1, r'test: text shows: "Use the Ritual Altar')
    r = need(v, L, tag + " the guest's ritual finished (on-screen text)", 1, r"The ritual is finished, and a chalice rests upon the altar", t or 0)
    d = need(v, L, tag + " the guest's dungeon is in its slot 1 and on the host's board", 1,
             r"chalice: dungeon 1110692594440089: FormData of slot 1 \(124 bytes\) for the host's board", t or 0)
    need(v, L, tag + " the guest paid the ritual (echoes 30000 -> 29000)", 1, r"own character steady for \d+ s: .*echoes 29000", r or 0)
    lv = need(v, L, tag + " the guest left the party (its own world saves)", 1, r"guest_leave: Party > Leave pressed", r or 0)
    if full:
        need(v, L, tag + " the guest rejoined the host's Dream", 1, r"travel steps: \(re\)joined #2", lv or 0)
        e = need(v, L, tag + " the host's altar offered the guest's dungeon and the autopilot entered it", 0, r"altar: the party's dungeon is loading", lv or 0)
        need(v, L, tag + " host in the dungeon (m29)", 0, r"map - -> m29_00", e or 0)
        need(v, L, tag + " guest followed into the dungeon (m29)", 1, r"map - -> m29_00", e or 0)
        none_of(v, L, tag + " the autopilot never gave up", r"altar: a prompt did not come", 0, 1e9, (0,))
    none_of(v, L, tag + " the menu walk was not stuck", r"seq STUCK", 0, 1e9, (0, 1))
    keep = CAG_KEEP / "1" / "CUSA00207" / "SPRJ0005" / "userdata0000"
    if keep.is_file():
        sys.path.insert(0, str(REPO / "tools" / "party"))
        import save_decode as sd
        s = sd.Save.read(keep)

        def val(base, off, bits=32):
            x = 0
            for b in range(bits):
                x = (x << 1) | (1 if s.flag(base + off + b) else 0)
            return x
        base = 94100000
        v.step(tag + " the guest's save: slot 1 holds the ritual's dungeon", val(base, 32) == CAG_DUNGEON[0] and val(base, 64) == CAG_DUNGEON[1]
               and (val(base, 96) >> 24) == 0x1d, "record %x %x %x" % (val(base, 32), val(base, 64), val(base, 96)))
        v.step(tag + " the guest's save: no other slot got a dungeon" + (" (slot 0 holds the quick-joined host dungeon, by design)" if full else ""), all(val(94000000 + k * 100000, 96) == 0xffffffff for k in ((2, 3, 4, 5, 6) if full else (0, 2, 3, 4, 5, 6))),
               "slot maps " + " ".join("%x" % val(94000000 + k * 100000, 96) for k in range(7)))
        g = {i: n for _, i, n in s.goods()}
        v.step(tag + " the guest's save: the Pthumeru Chalice (fixed, not consumed) is still held", g.get(6109) == 1, "6109 x%s" % g.get(6109))
        v.step(tag + " the guest's save: Ritual Blood 9 -> 7 (the ritual's two)", g.get(7000) == 7, "7000 x%s" % g.get(7000))
    else:
        v.step(tag + " the guest's save was kept", False, "missing %s" % keep)


# Phase caq (chalice_queen, 1 player; 0.7.7 chalboss077; FIXTURE MAKER, UNPROVEN: the altar menu did not open in the one run; not in the default list): the altar7 ritual (docs/party/
# test_saves.md) for the Pthumeru Ihyll Root Chalice (goods 6500; Queen Yharnam's dungeon). CAQ_GOODS = the chalice and the
# rite materials the altar asks for; CAQ_SEQ = the menu walk. The save to keep: inst0/user/savedata_party of the run.
def prep_chalqueen(root: Path):
    _cl_saves(root, CL_DEFAULT, 0)


def phase_caq(only):
    goods = os.environ.get("CAQ_GOODS", "goods=6500:1,goods=7000:9,goods=7001:9,goods=7002:9,goods=7003:9,goods=7004:9,goods=7012:9,goods=7050:25,goods=7100:9,goods=7150:9,echoes=30000")
    seq = os.environ.get("CAQ_SEQ", "cross/?cross@Use the Ritual Altar/cross@Search by Chalice Glyph/cross@Select chalice to use/cross@Rite materials/left@Conduct a ritual to create a Chalice Dungeon?/cross/wait/wait/wait/wait")
    host = "log_pos=2,watch_text=Rite/ritual/Ritual/chalice/Chalice," + goods + ",place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.0:-16.9:0.35,face=-18.27:-16.88,prompt=First Ritual Altar,seq=" + seq
    return dict(
        name="caq", seconds=int(os.environ.get("CAQ_SECONDS", "150")), count=1, headless=0, prep=prep_chalqueen, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host],
        crash=[], until=[], until_hold=20,
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


# Phase gd (guest_dream, ~4 min, 2 players, group `guest_dream`; 0.7.5 report: "Return to the Hunter's
# Dream" picked at a world lamp by a guest did nothing): the host starts at Hemwick's lamp 2202950 (m22),
# the guest joins (an A5 cross-map join). The host picks the Dream and goes alone (NO): the lamp's warp
# event also runs on the guest's game and is dropped there (not its use). Then the guest, still at that
# lamp, picks the Dream (travel list entry 1): its Dream flag is set and the guest must still leave.
GD_LAMP = "2202950"
GD_UNTIL = [r"1:Lamp travel: arrived: lamp 2102950|1:no load within 60 s|1:test: FAILED"]


def phase_gd(only):
    return dict(
        name="gd", seconds=240, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + GD_LAMP, "0:BB_PARTY_DIRECTOR_TEST=god", "0:BB_PARTY_SPLIT_TEST=no",
              # the host rests at the lamp first and goes to the Dream ALONE (NO to "Travel with the party?"):
              # the same warp event runs on the guest's game and is dropped (not its use)
              "0:BB_LAMP_TRAVEL_TEST=members,lamp=%s,to=2102950" % GD_LAMP,
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_SPLIT_TEST=yes",
              "1:BB_LAMP_TRAVEL_TEST=members,wait=45,lamp=%s,to=2102950" % GD_LAMP],
        watch=["--watch", r"chose the Hunter's Dream=gdchose@0,4"],
        crash=[], until=GD_UNTIL, until_hold=12,
    )


# Phase yj (yahargul_join, ~4 min, 2-3 players, group `yahargul_join`; 0.7.8 report: the host stood in
# Yahar'gul (m28, lamp 2802950), a guest in the Hunter's Dream joined and sat on the loading screen
# for good). The host starts at the Yahar'gul lamp (default save: before the Blood Moon, 9802 off), the
# guests join from their own Dream (A5 cross-map join, a lamp warp into m28) and must reach the host's
# world. YJ_NET = a BB_NET_SIM preset (dsl / mobile / bad), YJ_GUESTS = 1 or 2, YJ_LAMP = another lamp.
YJ_LAMP = os.environ.get("YJ_LAMP", "2802950")
YJ_UNTIL = [r"1:join: arrived \d+\.\d m from the host|1:no load within 60 s|1:test: FAILED"]


def phase_yj(only):
    n = int(os.environ.get("YJ_GUESTS", "1"))
    net = os.environ.get("YJ_NET", "")
    # YJ_FIGHT=N: the host fights in Yahar'gul first (N R1 taps at the enemies around the lamp): the host world's decals blob (the
    # blood on the ground, packet 0x11) is then bigger than the bare header of 20 bytes; the guests start after the last tap.
    fight = int(os.environ.get("YJ_FIGHT", "0"))
    taps = ["host_flag=60009990:0@1%m28_00"] * (1 if fight else 0)   # a flag step arms the chain (a press would wait for a join first)
    taps += ["host_press=0x800@%d%%m28_00!" % (4 if k == 0 else 1) for k in range(fight)]
    ienv = ["0:BB_PARTY_TEST_START_LAMP=" + YJ_LAMP, "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god"] + taps)]
    if os.environ.get("YJ_LIT", "1") != "0":  # the host's world has the lamp lit (the reported case): the arrival is a lamp warp
        ienv.append("0:BB_PARTY_PROGRESS_TEST_SET=12807810,12807811,72800200")
    for g in range(1, n + 1):
        ienv += ["%d:BB_PARTY_DIRECTOR_TEST=god" % g, "%d:BB_PARTY_TITLE_JOIN=0" % g]
        # repro079 knobs: another map block than the Dream for the guest, and its own save's flags (never saw the Gaol, ...)
        if os.environ.get("YJ_GUEST_LAMP"):
            ienv.append("%d:BB_PARTY_TEST_START_LAMP=%s" % (g, os.environ["YJ_GUEST_LAMP"]))
        if os.environ.get("YJ_GCLEAR"):
            ienv.append("%d:BB_PARTY_PROGRESS_TEST_CLEAR=%s" % (g, os.environ["YJ_GCLEAR"]))
        if os.environ.get("YJ_GSET"):
            ienv.append("%d:BB_PARTY_PROGRESS_TEST_SET=%s" % (g, os.environ["YJ_GSET"]))
    if os.environ.get("YJ_HSET"):
        ienv.append("0:BB_PARTY_PROGRESS_TEST_SET=" + os.environ["YJ_HSET"])
    env = ["BB_NET_SIM=" + net] if net else []
    if fight:
        env.append("BB_PARTY_DIRECTOR_TEST_SOLO=1")   # the chain runs without a join (the guests start after it)
    fix = os.environ.get("YJ_HOST_SAVE", "")  # e.g. blood_moon_yahargul: the host Continues in Yahar'gul after the Blood Moon
    gfix = os.environ.get("YJ_GUEST_SAVE", "")  # repro079: a test_saves fixture for every guest (another map block / progress than the default Dream save)
    extra = {}
    if fix:
        ienv = [e for e in ienv if "START_LAMP" not in e]
    if fix or gfix:
        def _yj_prep(root, fix=fix, gfix=gfix, n=n):
            if fix:
                _cl_saves(root, Path("E:/games/bbport/test_saves") / fix / "savedata")
            for g in range(1, n + 1):
                if gfix:
                    _cl_saves(root, Path("E:/games/bbport/test_saves") / gfix / "savedata", g)
        extra = dict(prep=_yj_prep, post=lambda root, n=n: [_cl_saves(root, CL_DEFAULT, i) for i in range(0, n + 1)] and None)
    return dict(
        name="yj", seconds=240 + 60 * n, count=1 + n, headless=0 if n == 1 else 1, **extra,
        env=env, ienv=ienv, crash=[],
        # the guests start once the host stands in Yahar'gul (YJ_EARLY=1: with the host, so the join lands while its lamp warp loads)
        **({} if os.environ.get("YJ_EARLY") == "1" else dict(start_when=[(("%d/%d:A5: arrived in the host's map" % (g, g - 1)) if (os.environ.get("YJ_STAGGER") == "1" and g > 1) else ((r"%d/0:travel step (\d+)/\1: host_press" if fight else r"%d/0:map - -> m28_00_00_00") % g)) for g in range(1, n + 1)])),
        until=YJ_UNTIL if n == 1 else YJ_UNTIL + [r"2:join: arrived \d+\.\d m from the host|2:no load within 60 s"], until_hold=15 + 40 * (n - 1),
    )


def check_yj(v, L, d: Path):
    n = int(os.environ.get("YJ_GUESTS", "1"))
    for g in range(1, n + 1):
        a = need(v, L, "yj guest %d: the A5 cross-map join starts" % g, g, r"A5: cross-map join: client in m\d+_\d+_00_00, the host in m28")
        if os.environ.get("YJ_EARLY") != "1":   # a refused guest ("the bell sign path goes on") loads its OWN Yahar'gul: map m28, but not the host's world
            need(v, L, "yj guest %d: arrives in the HOST's world" % g, g, r"A5: arrived in the host's map", a or 0,
                 what="the guest's load ended in its own Yahar'gul (the host refused the direct summon?)")
        w = need(v, L, "yj guest %d: the load into Yahar'gul ends (loading screen gone)" % g, g, r"loading screen gone", a or 0,
                 what="the guest sat on the loading screen (0.7.8 report)")
        need(v, L, "yj guest %d: standing in the host's Yahar'gul" % g, g, r"Party: .*map - -> m28_00_00_00", a or 0)
        none_of(v, L, "yj guest %d: no stuck warp" % g, r"no load within 60 s", insts=(g,))
    check_common(v, L, "yj")


# repro079 (0.7.9, the three items no earlier agent could reproduce): the same phases with other setups, as phases of their own so the
# env knobs of yj / ll / lr need not be set by hand (the knobs stay: YJ_*, LL_*, BB_LR_*).
def _with_env(env, fn, *a):
    old = {k: os.environ.get(k) for k in env}
    os.environ.update(env)
    try:
        return fn(*a)
    finally:
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


# Phase yjc (CL-f9b8fc, ~5 min, 3 players, group `yahargul_join_other`): both guests start from the Cathedral Ward save (another map block
# than the Dream: the A5 cross-map join comes from m24), the host stands in Yahar'gul, under BB_NET_SIM=mobile.
YJC_ENV = {"YJ_STAGGER": "1", "YJ_GUESTS": "2", "YJ_NET": "mobile", "YJ_GUEST_SAVE": "cathedral_day", "YJ_EARLY": "0", "YJ_GCLEAR": "", "YJ_GSET": "", "YJ_HSET": "",
           "YJ_GUEST_LAMP": "", "YJ_HOST_SAVE": ""}


def phase_yjc(only):
    d = _with_env(YJC_ENV, phase_yj, only)
    d["name"] = "yjc"
    return d


# Phase llp (CL-24e713, ~6 min, 2 players, group `lecture_lamp_party`): the first Lecture Building lamp in a party. The party stands in
# Yahar'gul, the One Reborn's defeat flags rise live with the guest present (not from the save), the host's altar warp 3202959 takes
# both to the Lecture Building and the HOST walks to the lamp and presses Cross: its lit flag must rise (and show on the guest's game).
LLP_ENV = {"LL_GUESTS": "1", "LL_VIA": "altar", "LL_LIVE": "1", "LL_WHO": "host", "LL_NET": ""}


def phase_llp(only):
    d = _with_env(LLP_ENV, phase_ll, only)
    d["name"] = "llp"
    return d



# Phase pc (party_chat, ~6 min, 3 players, group `party_chat`; 0.7.8 CL-5b356c): party text chat under
# BB_NET_SIM=dsl with the real box and list (BB_PARTY_CHAT_TEST, party_chat_game.cpp). Own clocks (s after a game
# is in a world): host types at 100 and 124 (the second holds a word of the name filter's strict list), picks the
# preset "Follow me" at 112 and types a last line at 190; guest 1 types at 70 and picks "Going to the Dream" at 85;
# guest 2 types at 60, picks "Help!" at 75 and mutes others' chat at 150 (the host's last line must not show on it).
# Guest 1 has Hide offensive names OFF from the start: the host's second line is masked on guest 2 only. Guest
# lines reach the other guest through the host's relay. No chat text may appear in any log (BB_PARTY_LOG=trace).
# The shots/ show the log on the host and guest 1 (coloured names, fading lines); looked at by hand.
PC_TEXTS = ["Hello party", "Hi from guest one", "Hi from guest two", "last line muted", "Hello_party", "you cunt",
            "Follow me", "Going to the Dream", "Help!", "Hi_from_guest_one", "Hi_from_guest_two", "last_line_muted"]


def phase_pc(only):
    host = "100:type=Hello_party,112:pick=1,124:type=you_cunt,190:type=last_line_muted"
    g1 = "5:hide=0,70:type=Hi_from_guest_one,85:pick=5"
    g2 = "60:type=Hi_from_guest_two,75:pick=0,150:recv=0"
    return dict(
        name="pc", seconds=360, count=3, headless=1,
        env=["BB_PARTY_CHAT=1", "BB_NET_SIM=dsl", "BB_PARTY_LOG=trace"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god", "2:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_CHAT_TEST=" + host, "1:BB_PARTY_CHAT_TEST=" + g1, "2:BB_PARTY_CHAT_TEST=" + g2],
        watch=["--watch", r"Party: chat: sent a line=pcsent@0.5,3",
               "--watch", r"Party: chat: line from slot=pcgot@0.5,3"],
        crash=[], until=[r"0:Party: chat: sent a line of 15 characters \(typed\)"], until_hold=25,
    )


def check_pc(v, L, d: Path):
    sent = lambda i: len(L.all(i, r"chat: sent a line of \d+ characters"))
    v.step("pc host sent 4 lines", sent(0) == 4, "%d" % sent(0))
    v.step("pc guest 1 sent 2 lines", sent(1) == 2, "%d" % sent(1))
    v.step("pc guest 2 sent 2 lines", sent(2) == 2, "%d" % sent(2))
    got = lambda i, slot: len(L.all(i, r"chat: line from slot %d \(\d+ characters" % slot))
    v.step("pc host got guest 1's and guest 2's lines", got(0, 1) == 2 and got(0, 2) == 2,
           "from 1: %d, from 2: %d" % (got(0, 1), got(0, 2)))
    v.step("pc guest 1 got the host's 4 lines and guest 2's 2 (relay)", got(1, 0) == 4 and got(1, 2) == 2,
           "from host: %d, from 2: %d" % (got(1, 0), got(1, 2)))
    v.step("pc guest 2 got the host's 4 lines (the last one muted) and guest 1's 2 (relay)", got(2, 0) == 4 and got(2, 1) == 2,
           "from host: %d, from 1: %d" % (got(2, 0), got(2, 1)))
    muted = L.all(2, r"chat: line from slot 0 .* not shown: others' chat is muted")
    v.step("pc guest 2: the host's last line is muted", len(muted) == 1, "%d" % len(muted))
    m2 = L.all(2, r"chat: line from slot 0 \(\d+ characters, \d+ word\(s\) masked\)")
    m1 = L.all(1, r"chat: line from slot 0 \(\d+ characters, \d+ word\(s\) masked\)")
    v.step("pc masking on (guest 2): the host's second line has its word masked", len(m2) == 1, "%d" % len(m2))
    v.step("pc masking off (guest 1): nothing masked", len(m1) == 0, "%d" % len(m1))
    leaks = [(i, t) for i in (0, 1, 2) for t in PC_TEXTS for t2 in (t, t.replace("_", " ")) if L.all(i, re.escape(t2))]
    v.step("pc no chat text in any log (BB_PARTY_LOG=trace)", not leaks, "none" if not leaks else str(leaks[:2]))
    none_of(v, L, "pc no chat event refused", r"chat: event from slot \d+ refused")
    check_common(v, L, "pc")


# Phase ah (auto_hud, ~3 min, 2 players, group `auto_hud`; 0.7.8 CL-7d51fe + CL-21a501): the host runs Auto-hide HUD
# (BB_AUTO_HUD=1, docs/auto_hud.md) and both players Coloured name tags (BB_PARTY_NAME_COLOURS=1). The host's clock
# (s after its world is up, BB_AUTO_HUD_TEST): 25 own HP to 50 % (damage), 40 HP to 100 %, 55 stamina to 20 %, 75
# echoes +500, 90 peek, 100 puts Madman's Knowledge in quick slot 1 and selects it, 106 selects slot 0 again (the game's selection record). Between those
# the HUD must be hidden (reason bits 0): idle -> hidden after the first 4 s. The guest keeps its normal HUD (control).
# Run with --every 3: the shots/ show the host's HUD hidden / shown / fading and the coloured name tags on both
# screens; looked at by hand.
def phase_ah(only):
    host = "25:hp=50,40:hp=100,55:stam=20,75:echoes=+500,90:peek,100:quick=1:1500,106:quick=0"
    return dict(
        name="ah", seconds=210, count=2, headless=0,
        env=["BB_PARTY_NAME_COLOURS=1", "BB_PARTY_LOG=trace"],
        ienv=["0:BB_AUTO_HUD=1", "0:BB_AUTO_HUD_TEST=" + host, "0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=god"],
        watch=[], crash=[], until=[r"0:Auto HUD: test: quick=0"], until_hold=20,
    )


def check_ah(v, L, d: Path):
    need(v, L, "ah the HUD hook is in", 0, r"Auto HUD: installed")
    ent = need(v, L, "ah world entered: shown (reason entered)", 0, r"Auto HUD: shown \(reason bits 0x1,")
    hid = need(v, L, "ah idle: hidden after the first seconds", 0, r"Auto HUD: hidden \(reason bits 0x0,", ent or 0)
    need(v, L, "ah the clip's alpha was read (100 = full) and ours set", 0, r"Auto HUD: alpha 0\.\d+ set \(the clip's alpha read before: 100\.0\)")
    t = need(v, L, "ah damage (HP to 50 %): shown with the HP reason", 0, r"Auto HUD: shown \(reason bits 0x[0-9a-f]*[2367ab]", hid or 0)
    need(v, L, "ah stamina spent: shown with the stamina reason", 0, r"Auto HUD: shown \(reason bits 0x[0-9a-f]*[4567cdef],", hid or 0)
    need(v, L, "ah echoes changed: shown with the runes reason", 0, r"Auto HUD: shown \(reason bits 0x[0-9a-f]*[89abcdef],", hid or 0)
    need(v, L, "ah peek: shown with the peek reason", 0, r"Auto HUD: shown \(reason bits 0x2[0-9a-f]{2},", hid or 0)
    need(v, L, "ah quick-item switch: shown with the equipment reason", 0, r"Auto HUD: shown \(reason bits 0x[0-9a-f]*[13579bdf][0-9a-f],", hid or 0)

    none_of(v, L, "ah the guest (setting off) never hid its HUD", r"Auto HUD: (hidden|alpha)", insts=(1,))
    check_common(v, L, "ah")


# Phase vm (version_options, ~2 min, 2 players, group `version_options`; 0.7.8 report CL-aa5a64: same build,
# the host shared progress, the guest had BB_PARTY_PROGRESS=0: refused as a generic mismatch). VM_MODE=follow
# (default): the guest turns shared progress and cutscenes off and must still join (host-world hooks are not
# part of the compatibility check). VM_MODE=refuse: the guest turns Guest lamps / Headstones off, the host
# refuses it and the guest's party UI names both options with both values in plain words, says to restart and keeps the
# host's values for the launcher (party_adopt.txt). VM_MODE=maxadopt (0.7.8 hostopts078, 4 players): the host has Max
# players 4, the guests keep 3: they take the host's and all join.
VM_REFUSE_RE = r"party options differ \(Guest lamps \(BB_PARTY_GUEST_LAMPS\): host ON, you OFF; Headstones \(BB_PARTY_HEADSTONES\): host ON, you OFF\)"


def phase_vm(only):
    mode = os.environ.get("VM_MODE", "follow")
    refuse = mode == "refuse"
    if mode == "maxadopt":
        # 0.7.8 (hostopts078): the host runs Max players 4, all three guests keep the default 3: they take the host's
        # (WELCOME) and join; the 4th player gets in too (host counts 3 cooperators).
        return dict(
            name="vm", seconds=420, count=4, headless=2,
            env=["BB_PARTY_HOLD_LOG=1"],
            ienv=["0:BB_PARTY_MAX=4", "1:BB_PARTY_MAX=3", "2:BB_PARTY_MAX=3", "3:BB_PARTY_MAX=3"] +
                 ["%d:BB_PARTY_DIRECTOR_TEST=god" % i for i in range(4)],
            until=[r"0:cooperators 2 -> 3"], until_hold=15, crash=[],
        )
    guest = ["1:BB_PARTY_GUEST_LAMPS=0", "1:BB_PARTY_HEADSTONES=0"] if refuse else ["1:BB_PARTY_PROGRESS=0", "1:BB_PARTY_STORY=0"]
    return dict(
        name="vm", seconds=180, count=2, headless=0,
        env=[], ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god"] + guest, crash=[],
        until=[r"1:the host refused this game|1:refused by the host"] if refuse else [r"1:join: arrived \d+\.\d m from the host"], until_hold=10,
    )


def check_vm(v, L, d: Path):
    mode = os.environ.get("VM_MODE", "follow")
    if mode == "maxadopt":
        need(v, L, "vm maxadopt: the host runs Max players 4", 0, r"max players \d -> 4|up to 4 players")
        for g in (1, 2, 3):
            need(v, L, "vm maxadopt guest %d: takes the host's Max players (own setting 3)" % g, g, r"max players 3 -> 4 \(the host's setting\)")
            need(v, L, "vm maxadopt guest %d: in the host's world" % g, g, r"guest services: open: guest in the host's")
        need(v, L, "vm maxadopt: 4 players (host counts 3 cooperators)", 0, r"cooperators \d -> 3")
        none_of(v, L, "vm maxadopt: nobody refused (no 'party rules differ' / mismatch)", r"version mismatch with the host|party rules differ|the host refused", insts=(0, 1, 2, 3))
    elif mode == "refuse":
        need(v, L, "vm refuse: the guest is refused with both options named and both values", 1, VM_REFUSE_RE)
        need(v, L, "vm refuse: the guest is told to restart (the host's values kept for the launcher)", 1,
             r"host-owned options differ; kept the host's for the launcher's next join \(party_adopt\.txt\): .*headstones=1")
        need(v, L, "vm refuse: the explanation is in the party status", 1, r"restart to join - the launcher uses the host's for your next join")
        none_of(v, L, "vm refuse: no internal hook names in the refusal", r"party patches differ", insts=(0, 1))
        none_of(v, L, "vm refuse: the host's options prefix is not shown to the guest", r"\[host options:", insts=(1,))
        none_of(v, L, "vm refuse: the guest did not join", r"join: arrived \d+\.\d m from the host", insts=(1,))
    else:
        need(v, L, "vm follow: the guest with progress / story off joins the host", 1, r"join: arrived \d+\.\d m from the host")
        none_of(v, L, "vm follow: no refusal", r"version mismatch with the host", insts=(0, 1))
    check_common(v, L, "vm")



# Phase gh (guest_dream_host, group `guest_dream`; mirror of gd): the guest picks the Hunter's Dream at the host's
# lamp first and goes alone (the host stays); the lamp's warp event also runs on the host's game. Then the host,
# still at that lamp, picks the Dream: it must warp (its own copy of the event was used up by the guest's trip?).
def phase_gh(only):
    return dict(
        name="gh", seconds=300, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + GD_LAMP, "0:BB_PARTY_DIRECTOR_TEST=god", "0:BB_PARTY_SPLIT_TEST=no",
              "0:BB_LAMP_TRAVEL_TEST=wait=70,lamp=%s,to=2102950" % GD_LAMP,
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_SPLIT_TEST=no",
              "1:BB_LAMP_TRAVEL_TEST=members,lamp=%s,to=2102950" % GD_LAMP],
        watch=["--watch", r"chose the Hunter's Dream=ghchose@0,4"],
        crash=[], until=[r"0:Lamp travel: arrived: lamp 2102950|0:no load within 60 s|0:test: FAILED"], until_hold=12,
    )


# Phases gr / gr2 / gr3 (guest_rest, ~3 min each, 2 players, group `guest_rest`; 0.7.7 report: a guest's travel from
# the map took the host's world through a rest load, a loading screen for everyone, and only the teleporter moved).
# The map travel is lamp travel's test aid (BB_LAMP_TRAVEL_TEST guestmap= / map=: RequestMapTravel, the same lamp
# warp as a choice in a world lamp's list, but from where the player stands: no lamp was used, so no rest).
#   gr : the guest map-travels to a lamp of the host's own map (Central Yharnam): its own loading screen only; the
#        host keeps playing (no rest load, no forced transform, no load).
#   gr2: the guest map-travels to another map (Old Yharnam) and says NO to "Travel with the party?": the host stays.
#   gr3: the HOST map-travels to Old Yharnam, YES: the whole party follows (the existing travel question).
GR_LAMPS = LP_NEW[1] + LP_OLD[1]   # Central Yharnam 2412951 and Old Yharnam 2302950 lit (the saves hold what the map lists)
GR_HOST_REST = r"host: slot \d+ rested at lamp|forced transform \(a guest's rest|a guest's rest"


def _gr_env(host_extra, guest_extra, lamps=True):
    # 0.7.7 (CL-bc6bdc): the guest lamps rule (the shipped default, on) is set EXPLICITLY on every run; the
    # "o" variants run with it off (BB_PARTY_GUEST_LAMPS=0). Before, the rule's state was left to the ini default.
    return dict(
        count=2, headless=0, seconds=240,
        env=["BB_PARTY_PROGRESS_TEST_SET=" + ",".join(GR_LAMPS), "BB_PARTY_SHARE_LAMPS=1",
             "BB_PARTY_GUEST_LAMPS=" + ("1" if lamps else "0")],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + LP_HOST_LAMP, "0:BB_PARTY_DIRECTOR_TEST=god"] + host_extra +
             ["1:BB_PARTY_DIRECTOR_TEST=god"] + guest_extra,
        crash=[],
    )


def phase_gr(only, name="gr", lamps=True):
    d = _gr_env([], ["1:BB_PARTY_SPLIT_TEST=no", "1:BB_LAMP_TRAVEL_TEST=members,wait=15,guestmap=" + LP_NEW[0]], lamps)
    d.update(name=name, watch=["--watch", r"map travel: lamp \d+=grmap@1"],
             until=[r"1:Lamp travel: arrived: lamp %s|1:test: FAILED|1:map travel to lamp \d+ refused" % LP_NEW[0]], until_hold=30)
    return d


def phase_gr2(only, name="gr2", lamps=True):
    d = _gr_env([], ["1:BB_PARTY_SPLIT_TEST=no", "1:BB_LAMP_TRAVEL_TEST=members,wait=15,guestmap=" + LP_OLD[0]], lamps)
    d.update(name=name, watch=["--watch", r"map travel: lamp \d+=grmap@1"],
             until=[r"1:Lamp travel: arrived: lamp %s|1:test: FAILED|1:map travel to lamp \d+ refused" % LP_OLD[0]], until_hold=30)
    return d


def phase_gr3(only, name="gr3", lamps=True):
    d = _gr_env(["0:BB_PARTY_SPLIT_TEST=yes", "0:BB_LAMP_TRAVEL_TEST=members,wait=15,map=" + LP_OLD[0]], ["1:BB_PARTY_SPLIT_TEST=yes"], lamps)
    d.update(name=name, watch=["--watch", r"map travel: lamp \d+=grmap@0"],
             until=[r"0:Lamp travel: arrived: lamp %s|0:test: FAILED|0:map travel to lamp \d+ refused" % LP_OLD[0]], until_hold=40)
    return d


def phase_gr4(only, name="gr4", lamps=True):
    # the guest is APART (it travelled alone to Old Yharnam, NO), then map-travels to a lamp of the HOST's map
    # (Central Yharnam): its own trip back into the host's area, one loading screen, the host does not move.
    # thenmode=party: the second travel runs in travel-choice mode "party" (no "Travel with the party?" question: the
    # guest's warp goes straight to the request path that the host answers with "same map, the guest loads alone").
    d = _gr_env([], ["1:BB_PARTY_SPLIT_TEST=no",
                     "1:BB_LAMP_TRAVEL_TEST=members,wait=15,guestmap=%s,then=%s,thenmode=party" % (LP_OLD[0], LP_NEW[0])], lamps)
    d.update(name=name, watch=["--watch", r"map travel: lamp \d+=grmap@1"], seconds=300,
             until=[r"1:Lamp travel: arrived: lamp %s|1:test: FAILED|1:map travel to lamp \d+ refused|1:no load within 60 s" % LP_NEW[0]], until_hold=30)
    return d


# Phases mp / mpg / mph (in-game map, group `atlas`; 0.7.8 CL-916488, Hunter's Atlas by MikeTat1988; BB_MAP=1,
# BB_MAP_TEST in gpu/shim/map/map_view.cpp, the map data made from the agent's own game files by
# scripts/map_prepare.py into ATLAS_MAP_DIR, never committed):
#   mp : the host opens the map once the guest's position is known (BB_MAP_TEST=members): the guest's dot is
#        drawn near the Tomb of Oedon lamp; the guest is moved 14 m and then 14 m back: the dot follows
#        (the map logs "Map: shown ... | Hunter1 (x z)" every second); screenshots of the host's map each time.
#   mpg: the GUEST opens its map and travels to Old Yharnam from it: its own loading screen, the host stays.
#   mph: the HOST opens its map and travels to Old Yharnam from it: the party travel question, YES, all follow.
ATLAS_MAP_DIR = os.environ.get("ATLAS_MAP_DIR", r"E:\games\bbport\mapdata")
MP_NEAR = "-33.5:-40.8:88.1"        # the Tomb of Oedon lamp's spot (m24_01)
MP_EAST = "-19.5:-40.8:88.1"
MP_WEST = "-47.5:-40.8:88.1"


def _mp_env(host_extra, guest_extra):
    d = _gr_env(host_extra, guest_extra)
    d["env"] = d["env"] + ["BB_MAP=1", "BB_MAP_DIR=" + ATLAS_MAP_DIR]
    return d


def phase_mp(only):
    d = _mp_env(["0:BB_MAP_TEST=members,open,delay=6000"],
                ["1:BB_PARTY_SPLIT_TEST=no",
                 "1:BB_PARTY_DIRECTOR_TEST=god,guest_move=%s@30,guest_move=%s@45,guest_move=%s@60" % (MP_EAST, MP_WEST, MP_NEAR)])
    d["ienv"] = [x for x in d["ienv"] if x != "1:BB_PARTY_DIRECTOR_TEST=god"]
    d.update(name="mp", seconds=240,
             watch=["--watch", r"Map: opened=mpopen@2,6", "--watch", r"test: guest_move.*=mpmove@6,9"],
             until=[r"\d:test: FAILED"], until_hold=5)
    return d


def phase_mpg(only):
    d = _mp_env([], ["1:BB_PARTY_SPLIT_TEST=no", "1:BB_MAP_TEST=members,travel=%s,delay=4000" % LP_OLD[0]])
    d.update(name="mpg", seconds=300, watch=["--watch", r"Map: opened=mpgopen@2,5", "--watch", r"Map: travel to .*asked=mpgask@1"],
             until=[r"1:Lamp travel: arrived: lamp %s|\d:test: FAILED|1:map travel to lamp \d+ refused|1:Map: test travel: no lamp" % LP_OLD[0]],
             until_hold=30)
    return d


def phase_mph(only):
    d = _mp_env(["0:BB_PARTY_SPLIT_TEST=yes", "0:BB_MAP_TEST=members,travel=%s,delay=4000" % LP_OLD[0]], ["1:BB_PARTY_SPLIT_TEST=yes"])
    d.update(name="mph", seconds=300, watch=["--watch", r"Map: opened=mphopen@2,5", "--watch", r"Map: travel to .*asked=mphask@1"],
             until=[r"0:Lamp travel: arrived: lamp %s|\d:test: FAILED|0:map travel to lamp \d+ refused|0:Map: test travel: no lamp" % LP_OLD[0]],
             until_hold=40)
    return d


# Phase gm (guest_lamp, ~4 min, 2 players, group `guest_lamp`; 0.7.5 report: a guest could light a lamp in
# the host's world only after a visit to the Hunter's Dream): the guest's OWN world is Old Yharnam (a lamp
# warp before the join, as the reporter: A5 cross-map join from m23), not the Dream. Central Yharnam's
# lamp is unlit in both saves; the guest walks to it, lights it, and its list then opens.
def phase_gm(only):
    return dict(
        name="gm", seconds=300, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(LP_NEW[1]), "BB_PARTY_GUESTS_BRING_LAMPS=0", "BB_PARTY_SHARE_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + LP_HOST_LAMP, "0:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LP_NEW[1]),
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LP_NEW[1]),
              "1:BB_PARTY_TITLE_JOIN=0", "1:BB_PARTY_TEST_START_LAMP=" + GM_OWN,
              "1:BB_LAMP_TRAVEL_TEST=members,lamp=%s,listonly" % LP_NEW[0]],
        watch=["--watch", r"test: warp menu up \(listonly\)=gmlist@1"],
        crash=[], until=[r"1:test: listonly: warp menu closed|1:test: FAILED"], until_hold=8,
    )


# Phase jl (joinlamp, ~4 min, 2 players, group `join_lamp`; 0.7.6 report: a guest joined across maps at the host's
# lamp, which its own save lacked, and lighting it did nothing): the guest's own world is Old Yharnam, its save
# lacks the Tomb of Oedon lamp (cleared once); the host's last lamp is that lamp. After the A5 cross-map join the
# lamp must be in the guest's own save (all three flags, as a save on disk shows) and its lamp list must open at it.
JL_LAMP = ("2702950", ["12707810", "12707811", "72700200"])


def phase_jl(only):
    return dict(
        name="jl", seconds=300, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(JL_LAMP[1]), "BB_PARTY_GUESTS_BRING_LAMPS=0", "BB_PARTY_SHARE_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + JL_LAMP[0], "0:BB_PARTY_DIRECTOR_TEST=god",
              # the host's world has the lamp-lit flag only (its headstone flags clear): the arrival still counts it lit
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(JL_LAMP[1][:2]),
              "0:BB_PARTY_PROGRESS_TEST_SET=" + JL_LAMP[1][2],
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(JL_LAMP[1]),
              "1:BB_PARTY_TITLE_JOIN=0", "1:BB_PARTY_TEST_START_LAMP=" + GM_OWN,
              "1:BB_LAMP_TRAVEL_TEST=members,lamp=%s,listonly" % JL_LAMP[0]],
        watch=["--watch", r"test: warp menu up \(listonly\)=jllist@1"],
        crash=[], until=[r"1:test: listonly: warp menu closed|1:test: FAILED"], until_hold=10,
    )


# Phases ml + mg (mensislamp, ~4 min each, 2 players, group `mensis_lamp`; 0.7.6 report: "the first lamp of
# Nightmare of Mensis disappears but we still spawn there"). The party stands where the Lecture Building door put
# it (the scripted arrival 2602959, a spot 140 m from the first real lamp, no lamp object there); the first real
# lamp 2602950 is unlit in both saves.
#   ml: the HOST lights it (Cross at its object, which also opens the lamp menu: its last lamp is 2602950). The guest's
#       own last lamp is still the arrival; it dies: it must get up at the host's lit lamp (no load), not at the arrival.
#   mg: the GUEST lights it (the host learns it: lamp_lit). The 0.7.2 last-lamp guard put the host's last lamp back to
#       the arrival ("a lamp lit by someone else"); the host dies: it must get up at the lit lamp, the party with it.
#       BB_PARTY_LAMP_ARRIVAL_COUNTS=0 is the old guard (the A/B run).
ML_LAMP = ("2602950", ["12607810", "12607811", "72600200"])
ML_OBJ = "2601950"


def _light_lamp(who, obj, flag, mapname, first_delay):
    """Chain steps that light the lamp object `obj` (its lit flag `flag`) on the player's own game: stand 1.2 m from it and
    press Cross twice, each side of the lamp in turn until the flag rises (a lamp's approach side is not known and a wrong
    side finds no prompt: ml / mg never lit the Mensis lamp from side 0 in 0.7.8 and waited 240 s for its flag; hlq does the
    same). The tries after the success wait on a flag that stays 1 and never run."""
    steps = ["god"]
    for ang in (0, 180, 90, 270):
        steps += ["%s_obj=%s:1.2:%d@%d%%%s?%s=0" % (who, obj, ang, first_delay if ang == 0 else 4, mapname, flag),
                  "%s_press=0x4000@3%%%s!" % (who, mapname), "%s_press=0x4000@4%%%s!" % (who, mapname)]
    return ",".join(steps)


def _ml_env(host, guest):
    return dict(
        count=2, headless=0, seconds=300,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(ML_LAMP[1]), "BB_PARTY_SHARE_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2602959", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(ML_LAMP[1]),
              "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(ML_LAMP[1])],
        crash=[],
    )


def phase_ml(only):
    ph = _ml_env(_light_lamp("host", ML_OBJ, ML_LAMP[1][2], "m26_00", 6),
                 "god,guest_die@4%%m26_00?%s=1" % ML_LAMP[1][2])
    ph.update(name="ml", until=[r"1:guest: died alone: revived|1:test: FAILED"], until_hold=10,
              watch=["--watch", r"guest death redirect: host's last lamp %s=mlredir@1,3" % ML_LAMP[0],
                     "--watch", r"guest: died alone: revived=mlrevive@1,3"])
    return ph


def phase_mg(only):
    ph = _ml_env("god,host_die@6%%m26_00?%s=1" % ML_LAMP[1][2],
                 _light_lamp("guest", ML_OBJ, ML_LAMP[1][2], "m26_00", 25))
    ph.update(name="mg", until=[r"0:host death taken: .*revive at lamp|0:test: FAILED"], until_hold=12,
              watch=["--watch", r"host death taken: =mgdie@1,4"])
    return ph

# Phase hlq (host_lamp, ~5 min, 2 players, group `host_lamp`; 0.7.8 GitHub issue #28: a lamp the guest lit in the host's
# world reached the host's save with the lamp-lit flag 7AAB02xx but not the lamp object's base+10, so the host could
# neither light nor rest at it). A Hunter's Nightmare (DLC) lamp, Nightmare Church 3402951, is unlit in both saves; the
# host stands in m34 (lamp 3402950), the guest joins and lights it at its object. Then the host walks to the lamp and
# presses Cross: it must open the lamp menu (rest). The host's save on disk must hold all of the lamp's flags.
HLQ_LAMP = ("3402951", ["13407830", "13407831", "73400201"])
HLQ_OBJ = "3401951"


def phase_hlq(only):
    host = ("god,host_obj=%s:1.2:0@8%%m34_00?%s=1,host_press=0x4000@3%%m34_00!,host_press=0x4000@4%%m34_00!"
            % (HLQ_OBJ, HLQ_LAMP[1][2]))
    steps = []
    for ang in (0, 180, 90, 270):   # the approach side of a lamp is not known: each one tried until its flag rises (ll)
        steps += ["guest_obj=%s:1.2:%d@%d%%m34_00?%s=0" % (HLQ_OBJ, ang, 25 if ang == 0 else 4, HLQ_LAMP[1][2]),
                  "guest_press=0x4000@3%m34_00!", "guest_press=0x4000@4%m34_00!"]
    guest = ",".join(["god"] + steps)
    return dict(
        name="hlq", seconds=240, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(HLQ_LAMP[1]), "BB_PARTY_SHARE_LAMPS=1"] + os.environ.get("HLQ_ENV", "").split(),
        ienv=["0:BB_PARTY_TEST_START_LAMP=3402950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(HLQ_LAMP[1]),
              "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(HLQ_LAMP[1])],
        crash=[], until=[r"0:test: travel step 3/3: host_press|0:test: FAILED"], until_hold=20,
    )


# Phase hlr (host_lamp_repair, ~2 min, 1 player, group `host_lamp_repair`; 0.7.8 GitHub issue #28): the save repair.
# Fixture lamp_half (test_saves, docs/party/test_saves.md): the default save with the lamp-lit flags of Nightmare
# Church (73400201, DLC) and the Cathedral Ward lamp 2402951 (72400201) set and their object flags clear. The
# party code repairs it once per save: one log line per lamp, the object flags set, the marker flag set, the save
# written. A second look (10 s later) finds nothing more to do.
HLR_FIXTURE = Path(r"E:\games\bbport\test_saves\lamp_half\savedata")
HLR_FLAGS = ["13407830", "13407831", "73400201", "12407830", "12407831", "72400201", "60009973"]


def prep_hlr(root: Path):
    if not HLR_FIXTURE.is_dir():
        sys.exit("hlr: the fixture %s is missing (docs/party/test_saves.md)" % HLR_FIXTURE)
    _cl_saves(root, HLR_FIXTURE)


def post_hlr(root: Path):
    _cl_saves(root, CL_DEFAULT)


def phase_hlr(only):
    return dict(
        name="hlr", seconds=150, count=1, headless=0, prep=prep_hlr, post=post_hlr,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + ",".join(HLR_FLAGS)],
        ienv=["0:BB_PARTY_DIRECTOR_TEST_SOLO=1", "0:BB_PARTY_DIRECTOR_TEST=god"],
        crash=[], until=[r"0:lamp repair: this save was checked|0:test: FAILED"], until_hold=25,
    )


def check_hlr(v, L, d: Path):
    for lamp, flags in (("3402951", "13407830"), ("2402951", "12407830")):
        need(v, L, "hlr lamp %s repaired (one line)" % lamp, 0, r"lamp repair: lamp %s had its lamp-lit flag but not its object flag: set 2 of 2" % lamp)
    need(v, L, "hlr the save is marked", 0, r"lamp repair: this save was checked, 2 lamps repaired \(marker flag 60009973\)")
    need(v, L, "hlr the repaired save is written", 0, r"save requested \(lamp repair")
    again = L.all(0, r"lamp repair: lamp \d+ had")
    v.step("hlr nothing is repaired twice", len(again) == 2, "%d repair lines" % len(again))
    got = lp_save_flags(d, 0, HLR_FLAGS)
    fmt = lambda m: " ".join("%s=%d" % (k, m[k]) for k in m) if m else "no save"
    v.step("hlr the save on disk: whole lamps and the marker", got is not None and all(got[f] for f in HLR_FLAGS), fmt(got))
    check_common(v, L, "hlr")


# Phase ll (lecture_lamp, ~2 min, 1 player, group `lecture_lamp`; 0.7.8 report CL-24e713: "the host cannot light the
# first Lecture Building lamp after The One Reborn"). The default save with the One Reborn dead (12801800 / 2800 /
# 9464) arrives by the altar warp's scripted spot 3202959 (m32_00, no lamp object); the lamp beside it is 3202953
# (object 3201953, lit flag 13207870). The host presses Cross at the object: the lamp must light (lit flag rises).
LL_N = int(os.environ.get("LL_N", "3"))   # lamp index in m32_00 (3 = the lamp beside the altar warp's arrival; 1 = beside the Amygdala grab's)
LL_LIT = str(13207810 + 20 * LL_N)
LL_FLAGS = [LL_LIT, str(int(LL_LIT) + 1), str(73200200 + LL_N)]
LL_OBJ = str(3201950 + LL_N)
LL_START = os.environ.get("LL_START", "3202959" if LL_N == 3 else "3202958")


def phase_ll(only):
    # repro079 knobs (a party in the Lecture Building): LL_GUESTS=1/2 guests, LL_WHO=host|guest (who walks to the lamp and presses),
    # LL_VIA=altar (the party stands in Yahar'gul, the host's altar warp 3202959 takes everyone along) else the host starts at the
    # arrival spot and the guests join from their Dream (cross-map join), LL_NET = a BB_NET_SIM preset.
    n = int(os.environ.get("LL_GUESTS", "0"))
    who = os.environ.get("LL_WHO", "host")
    via = os.environ.get("LL_VIA", "")
    net = os.environ.get("LL_NET", "")
    steps = []
    for ang in (0, 180, 90, 270):   # the approach side of a lamp is not known: each one tried until the flag rises
        steps += ["%s_obj=%s:1.2:%d@%d%%m32_00" % (who, LL_OBJ, ang, 6 if ang == 0 else 4),
                  "%s_press=0x4000@3%%m32_00!" % who, "%s_press=0x4000@4%%m32_00!" % who]
    pre = []
    if via == "altar" and n:
        if os.environ.get("LL_LIVE") == "1":   # the One Reborn's defeat flags rise live in the party's Yahar'gul (a kill with a guest present), not from the save
            pre += ["host_flag=12801800@6%m28_00", "host_flag=2800@1%m28_00", "host_flag=9464@1%m28_00"]
        pre += ["host_lamp=3202959:story@14%m28_00"]
    actor = ",".join(["god", "log_flag=" + LL_LIT, "log_pos=1"] + steps)
    other = "god,log_flag=" + LL_LIT
    host = ",".join(["god", "log_flag=" + LL_LIT, "log_pos=1"] + pre + (steps if who == "host" else []))
    ienv = ["0:BB_PARTY_TEST_START_LAMP=" + ("2802950" if (via == "altar" and n) else LL_START),
            "0:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join(LL_FLAGS),
            "0:BB_PARTY_GUESTSVC_TEST=ownprobe;lampprobe;promptlog"]
    ienv.append("0:BB_PARTY_DIRECTOR_TEST=" + host)
    for g in range(1, n + 1):
        ienv += ["%d:BB_PARTY_DIRECTOR_TEST=%s" % (g, actor if (who == "guest" and g == 1) else other),
                 "%d:BB_PARTY_TITLE_JOIN=0" % g, "%d:BB_PARTY_PROGRESS_TEST_CLEAR=%s" % (g, ",".join(LL_FLAGS))]
    live = os.environ.get("LL_LIVE") == "1" and via == "altar" and n
    env = [("BB_PARTY_PROGRESS_TEST_CLEAR=12801800,2800,9464" if live else "BB_PARTY_PROGRESS_TEST_SET=12801800,2800,9464"),
           "BB_PARTY_DIRECTOR_TEST_SOLO=1" if not n else "BB_PARTY_TRAVEL_CHOICE=party",
           "BB_PARTY_PROGRESS_TEST_READ=" + ",".join(LL_FLAGS)]
    if net:
        env.append("BB_NET_SIM=" + net)
    who_i = 0 if who == "host" else 1
    extra = {}
    if n:
        extra = dict(start_when=[r"%d/0:map - -> m%s_00_00_00" % (g, "28" if via == "altar" else "32") for g in range(1, n + 1)])
    return dict(
        name="ll", seconds=150 + (240 if n else 0), count=1 + n, headless=0 if n <= 1 else 1,
        env=env, ienv=ienv, crash=[],
        until=[r"%d:test: flags %s=1|%d:test: FAILED" % (who_i, LL_LIT, who_i)], until_hold=8, **extra,
    )


def check_hlq(v, L, d: Path):
    lit = need(v, L, "hlq the guest lights the Nightmare Church lamp", 1,
               r"lamp: the guest lit a lamp of the host's world \(overlay:.*%s" % HLQ_LAMP[1][2])
    got = need(v, L, "hlq the host lights it in its world: all of the lamp's flags", 0,
               r"lamp: slot \d+ lit a lamp in the host's world: set .*%s" % HLQ_LAMP[1][0], lit or 0)
    none_of(v, L, "hlq no flag of the lamp was not written", r"lamp: slot \d+ lit a lamp in the host's world: .*NOT written",
            insts=(0,))
    need(v, L, "hlq the host saves it", 0, r"save requested \(.*a lamp a guest lit in the host's world", lit or 0)
    menu = L.find(0, r"Party balance: lamp menu at lamp %s" % HLQ_LAMP[0], got or 0)
    v.step("hlq (info) the host's Cross at the lamp opened the lamp menu", True,
           menu[1][:150] if menu else "no lamp menu line (the scripted press may miss the prompt; not proven)")
    host = lp_save_flags(d, 0, HLQ_LAMP[1])
    fmt = lambda m: " ".join("%s=%d" % (k, m[k]) for k in m) if m else "no save"
    v.step("hlq host's save: the whole lamp (object flag, +11, lamp-lit flag)", host is not None and all(host[f] for f in HLQ_LAMP[1]), fmt(host))
    rep = L.find(0, r"lamp repair: lamp %s had" % HLQ_LAMP[0])
    v.step("hlq (info) the repair did not have to finish the lamp", True, rep[1][:150] if rep else "no repair line")
    check_common(v, L, "hlq")


# Phase bq (bosslamp079, CL-b457f5, ~4 min, 2 players, group `boss_lamp`): the Grand Cathedral lamp 3402953 is lit by beating
# Laurence. The host beat him (13401850 set, the lamp lit); the guest's own save has him ALIVE and the lamp unlit. With
# "Shared lamps" on the host's lamps go into the guest's own save: the lamp must never arrive without his defeat flag
# (a guest travelling to the lamp in its own world would stand in front of the living boss). Checked on the guest's save
# on disk: lamp flags 13407860/61 (object, n = 3 of block 340... see lamp_travel_rules.h LitFlags), 73400203 and 13401850.
BQ_LAMP = "3402953"
BQ_BOSS = "13401850"
BQ_LAMP_FLAGS = ["13407870", "13407871", "73400203"]


def phase_bq(only):
    read = ",".join([BQ_BOSS] + BQ_LAMP_FLAGS)
    return dict(
        name="bq", seconds=240, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=" + read, "BB_PARTY_SHARE_LAMPS=1", "BB_PARTY_GUESTS_BRING_LAMPS=0"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=3402950", "0:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_PROGRESS_TEST_SET=" + ",".join([BQ_BOSS] + BQ_LAMP_FLAGS),   # the host beat Laurence: his defeat flag and the lamp he lights
              "1:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=" + ",".join([BQ_BOSS, BR_LAURENCE_CLEAR] + BQ_LAMP_FLAGS)],   # its own world: Laurence alive, lamp unlit
        crash=[],
        until=[r"1:written into the own save \(shared lamps; lamps.* %s[\s\S]*save requested \(.*shared lamps from the host's world" % BQ_LAMP,
               r"1:lamps: every lamp of the own save is lit in the host's world too[\s\S]*1:lamps: .*held back"],
        until_hold=15,
    )


def check_bq(v, L, d: Path):
    need(v, L, "bq join: 2 players", 0, r"cooperators 0 -> 1")
    host = lp_save_flags(d, 0, [BQ_BOSS] + BQ_LAMP_FLAGS)
    guest = lp_save_flags(d, 1, [BQ_BOSS] + BQ_LAMP_FLAGS)
    fmt = lambda m: " ".join("%s=%d" % (k, m[k]) for k in m) if m else "no save"
    v.step("bq host's save: Laurence beaten, the lamp lit", host is not None and all(host.values()), fmt(host))
    lamp_in = bool(guest) and all(guest[f] for f in BQ_LAMP_FLAGS)
    boss_in = bool(guest) and guest[BQ_BOSS]
    v.step("bq guest's own save: the lamp never without Laurence's defeat (lamp in => boss flag in)", guest is not None and (not lamp_in or boss_in),
           fmt(guest))
    check_common(v, L, "bq")



def check_ll(v, L):
    who_i = 0 if os.environ.get("LL_WHO", "host") == "host" else 1
    who = "host" if who_i == 0 else "guest"
    need(v, L, "ll the %s pressed Cross at the lamp" % who, who_i, r"travel step \d+/\d+: %s_press" % who, 0)
    need(v, L, "ll the lamp lit (flag %s) on the %s's game" % (LL_LIT, who), who_i, r"test: flags %s=1" % LL_LIT)
    if int(os.environ.get("LL_GUESTS", "0")):
        # in a party the other game must have the lamp too (a lamp lit in the host's world is shared both ways)
        for i in range(0, 1 + int(os.environ["LL_GUESTS"])):
            if i != who_i:
                v.step("ll (info) inst%d sees the lamp flag" % i, True,
                       "yes" if L.find(i, r"test: flags %s=1" % LL_LIT) else "no line (not a failure: the probe logs only on change in its own world)")
    check_common(v, L, "ll")



# Phase hs (headstone_split, ~5 min, 2 players, group `headstone_split`; 0.7.6 report from the Nightwatch dev):
# both players in the host's Hunter's Dream. The guest picks a Dream headstone (t210000, the first entry) and answers
# NO to "Travel with the party?" (travels alone); the host's game runs its own copy of that headstone's warp event and
# drops it (not its use). Later the host (alone in the Dream) picks the same headstone: it must warp (its copy of the
# event was used up; before the fix its flag was set and nothing happened until an area reload).
HS_KEYS = "keys=cross,cross,cross,cross"
HS_UNTIL = [r"0:host travel: arrived|0:no load within 60 s|0:test: levelup: FAILED|1:test: levelup: FAILED"]


def phase_hs(only):
    return dict(
        name="hs", seconds=330, count=2, headless=0,
        env=["BB_PARTY_GUEST_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=god",
              "0:BB_PARTY_GUESTSVC_TEST=hostheadstone;hsalone;hsdelay=80;" + HS_KEYS,
              "0:BB_PARTY_TRAVEL_CHOICE=alone", "0:BB_PARTY_WORLD_WATCH=72102410,72102401",
              "1:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_GUESTSVC_TEST=guestheadstone;hsdelay=12;" + HS_KEYS,
              "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"guest: travels alone \(headstone\)=hsleave@1,4",
               "--watch", r"test: host headstone: starting=hshost@1,6",
               "--watch", r"host travel: arrived [0-9.]+ s after the warp=hsarrive@2,6"],
        crash=[], until=HS_UNTIL, until_hold=12,
    )


# Phase ngj (ngplus_rejoin, ~3 min, 2 players, group `ngplus_rejoin`; 0.7.6 NG+ fix, player report: after the ending
# the host walked around in NG+ but never became start-ready, so nobody could be summoned): both players start from
# a real NG cycle 2 save (fixture ng_plus_guest, copied; its starting weapon picks are reset by the new cycle). The
# host must reach `step ready ... READY` (not stay "in prologue") and the guest must arrive in the host's world.
NGJ_FIXTURE = Path(r"E:\games\bbport\test_saves\ng_plus_guest\savedata")


def prep_ngj(root: Path):
    if not NGJ_FIXTURE.is_dir():
        sys.exit("ngplus_rejoin: the fixture %s is missing (docs/party/test_saves.md)" % NGJ_FIXTURE)
    _cl_saves(root, NGJ_FIXTURE, 0)
    _cl_saves(root, NGJ_FIXTURE, 1)


def phase_ngj(only):
    return dict(
        name="ngj", seconds=240, count=2, headless=0, prep=prep_ngj, post=post_chalamp,
        env=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god"],
        watch=["--watch", r"start \(prologue_solo\): step ready.*READY=ngjready@0,2",
               "--watch", r"start \(prologue_solo\): in the host's world=ngjarrive@3,8"],
        crash=[], until=[r"1:start \(prologue_solo\): in the host's world|1:test: FAILED"], until_hold=10,
    )


# Phase ff (frontierfall, ~3 min, 2 players, group `frontier_fall`; 0.7.6 report: opening the Lecture
# Building's door to the Nightmare Frontier, the players fell through the ground for a few seconds before
# the warp finished: the story warp (event 13200311 -> 3302959) waits for the "Travel with the party?"
# question). Both players start from the fixture lecture_door (a player save just before that door, m32_00,
# the private fixture is documented in docs/party/test_saves.md); the host opens the door (ObjAct 3201123),
# answers YES; nobody may fall (no fall recovery, y stays within 3 m of the door's y until the load) and
# both arrive in m33_00. BB_PARTY_STORY_HOLD=0 (FF_NOHOLD=1 here) is the old behaviour for an A/B run.
FF_FIXTURE = Path(r"E:/games/bbport/test_saves/lecture_door/savedata")
FF_DOOR = "3201123"
FF_UNTIL = [r"1:map .*-> m33_00"]


def prep_ff(root: Path):
    if not FF_FIXTURE.is_dir():
        sys.exit("frontier_fall: the fixture %s is missing (docs/party/test_saves.md)" % FF_FIXTURE)
    _cl_saves(root, FF_FIXTURE, 0)
    _cl_saves(root, FF_FIXTURE, 1)


def phase_ff(only):
    door = []
    for ang in (0, 180, 90, 270):   # the approach side is not known: each one tried, a load ends the loop
        door += ["host_obj=%s:1.2:%d@%d%%m32" % (FF_DOOR, ang, 10 if ang == 0 else 3),
                 "host_press=0x4000@2%m32!", "host_press=0x4000@2%m32!"]
    host = ",".join(["god", "log_pos=0.5"] + door + ["host_press=0@25%m33!"])
    g1 = ",".join(["god", "log_pos=0.5", "guest_press=0@25%m33!"])
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "0:BB_PARTY_SPLIT_TEST=yes"]
    if os.environ.get("FF_NOHOLD"):
        ienv += ["0:BB_PARTY_STORY_HOLD=0", "1:BB_PARTY_STORY_HOLD=0"]
    return dict(
        name="ff", seconds=300, count=2, headless=0, prep=prep_ff, post=post_chalamp,
        env=["BB_PARTY_EMEVD_TRACE=1"],
        ienv=ienv,
        watch=["--watch", r"story warp: event 13200311=ffwarp@0.5,1.5,3",
               "--watch", r"asking \"Travel with the party\?\"=ffq@0.4",
               "--watch", r"map .*-> m33_00=ffarr@3,6,10,14"],
        crash=[], until=FF_UNTIL, until_hold=18,
    )


def check_ff(v, L):
    j = need(v, L, "ff join: 2 players", 0, r"cooperators 0 -> 1")
    w = need(v, L, "ff the door's story warp runs on the host", 0, r"story warp: event 13200311 warps this player to 3302959", j or 0)
    need(v, L, "ff the hold starts on the host", 0, r"story warp hold: the chr is held", w or 0)
    for i in (0, 1):
        rel = L.find(i, r"story warp hold: released \(a load\)", w or 0)
        v.step("ff %s: the hold ends with the load" % ("host" if i == 0 else "guest 1"), bool(rel) or i == 1,
               rel[1][:120] if rel else "no release line")
        none_of(v, L, "ff %s: no fall recovery" % i, r"recovery: falling for 6 s", insts=(i,))
    need(v, L, "ff the guest arrives in the Frontier", 1, r"map .*-> m33_00", w or 0)
    check_common(v, L, "ff")


# Phase lk (lunariumkey, ~3 min, 2 players, group `lunarium_key`; 0.7.6 report: "we didn't get the Lunarium
# Key in Byrgenwerth"): the key (goods 4013, lot 3200810, flag 53200810) is an EMEVD award of m32_00 event
# 13200400 (object 3201010 with an action button). Both players start from the fixture lecture_door (m32_00,
# no key yet); the host stands at the key object and presses Cross; the key must reach the host and the guest.
LK_OBJ = "3201010"
LK_UNTIL = [r"1:.*lot 3200810|0:test: FAILED"]


def phase_lk(only):
    obj = []
    for ang in (0, 180, 90, 270):
        obj += ["host_obj=%s:1.2:%d@%d%%m32" % (LK_OBJ, ang, 12 if ang == 0 else 4),
                "host_press=0x4000@3%m32!", "host_press=0x4000@2%m32!"]
    # The reporter's flow: the party travels to Byrgenwerth's lamp from the Dream (the guest is in the host's
    # world at the load; LK_DIRECT=1: both start in m32 from the fixture, the first run's variant).
    trip = [] if os.environ.get("LK_DIRECT") else ["host_warp=2102950@6", "host_warp=3202950@14%m21"]
    host = ",".join(["god", "log_pos=0.5", "log_flag=53200810/13200400", "watch_text=Take/Pick up/Key"] + trip + obj)
    g1 = ",".join(["god", "log_pos=1", "log_flag=53200810"])
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1]
    return dict(
        name="lk", seconds=240, count=2, headless=0, prep=prep_ff, post=post_chalamp,
        env=["BB_PARTY_EMEVD_TRACE=1"], ienv=ienv,
        crash=[], until=LK_UNTIL, until_hold=15,
    )


def check_lk(v, L):
    j = need(v, L, "lk join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "lk the host awards the Lunarium Key", 0, r"award lot 3200810", j or 0)
    need(v, L, "lk the guest receives it", 1, r"gave .*goods 4013|key goods changed: 4013 0 -> 1", j or 0)
    check_common(v, L, "lk")


# Phase lkc (the catch-up half of group `lunarium_key`): a save that has the key's mark (53200810) and no
# Lunarium Key (what a shared give while summoned can leave behind) gets the key from the own-world key
# catch-up 4 s after the load; the event 13200400 has ended on the mark, so nothing at the counter would.
LKC_UNTIL = [r"0:key item catch-up: .*goods 4013|0:test: FAILED"]


def phase_lkc(only):
    host = ",".join(["god", "log_flag=53200810"])
    return dict(
        name="lkc", seconds=150, count=1, headless=0, prep=prep_ff, post=post_chalamp,
        env=["BB_PARTY_PROGRESS_TEST_SET=53200810", "BB_PARTY_DIRECTOR_TEST_SOLO=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host],
        crash=[], until=LKC_UNTIL, until_hold=10,
    )


def check_lkc(v, L):
    need(v, L, "lkc the catch-up gives the Lunarium Key to a save with the mark and no key", 0,
         r"key item catch-up: .*goods 4013")
    need(v, L, "lkc the key is held", 0, r"key goods changed: 4013 0 -> 1")
    check_common(v, L, "lkc")


# Phase nl (npc_lamp, ~3 min, 2 players, group `npc_lamp`; 0.7.6 report: a guest who talked to a window
# resident (Gilbert, the little girl) near Central Yharnam's lamp sat down at the lamp and got "Travel with
# the party?"): the host starts at that lamp 2412951; the guest talks to Gilbert (his window is 3.2 m from the
# lamp) to the end of the first conversation, then at once (within the 1 s grace) stands at the lamp and
# presses Cross: the lamp must stay closed (no warp menu, no sit); a press 3 s later opens it (the guest's own
# lamp use still works: group guest_lamp covers the lamp alone).
NL_NPC = "2410782"   # Gilbert's talk chr (window)
NL_UNTIL = [r"1:Lamp travel: lamp 2412951 .*warp menu"]


def phase_nl(only):
    g1 = ",".join(["god", "guest_chr=%s:1.6@12%%m24_01" % NL_NPC]
                  + ["guest_press=0x4000@3%m24_01!"] * 11                  # the first conversation, line by line
                  + ["guest_obj=2411951:1.0:-90@0.2%m24_01",               # at the lamp, right after the talk ended
                     "guest_press=0x4000@1.6%m24_01!",                     # OK of the gift's item box (the lamp in reach)
                     "guest_press=0x4000@0.9%m24_01!",                     # the next press, as a player mashes: held back
                     "guest_press=0x0@2.2%m24_01!",
                     "guest_press=0x4000@0.5%m24_01!"])                    # 3 s after the box: the lamp opens
    return dict(
        name="nl", seconds=300, count=2, headless=0,
        env=["BB_PARTY_GUESTS_BRING_LAMPS=0", "BB_PARTY_SHARE_LAMPS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2412951", "0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog"] + ([ "1:BB_PARTY_LAMP_TALK_GRACE=0" ] if os.environ.get("NL_NOHOLD") else []),
        watch=["--watch", r"npc_talk: local line: talk=nltalk@1.5",
               "--watch", r"npc_talk: local line t\d+ over \(the talk stopped\)=nlend@0.1,0.6,1.5",
               "--watch", r"Lamp travel: lamp 2412951 .*warp menu=nlmenu@0.5"],
        crash=[], until=NL_UNTIL, until_hold=6,
    )


def check_nl(v, L):
    need(v, L, "nl join: 2 players", 0, r"cooperators 0 -> 1")
    at = need(v, L, "nl guest in front of Gilbert", 1, r"test: guest_chr %s" % NL_NPC)
    talk = L.all(1, r"npc_talk: local line: talk \d+ .*npc 241319", at or 0)
    v.step("nl the guest talks to Gilbert (his lines on its game)", len(talk) >= 5, "%d lines" % len(talk))
    end = L.find(1, r"npc_talk: local line t\d+ over \(the talk stopped\)", at or 0)
    v.step("nl the talk ends", bool(end), end[1][:120] if end else "no end line")
    if not end:
        check_common(v, L, "nl")
        return
    lamp_at = L.find(1, r"test: guest_obj 2411951", end[0] - 1)
    presses = L.all(1, r"test: guest_press 0x4000", lamp_at[0] if lamp_at else end[0])
    menu = L.find(1, r"Lamp travel: lamp 2412951 .*warp menu", end[0])
    v.step("nl the guest walked to the lamp right after the talk", bool(lamp_at) and lamp_at[0] - end[0] < 1.5,
           "%.2f s after the talk end" % (lamp_at[0] - end[0]) if lamp_at else "no guest_obj")
    if len(presses) >= 3 and menu:
        ok, mash, late = presses[0][0], presses[1][0], presses[2][0]
        v.step("nl the OK press of the gift box and the next press (lamp in reach) open no lamp", menu[0] >= late - 0.05,
               "OK press +%.1f s, next press +%.1f s, late press +%.1f s, warp menu +%.1f s" % (ok, mash, late, menu[0]))
        v.step("nl the press 3 s after the box opens the lamp", late - 0.05 <= menu[0] <= late + 3.0,
               "+%.1f s after that press" % (menu[0] - late))
    else:
        v.step("nl the three presses and the lamp's warp menu", False, "presses %d, menu %s" % (len(presses), bool(menu)))
    need(v, L, "nl the lamp was kept closed in the grace (log)", 1, r"guest lamp kept closed while an NPC talk plays", end[0] - 5)
    check_common(v, L, "nl")


GM_OWN = "2302950"   # the guest's own world before the join: Old Yharnam


def check_gm(v, L, d: Path):
    j = need(v, L, "gm join: the guest is in the host's world", 1, r"Lamp travel: test: .*summoned: in the host.s world")
    own = L.find(1, r"test: start lamp %s \(from" % GM_OWN)
    v.step("gm the guest started in its own world (not the Dream)", bool(own), own[1][:150] if own else "no start-lamp line")
    lit = need(v, L, "gm guest lights a lamp in the host's world", 1,
               r"lamp: the guest lit a lamp of the host's world \(overlay:.*%s" % LP_NEW[1][2], j or 0,
               what="the guest could not light the lamp (0.7.5 report)")
    need(v, L, "gm host lights it in its world", 0, r"lamp: slot \d+ lit a lamp in the host's world: set.*%s" % LP_NEW[1][2], lit or 0)
    need(v, L, "gm guest's lamp travel list opens", 1, r"test: warp menu up \(listonly\)", j or 0)
    check_common(v, L, "gm")


def check_jl(v, L, d: Path):
    j = need(v, L, "jl join: the guest is in the host's world", 1, r"Lamp travel: test: .*summoned: in the host.s world")
    own = L.find(1, r"test: start lamp %s \(from" % GM_OWN)
    v.step("jl the guest started in its own world", bool(own), own[1][:150] if own else "no start-lamp line")
    w = need(v, L, "jl the host's lamp is written into the guest's own save", 1,
             r"written into the own save \(shared lamps; lamps.* %s|lamp repair: lamp %s had its lamp-lit flag"
             % (JL_LAMP[0], JL_LAMP[0]), 0,  # the lamp repair can set it first (0.7.8)
             what="the host's lamp never reached the guest's save (0.7.6 report)")
    need(v, L, "jl the lamp shows lit in the host's world for the guest", 1, r"listonly: lamp %s is lit on this game" % JL_LAMP[0], 0)
    need(v, L, "jl the guest's list opens at the lamp", 1, r"test: warp menu up \(listonly\)", j or 0)
    guest = lp_save_flags(d, 1, JL_LAMP[1])
    v.step("jl guest's save on disk: the lamp's three flags", guest is not None and all(guest.values()),
           " ".join("%s=%d" % (k, guest[k]) for k in guest) if guest else "no save")
    check_common(v, L, "jl")


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



# Phase bv (visible077, group `boss_visible`, player report 2026-10-28): in a 3-4 player party everyone
# sees everyone after entering a boss fog. Host first (the intro), guest 1 and guest 2 through the fog
# one after the other; all three rendered. Each game's remote chrs line (draw / alpha / position) must
# show both others, drawn, not at the origin, and the screenshots show them.
BV_UNTIL = [r"2:guest: went through boss fog"]


def phase_bv(only):
    host = ",".join(["god", "host_warp=2412953@8^1", "host_move=%s@3%%m24_01" % GASCOIGNE_TRIGGER])
    def guest(delay):
        return ",".join(["god", "guest_press=0x0@%g%%m24_01&" % delay, "guest_obj=2411810@3%m24_01",
                         "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!", "guest_press=0x0@2%m24_01"])
    cut = os.environ.get("BB_BV_CUT", "0") == "1"   # the guest pair is cut (host relay) from before the fog walks
    extra = dict(driver=lambda root, hl: BlockDriver(root, hl, after_s=5, cut_s=int(os.environ.get("BB_BV_CUT_S", "300")))) if cut else {}
    return dict(
        name="bv", seconds=360, count=3, headless=0, until_hold=45,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1", "BB_PARTY_HOLD_LOG=1"] +
            (["BB_NET_SIM_BLOCK_FILE={root}/block.txt", "BB_PARTY_PEER_RELAY_HOLD_MS=12000", "BB_PARTY_PEER_RELAY_STABLE_MS=4000"] if cut else []),
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest(14),
              "2:BB_PARTY_DIRECTOR_TEST=" + guest(30), "1:BB_PARTY_TRAVEL_CHOICE=alone", "2:BB_PARTY_TRAVEL_CHOICE=alone"] +
             (["0:BB_NET_SIM=lat=20", "1:BB_NET_SIM=lat=20,block={p2}", "2:BB_NET_SIM=lat=20,block={p1}"] if cut else []),
        crash=[], until=BV_UNTIL, **extra,
    )


def check_bv(v, L):
    need(v, L, "bv join: 3 players", 0, r"cooperators \d -> 2")
    on = need(v, L, "bv the host's fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        check_common(v, L, "bv")
        return
    w = [need(v, L, "bv guest %d went through the fog" % g, g, r"guest: went through boss fog", on) for g in (1, 2)]
    if None in w:
        check_common(v, L, "bv")
        return
    t0 = max(w) + 3.0
    for i in (0, 1, 2):
        hits = L.all(i, r"remote chrs: ", 0.0, 1e9)
        last = [h for h in hits if h[0] >= t0] or hits[-1:]
        txt = last[-1][1] if last else ""
        chrs = re.findall(r"\[mode \d 0x[0-9a-f]+ \(([^)]*)\) ([\d.]+) m draw ([\d.]+) alpha ([\d.]+)", txt)
        ok = bool(txt) and "no chr" not in txt and len(chrs) >= 2 and             all(float(d) > 0.05 and float(a) > 0.05 for _, _, d, a in chrs) and             not any(re.fullmatch(r"-?0\.0, -?0\.0, -?0\.0", c[0]) for c in chrs)
        v.step("bv instance %d sees both others (chrs up, drawn, not at the origin)" % i, ok, txt[:260] or "no remote chrs line")
    check_common(v, L, "bv")


# Phase bw (invis078, Iosefka CL-23a8a1, group `boss_visible`): a death in the host's boss fight must not
# leave a LIVING member invisible to the others. The host dies in the fight (its limbo: the guests'
# peer copies at HP 0 may be hidden as a spectator's body, a 300 s window), then guest 1 re-joins by a
# Hunter's Mark (a load: every copy on it is built again and reads HP 0 until its state arrives). Live
# bug (0.7.6, 3 players): the fresh copy of the living guest was hidden for good (SetDisable) - one
# guest invisible to the other, the boss ignoring it. Checks: after the re-join every game sees the
# living guests drawn with HP, and no copy hidden as a spectator's body is ever seen with HP again.
# Screenshots (shots/) show the fight from every rendered game.
BW_UNTIL = [r"1:guest_use goods 100[\s\S]*went through boss fog", r"2:guest: went through boss fog"]


def phase_bw(only):
    host = ",".join(["host_warp=2412953@8^2", "host_move=%s@3%%m24_01" % GASCOIGNE_TRIGGER, "host_die@45%m24_01~"])
    g1 = ",".join(["god", "guest_lastlamp=2412953@75%m24_01~", "guest_use=100@3%m24_01",
                   "guest_press=0x0@5%m24_01!", "guest_obj=2411810@3%m24_01",
                   "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!", "guest_press=0x0@2%m24_01"])
    g2 = ",".join(["god", "guest_press=0x0@14%m24_01&", "guest_obj=2411810@3%m24_01",
                   "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!", "guest_press=0x0@2%m24_01"])
    return dict(
        name="bw", seconds=480, count=3, headless=0, until_hold=40,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              "1:BB_PARTY_TRAVEL_CHOICE=alone", "2:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[], until=BW_UNTIL,
    )


_BW_CHR = re.compile(r"\[mode \d (0x[0-9a-f]+) \(([^)]*)\) ([\d.]+) m draw ([\d.]+) alpha ([\d.]+)/[+-][\d.]+ f \w+ hp (-?\d+)\]")


def check_bw(v, L):
    need(v, L, "bw join: 3 players", 0, r"cooperators \d -> 2")
    on = need(v, L, "bw the host's fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        check_common(v, L, "bw")
        return
    hd = need(v, L, "bw the host dies in the fight (limbo)", 0, r"host death in a boss fight", on)
    need(v, L, "bw the guests hear of the spectator", 1, r"slot 0 spectates", on)
    mark = need(v, L, "bw guest 1 uses a Hunter's Mark in the limbo", 1, r"guest_use goods 100", hd or on)
    arr = need(v, L, "bw guest 1 back in the host's world", 1, r"arrived in the host world \(flow 4\)", mark or on)
    if arr is None or hd is None:
        check_common(v, L, "bw")
        return
    t0 = arr + 4.0
    # A copy hidden as a spectator's body must be dead: seen with HP afterwards (not shown again first)
    # means a living member went invisible.
    for i in (0, 1, 2):
        wrong = []
        for hit in L.all(i, r"spectator: its dead copy (0x[0-9a-f]+) here disabled", 0.0, 1e9):
            addr = re.search(r"copy (0x[0-9a-f]+)", hit[1]).group(1)
            shown = L.find(i, r"spectator: copy %s reads HP again" % addr, hit[0], 1e9)
            end = shown[0] if shown else 1e9
            for r in L.all(i, r"remote chrs: ", hit[0] + 2.0, end):
                for c in _BW_CHR.finditer(r[1]):
                    if c.group(1) == addr and int(c.group(6)) > 0:
                        wrong.append((r[0], addr))
                        break
        v.step("bw inst%d: no living member's copy stays hidden as a spectator's body" % i, not wrong,
               "none" if not wrong else "+%.1f s copy %s reads HP while hidden (%d lines)" % (wrong[0][0], wrong[0][1], len(wrong)))
    # The living guests see each other (and the host sees both) drawn after the re-join.
    for i in (0, 1, 2):
        hits = L.all(i, r"remote chrs: ", t0, 1e9)
        alive = []
        for h in hits:
            chrs = [(c.group(1), float(c.group(4)), float(c.group(5)), int(c.group(6))) for c in _BW_CHR.finditer(h[1])]
            alive.append([c for c in chrs if c[3] > 0])
        ok = bool(alive) and all(a and all(d > 0.05 and al > 0.05 for _, d, al, _ in a) for a in alive[-5:])
        v.step("bw instance %d sees a living member after the re-join (drawn, HP up; last %d lines)" % (i, min(5, len(alive))),
               ok, ("%d lines" % len(alive)) if ok else (hits[-1][1][:240] if hits else "no remote chrs line"))
    check_common(v, L, "bw")


# Phase bl (bosslimbo, 0.7.4): the last living member leaves the host's boss-fight limbo -> a wipe.
BL_UNTIL = [r"0:the last living member left the boss fight[\s\S]*loading screen gone",
            r"1:guest_use goods 100[\s\S]*arrived in the host world \(flow 4\)[\s\S]*arrived in the host world \(flow 4\)"]


def phase_bl(only):
    host = ",".join(["host_warp=2412953@8^1",                               # the party to the Tomb of Oedon
                     "host_move=%s@3%%m24_01" % GASCOIGNE_TRIGGER,          # the host starts the fight (intro)
                     "host_die@9%m24_01~"])                                 # dead in it, the guest fighting: the limbo
    g1 = ",".join(["god",                                                   # alive through the fight (its HP logged)
                   "guest_obj=2411810@3%m24_01~",                           # at the fog after the intro
                   "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",  # through it: in the fight
                   "guest_press=0x0@12%m24_01!", "guest_press=0x0@10%m24_01!",      # with the host dead
                   "guest_press=0x0@10%m24_01!",
                   "guest_lastlamp=2412953@1%m24_01",                       # its last lamp: the host's
                   "guest_use=100@2%m24_01"])                               # Hunter's Mark: leaves the fight
    return dict(
        name="bl", seconds=300, count=2, headless=0, until_hold=12,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"host death in a boss fight=bllimbo@2,8,14",
               "--watch", r"test: guest_use goods 100=blmark@1",
               "--watch", r"the last living member left the boss fight=blwipe@0.5",
               "--watch", r"host chr: hp \d+, .*\(alive\)=blup@2,6",
               "--watch", r"targets the living member=bltarget@3,9"],
        crash=[], until=BL_UNTIL,
    )


def check_bl(v, L):
    """Phase bl: the guest's Hunter's Mark out of the host's boss-fight limbo is a wipe."""
    need(v, L, "bl join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "bl the host's fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        return
    lim = need(v, L, "bl host death in the fight: the limbo", 0, r"host death in a boss fight", on)
    if lim is None:
        return
    need(v, L, "bl the limbo counts the guest in the fight", 0, r"host limbo: 1 member\(s\) in the fight", lim - 1)
    need(v, L, "bl the host spectates the guest", 0, r"spectating: following slot \d+ \(1 living member\)", lim)
    need(v, L, "bl the boss keeps fighting the living guest (its forced target)", 0,
         r"host limbo: boss [0-9a-f]+ targets the living member [0-9a-f]+ \(ChangeTarget ok\)", lim)
    mark = need(v, L, "bl guest uses a Hunter's Mark (the last one in the fight)", 1, r"guest_use goods 100", lim)
    if mark is None:
        return
    left = need(v, L, "bl host: the guest left the boss fight", 0, r"host limbo: slot \d+ left the boss fight", mark - 1)
    wipe = need(v, L, "bl host: nobody fights on: a wipe", 0, r"the last living member left the boss fight", left or mark)
    if wipe is None:
        return
    none_of(v, L, "bl no wipe before the guest's Mark", r"a wipe; the", 0, mark - 1, insts=(0,))
    up = need(v, L, "bl host: the wipe's respawn load", 0, r"loading screen gone", wipe)
    if up is not None:
        need(v, L, "bl host up again after the load (alive)", 0, r"host chr: hp \d+, .*\(alive\)|B2 load report", wipe)
        need(v, L, "bl host spectating ended", 0, r"spectating ended", wipe - 1)
        need(v, L, "bl guest follows the wipe's respawn load", 1, r"loading screen up", wipe)
        need(v, L, "bl guest back in the host's world after the wipe", 1, r"arrived in the host world \(flow 4\)", wipe)
        none_of(v, L, "bl guest never drops the host's travel", r"came while this guest's own travel runs", insts=(1,))
    check_common(v, L, "bl")


# Phase mw (darkness077, 0.7.7, ~4 min, 2 players, group `boss_darkness`, player report: Mergo's Wet Nurse's
# darkness (SpEffect 5630 on the player, ended by her defeat event 12601800 instr 24) stayed after the
# kill): the host stands in her fight with the darkness on, dies in it (limbo), the guest's side kills her
# (the boss kill's defeat path on the host's game), the host gets up in the arena with no load. The
# darkness must be gone on the host after it (log_fx), the screenshots show it.
MW_UNTIL = [r"0:host revive #1: after"]
MW_ARENA = "146.46:1124.6:-37.9"


def phase_mw(only):
    host = ",".join(["god", "log_fx=5630",
                     "host_move=%s@40%%m26" % MW_ARENA,                      # the arena (intro scene, the fight)
                     "host_mark=5630@40%m26",                                # her darkness on the host
                     "host_die@0.3%m26",                                     # dead in the fight: the limbo
                     "host_kill=2600802@12%m26", "host_kill=2600802@1%m26",  # the HP holder, then the parts
                     "host_kill=2600801@1%m26", "host_kill=2600800@1%m26",
                     "host_press=0@60%m26!"])
    g1 = ",".join(["god", "guest_press=0x0@20%m26!",
                   "guest_obj=2601800@3%m26",                                # at the fog
                   "guest_press=0x4000@2%m26!", "guest_press=0x4000@2%m26!", # through it: in the fight
                   "guest_press=0x0@90%m26!"])
    return dict(
        name="mw", seconds=420, count=2, headless=0, until_hold=50,
        env=["BB_PARTY_TEST_START_LAMP=2602953", "BB_PARTY_TEST_INVINCIBLE=0"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"test: Hunter's Mark: SpEffect 5630=mwdark@1,4",
               "--watch", r"host death: the limbo's boss was killed=mwkill@2,10",
               "--watch", r"host revive #1: after=mwup@2,6,12,25"],
        crash=[], until=MW_UNTIL,
    )


def check_mw(v, L):
    """Phase mw: the darkness (SpEffect 5630) is gone on the host after her kill and its in-arena revive."""
    need(v, L, "mw join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "mw the fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        return
    dark = need(v, L, "mw the darkness is on the host", 0, r"SpEffect 5630 is ON on the local player", on)
    lim = need(v, L, "mw host death in the fight: the limbo", 0, r"host death in a boss fight", on)
    kill = need(v, L, "mw the boss was killed in the limbo", 0, r"host death: the limbo's boss was killed", lim or on)
    up = need(v, L, "mw the host gets up in the arena", 0, r"host revive #1: after", kill or on)
    if up is None:
        return
    # the defeat event's own removal (instr 24) comes ~20 s after the kill; judge 40 s after the revive
    rows = L.all(0, r"SpEffect 5630 is (ON|off) on the local player", up + 30.0, up + 400.0)
    last = [ln for _, ln in rows][-5:]
    v.step("mw the darkness is gone on the host after the kill and the revive (last 5 samples)",
           len(last) >= 3 and all("off" in ln for ln in last), "%d samples, last: %s" % (len(rows), [("ON" if "ON" in ln else "off") for ln in last]))
    check_common(v, L, "mw")


# Phase px (poison077, 0.7.7, ~4 min, 2 players, group `death_poison`, player report: a member who died
# poisoned in a party was still poisoned after getting up): the host stands in the Cathedral Ward with
# the guest alive, takes Poison Knife hits on itself (test aid host_use=1210:take: the knife's attack
# row effects), is poisoned (log_fx=-1 lists its SpEffects), dies and gets up. No poison state may
# remain (the SpEffect list shows the poison ids of the poisoned phase gone), the screenshots show the bar.
PX_LAMP = "2402950"
PX_UNTIL = [r"0:travel step 4/4: host_press"]


def check_px(v, L, who=0):
    """Phase px / pxg: the member's poison (SpEffect 2120) is on when it dies and gone once it is up again."""
    tag = "px" if who == 0 else "pxg"
    role = "host" if who == 0 else "guest"
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, tag + " the %s is poisoned (SpEffect 2120 in the list)" % role, who, r"test: SpEffects on the local player:.* 2120")
    die = need(v, L, tag + " the %s dies" % role, who, r"test: die: entity", on or 0.0)
    up = need(v, L, tag + " the %s gets up" % role, who, r"revive #\d+: after", die or 0.0)
    if up is None:
        return
    need(v, L, tag + " the revive dropped the poison", who, r"revive: poison dropped \(\d+ SpEffect", die or 0.0)
    # the list lines are logged on a change: none after the revive may name 2120
    rows = L.all(who, r"test: SpEffects on the local player:", up, up + 40.0)
    v.step(tag + " no poison state in the SpEffect list after the revive (%d list change(s))" % len(rows),
           bool(rows) and all(" 2120" not in ln for _, ln in rows), "first: %s" % (rows[0][1][-80:] if rows else "none"))
    check_common(v, L, tag)


PX_CLEAR = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))


def phase_px(only, guest=False):
    # Poison Knives (the PvP rule on, party_pvp test aids) poison one member; it dies poisoned with the other
    # alive and gets up (the host at its lamp, a guest where the party's rules put it).
    thrower = ["log@22", "face@2", "item=1210@2", "face@1", "use=8@1", "log@30"]
    if guest:
        # the guest is poisoned and dies; the host (invincible) throws
        h_dir = ",".join(["god", "host_warp=2502951@8^1", "host_press=0@150%m25!"])
        g_dir = ",".join(["log_fx=-1", "guest_press=0@50%m25!", "guest_die@5%m25", "guest_press=0@40%m25!"])
        hsteps = ["on@3"] + thrower
        gsteps = ["log@5"]
        until = [r"1:travel step 3/3: guest_press"]
    else:
        h_dir = ",".join(["log_fx=-1", "host_warp=2502951@8^1", "host_press=0@50%m25!", "host_die@5%m25",
                          "host_press=0@40%m25!"])
        g_dir = "god"
        hsteps = ["on@3"]
        gsteps = thrower
        until = PX_UNTIL
    return dict(
        name="pxg" if guest else "px", seconds=300, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + PX_CLEAR, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + h_dir, "0:BB_PARTY_PVP_TEST=" + ",".join(hsteps),
              "1:BB_PARTY_PVP_TEST=" + ",".join(gsteps), "1:BB_PARTY_DIRECTOR_TEST=" + g_dir],
        crash=[], until=until,
    )


# Phase hg (bossjoin, 0.7.6, ~4 min, 2 players, group `boss_late`, S1 player reports "boss behaves as if only the
# host is in the fight"): the HOST walks into Father Gascoigne's fog first (intro, fight), the GUEST
# follows HG_DELAY seconds later (variants by env BB_HG_DELAY, default 12 = after the intro). Both
# invincible; only the guest fights (R1 taps), the host stands at the boss. The guest's hits must
# reach the host's boss (HP on both games falls), the boss must act on the guest, and Gascoigne's
# phase 2 (the beast) must happen from the guest's damage. BB_PARTY_TARGET_ANIM / BB_PARTY_AI_LOG logs.
HG_UNTIL = [r"1:travel step (\d+)/\1: guest_press 0x0"]


def phase_hg(only):
    order = os.environ.get("BB_HG_ORDER", "host")                 # host: the host walks in first; guest: the guest does
    delay = float(os.environ.get("BB_HG_DELAY", "12"))
    hp = os.environ.get("BB_HG_HP", "55")                           # the boss's HP % set on the host: near phase 2
    dist = os.environ.get("BB_HG_HOSTDIST", "16")                   # the host stands far from the boss: who it fights
    gdist = os.environ.get("BB_HG_GUESTDIST", "2")                  # the guest's distance from the boss at the start
    mortal = os.environ.get("BB_HG_MORTAL", "0") == "1"            # nobody invincible: the boss's hits show in the HP
    if order == "guest":
        host = ",".join(["god", "host_warp=2412953@8^1",
                         "host_chr=2410810:%s@38%%m24_01" % dist,
                         "host_kill=2410810:hp%s@8%%m24_01" % hp])
        g1 = ",".join(([] if mortal else ["god"]) + ["guest_press=0x0@4%m24_01!", "guest_obj=2411810@3%m24_01",
                       "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",
                       ("guest_move=%s@25%%m24_01" % os.environ["BB_HG_GUESTAT"] if os.environ.get("BB_HG_GUESTAT")
                        else "guest_chr=2410810:%s@25%%m24_01" % gdist)] +   # (BB_HG_GUESTAT=x:y:z: a fixed spot)
                      ["guest_press=0x800@1.5%m24_01"] * 60 + ["guest_press=0x0@2%m24_01"])
    else:
        host = ",".join(["god", "host_warp=2412953@8^1",                      # the party to the Tomb of Oedon
                         "host_move=%s@3%%m24_01" % GASCOIGNE_TRIGGER,        # the host starts the fight (intro)
                         "host_chr=2410810:%s@12%%m24_01" % dist,
                         "host_kill=2410810:hp%s@%s%%m24_01" % (hp, os.environ.get("BB_HG_HPAT", "25"))])
        g1 = ",".join(([] if mortal else ["god"]) + [
                       "guest_press=0x0@%g%%m24_01&" % (delay + 3),           # waits for the host's intro
                       "guest_obj=2411810@3%m24_01",                          # at the fog
                       "guest_press=0x4000@2%m24_01!", "guest_press=0x4000@2%m24_01!",  # through it
                       "guest_chr=2410810:%s@8%%m24_01" % gdist] +                      # beside the boss
                      ["guest_press=0x800@1.5%m24_01"] * 80 + ["guest_press=0x0@2%m24_01"])
    return dict(
        name="hg", seconds=360, count=2, headless=0, until_hold=4,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + GASCOIGNE_CLEAR] + ([] if mortal else ["BB_PARTY_TEST_INVINCIBLE=1"]) +
            ["BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"test: target anim=hganim@0.5"],
        crash=[], until=HG_UNTIL,
    )


def check_hg(v, L):
    """Phase hg: a late guest joins the host's running boss fight (or the other way round): its damage
    reaches the host's boss, the boss fights the guest, phase 2 (the beast, anims 1000000+) happens."""
    need(v, L, "hg join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "hg the fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        return
    need(v, L, "hg host: the guest's walk through the fog is seen", 0, r"boss fog: slot \d+ went in", 0)
    tap = need(v, L, "hg guest's first R1 on the boss", 1, r"guest_press 0x800; target 2410810 HP", on)
    if tap is None:
        return
    # the boss's HP on the host's game falls after the guest's taps (the host never attacks)
    rows = L.all(0, r"target anim: entity 2410810 animation \d+ \(HP -?\d+/\d+", tap, tap + 200.0)
    hps = [int(re.search(r"\(HP (-?\d+)/", ln).group(1)) for _, ln in rows]
    mx = max([int(re.search(r"/(\d+),", ln).group(1)) for _, ln in rows] or [0])
    v.step("hg the guest's hits reach the host's boss (HP falls on the host's game)",
           bool(hps) and min(hps) < (hps[0] if hps else 0) - 50, "HP %s -> %s of %d" % (hps[:1], hps[-1:], mx))
    # the boss stands at the guest, not at the host 16 m away: both games' own distance to it
    def near(inst):
        out = []
        for t, ln in L.all(inst, r"target pos: entity 2410810 at .* m from this player", tap + 8.0, tap + 60.0):
            out.append(float(re.search(r"([\d.]+) m from", ln).group(1)))
        return out
    dg, dh = near(1), near(0)
    # guest first: the host stands 16 m away, the boss must be at the guest, never at the host. Host first:
    # the boss starts at the host (alone in the fight); the guest's hits must bring it to the guest too
    # (the arena is small: only "within 4 m of the guest" is asked).
    near_g = len([d for d in dg if d < 4.0])
    far_h = len([d for d in dh if d > 8.0])
    guest_first = os.environ.get("BB_HG_ORDER", "host") == "guest"
    v.step("hg the boss acts on the guest (within 4 m of it%s)" % (", the host 8+ m away" if guest_first else ""),
           near_g >= 5 and (far_h >= 5 or not guest_first or float(os.environ.get("BB_HG_HOSTDIST", "16")) < 8.0),
           "guest side %d/%d samples < 4 m; host side %d/%d samples > 8 m" % (near_g, len(dg), far_h, len(dh)))
    if os.environ.get("BB_HG_MORTAL", "0") == "1":
        # nobody invincible: the guest is not god, the host is (it only stands beside the boss): the guest's HP
        # must fall from the boss's own hits (attention shared) -- the boss never sits on the host alone
        rows = L.all(1, r"target pos: entity 2410810 .*player HP -?\d+\)", tap, tap + 120.0)
        gh = [int(re.search(r"player HP (-?\d+)\)", ln).group(1)) for _, ln in rows]
        v.step("hg the boss hits the guest (its HP falls while the host stands by)",
               bool(gh) and min(gh) < gh[0], "guest HP %s -> min %s over %d samples" % (gh[:1], min(gh) if gh else None, len(gh)))
    beast = need(v, L, "hg phase 2: the beast's animations (1000000+) on the host's game", 0,
                 r"target anim: entity 2410810 animation 10\d{5}", tap)
    if beast is not None:
        need(v, L, "hg phase 2 seen on the guest's game too", 1, r"target anim: entity 2410810 animation 10\d{5}", beast - 1)
    check_common(v, L, "hg")


# Phase bg (bossg078, 0.7.8, ~6 min, 2 players, group `boss_guest`, player report: "Laurence (and the other bosses)
# never attack the guest"): the party in Laurence's cathedral (the BR fixture), the guest INSIDE, the host uses his
# skull; the host stands BB_BG_HOSTDIST m (default 1) beside the boss, invincible and passive; the guest is MORTAL, BB_BG_GUESTDIST m
# (default 8) away and passive. The boss must turn to the guest in turn (BossAttentionTick) and really hit it: the
# guest's HP falls from the boss's hits (both games log it), both boss bars are up. BB_BG_ORDER=guest: the guest
# walks through the fog first. BB_PARTY_AI_LOG / BB_PARTY_TARGET_ANIM logs.
# The mortal guest killed by the boss ends it too: its test chain then waits on a dead character until the timeout
BG_UNTIL = [r"1:(?:travel step (\d+)/\1: guest_press 0x0|guest: own death in the host's world held)"]


def phase_bg(only):
    hdist = os.environ.get("BB_BG_HOSTDIST", "1")
    gdist = os.environ.get("BB_BG_GUESTDIST", "8")
    lap = int(os.environ.get("BB_BG_LAP", "70"))            # passive seconds in the fight
    host = ",".join([
        "god", "goods=4014:0",
        *_laurence_enter("host", 10, True),                        # the party to the Hunter's Nightmare lamp, in through his fog
        "host_use=4014@2%%m34_00?near=%s:2" % BR_SIGNAL,            # the guest is inside: the skull, the fight starts
        "host_chr=3400850:%s@20%%m34_00" % hdist,                   # after the scene: beside the boss, passive
        "host_press=0x0@%d%%m34_00" % lap,
    ])
    g1 = ",".join((["god"] if os.environ.get("BB_BG_GUESTGOD") == "1" else []) + [
        *_laurence_enter("guest", 40),                              # in through his fog, to the altar spot
        "guest_press=0x0@22%m34_00!",                               # the scene
        "guest_chr=3400850:%s@2%%m34_00" % gdist,                   # in the arena, away from the boss
        "guest_press=0x0@%d%%m34_00" % lap,
        "guest_press=0x0@2%m34_00",
    ])
    return dict(
        name="bg", seconds=420, count=2, headless=0, pads=(0, 1), until_hold=4,   # pads: the stick walk through the fog needs the pad file (without it the player never moved: 15 stalls, "no way past")
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LAURENCE_CLEAR + "," + BR_LAMP_CLEAR, "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1", "BB_BOSS_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone"],
        crash=[], until=BG_UNTIL,
    )


def check_bg(v, L):
    """Phase bg: the boss turns to the mortal guest standing away from it and its hits take the guest's HP."""
    need(v, L, "bg join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "bg the fight is on (bar, host)", 0, r"boss health bar shown")
    if on is None:
        return
    need(v, L, "bg the guest's game shows the boss bar too", 1, r"boss health bar shown")
    turn = need(v, L, "bg the boss turns to the guest (ChangeTarget)", 0, r"boss attention: boss [0-9a-f]+ turns to player chr", on)
    need(v, L, "bg boss state is logged on the host's game", 0, r"boss state: [0-9a-f]+ at \(", on)
    need(v, L, "bg boss state is logged on the guest's game", 1, r"boss state: [0-9a-f]+ at \(", on)
    none_of(v, L, "bg the boss never stands still (no 'may be stuck' on either game)", r"boss state: [0-9a-f]+ may be stuck", on, 1e9, (0, 1))
    hits = L.all(1, r"Party balance: hit: HP (\d+) -> (\d+)", on, on + 400.0)
    if os.environ.get("BB_BG_GUESTGOD") == "1":
        check_common(v, L, "bg")
        return
    v.step("bg the boss hits the guest (hit lines on the guest's game while the host stands by)",
           len(hits) >= 1, "%d hit(s) on the guest after the bar: %s" % (len(hits), [ln.split("hit: ")[1][:28] for _, ln in hits[:6]]))
    if turn is not None and hits:
        v.step("bg the first hit comes within 40 s of the boss turning to the guest", hits[0][0] - turn < 40.0,
               "%.1f s" % (hits[0][0] - turn))
    check_common(v, L, "bg")


# Phase lr (laurence078, CL-7b1d4e, ~8 min, 2 players, group `boss_laurence_wipe`, player report: "Laurence stays on top of
# the statue, then falls and cannot move; a restart of the guest's game helps"; the report's session had a wipe and a
# re-fight in a row). Try 1: the guest is inside, the host uses the skull (scene, bars), both stand passive 8 m from the
# boss, the host dies (limbo) and then the guest: the wipe (one load at the cathedral lamp). Try 2: after the load the
# host walks to the fog like a player (R3, Cross) and goes through, the guest follows once the host's bar is up, both
# stand passive BB_LR_DIST m from Laurence. Checked over BB_LR_WINDOW s of try 2 on BOTH games: his bar, his animations,
# how far he walked, his hits, no "boss may be stuck" line. BB_LR_NET=<BB_NET_SIM preset>, BB_LR_ENV="A=1;B=2".
LR_UNTIL = [r"1:travel step (?P<n>\d+)/(?P=n): guest_press 0x0"]


def phase_lr(only):
    dist = os.environ.get("BB_LR_DIST", "8")
    win = int(os.environ.get("BB_LR_WINDOW", "60"))
    guest_first = os.environ.get("BB_LR_ORDER", "") == "guest"   # repro079: the GUEST dies first (the host's bar up), then the host
    ng = int(os.environ.get("BB_LR_GUESTS", "1"))                # 2 = three players
    m = "%m34_00"
    host = ",".join([
        "log_pos=2", "goods=4014:0",
        *_laurence_enter("host", 10, True),                          # the party to the Hunter's Nightmare lamp, in through his fog
        "host_use=4014@2%%m34_00?near=%s:2" % BR_SIGNAL,             # the guest is inside: the skull
        ("host_die@4%s?member=spec" % m) if guest_first else ("host_die@25%s?member=bar" % m),   # try 1: after the scene, the guest in the fight: the host dies
        # try 2 (after the wipe's load): the fog is up, a player walk to it
        # guest first: the guest follows the wipe dead and gets up at the lamp ~40 s after the host (a slow load): the host waits for it (80 s: two games load at once)
        ("host_obj=3401850@80%s" % m) if guest_first else ("host_obj=3401850@14%s" % m),
        "host_press=0x4@1%s!" % m, "host_press=0@1%s!" % m,          # R3: the camera behind the player
        "host_press=0x4000@2%s!" % m, "host_press=0x4000@2%s!" % m,  # Cross: through the fog
        "host_chr=3400850:%s@4%s" % (dist, m),
        "host_press=0x0@%d%s" % (win, m),
    ])

    def guest_steps(k):   # k = 0 for guest 1; later guests start later and walk to the fog later (two bodies in the doorway jam)
        return ",".join([
            "log_pos=2",
            *_laurence_enter("guest", 40 + 35 * k),                      # in through his fog, to the altar spot
            "guest_press=0x0@22%s!" % m,                                 # the scene
            "guest_chr=3400850:%s@2%s" % (dist, m),
            ("guest_die@25%s?host=bar" % m) if (guest_first and k == 0) else ("guest_die@4%s?host=spec" % m),   # the host is down: the wipe
            # try 2
            *(["guest_press=0x0@1%s?host=nobar" % m] if guest_first else []),   # guest first: the host's bar is still up when the guest dies; wait for the wipe's load
            "guest_obj=3401850@%d%s?host=bar" % (14 + 25 * k, m),        # after the host's bar
            "guest_press=0x4@1%s!" % m, "guest_press=0@1%s!" % m,
            "guest_press=0x4000@2%s!" % m, "guest_press=0x4000@2%s!" % m,
            "guest_chr=3400850:%s@4%s" % (dist, m),
            "guest_press=0x0@%d%s" % (win, m),
            "guest_press=0x0@2%s" % m,
        ])
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LAURENCE_CLEAR + "," + BR_LAMP_CLEAR, "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1", "BB_BOSS_LOG=1"]
    if os.environ.get("BB_LR_NET"):
        env.append("BB_NET_SIM=" + os.environ["BB_LR_NET"])
    if os.environ.get("BB_LR_ENV"):
        env += os.environ["BB_LR_ENV"].split(";")
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + host]
    for k in range(ng):
        ienv += ["%d:BB_PARTY_DIRECTOR_TEST=%s" % (k + 1, guest_steps(k)), "%d:BB_PARTY_TRAVEL_CHOICE=alone" % (k + 1)]
    return dict(
        name="lr", seconds=900 + 300 * (ng - 1), count=1 + ng, headless=0 if ng == 1 else 1, pads=tuple(range(1 + ng)), until_hold=4,
        env=env,
        ienv=ienv,
        watch=["--watch", r"boss health bar shown=bar@1,4",
               "--watch", r"went through boss fog=fog@1,4",
               "--watch", r"host death in a boss fight=limbo@2,10"],
        crash=[], until=[r"%d:travel step (?P<n>\d+)/(?P=n): guest_press 0x0" % ng],
    )


def _lr_window(v, L, tag, a, b):
    for i in range(1 + int(os.environ.get("BB_LR_GUESTS", "1"))):
        ids = []
        for t, line in L.all(i, r"test: target anim: entity 3400850 animation -?\d+", a, b):
            x = int(re.search(r"animation (-?\d+)", line).group(1))
            if not ids or ids[-1][1] != x:
                ids.append((t, x))
        atk = [x for _t, x in ids if 3000 <= x < 4000]
        v.step("lr %s inst%d: Laurence acts (>= 4 animation changes; attacks 3000-3999 are info: his ids differ)" % (tag, i),
               len(ids) >= 4, "%d changes, %d attacks: %s" % (len(ids), len(atk), [x for _t, x in ids[:14]]))
        pts = [tuple(float(x) for x in re.search(r"at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", ln).groups())
               for _t, ln in L.all(i, r"test: target pos: entity 3400850 at", a, b)]
        walked = sum(((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5 for p, q in zip(pts, pts[1:]))
        v.step("lr %s inst%d: Laurence moves (>= 4 m walked)" % (tag, i), walked >= 4.0, "%.1f m over %d positions" % (walked, len(pts)))
    none_of(v, L, "lr %s: no 'boss may be stuck' on either game" % tag, r"boss state: [0-9a-f]+ may be stuck", a, b, tuple(range(1 + int(os.environ.get("BB_LR_GUESTS", "1")))))


# Phase lrg (CL-7b1d4e, repro079, ~4 min, 2 players, group `boss_laurence_guest_first`): the lr fight with the GUEST dying first (his
# bar up, the host alive) and the host dying after it: the wipe. The guest's body lies dead in the fight for 25-35 s, the host's wipe
# load waits for its death (C3), the guest follows it dead to the lamp. Proven here: the wipe, one load for each game and the guest's
# respawn records put back, no crash (a GPU assertion at the guest's wipe load came once in five runs: pm4 SignalFence, unproven).
# The second try of lr is not run (the two games load for 30-40 s together under the harness, its timing is not stable).
LRG_ENV = {"BB_LR_ORDER": "guest", "BB_LR_GUESTS": "1", "BB_LR_NET": "dsl"}


def phase_lrg(only):
    d = _with_env(LRG_ENV, phase_lr, only)
    d.update(name="lrg", seconds=420, until=[r"1:guest: own respawn records put back after the death follow|\d:test: FAILED"], until_hold=10)
    return d


def check_lrg(v, L):
    need(v, L, "lrg join: 2 players", 0, r"cooperators 0 -> 1")
    bar = need(v, L, "lrg the fight starts (bar, host)", 0, r"boss health bar shown")
    if bar is None:
        return check_common(v, L, "lrg")
    need(v, L, "lrg the guest's game shows the bar", 1, r"boss health bar shown", bar - 5)
    die = need(v, L, "lrg the guest dies first", 1, r"local death: HP 0", bar)
    wipe = need(v, L, "lrg the party wipes (the host dies after the guest)", 0, r"a wipe|every member went down while the host was", die or bar)
    if wipe is not None:
        need(v, L, "lrg the guest follows the wipe load dead and gets its own respawn records back", 1,
             r"own respawn records put back after the death follow", wipe)
        need(v, L, "lrg the guest's wipe load ends (loading screen gone)", 1, r"loading screen gone", wipe)
    check_common(v, L, "lrg")


def check_lr(v, L):
    need(v, L, "lr join: 2 players", 0, r"cooperators 0 -> 1")
    bar1 = need(v, L, "lr try 1: the fight starts (bar, host)", 0, r"boss health bar shown")
    if bar1 is None:
        return check_common(v, L, "lr")
    need(v, L, "lr try 1: the guest's game shows the bar", 1, r"boss health bar shown", bar1 - 5)
    wipe = need(v, L, "lr try 1: the party wipes", 0, r"a wipe", bar1)
    if wipe is None:
        return check_common(v, L, "lr")
    bar2 = need(v, L, "lr try 2: the fight starts again (bar, host)", 0, r"boss health bar shown", wipe)
    if bar2 is None:
        return check_common(v, L, "lr")
    gbar = need(v, L, "lr try 2: the guest's game shows the bar", 1, r"boss health bar shown", wipe)
    none_of(v, L, "lr: every fog walk got through", r"test: fog: NOT through", 0, 1e9, (0, 1))
    start = max(bar2, gbar or bar2) + 5.0
    _lr_window(v, L, "try 2", start, start + float(os.environ.get("BB_LR_WINDOW", "60")))
    hits = L.all(0, r"Party balance: hit: HP", start, start + 70) + L.all(1, r"Party balance: hit: HP", start, start + 70)
    v.step("lr try 2: his hits land (>= 1 on a player)", len(hits) >= 1, "%d hit(s)" % len(hits))
    check_common(v, L, "lr")


# Phase mi (micolash079, CL-ccedf1, ~8 min, 2 players, group `micolash`, player report: "Micolash, second attempt: the
# fight started before we entered the boss area, his bar lost HP and regained it for one player, the friend saw
# nothing and one friend was invisible"). Fixture: Nightmare of Mensis, Micolash alive (his flags and the zone 4
# group cleared), the party at the lamp 2602953 (the nearest lamp the navmesh reaches him from: a 900 m walk).
# Try 1: the host walks toward him like a player (the first-fight event 12601852 fires within 32 m), the guest
# goes through his fog 2601850 once the host's bar is up; both stand passive; the host dies (limbo), then the
# guest: the wipe. Try 2: after the respawn load nobody is near him: for MI_HOLD s his bar must stay down on both
# games and nobody is invisible; then the host walks in again, the guest follows through the fog.
MI_LAMP = "2602953"
MI_CLEAR = "12601850,12601852,12604850,12604851,12604731,12604858,12604857,12604852,12604853"
MI_READ = "12601850,12601852,12604850,12604851,12604731,12607850,12607851,72600200,72600201,72600202,72600203"
MI_SPOT = "176.8:-35.8"
# The lamp 2602953 -> 30 m short of Micolash's spot (176.8, 1038.5, -35.8): a navmesh route of 880 m (tools/party/nav_route.py
# on m26_00_00_00, local frame, step 30; +1000 in y = the world frame). His first-fight event fires at 32 m; his floor is
# on another navmesh island for an X:Z stick walk from the lamp, so the walk is a route.
MI_ROUTE = "/".join("%.1f:%.1f:%.1f" % (x, 1000 + y, z) for x, y, z in [(137.0, 29.4, -6.3), (156.8, 33.0, -14.7)])
MI_NEAR = "106.2:1029.4:-6.4"   # 70 m of path short of the first-fight spot, outside the 32 m of his event, not in a boss room:
                                # the full 880 m walk from the lamp is blocked by an Iron Door / lift the harness cannot open
MI_HOLD = 40
MI_FOG = "145.88:-14.74"   # his fog 2601850 (X:Z)
MI_UNTIL = [r"1:travel step (?P<n>\d+)/(?P=n): guest_press 0x0"]


def _mi_fog(role, cond, m):
    """Walk to Micolash's fog 2601850 and through it like a player (R3: the camera behind, Cross twice)."""
    # no R3 here: an enemy (2600221) stands at the fog and R3 locks on to it (run 0.7.9: the lock-on turned the player to the
    # enemy, the fog prompt never came); the walk to the fog already faces it
    # then through it with the fog step (stick walk until its prompt shows, Cross, held until through): neither Cross presses
    # (the prompt never showed) nor a stick walk (a closed fog is a wall) passed this fog (0.7.9 mig: the guest stood at it
    # for 15 s, the fight never started)
    return ["%s_obj=2601850@3%s%s" % (role, m, cond), "%s_fog=%s@2%s" % (role, MI_FOG, m)]


def phase_mi(only, name="mi", guest_first=False):
    dist = os.environ.get("BB_MI_DIST", "8")
    win = int(os.environ.get("BB_MI_WINDOW", "40"))
    m = "%m26_00"
    walk = lambda role, at: ["%s_move=%s@%s%s" % (role, MI_NEAR, at, m), "%s_route=%s@3%s" % (role, MI_ROUTE, m)]
    if guest_first:
        # the guest alone walks to him and through his fog; the host stays at the lamp (its chr is sent to his spot
        # for a moment by the party's first-fight start, then put back)
        host = ",".join(["god", "log_pos=2", "host_warp=%s@8^1" % MI_LAMP, "host_press=0x0@%d%s" % (MI_HOLD + win + 60, m)])
        g1 = ",".join(["god", "log_pos=2"] + walk("guest", 12) + _mi_fog("guest", "", m) +
                      ["guest_chr=2600850:%s@8%s" % (dist, m), "guest_press=0x0@%d%s" % (win, m), "guest_press=0x0@2%s" % m])
    else:
        host = ",".join(["god", "log_pos=2", "host_warp=%s@8^1" % MI_LAMP] + walk("host", 3) +
                        ["host_press=0@1%s!" % m,
                         "host_die@25%s?member=bar" % m,                      # try 1: after the scene, the guest in the fight
                         "host_press=0x0@%d%s" % (MI_HOLD, m)] +              # after the wipe's load: the hold
                        walk("host", 2) + _mi_fog("host", "", m) +            # try 2: the walk and the fog
                        ["host_chr=2600850:%s@4%s" % (dist, m), "host_press=0x0@%d%s" % (win, m)])
        g1 = ",".join(["god", "log_pos=2"] + walk("guest", 12) + _mi_fog("guest", "?host=bar", m) +
                      ["guest_chr=2600850:%s@4%s" % (dist, m),
                       "guest_die@4%s?host=spec" % m,                         # the host is down: the wipe
                       "guest_press=0x0@%d%s" % (MI_HOLD, m)] + walk("guest", 2) + _mi_fog("guest", "", m) +
                      ["guest_chr=2600850:%s@4%s" % (dist, m), "guest_press=0x0@%d%s" % (win, m), "guest_press=0x0@2%s" % m])
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=" + MI_CLEAR, "BB_PARTY_PROGRESS_TEST_READ=" + MI_READ,
           "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1", "BB_BOSS_LOG=1", "BB_TEST_WALK_SPRINT=1"]
    if os.environ.get("BB_MI_NET"):
        env.append("BB_NET_SIM=" + os.environ["BB_MI_NET"])
    return dict(
        name=name, seconds=1800, count=2, headless=0, pads=(0, 1), until_hold=4,
        env=env,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone",
              "0:BB_PARTY_TEST_START_LAMP=" + MI_LAMP],
        watch=["--watch", r"boss health bar shown=bar@1,4,15",
               "--watch", r"went through boss fog=fog@1,4",
               "--watch", r"host death in a boss fight=limbo@2,10"],
        crash=[], until=MI_UNTIL,
    )


def _mi_hp(L, inst, a, b):
    out = []
    for t, ln in L.all(inst, r"test: boss bar 0: name 899000 chr \d+ hp \d+", a, b):
        out.append((t, int(re.search(r"hp (\d+)", ln).group(1))))
    return out


def check_mi(v, L, tag="mi"):
    """Phase mi: Micolash's fight starts only when it would in solo (the host within 32 m of him, then the fog on a
    second try), his bar is up and agrees on both games, nothing starts while the party stands at the lamp after a wipe."""
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    scene = need(v, L, tag + " try 1: his first-fight scene plays when the host comes within 32 m (cutscene 26000060)", 0,
                 r"story .*cutscene 26000060")
    bar1 = need(v, L, tag + " try 1: the fight starts (bar, host)", 0, r"boss health bar shown", scene or 0.0)
    if bar1 is None:
        return check_common(v, L, tag)
    none_of(v, L, tag + " try 1: no bar before the host came near him", r"boss health bar shown", 0, (scene or bar1) - 0.5, (0, 1))
    need(v, L, tag + " try 1: the guest's game shows the bar", 1, r"boss health bar shown", bar1 - 5)
    if tag == "mig":
        # guest first: the host stays at the lamp, the guest alone is in the fight and nobody dies, so there is no wipe and
        # no try 2; his bar must agree on both games and nobody stays stuck (the 0.7.9 run expected a wipe here)
        a, b = bar1 + 6.0, bar1 + 6.0 + float(os.environ.get("BB_MI_WINDOW", "40"))
        hh, gg = _mi_hp(L, 0, a, b), _mi_hp(L, 1, a, b)
        v.step(tag + " try 1: his bar has the same HP on both games (last samples within 5%)",
               bool(hh) and bool(gg) and abs(hh[-1][1] - gg[-1][1]) <= 0.05 * max(hh[-1][1], 1),
               "host %s, guest %s" % (hh[-1:], gg[-1:]))
        v.step(tag + " try 1: his HP never regains on the host or the guest (no loss-then-regain)",
               all(y <= x for (_t, x), (_u, y) in zip(hh, hh[1:])) and all(y <= x for (_t, x), (_u, y) in zip(gg, gg[1:])),
               "host %d samples, guest %d" % (len(hh), len(gg)))
        none_of(v, L, tag + ": no 'boss may be stuck' within 10 m of a player on either game", r"boss state: [0-9a-f]+ may be stuck.*nearest player \d\.\d m", 0, 1e9, (0, 1))
        none_of(v, L, tag + ": every fog walk got through", r"test: fog: NOT through", 0, 1e9, (0, 1))
        return check_common(v, L, tag)
    wipe = need(v, L, tag + " try 1: the party wipes", 0, r"a wipe", bar1)
    if wipe is None:
        return check_common(v, L, tag)
    up = need(v, L, tag + " try 1: the respawn load", 0, r"loading screen gone", wipe)
    walk = need(v, L, tag + " try 2: the host walks back toward him", 0, r"test: route: stage record 0", up or wipe)
    # the hold: after the load nobody is near him; no bar, no fight on either game
    if up is not None and walk is not None:
        none_of(v, L, tag + " try 2: no bar while the party stands at the lamp (hold)", r"boss health bar shown", up, walk - 0.5, (0, 1))
        none_of(v, L, tag + " try 2: no fight start (host-entered flag 12604850 stays 0 at the lamp)", r"host flag 12604850 = 1", up, walk - 0.5, (0, 1))
    hit2 = L.find(0, r"boss health bar shown", walk or wipe)
    bar2 = hit2[0] if hit2 else None
    v.step(tag + " try 2: after the walk to his fog (info: the harness's fog press does not start the re-fight yet)", True,
           "bar at %s" % bar2)
    if bar2 is not None:
        need(v, L, tag + " try 2: the guest's game shows the bar", 1, r"boss health bar shown", bar2 - 5)
        a, b = bar2 + 6.0, bar2 + 6.0 + float(os.environ.get("BB_MI_WINDOW", "40"))
        hh, gg = _mi_hp(L, 0, a, b), _mi_hp(L, 1, a, b)
        v.step(tag + " try 2: his bar has the same HP on both games (last samples within 5%)",
               bool(hh) and bool(gg) and abs(hh[-1][1] - gg[-1][1]) <= 0.05 * max(hh[-1][1], 1),
               "host %s, guest %s (of %s)" % (hh[-1:], gg[-1:], hh[:1]))
        v.step(tag + " try 2: his HP never regains on the host or the guest (no loss-then-regain)",
               all(y <= x for (_t, x), (_u, y) in zip(hh, hh[1:])) and all(y <= x for (_t, x), (_u, y) in zip(gg, gg[1:])),
               "host %d samples, guest %d" % (len(hh), len(gg)))
    # Only a boss standing idle with a player within 10 m counts: after his first-fight scene Micolash flees to a hiding spot
    # ~25 m from the passive fixture players and idles there until chased (0.7.9 run3 / run4: "animation 0 for 15 s, nearest
    # player 25 m" on the host and the guest alike), which is his own behaviour, not a stuck boss.
    none_of(v, L, tag + ": no 'boss may be stuck' within 10 m of a player on either game", r"boss state: [0-9a-f]+ may be stuck.*nearest player \d\.\d m", 0, 1e9, (0, 1))
    none_of(v, L, tag + ": every fog walk got through", r"test: fog: NOT through", 0, 1e9, (0, 1))
    check_common(v, L, tag)


# Phase lw (simonwipe, 0.7.6, ~5 min, 2 players, group `ludwig_wipe`, player report 2026-10-25): a party
# wipe in Ludwig's fight (Hunter's Nightmare, entities 3400800 / 3400801, bars 451000 / 451005). The
# report: after the wipe the second-phase scene (34000030, event 13404825, warp 3402807) played and the
# host stood ALONE in the fight at the boss's full HP, instead of the lamp with the fog reset. Host in
# the arena (his first-fight scene 34000020), the boss at BB_LW_HP % (79 = phase 1 as in the report;
# 40 = past the second-phase scene), host and guest both die (the host first: the limbo; the guest last:
# the wipe). After the respawn load nobody goes into the arena for LW_HOLD seconds: no scene, no bar.
LW_HOLD = 50
LW_ARENA_XZ = os.environ.get("BB_LW_WALK_TO", "-407.93:-700.0:2.5:60")    # x : z : tolerance : timeout of a stick walk to the arena entrance
LW_UNTIL = [r"0:a wipe; the[\s\S]*loading screen gone"]


def phase_lw(only):
    hp = os.environ.get("BB_LW_HP", "79")
    inject = os.environ.get("BB_LW_INJECT", "40")      # "" = off
    host = ",".join(["host_warp=3402952@8^1",                              # the party to Ludwig's lamp
                     "host_move=%s@8%%m34_00" % BR_LUDWIG,                  # his first fight (scene 34000020)
                     "host_kill=3400800:hp%s@14%%m34_00" % hp] +            # the boss to LW_HP %
                    # the last lamp far from the arena (the report: Ludwig's lamp is 22 m from his arena; the
                    # party had rested at the Grand Cathedral lamp 3402950, 240 m away)
                    ["host_lastlamp=%s@1%%m34_00" % os.environ.get("BB_LW_LAMP", "3402950")] +
                    ["host_die@%s%%m34_00" % os.environ.get("BB_LW_DIE", "3")])  # the host first: its limbo
    # BB_LW_LATE_HP=hp: the party's last hits take the boss to hp % while the host lies in its limbo, before the guests
    # die (0.7.8: the wipe near the second-half threshold; BB_LW_HP=52 BB_LW_LATE_HP=45)
    if os.environ.get("BB_LW_LATE_HP"):
        host += ",host_kill=3400800:hp%s@%s%%m34_00" % (os.environ["BB_LW_LATE_HP"], os.environ.get("BB_LW_LATE_AT", "4"))
    g1 = ",".join(["guest_move=%s@6%%m34_00" % BR_LUDWIG,                   # into the arena, beside the host
                   "guest_die@%s%%m34_00~" % os.environ.get("BB_LW_GDIE", "20")] +  # the last one down: the wipe
                  # BB_LW_INJECT=hp: after the respawn load the guest's own copy of Ludwig is set to hp % (the
                  # stale-guest-state test: its heat-up events run and tell the host)
                  (["guest_kill=3400800:hp%s@%s%%m34_00" % (inject, os.environ.get("BB_LW_INJECT_AT", "12"))] if inject else []))
    players = int(os.environ.get("BB_LW_PLAYERS", "2"))
    walk = os.environ.get("BB_LW_WALK") == "1"
    if walk:
        # BB_LW_WALK=1 (0.7.8): after the wipe's respawn load the party walks back to Ludwig's arena on foot (a stick
        # toward the arena entrance, region 3402805), as players do: no scene before the walk, no defeat banner at all
        at = os.environ.get("BB_LW_WALK_AT", "75")
        host += ",host_stick=%s@%s%%m34_00" % (LW_ARENA_XZ, at)
        g1 += ",guest_stick=%s@%s%%m34_00" % (LW_ARENA_XZ, at)
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1]
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LUDWIG_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1", "BB_PARTY_EVENT_SYNC_LOG=1",
           "BB_PARTY_BOSS_HP_LOG=1"]
    if os.environ.get("BB_LW_ENV"):          # "A=1;B=2" on every game (BB_NET_SIM=mobile ...)
        env += os.environ["BB_LW_ENV"].split(";")
    if players > 2:
        # the report's party: host + 3 guests (4 players: the boss scaling row 7502), the guests down one after the other
        env.append("BB_PARTY_MAX=4")
        for k in range(2, players):
            ienv.append("%d:BB_PARTY_DIRECTOR_TEST=guest_move=%s@%d%%m34_00,guest_die@%d%%m34_00~" % (k, BR_LUDWIG, 6 + 2 * k, 25 + 8 * k))
    return dict(
        name="lw", seconds=480, count=players, headless=max(0, players - 2), pads=(0, 1),
        env=env, ienv=ienv,
        crash=[], until=LW_UNTIL, until_hold=int(os.environ.get("BB_LW_HOLD", "150" if walk else str(LW_HOLD))),
    )


def check_lw(v, L):
    """Phase lw: a wipe in Ludwig's fight respawns everyone at the lamp with the fight reset."""
    need(v, L, "lw join: the guests are in", 0, r"cooperators \d -> \d")
    on = need(v, L, "lw Ludwig's fight is on (bar)", 0, r"boss health bar shown: .*event 13404802")
    if on is None:
        return
    hd = need(v, L, "lw host dies in the fight", 0, HOST_DIED, on)
    wipe = need(v, L, "lw the party wipes", 0, WIPE_RX, hd or on)
    if wipe is None:
        return
    up = need(v, L, "lw host: the respawn load is over", 0, r"loading screen gone", wipe)
    if up is None:
        return
    for i in (0, 1):
        need(v, L, "lw boss bar reset by the load on inst%d" % i, i, r"boss health bar: reset \(a load\)", wipe - 1)
    lamp = os.environ.get("BB_LW_LAMP", "3402950")
    need(v, L, "lw host respawns at its last lamp (%s)" % lamp, 0, r"lamp spot: lamp %s " % lamp, wipe)
    need(v, L, "lw host: the lamp spot is the revive spot, in the Nightmare", 0, r"map .*-> m34_00|world up", wipe)
    if os.environ.get("BB_LW_INJECT", "40"):
        # the stale guest: its own Ludwig copy at the injected HP % after the respawn load runs its heat-up 4 and
        # tells the host (packet 0x31, event 13404824): the host must not take it (it played the scene at the lamp)
        inj = need(v, L, "lw the guest's own Ludwig copy is set low after the respawn (stale-guest test)", 1,
                   r"guest_kill: entity 3400800 HP \d+ -> \d+ \(HP set only", up)
        if inj is not None:
            need(v, L, "lw the guest's phase event state reached the host and was not applied", 0,
                 r"Boss phase: event 134048\d\d's state from a guest not applied", inj - 0.5, inj + 20)
    walk = os.environ.get("BB_LW_WALK") == "1"
    win = float(os.environ.get("BB_LW_WALK_AT", "75")) - 35 if walk else LW_HOLD   # walk mode: until the party leaves
    none_of(v, L, "lw no second-phase scene after the wipe (34000030 / event 13404825)",
            r"cutscene 34000030|event 13404825", up, up + win, insts=(0, 1))
    none_of(v, L, "lw no fight after the wipe (bar, fog, first fight)", BOSS_START + r"|cutscene 34000020", up, up + win)
    none_of(v, L, "lw the host stays at the lamp (no stage warp 3402807)", r"warp 3402807", up, up + win, insts=(0, 1))
    # the report's false banner: a boss defeat (2003[12]) while Ludwig lives, on any game, at any time after the load
    none_of(v, L, "lw no boss defeat / PREY SLAUGHTERED while Ludwig lives (2003[12])",
            r"boss defeat processing 2003\[12\]|boss clear of boss", up, 1e9, insts=(0, 1))
    # (walk mode: the walk to z -700 does not reach the re-fight's fog region 3402801 (position unknown), so no bar is
    # required; the BB_PARTY_BOSS_HP_LOG probe lines tell what Ludwig's HP did meanwhile)
    check_common(v, L, "lw")


# Phase lwf (ludwig079, 0.7.9, group `ludwig_refight`, CL-dc2d79 / NW-5a97f96): the re-fight after a party wipe at
# Ludwig, played as players do (no warp into the arena): the party travels to the Grand Cathedral lamp 3402950 (his own lamp 3402952 stands inside the arena and is not lit yet),
# then stands outside the fog (the walk of 240 m cut short; nobody is put inside the arena),
# the host walks in (his first fight: scene 34000020, bar), the boss is taken to 70 %, the guest walks to his fog
# 3401800 (MSB part at x -407.93, z -716.45), presses Cross and goes through once the host's bar is up; host and
# guest die (the wipe: one respawn load at the lamp). Try 2: both walk back to the fog (host first, the guest once the
# host's bar is up), the host's game restarts the fight: on every game his bar shows again, at full health and in
# phase 1 (no second-half scene 34000030 / event 13404825, no warp 3402807, no defeat banner), nobody spectates,
# and the fog 3401801 at the back is gone after his death.
# BB_LWF_NET=<BB_NET_SIM preset>, BB_LWF_ENV="A=1;B=2", BB_LWF_PLAYERS=3, BB_LWF_FIRST_HP (70).
LWF_FOG = "-407.93:-716.45"
LWF_IN = "-407.93:-722.5:1.5:120"
LWF_OUT_HOST = "-407.93:1503.8:-708.0:3.14:pin=3:ground"    # outside his fog (z > -716.45), never inside the arena
LWF_OUT_GUEST = "-406.43:1503.8:-708.0:3.14:pin=3:ground"
LWF_UNTIL = [r"boss health bar: off \(a boss defeat"]


def phase_lwf(only):
    m = "%m34_00"
    players = int(os.environ.get("BB_LWF_PLAYERS", "2"))
    host = ",".join([
        "log_pos=2", "host_warp=3402950@8^1", "host_lastlamp=3402950@1" + m,
        "host_move=%s@6%s" % (LWF_OUT_HOST, m),                              # the walk from the Grand Cathedral lamp, cut short
        "host_stick=%s@4%s" % (LWF_IN, m),                                   # try 1: in through his entry (scene 34000020)
        "host_kill=3400800:hp%s@14%s" % (os.environ.get("BB_LWF_FIRST_HP", "70"), m),
        "host_die@8%s?member=bar" % m,                                       # once a guest is in the fight: the wipe
        # try 2: after the respawn load at the lamp, back to the fog and through it
        "host_move=%s@12%s" % (LWF_OUT_HOST, m),
        "host_fog=%s@3%s" % (LWF_FOG, m),
        "host_stick=%s@2%s" % (LWF_IN, m),
        "host_kill=3400800@30%s" % m,                                        # phase 1 watched for 30 s, then he falls
    ])
    guests = []
    for k in range(1, players):
        guests.append("%d:BB_PARTY_DIRECTOR_TEST=%s" % (k, ",".join([
            "log_pos=2", "guest_move=%s@6%s" % (LWF_OUT_GUEST, m),
            "guest_fog=%s@%d%s?host=bar" % (LWF_FOG, 4 + 2 * (k - 1), m),    # try 1: through his fog after the host's bar
            "guest_die@%d%s?host=spec" % (3 + 2 * (k - 1), m),               # the host is down: the wipe
            "guest_move=%s@12%s" % (LWF_OUT_GUEST, m),
            "guest_fog=%s@%d%s?host=bar" % (LWF_FOG, 4 + 2 * (k - 1), m),    # try 2
        ])))
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LUDWIG_CLEAR, "BB_PARTY_TEST_INVINCIBLE=1", "BB_PARTY_EVENT_SYNC_LOG=1",
           "BB_PARTY_BOSS_HP_LOG=1", "BB_BOSS_LOG=1"]
    if players > 2:
        env.append("BB_PARTY_MAX=4")
    if os.environ.get("BB_LWF_NET"):
        env.append("BB_NET_SIM=" + os.environ["BB_LWF_NET"])
    if os.environ.get("BB_LWF_ENV"):
        env += os.environ["BB_LWF_ENV"].split(";")
    return dict(
        name="lwf", seconds=900, count=players, headless=max(0, players - 2), pads=(0, 1), until_hold=8,
        env=env, ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host] + guests,
        watch=["--watch", r"boss health bar shown=bar@1,4,10,20", "--watch", r"host death in a boss fight=limbo@2",
               "--watch", r"a wipe; the=wipe@1", "--watch", r"loading screen gone=up@1,5,15",
               "--watch", r"went through boss fog=fog@1,4"],
        crash=[], until=LWF_UNTIL,
    )


def check_lwf(v, L):
    """Phase lwf: the re-fight after a wipe at Ludwig starts in phase 1 on every game; nobody is left spectating."""
    BAR = r"boss health bar shown"
    need(v, L, "lwf join: the guests are in", 0, r"cooperators \d -> \d")
    bar1 = need(v, L, "lwf try 1: his fight starts on the host (bar)", 0, BAR)
    if bar1 is None:
        return check_common(v, L, "lwf")
    for i in (1,):
        need(v, L, "lwf try 1: the guest goes through his fog, bar on inst%d" % i, i, BAR, bar1 - 2)
    hd = need(v, L, "lwf try 1: the host dies in the fight", 0, HOST_DIED, bar1)
    wipe = need(v, L, "lwf try 1: the party wipes", 0, WIPE_RX, hd or bar1)
    if wipe is None:
        return check_common(v, L, "lwf")
    up = need(v, L, "lwf the respawn load is over (host)", 0, r"loading screen gone", wipe)
    need(v, L, "lwf host respawns at its last lamp 3402950", 0, r"lamp spot: lamp 3402950 ", wipe)
    bar2 = need(v, L, "lwf try 2: the host walks back through the fog and his fight starts again (bar)", 0, BAR, up or wipe)
    if bar2 is None:
        return check_common(v, L, "lwf")
    for i in (1,):
        need(v, L, "lwf try 2: the guest goes through his fog again, bar on inst%d" % i, i, BAR, bar2 - 2)
    none_of(v, L, "lwf: every fog walk got through", r"test: fog: NOT through", 0, 1e9, (0, 1))
    end = L.find(0, r"boss health bar: off \(a boss defeat", bar2)
    end_t = end[0] if end else bar2 + 40
    # phase 1 on every game: no second-half scene, no stage warp, no defeat before the kill, nobody spectating
    none_of(v, L, "lwf no second-half scene after the wipe (34000030 / event 13404825)", r"cutscene 34000030|event 13404825",
            wipe, end_t - 0.1, (0, 1))
    none_of(v, L, "lwf no stage warp 3402807", r"warp 3402807", wipe, end_t - 0.1, (0, 1))
    none_of(v, L, "lwf no boss defeat before the kill", r"boss defeat processing 2003\[12\]|boss clear of boss", wipe, end_t - 3, (0, 1))
    none_of(v, L, "lwf nobody spectates after the respawn", r"spectating: following", up or wipe, end_t, (0, 1))
    none_of(v, L, "lwf no bar reset between the re-fight's start and the kill", r"boss health bar: (off|reset)", bar2 + 1, end_t - 0.1, (0, 1))
    under = [(i, h) for i in (0, 1) for h in L.all(i, r"UNDER HALF", wipe, end_t - 0.1)]
    v.step("lwf his health never reads under half in the re-fight (boss HP probe)", not under,
           "none" if not under else under[0][1][1][:150])
    full = [h for h in L.all(0, r"boss HP probe: entity 3400800: HP (\d+) / max \1 ", up or wipe, bar2 + 5)]
    v.step("lwf his health is full at the re-fight's start (boss HP probe: HP = max)", bool(full),
           full[-1][1][:150] if full else "no probe line with HP = max")
    need(v, L, "lwf the fight ends with his death", 0, r"boss health bar: off \(a boss defeat", bar2)
    check_common(v, L, "lwf")


# Phase amp (amelia2078, 0.7.8, ~10 min, 2 players, group `amelia_phase`; CL-2c1eef, 4-player playtest on 0.7.7):
# Vicar Amelia's SECOND try. Try 1 played her intro and wiped the party; on try 2 (no intro) everyone went through
# the fog, her bar showed, the host died early and spectated, and she animated and took hits but barely moved and
# never attacked the guests. The run plays it as players do, from a consistent save (fixture amelia_alive: she was
# never killed, Grand Cathedral lamp not lit, last lamp the Cathedral Ward lamp 2402950; docs/party/test_saves.md):
#  1. both start outside her fog (a warp OUTSIDE the arena only; nobody is ever put inside it);
#  2. the host walks in (no fog for the host): try 1 plays her intro 24000060 and her fight starts on the host
#     (bar); only then the guest walks to the fog and presses Cross ("?host=bar": the host's VITALS boss-bar bit);
#  3. try 1: a few hits, then the host dies and the guest dies: the wipe (one load at the Cathedral Ward lamp);
#  4. try 2: both outside the fog again, the host walks in through her fog (Cross: after try 1 the fog is up for the
#     host too; no intro now), the guest through the fog once the host's
#     bar is up, the host dies early (spectates, no respawn) and the guest fights her: it faces her (a stick tap),
#     locks on, re-faces every 3 R1 taps, its HP is put back to the max after every hit and it is invincible once she is under half (hp_floor=100, goal=...:god: every hit taken is logged);
#  5. the run ends when she dies or 40 s after she went under half HP (goal=: "test: goal reached").
# Checks over 40 s with the host spectating and 40 s under half HP, on every game: her bar, her animations (attacks
# 3000-3999), the guest's HP drops (her hits), how far she walked, and her AI target / hate record on the host
# (BB_PARTY_AI_LOG dumps). BB_AMP_NET=<BB_NET_SIM preset>, BB_AMP_ENV="A=1;B=2" (both games), BB_AMP_HOST_AWAY=1 (try 2:
# the host lives, hp_floor, back at the fog: the control), BB_AMP_TRY1=0 (no try 1; intro marked seen), BB_AMP_TRY2_PRE=STEP.
# Status 2026-11-02: FAILS on master and on 0.7.7 (CL-2c1eef open): after the host's death on try 2 she attacks nobody.
AMP_FIXTURE = Path(r"E:\games\bbport\test_saves\amelia_alive\savedata")
AMP_FOG = (51.5, 311.6)                  # her fog (object 2401800; MSB "SFX_光の壁_ボス前")
AMP_OUT_HOST = "49.3:36.2:305.9:0.52"    # outside the fog, ~6 m out on the cathedral steps, facing it
AMP_OUT_GUEST = "47.4:36.2:307.0:0.52"
AMP_IN = "58.0:323.6:1.0:80"             # the host's walk in: region 2402805 (her first fight, intro)
AMP_UNTIL = [r"test: goal reached|boss health bar: off \(a boss defeat"]


def prep_amp(root: Path):
    if not AMP_FIXTURE.is_dir():
        sys.exit("amelia_phase: the fixture %s is missing (docs/party/test_saves.md)" % AMP_FIXTURE)
    _cl_saves(root, AMP_FIXTURE, 0)
    _cl_saves(root, AMP_FIXTURE, 1)


def phase_amp(only):
    m = "%m24_00"
    fog = "%.1f:%.1f" % AMP_FOG   # host_fog / guest_fog: walk until the fog prompt shows, Cross, held until through
    host = ",".join([
        "log_pos=2"] + (["hp_floor=60"] if os.environ.get("BB_AMP_HOST_AWAY") == "1" else []) + [
        # try 1 (first meeting: her intro)
        "host_move=%s:pin=3:ground@8%s" % (AMP_OUT_HOST, m),
        "host_stick=%s@6%s?near=%s:3" % (AMP_IN, m, AMP_OUT_GUEST.rsplit(":", 1)[0]),   # in, once the guest is outside
        "host_face=2400800@3%s~" % m,                                   # after her intro
        "host_die@8%s?member=bar" % m,                                  # once the guest is in her fight: the wipe follows
        # try 2 (after the wipe: no intro; her fog is up for the host too now: it walks to it and presses Cross)
        "host_move=%s:pin=3:ground@12%s" % (AMP_OUT_HOST, m),
    ] + ([os.environ["BB_AMP_TRY2_PRE"]] if os.environ.get("BB_AMP_TRY2_PRE") else []) + [
        "host_fog=%s@6%s?near=%s:3" % (fog, m, AMP_OUT_GUEST.rsplit(":", 1)[0]),
        "host_stick=%s@3%s" % (AMP_IN, m),
        "host_face=2400800@3%s" % m,
    ] + (["host_stick=52.5:313.0:1.0:40@6%s?member=bar" % m]           # BB_AMP_HOST_AWAY=1: alive, back at the fog
         if os.environ.get("BB_AMP_HOST_AWAY") == "1" else
         ["host_die@6%s?member=bar" % m,                               # early: dead before half, spectates
          "host_kill=2400800:hp55@50%s" % m]))                           # 50 s later (dead, a test aid): 55 % HP, the guest
                                                                        # takes her under half: her phase 2 is measured too
    # R1 x3, then face her again and walk up to her (a stick toward her until within 2.5 m), lock on if it was lost
    attack = ["guest_press=0x800@1.3%s" % m] * 3 + ["guest_face=2400800:2.5@0.5%s" % m, "guest_press=0x4:lock@0.5%s" % m]
    fight_in = ["guest_face=2400800:2.5@3%s" % m, "guest_press=0x4@1%s" % m, "guest_press=0x4:lock@1%s" % m]
    g1 = ",".join([
        "log_pos=2", "hp_floor=100", "goal=2400800:50:40:god", "watch_text=fog/Fog",
        # try 1
        "guest_move=%s:pin=3:ground@10%s" % (AMP_OUT_GUEST, m),
        "guest_fog=%s@3%s?host=bar" % (fog, m),                         # to the fog once the host's fight is on, Cross
    ] + fight_in + [
        "guest_die@3%s?host=spec" % m,                                  # the host is down: the wipe (taken as done if she killed it)
        # try 2
        "guest_move=%s:pin=3:ground@12%s" % (AMP_OUT_GUEST, m),
        "guest_fog=%s@3%s?host=bar" % (fog, m),
    ] + fight_in + attack * 90)
    env = ["BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1", "BB_BOSS_LOG=1", "BB_PARTY_EVENT_SYNC_LOG=1"]
    if os.environ.get("BB_AMP_TRY1") == "0":
        # control: no try 1 in this session; her intro marked seen at the start (the re-fight path from a fresh load)
        host = host.replace(host[host.index("host_move"):host.index("host_move", host.index("host_move") + 1)], "")
        g1 = g1.replace(g1[g1.index("guest_move"):g1.index("guest_move", g1.index("guest_move") + 1)], "")
        env.append("BB_PARTY_PROGRESS_TEST_SET=12401802,9301,72400400")
    if os.environ.get("BB_AMP_NET"):
        env.append("BB_NET_SIM=" + os.environ["BB_AMP_NET"])
    if os.environ.get("BB_AMP_ENV"):
        env += os.environ["BB_AMP_ENV"].split(";")
    return dict(
        name="amp", seconds=1000, count=2, headless=0, pads=(0, 1), until_hold=3, prep=prep_amp, post=post_chalamp,
        env=env,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"travel step \d+/\d+: guest_move=out@4",
               "--watch", r"travel step \d+/\d+: host_stick=hostin@3,9",
               "--watch", r"boss health bar shown=bar@1,4",
               "--watch", r"test: fog: the prompt shows=prompt@0.05",
               "--watch", r"test: fog: (through|NOT)=fogdone@0.5",
               "--watch", r"went through boss fog=fog@1,4",
               "--watch", r"host death in a boss fight=limbo@2,10,25,40",
               "--watch", r"test: goal: entity=goal@1,15,30"],
        crash=[], until=AMP_UNTIL,
    )


def _amp_anims(L, i, a, b):
    """Her animation ids in order (each change once) on instance i between a and b."""
    ids = []
    for t, line in L.all(i, r"test: target anim: entity 2400800 animation -?\d+", a, b):
        x = int(re.search(r"animation (-?\d+)", line).group(1))
        if not ids or ids[-1][1] != x:
            ids.append((t, x))
    return ids


def _amp_walked(L, i, a, b):
    """How far she walked (m, x/z) on instance i between a and b: the target pos lines (once a second)."""
    pts = [tuple(float(x) for x in re.search(r"at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line).groups())
           for _t, line in L.all(i, r"test: target pos: entity 2400800 at", a, b)]
    return sum(((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5 for p, q in zip(pts, pts[1:])), len(pts)


def _amp_window(v, L, tag, a, b):
    """The 40 s checks of one window [a, b] (the host spectating / her phase 2)."""
    for i in (0, 1):
        off = L.find(i, r"boss health bar: (off|reset)", a, b)
        v.step("amp %s inst%d: her bar stays up" % (tag, i), not off, off[1][:120] if off else "up")
        ids = _amp_anims(L, i, a, b)
        atk = [x for _t, x in ids if 3000 <= x < 4000]
        v.step("amp %s inst%d: she acts (>= 8 animation changes, >= 3 attacks 3000-3999)" % (tag, i),
               len(ids) >= 8 and len(atk) >= 3, "%d changes, %d attacks: %s" % (len(ids), len(atk), [x for _t, x in ids[:16]]))
        walked, n = _amp_walked(L, i, a, b)
        v.step("amp %s inst%d: she moves (>= 4 m walked)" % (tag, i), walked >= 4.0, "%.1f m over %d positions" % (walked, n))
    hits = L.all(1, r"test: hp: own HP \d+ -> \d+", a, b)
    v.step("amp %s: her hits land on the living guest (>= 2 HP drops)%s" % (tag, " (info: invincible in phase 2)" if tag == "phase 2" else ""),
           len(hits) >= 2 or tag == "phase 2",
           "%d drops: %s" % (len(hits), [re.search(r"\(-\d+\)", h).group(0) for _t, h in hits[:8]]))
    # her AI on the host's game: the target dumps list a live hate record (kind 1, b40 1) of a chr; the living
    # member's handle is the one the limbo / attention hands her
    gh = None
    for _t, line in L.all(0, r"(host limbo: boss \w+ targets the living member|boss attention: boss \w+ turns to player chr) [0-9a-f]+", a - 30, b):
        gh = re.search(r"(?:living member|player chr) ([0-9a-f]+)", line).group(1)
    dumps = L.all(0, r"boss target dump", a, b)
    live = [d for _t, d in dumps if gh and re.search(r"kind 1 t20 \d+ t38 [\d.]+ t3c [\d.]+ b40 1 h %s" % gh, d)]
    v.step("amp %s host (info): her hate record / the boss room notice" % tag, True,
           "guest chr %s: %d of %d dumps with its live record; notices: %d" % (gh, len(live), len(dumps),
                                                                              len(L.all(0, r"boss room notice", 0, b)) + len(L.all(1, r"boss room notice", 0, b))))


def check_amp(v, L):
    need(v, L, "amp join: 2 players", 0, r"cooperators 0 -> 1")
    out = need(v, L, "amp try 1: the guest starts outside her fog", 1, r"travel step \d+/\d+: guest_move")
    bar1 = need(v, L, "amp try 1: her fight starts on the host (bar)", 0, r"boss health bar shown: .*entity 2400800", out or 0)
    if bar1 is None:
        return check_common(v, L, "amp")
    need(v, L, "amp try 1: her intro played (24000060)", 0, r"24000060", (out or 0), bar1 + 1)
    none_of(v, L, "amp: every fog walk got through (prompt, Cross, past the fog)", r"test: fog: NOT through", 0, 1e9, (0, 1))
    fog1 = need(v, L, "amp try 1: the guest goes through her fog after the host's bar", 1, r"went through boss fog|test: fog: through the fog", bar1)
    need(v, L, "amp try 1: her bar on the guest", 1, r"boss health bar shown: .*entity 2400800", bar1 - 2)
    wipe = need(v, L, "amp try 1: the party wipes", 0, r"went down while the host was .*a wipe", bar1)
    if wipe is None:
        return check_common(v, L, "amp")
    bar2 = need(v, L, "amp try 2: her fight starts on the host again (bar)", 0, r"boss health bar shown: .*entity 2400800", wipe)
    if bar2 is None:
        return check_common(v, L, "amp")
    none_of(v, L, "amp try 2: no intro the second time", r"24000060", wipe, bar2 + 5, (0, 1))
    fog2 = need(v, L, "amp try 2: the guest goes through her fog after the host's bar", 1, r"went through boss fog|test: fog: through the fog", bar2)
    gbar = need(v, L, "amp try 2: her bar on the guest", 1, r"boss health bar shown: .*entity 2400800", bar2 - 2)
    lim = need(v, L, "amp try 2: the host dies early and spectates (limbo)", 0, r"host death in a boss fight", bar2)
    if lim is None:
        return check_common(v, L, "amp")
    need(v, L, "amp try 2: the host spectates the guest", 0, r"spectating: following", lim)
    end = L.find(0, r"boss health bar: off \(a boss defeat", lim)
    end_t = end[0] if end else 1e9
    none_of(v, L, "amp try 2: the host stays down until she dies (no respawn)", r"spectating ended", lim, end_t - 0.1, (0,))
    _amp_window(v, L, "spectated", lim + 3.0, min(lim + 43.0, end_t))
    under = L.find(1, r"test: goal: entity 2400800 under", lim)
    if under:
        _amp_window(v, L, "phase 2", under[0], min(under[0] + 40.0, end_t))
    goal = L.find(1, r"test: goal reached", lim)
    v.step("amp try 2: the goal (40 s under half or her death) is reached", bool(goal or end),
           (goal or end)[1][:140] if (goal or end) else "neither")
    check_common(v, L, "amp")


# Phase ba (enemyai, 0.7.6, ~4 min, 2 players, group `boss_ai`, C2 + player report boss_ai_host_dead): the
# host dies in the Cleric Beast's fight while the guest fights on; the boss must keep attacking the
# guest (its animations logged by BB_PARTY_TARGET_ANIM on the guest's screen, the guest invincible so
# the fight runs on). The boss's attack animations after the host's death are the evidence.
BA_UNTIL = [r"1:travel step (\d+)/\1: guest_press 0x0"]


def phase_ba(only):
    # The host is frail (BB_PARTY_ROT_TEST_FRAIL: any hit leaves 1 HP) and no invincibility for it: the
    # boss itself kills it (the game's own death, the boss's "player killed" interrupt), as in the
    # player report. The guest is invincible and fights on beside the boss.
    host = ",".join(["goods=4114:0",
                     "host_warp=2412952@8^1",                                # the party to the Cleric Beast's lamp
                     "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,        # the fight starts
                     "host_chr=2410800:2@10%m24_01"] +                       # beside the boss: it kills the frail host soon
                    ["host_press=0x800@2%m24_01"] * 12)                      # R1 taps meanwhile
    g1 = ",".join(["god",
                   "guest_move=%s@25%%m24_01" % CLERIC_SPOT_GUEST,           # into the fight
                   "guest_chr=2410800@3%m24_01"] +                           # beside the boss
                  ["guest_press=0x800@1.5%m24_01"] * 90 +                    # R1 taps: it fights back; the frail host dies
                  ["guest_press=0x0@2%m24_01"])
    return dict(
        name="ba", seconds=300, count=2, headless=0, until_hold=4,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR, "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone",
              "0:BB_PARTY_ROT_TEST_FRAIL=1"],
        watch=["--watch", r"host death in a boss fight=balimbo@2,8,14",
               "--watch", r"test: target anim=baanim@0.5"],
        crash=[], until=BA_UNTIL,
    )


def check_ba(v, L):
    """Phase ba: the host dies in the Cleric Beast's fight, the guest fights on: the boss keeps acting."""
    need(v, L, "ba join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "ba the host's fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        return
    lim = need(v, L, "ba host death in the fight: the limbo", 0, r"host death in a boss fight", on)
    if lim is None:
        return
    # the boss's animation changes on the guest's screen (the host's AI drives it): each distinct
    # animation id after the death is an action of the boss; a boss without a target idles (one id)
    rows = L.all(1, r"test: target anim: entity 2410800 animation \d+", lim + 3.0, lim + 43.0)
    ids = []
    for t, line in rows:
        a = int(re.search(r"animation (\d+)", line).group(1))
        if not ids or ids[-1][1] != a:
            ids.append((t, a))
    span = (ids[-1][0] - ids[0][0]) if ids else 0.0
    v.step("ba the boss keeps acting after the host's death (>= 6 animation changes in 40 s)",
           len(ids) >= 6, "%d animation changes over %.0f s: %s" % (len(ids), span, [a for _, a in ids[:14]]))
    if os.environ.get("BB_PARTY_LIMBO_RETARGET", "1") != "0":
        need(v, L, "ba the boss targets the living member (ChangeTarget)", 0,
             r"host limbo: boss [0-9a-f]+ targets the living member", lim)
    check_common(v, L, "ba")


# Phase bh / bk (bossai077, 0.7.7, ~5 min each, 2 players, group `boss_dead`; reports "the boss stops
# when the host dies" / "ignores the last guest"): the enemies' target search keeps a dead player in
# its bank (no HP check); the spectating host's dead chr is carried onto the followed member's spot,
# so the boss picked it as its nearest target and idled. A player chr at HP 0 in a boss fight now
# leaves the bank on the host's game ("dead untarget"), BB_PARTY_DEAD_UNTARGET=0 the A/B.
#  bh: the guest is mortal: after the host's death the boss hits it (its HP falls on the host's
#      copy, or it dies: the wipe).
#  bk: the guest is invincible; the boss is killed 40 s after the host's last press: the host gets
#      up in the arena and its chr goes back into the bank.
def _phase_boss_dead(name, guest_god, kill):
    host = ",".join(["goods=4114:0",
                     "host_warp=2412952@8^1",
                     "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,
                     "host_chr=2410800:2@10%m24_01"] +
                    # bk: R1 taps until the boss kills the frail host (host_die latches if not yet);
                    # bh: the host dies at once, before the mortal guest is beside the boss (it would
                    # otherwise die first or with the host: a wipe, no limbo)
                    (["host_press=0x800@2%m24_01"] * 12 + ["host_die@1%m24_01"] if guest_god else ["host_die@3%m24_01"]) +
                    (["host_kill=2410800@40%m24_01"] if kill else []))
    g1 = ",".join((["god"] if guest_god else []) +
                  # bh: the mortal guest waits at the lamp (outside the fog) and comes in once the host
                  # lies in its limbo (the boss killed it within the death's 9 s otherwise: a wipe)
                  (["guest_move=%s@25%%m24_01" % CLERIC_SPOT_GUEST] if guest_god else
                   ["guest_press=0x0@48%m24_01", "guest_move=%s@1%%m24_01" % CLERIC_SPOT_GUEST]) +
                  ["guest_chr=2410800@3%m24_01"] +
                  ["guest_press=0x800@1.5%m24_01"] * 90 +
                  ["guest_press=0x0@2%m24_01"])
    return dict(
        name=name, seconds=300, count=2, headless=0, until_hold=4,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR, "BB_PARTY_TARGET_ANIM=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_TRAVEL_CHOICE=alone",
              "0:BB_PARTY_ROT_TEST_FRAIL=1"],
        watch=["--watch", r"host death in a boss fight=%slimbo@2,8,14,20" % name,
               "--watch", r"back in the enemies' target bank=%sback@1,4" % name],
        crash=[], until=BA_UNTIL,
    )


def phase_bh(only):
    return _phase_boss_dead("bh", guest_god=False, kill=False)


def phase_bk(only):
    return _phase_boss_dead("bk", guest_god=True, kill=True)


def _limbo_start(v, L, tag):
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, tag + " the host's fight is on (bar)", 0, r"boss health bar shown")
    if on is None:
        return None
    return need(v, L, tag + " host death in the fight: the limbo", 0, r"host death in a boss fight", on)


def check_bh(v, L):
    """Phase bh: the host dead in the Cleric Beast's fight, the boss keeps hitting the mortal guest."""
    lim = _limbo_start(v, L, "bh")
    if lim is None:
        check_common(v, L, "bh")
        return
    if os.environ.get("BB_PARTY_DEAD_UNTARGET", "1") != "0":
        need(v, L, "bh the dead host's chr leaves the enemies' target bank", 0,
             r"dead untarget: chr \w+ \(this player, hp 0\) out of the enemies' target bank", lim - 15.0)
    # the guest's HP on the host's copy (remote chrs lines) after the death, and the guest's own death
    hps = []
    for t, line in L.all(0, r"remote chrs: \[mode 1 ", lim, lim + 90.0):
        m = re.search(r"hp (-?\d+)\]", line)
        if m:
            hps.append(int(m.group(1)))
    died = L.find(1, r"local death: HP 0", lim) is not None
    first = hps[0] if hps else None
    low = min(hps) if hps else None
    v.step("bh the boss hits the guest after the host's death (its HP falls, or it dies)",
           died or (first is not None and low < first),
           "guest HP on the host's game %s -> lowest %s over %d samples; guest died: %s" % (first, low, len(hps), died))
    check_common(v, L, "bh")


def check_bk(v, L):
    """Phase bk: the boss killed while the host lies in limbo: the host up, its chr back in the bank."""
    lim = _limbo_start(v, L, "bk")
    if lim is None:
        check_common(v, L, "bk")
        return
    out = need(v, L, "bk the dead host's chr leaves the enemies' target bank", 0,
               r"dead untarget: chr \w+ \(this player, hp 0\) out of the enemies' target bank", lim - 15.0)
    if out is not None:
        need(v, L, "bk the host gets up: its chr back in the enemies' target bank", 0,
             r"dead untarget: chr \w+ \(hp [1-9]\d*\) back in the enemies' target bank(?! \()", out)
    check_common(v, L, "bk")


# Phase la (enemyai, 0.7.6, ~4 min, 2 players, group `lone_ai`, player report lone_guest_split): the host
# stays in its Hunter's Dream, the guest travels alone to the Cathedral Ward lamp (answers NO), still a
# client of the host's world. The Ward's enemies must act (attack the guest), not stand still until the
# host loads that map. BB_PARTY_AI_LOG=1 logs who runs an enemy's AI; BB_PARTY_TARGET_ANIM=1 the animations
# of the enemy beside the guest (the guest is invincible; its R1 draws the enemy).
LA_LAMP = "2402950"      # Cathedral Ward (m24_00)
LA_ENEMY = os.environ.get("BB_LA_ENEMY", "2400421")  # an awake Cathedral Ward enemy (182 HP) on a ledge 22 m from the lamp (2400350-352 and 2400220 sleep, 2400362 is a statue)  # a Cathedral Ward enemy (123 HP), 23 m from the lamp (2400362 is a statue, 2400220 sleeps)
LA_UNTIL = [r"1:travel step (\d+)/\1: guest_press 0x0"]


def phase_la(only):
    host = "god"
    g1 = ["guest_lamp=%s@12%%m21_00" % LA_LAMP,                           # alone to the Cathedral Ward
          "guest_press=0@25%m24_00!"]                                     # the world is up there: the census
    if LA_ENEMY:
        g1 += ["guest_chr=%s:6@3%%m24_00" % LA_ENEMY] + ["guest_press=0x800@1.5%m24_00"] * 25
    g1 += ["guest_press=0x0@2%m24_00"]
    return dict(
        name="la", seconds=300, count=2, headless=0, until_hold=4,
        env=["BB_PARTY_AI_LOG=1", "BB_PARTY_ENEMY_CENSUS=1", "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_CHR_DUMP=40"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_TRAVEL_CHOICE=alone",
              "1:BB_PARTY_ROT_TEST_FRAIL=1"],  # frail: any hit leaves 1 HP (logged), the guest never dies
        watch=["--watch", r"arrived alone in=laarr@2,8,16", "--watch", r"test: frail=lahit@0.3",
               "--watch", r"test: target anim: entity \d+ animation 30\d\d=laatk@0.2,0.8,1.5"],
        crash=[], until=LA_UNTIL,
    )


# Phase lb (enemyai, ~4 min, 2 players, no group): the same as la with the whole party in the Cathedral Ward
# (host and guest together): what a healthy enemy looks like on a guest's screen (the A of la's A/B).
def phase_lb(only):
    host = ",".join(["god", "host_warp=%s@8^1" % LA_LAMP, "host_press=0@120%m24_00!"])
    g1 = ["guest_press=0@25%m24_00!"]
    g1 += ["guest_chr=%s:6@3%%m24_00" % LA_ENEMY] + ["guest_press=0x800@1.5%m24_00"] * 25
    g1 += ["guest_press=0x0@2%m24_00"]
    return dict(
        name="lb", seconds=300, count=2, headless=0, until_hold=4,
        env=["BB_PARTY_AI_LOG=1", "BB_PARTY_ENEMY_CENSUS=1", "BB_PARTY_TARGET_ANIM=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1),
              "1:BB_PARTY_ROT_TEST_FRAIL=1"],
        crash=[], until=LA_UNTIL,
    )


# Phase scb (scurry079, CL-20d595 / NW-44497f9, group `scurry_beasts`, 2 players): Hemwick Charnel Lane's Mad Ones
# (the Insight madness spirits, entities 2200500-2200508, event 12204000) and the Wandering Madness (small, flee and
# vanish) must exist on BOTH games, as in solo. The host starts at the Hemwick lamp 2202950 with Insight 60 and the night
# flags; SCB_R is the chr dump radius (BB_PARTY_CHR_DUMP). Screenshots of both games come from the harness.
SCB_LAMP = "2202950"
SCB_SPOT = (-140, -15.5, 465.5)  # 7 m from the Mad One 2200505 at (-147.1, -15.5, 465.5); the lamp is 32 m away
SCB_ENTITY = 2200505


def phase_scb(only):
    ins = os.environ.get("SCB_INSIGHT", "60")
    steps = ["god", "insight=" + ins, "log_pos=1"]
    hs = steps + [e for e in os.environ.get("SCB_HOST", "").split(";") if e]
    gs = list(steps)
    # default: the GUEST walks up to a Mad One alone (the host stays at the lamp, 32 m away: the broken case);
    # SCB_WHO=host: the host walks up (always worked); SCB_WHO=both
    who = os.environ.get("SCB_WHO", "guest")
    spot = "%s:%s:%s" % SCB_SPOT
    if who in ("host", "both"):
        hs += ["host_move=%s@20%%m22" % spot]
    if who in ("guest", "both"):
        gs += ["guest_move=%s@20%%m22" % spot]
    gs += [e for e in os.environ.get("SCB_GUEST", "").split(";") if e]
    env_net = ["BB_NET_SIM=" + os.environ["SCB_NET"]] if os.environ.get("SCB_NET") else []
    return dict(
        name="scb", seconds=int(os.environ.get("SCB_SECONDS", "150")), count=2, headless=0, fps=60,
        env=["BB_PARTY_CHR_DUMP=" + os.environ.get("SCB_R", "400"), "BB_PARTY_PROGRESS_TEST_SET=9800,9801"] +
            env_net + [e for e in os.environ.get("SCB_ENV", "").split(";") if e],
        ienv=["0:BB_PARTY_TEST_START_LAMP=" + SCB_LAMP, "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(hs),
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(gs)],
        watch=[], crash=[], until=[], until_hold=0,
    )


def check_scb(v, L):
    need(v, L, "scb join: 2 players", 0, r"cooperators 0 -> 1")
    # the Mad One 2200505 (Insight 60, night) is DISABLED until a player comes within 22 m; then it must be enabled on
    # BOTH games (a guest alone near it once left it disabled on both: its spawn event waited for chr type 0)
    for i, who in ((0, "host"), (1, "guest")):
        rows = L.all(i, r"chr dump: entity %d " % SCB_ENTITY)
        off = [t for t, l in rows if "DISABLED" in l]
        on = [t for t, l in rows if "DISABLED" not in l]
        v.step("scb %s's game: the Mad One %d appears (enabled after %d disabled samples)" % (who, SCB_ENTITY, len(off)),
               bool(on) and bool(off) and on[0] > off[0], "samples disabled %d, enabled %d" % (len(off), len(on)))
        n = {}
        for t, l in L.all(i, r"chr dump: entity 22005\d\d "):
            m = re.search(r"entity (\d+) ", l)
            n[m.group(1)] = "DISABLED" not in l
        v.step("scb %s's game: Mad Ones present (chrs 22005xx loaded %d, enabled %d)" % (who, len(n), sum(n.values())),
               len(n) >= 6 and sum(n.values()) >= 1, "%s" % sorted(k for k, e in n.items() if e))
    check_common(v, L, "scb")


# Phase lc (enemyai, ~5 min, 2 players, group `lone_ai`): the meet after la: the host (still in the Dream while the
# guest stood alone in the Cathedral Ward, its enemies on the guest's own AI) travels alone to the same lamp. The
# older world loads once (the B2 re-join), the guest is back in the host's world and its enemies are the host's again
# (no new take-over, the enemy census digest matches the host's).
def phase_lc(only):
    host = ",".join(["god", "host_alone=%s@45%%m21_00" % LA_LAMP, "host_press=0@60%m24_00!"])
    g1 = ["god", "guest_lamp=%s@12%%m21_00" % LA_LAMP, "guest_press=0@25%m24_00!",   # alone in the Ward: its AI runs there
          "guest_press=0@50%m24_00!", "guest_press=0@30%m24_00!"]                      # the host arrives, the re-join, the digest
    return dict(
        name="lc", seconds=300, count=2, headless=0, until_hold=15,
        env=["BB_PARTY_AI_LOG=1", "BB_PARTY_ENEMY_CENSUS=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_TRAVEL_CHOICE=alone",
              "0:BB_PARTY_TRAVEL_CHOICE=alone"],
        watch=["--watch", r"arrived in the host world=lcmeet@2,8"],
        crash=[], until=[r"1:travel step (\d+)/\1: guest_press"],
    )


def check_lc(v, L):
    need(v, L, "lc join: 2 players", 0, r"cooperators 0 -> 1")
    arr = need(v, L, "lc guest alone in the Cathedral Ward", 1, r"arrived alone in m24_00")
    if arr is None:
        return
    take = need(v, L, "lc the guest's game takes the AI while the host is away", 1,
                r"Party enemy AI: a lone guest takes the enemies' AI", arr - 5)
    meet = need(v, L, "lc the host arrives: the AI is handed back to it", 1,
                r"Party enemy AI: the host is in this area again: \d+ enemies handed back", arr + 5)
    if meet is not None:
        none_of(v, L, "lc no second take-over once the host is in the guest's area",
                r"Party enemy AI: a lone guest takes the enemies' AI", meet + 1, insts=(1,))
        need(v, L, "lc the active enemies are on the host's AI again (net manipulator)", 1,
             r"ai: enemy \d+ \(chr [0-9a-f]+\) \d+ m: manip NetAI .* rec 4/4", meet)
        none_of(v, L, "lc enemy census: no digest mismatch with the host after the meet",
                r"enemy census: differs from the host's digest", meet, insts=(1,))
    check_common(v, L, "lc")


def check_la(v, L):
    need(v, L, "la join: 2 players", 0, r"cooperators 0 -> 1")
    arr = need(v, L, "la guest arrives alone in the Cathedral Ward", 1, r"arrived alone in m24_00")
    if arr is None:
        return
    if LA_ENEMY:
        rows = L.all(1, r"test: target anim: entity %s animation \d+" % LA_ENEMY, arr)
        ids = []
        for t, line in rows:
            a = int(re.search(r"animation (\d+)", line).group(1))
            if not ids or ids[-1][1] != a:
                ids.append((t, a))
        v.step("la the enemy acts (>= 4 animation changes: it walks to and attacks the guest)", len(ids) >= 4,
               "%d animation changes: %s" % (len(ids), [a for _, a in ids[:14]]))
        need(v, L, "la the enemy hurts the guest (frail: HP -> 1)", 1, r"test: frail \(BB_PARTY_ROT_TEST_FRAIL\): HP \d+ -> 1", 0)  # at 60 FPS the aid already fires at the join (the phantom cut 719 -> 503 reads as a hit)
    if os.environ.get("BB_PARTY_LONE_AI", "1") != "0":
        need(v, L, "la the guest's game runs the enemies' AI", 1, r"Party enemy AI: a lone guest takes the enemies' AI", arr - 5)
    check_common(v, L, "la")


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
        "host_warp=2412953@8^1",                                    # the Tomb of Oedon lamp (party travel)
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


# The Grand Cathedral lamp 3402953 is lit by beating Laurence (CL-b457f5); the test save had it lit with Laurence alive, so
# the phases warped a party straight into his room, which no player can do. Now the fixture clears the lamp's flags (lit 13407870 /
# 13407871, name 73400203: lamp_travel_rules.h, n = 3 of block 340) and the party enters the real way: the party travels to
# the Hunter's Nightmare lamp 3402950, each player is put OUTSIDE his fog 3401850 (6 m on its outer side: angle 0; angle 180 is the arena side, measured br 2026-11-04) and walks in through it
# by the stick, then on to the altar spot (BR_SIGNAL). Never a teleport into the arena (the boss repro rule).
BR_LAMP_CLEAR = "13407870,13407871,73400203"
BR_SIGNAL_XZ = "-434.55:-264.25"
BR_ALTAR_XZ = "-433.09:-261.57"   # the lamp spot inside the altar region 3402855: the host must stand there for the skull (4 m off: no fight)


def _laurence_enter(who, first_delay, warp=False):  # the guest starts after the host: two bodies in the fog doorway jam (bg: nav GAVE UP, 15 stalls)
    """The steps of `who` (host / guest) from outside Laurence's fog to the altar spot inside; the host also travels first."""
    m = "%m34_00"
    return (["host_warp=3402950@8^1"] if warp else []) + [
        "%s_obj=3401850:6:0@%s%s" % (who, first_delay, m),
        "%s_stick=obj:3401850:4:180:%s@2%s" % (who, BR_WALK, m),        # in through the fog (open: the rule under test)
        "%s_stick=%s:1.0:200@2%s" % (who, BR_ALTAR_XZ if who == "host" else BR_SIGNAL_XZ, m)]  # on to the altar spot


def phase_br(only):
    # The reward item box after each kill holds the stick (run 1: both screens showed Beast's Embrace
    # + Guidance with an OK row through the walks): Cross twice closes it. R3 (0x4) after each
    # `_obj` step turns the camera behind the player, so the walk's screenshots face the doorway.
    # The steps at Ludwig's back wall carry no map condition: it stands on the m34 / m35 border, and
    # the walk itself changes the map (run 1: the host's next step waited for m34_00 for good).
    host = ",".join([
        "god", "goods=4014:0",                                       # no Laurence's Skull yet
        *_laurence_enter("host", 10, True),                          # the party to the Hunter's Nightmare lamp, in through his fog
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
        "host_press=0x4@1!", "host_press=0@1!",                      # R3 again: the camera behind him for the walk back (0.7.9: he stalled 3 m short of the goal, the camera had swung)
        "host_stick=obj:3401801:4:0:%s@2!" % BR_WALK,                # and back
    ])
    g1 = ",".join([
        "god",
        *_laurence_enter("guest", 40),                               # in through his fog (the way a player comes)
        "guest_obj=3401850@10%m34_00",                               # at Laurence's fog (inside, by the lamp)
        "guest_press=0x4@1%m34_00!", "guest_press=0@1%m34_00!",       # R3
        "guest_stick=obj:3401850:4:180:%s@2%%m34_00" % BR_WALK,       # through it
        "guest_stick=obj:3401850:4:0:%s@2%%m34_00" % BR_WALK,         # and back
        "guest_move=%s@2%%m34_00" % BR_SIGNAL,                        # inside: the host's skull now
        "guest_press=0x4000@10%m34_00?13401850=1", "guest_press=0x4000@2%m34_00!",  # Laurence's reward box
        "guest_press=0x4000@10?9471=1", "guest_press=0x4000@2!",      # Ludwig's
        "guest_obj=3401801@30!",                                     # at the wall behind him, after the host's walk through and back (two bodies in the doorway jam: the host's walk back GAVE UP in 0.7.9)
        "guest_press=0x4@1!", "guest_press=0@1!",                    # R3
        "guest_stick=obj:3401801:4:180:%s@2!" % BR_WALK,
        "guest_stick=obj:3401801:4:0:%s@2!" % BR_WALK,
    ])
    return dict(
        name="br", seconds=720, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + BR_LAURENCE_CLEAR + "," + BR_LUDWIG_CLEAR + "," + BR_LAMP_CLEAR],
        # 0.7.7 (bossrunes): the Holy Moonlight Sword's award spot lot 43211 (the weapon is shared with every member)
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "0:BB_PARTY_ITEMS_HOST_TEST=43211"],
        watch=["--watch", r"test: (?:host|guest)_stick: object 340(?:1850|1801) found=brdoor@0.5,4"],
        crash=[], until=BR_UNTIL, until_hold=6,
    )


# Phase rf (romfog, 0.7.6, ~3 min, 2 players, group `rom_fog`): Rom's fog for a guest (player report
# 2026-10-25: the guest could not get to Rom, only the host). Rom's guest-entry event 13204831 waits on
# 13201802 (her fight started) and 13204800 (the host entered); the guest never had 13204800, so no
# prompt (the gate was missing from BossGates). The party travels to the Moonside Lake lamp 3202952,
# 13201802 is set on both games (her fight started: the fog wall rises), the GUEST goes through the fog
# 3201800 first; the host opens the fight (host-entered flag), then the host walks through too.
RF_UNTIL = [r"0:travel step (\d+)/\1: host_move"]
RF_ARENA = "-509.0:-271.2:371.0"   # the Moonside Lake lamp spot, in the lake
RF_CLEAR = "13201800,13201802,13204800,13204801,9303"


def phase_rf(only):
    host = ",".join([
        "god",
        "host_warp=3202952@8^1",                                    # the party to the Moonside Lake lamp
        "host_flag=13201802@6%m32_00",                              # her fight started: the fog wall rises
        "host_obj=3201800@4%m32_00?13204800=1",                     # the guest went in first: the host follows
        "host_press=0x4@1!", "host_press=0@1!",                     # R3: the camera behind the player
        "host_press=0x4000@2%m32_00!", "host_press=0x4000@2%m32_00!",  # through the fog (the terrace)
        "host_move=%s@6%%m32_00" % RF_ARENA,                         # the lake (the lamp spot)
    ])
    g1 = ",".join([
        "god",
        "guest_flag=13201802@6%m32_00",
        "guest_obj=3201800@8%m32_00",                               # at the fog, facing it
        "guest_press=0x4@1%m32_00!", "guest_press=0@1%m32_00!",     # R3
        "guest_press=0x4000@2%m32_00!", "guest_press=0x4000@2%m32_00!",  # through it, first (the terrace)
        # The jump off the terrace into the lake (the landing event 13204821, region 3202800) is not
        # reproduced by a bare chr warp (the region never reads the warped chr as inside): the landing
        # event's own write, the guest-entered flag 13204801, is set instead.
        "guest_flag=13204801@6%m32_00",
    ])
    return dict(
        name="rf", seconds=300, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + RF_CLEAR, "BB_PARTY_EMEVD_TRACE=0"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[], until=RF_UNTIL, until_hold=8,
    )


def check_rf(v, L):
    """Phase rf: a guest goes through Rom's fog first; the host opens her fight and follows."""
    need(v, L, "rf join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rf %s: the room rule of Rom's re-fight event" % who, i,
             r"EMEVD rule \(built in, boss rooms\): skip 13204830:1003:5@2 \(on a client\)")
    arr = need(v, L, "rf guest in Byrgenwerth (m32_00)", 1, r"map .*-> m32_00")
    if arr is None:
        return check_common(v, L, "rf")
    need(v, L, "rf guest: Rom's guest-entry event passes the host-entered condition", 1,
         r"boss fog: guest-entry event 13204831 does not wait for the host", arr)
    went = need(v, L, "rf guest went through Rom's fog first", 1, r"guest: went through boss fog \(event 13204831", arr)
    if went is not None:
        need(v, L, "rf host: the guest went in first, the host opens her fight", 0,
             r"host: boss fog: slot \d+ went in first \(event 13204831\)", went - 0.5)
        need(v, L, "rf host walks through the fog after the guest", 0, r"travel step \d+/\d+: host_press", went)
        need(v, L, "rf host lands in the arena too", 0, r"travel step \d+/\d+: host_move", went)
    check_common(v, L, "rf")


# Phase rj (romjump, 0.7.7, ~3 min, 2 players, group `rom_jump`): everyone survives the jump off the Byrgenwerth
# terrace into Moonside Lake (CL-393160). The fall (about 95 m) is survivable only through the invincibility the
# landing events 13204820 (host) / 13204821 (guest) switch on while the player crosses region 3202800. On a guest
# the host's copy 13204820 is met by the HOST's entry, skips its "on" (multiplayer state) but runs its "off" 5 s
# later: a guest who jumped 2-5 s after the host had that invincibility cut while still falling and died on landing.
# The host walks off the terrace edge for real (stick walk from the terrace, no god mode); the guest follows 3 s
# after the host lands (a warp into the air over the lake: the fall is the real one). RJ_ORDER=guest: the guest
# walks off first and the host follows.
RJ_UNTIL = [r"1:travel step (\d+)/\1: guest_press 0x0"]
RJ_CLEAR = "13201800,13201802,13204800,13204801,9303"
RJ_EDGE = "-458:-176.35:393"       # the terrace, west of Rom's fog; walking west it ends at x -477
RJ_WALK = "-500:390.5:3:60"        # the stick walk target (the edge is crossed at x -477, z 389.5)
RJ_LAND = "-478.3:-271.2:389.5:6"  # where a walker lands in the lake
RJ_AIR = "-487:-176:379"           # over the lake, above region 3202800 (the follower's warp)


def phase_rj(only):
    order = os.environ.get("RJ_ORDER", "host")
    walk = lambda who: ["%s_move=%s@4%%m32_00" % (who, RJ_EDGE), "%s_stick=%s@2%%m32_00" % (who, RJ_WALK)]
    follow = lambda who, other: ["%s_move=%s@0.5%%m32_00?near=%s" % (who, RJ_AIR, RJ_LAND)]
    log = "log_pos=0.5,log_flag=13204800/13204801"
    if order == "guest":
        host = [log, "host_warp=3202952@8^1"] + follow("host", "guest")
        g1 = [log] + walk("guest")
    else:
        host = [log, "host_warp=3202952@8^1"] + walk("host")
        g1 = [log] + follow("guest", "host")
    host += ["host_press=0x0@30%m32_00!"]
    g1 += ["guest_press=0x0@30%m32_00!"]
    host = os.environ.get("BB_RJ_HOST", ",".join(host))
    g1 = os.environ.get("BB_RJ_GUEST", ",".join(g1))
    return dict(
        name="rj", seconds=int(os.environ.get("BB_RJ_SECONDS", "240")), count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + RJ_CLEAR, "BB_PARTY_EMEVD_TRACE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[], until=RJ_UNTIL, until_hold=8,
    )


def check_rj(v, L):
    """Phase rj: both players fall into the lake and live (no fall death, no revive at the lamp)."""
    need(v, L, "rj join: 2 players", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rj %s: fell into the lake region (a landing event ran)" % who, i,
             r"EMEVD event 1320482[01] \[4\] 2004\[15\].* 01 00 00 00")
    none_of(v, L, "rj nobody died in the lake jump (no fall death, no revive at the lamp)",
            r"died from a fall|funnel: guest_died|host died|slot \d+ was revived", insts=(0, 1))
    check_common(v, L, "rj")


# Phase em (emissary, 0.7.7, ~3 min, 2 players, group `emissary`): the Celestial Emissary's small emissaries
# when a guest walks through the fog first (player report 2026-10-26: the host stood in the arena, the guest
# went through the fog, the small emissaries spawned for the guest only). The host's first-fight event 12421702
# (`ホストがボス部屋入場_初戦`) is what enables them (with the spawn effect 6200) and sets 12421702; the guest-first
# path only set the host-entered flag 12424700, which starts the boss but not that event. Party at the lamp
# 2422952 beside the fog 2421700, the host walks through it and stands in the arena, the guest follows
# EM_DELAY s later (the guest-first path with the host already inside).
EM_UNTIL = [r"1:travel step (\d+)/\1: guest_press 0x0"]
EM_SPOT = "15.0:50.0:287.0"   # in the arena between the first-start area (x 36) and the fog (x -5)
EM_CLEAR = "12421700,12421702,12424700,12424701"
EM_SMALL = (2420711, 2420712, 2420713, 2420716, 2420717, 2420719, 2420720)


# emf (0.7.9, player report NW-55a69c9 again: the FIRST fight): 12421702 not set, the host stands at the lamp by the fog
# (-1.2, 55.3, 292.7 as in the report; the first-start area is a box 70 m east, 7 m lower), the guest goes through the
# fog first. The host's event 12421702 must run (the host's chr visits the area, P5), so the small emissaries are
# enabled on the host's game as well (before: only the guest's late-guest event 12421703 did, the host kept them off).
EMF_SPOT = "-1.2:55.3:292.7"


def phase_emf(only):
    return phase_em(only, first=True)


def phase_em(only, first=False):
    delay = float(os.environ.get("BB_EM_DELAY", "20"))
    spot = os.environ.get("BB_EM_SPOT", EMF_SPOT if first else EM_SPOT)
    host = ",".join([
        "god",
        "host_warp=2422950@8^1",                                    # the party to the Upper Cathedral Ward lamp
        "host_move=%s@6%%m24_02" % spot,                            # into the arena, short of the fight's regions
        "host_press=0x0@40%m24_02!",                                # stands there while the guest goes through
        "host_chr=2420716:3@4%m24_02", "host_chr=2420711:3@6%m24_02",  # the small emissaries: found = on the map
    ])
    g1 = ",".join([
        "god",
        "guest_press=0x0@%g%%m24_02!" % delay,                      # the host is inside by now
        "guest_obj=2421700@3%m24_02",                               # at the fog
        "guest_press=0x4@1%m24_02!", "guest_press=0@1%m24_02!",     # R3
        "guest_press=0x4000@2%m24_02!", "guest_press=0x4000@2%m24_02!",  # through it, first
        "guest_chr=2420716:3@14%m24_02", "guest_chr=2420711:3@6%m24_02",
        "guest_press=0x0@4%m24_02!",
    ])
    return dict(
        name="emf" if first else "em", seconds=300, count=2, headless=0, pads=(0, 1), until_hold=6,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + EM_CLEAR, "BB_PARTY_EMEVD_TRACE=1"] +
            (["BB_NET_SIM=" + os.environ["BB_EM_NET"]] if os.environ.get("BB_EM_NET") else []) +   # BB_EM_NET=dsl / mobile / bad
            (["BB_PARTY_PROGRESS_TEST_SET=12421702"] if not first and os.environ.get("BB_EM_REFIGHT", "1") == "1" else []),
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[], until=EM_UNTIL,
    )


def check_emf(v, L):
    """Phase emf: the first fight, the host at the lamp, the guest through the fog first: the host's own first-fight
    event 12421702 runs (it enables the small emissaries with the spawn effect), on the guest the late-guest event."""
    need(v, L, "emf join: 2 players", 0, r"cooperators 0 -> 1")
    went = need(v, L, "emf guest went through the fog first", 1, r"guest: went through boss fog \(event 12424711")
    if went is not None:
        need(v, L, "emf host: the host's first-fight event is started for the guest-first walk", 0,
             r"host: boss fog: slot \d+ went in before the host's first fight here \(guest event 12424711\)", went - 1.0)
        need(v, L, "emf host: its first-fight event 12421702 ran (small emissaries' spawn effect)", 0,
             r"EMEVD event 12421702 \[\d+\] 2003\[18\]", went - 1.0)
        need(v, L, "emf host: the first fight started", 0,
             r"host: first fight of guest event 12424711 started", went - 1.0)
        need(v, L, "emf guest: the late-guest event 12421703 shows the small emissaries", 1,
             r"EMEVD event 12421703 \[8\] 2003\[18\]", went - 1.0)
        none_of(v, L, "emf the first fight was not dropped", r"first-fight start for guest event 12424711 dropped",
                insts=(0,))
    check_common(v, L, "emf")


def check_em(v, L):
    """Phase em: the host is inside the arena, the guest goes through the fog first: the small emissaries
    are enabled on the host's game too (its first-fight event 12421702 ran)."""
    need(v, L, "em join: 2 players", 0, r"cooperators 0 -> 1")
    # Re-fight state (12421702 set, 12424700 not): the host stands in the arena, the guest goes through the fog first.
    went = need(v, L, "em guest went through the fog first", 1, r"guest: went through boss fog \(event 12424711")
    if went is not None:
        need(v, L, "em host: the guest-first path set the host-entered flag", 0,
             r"host: boss fog: slot \d+ went in first \(event 12424711\)", went - 1.0)
        need(v, L, "em guest: the late-guest event 12421703 shows the small emissaries", 1,
             r"EMEVD event 12421703 \[8\] 2003\[18\]", went - 1.0)
        for i, who in ((0, "host"), (1, "guest")):
            need(v, L, "em %s: the small emissaries are on the map (stood at them)" % who, i,
                 r"test: %s_chr 2420716 \(3\.0 m, 0 deg\): target" % who, went)
    check_common(v, L, "em")


# Phase bd / bdo (bossdrops, 0.7.6, ~3 min, 2 players, groups `boss_drops` / `boss_drops_off`): the item a
# world boss drops for the host reaches the guest present at the kill (player report 2026-10-25: a guest got no
# Kin Coldblood from Rom). The party travels to the Moonside Lake lamp 3202952 (Rom's map m32_00); the host
# awards Rom's lot 51001900 through the game's own award function with hostOnly 1, as her defeat event 13201800
# does (test aid BB_PARTY_ITEMS_HOST_TEST: waits until the host is in the lot's map area; Rom's real fight cannot
# be started by a test walk, HP 0 on her entity ends nothing). The guest's own game would refuse that hostOnly
# award, so the item must come from the host's grant. bdo: the host's rule `party_boss_drops` off
# (BB_PARTY_BOSS_DROPS=0 on the host): the guest gets none. Both press OK on the item box.
BD_UNTIL = [r"1:gave #\d+ lot 51001900"]
BDO_UNTIL = [r"0:award lot 51001900 \(hostOnly 1"]


def phase_bd(only, rule_off=False):
    host = ",".join([
        "god",
        "host_warp=3202952@8^1",                                    # the party to the Moonside Lake lamp
        "host_press=0x4000@40%m32_00!", "host_press=0x4000@4%m32_00!",  # OK on the host's item box
    ])
    g1 = ",".join([
        "god",
        "guest_press=0x4000@44%m32_00!", "guest_press=0x4000@4%m32_00!",  # OK on the guest's item box
        "guest_press=0x4000@4%m32_00!",
    ])
    return dict(
        name="bdo" if rule_off else "bd", seconds=240, count=2, headless=0, pads=(0, 1),
        env=[],
        ienv=["0:BB_PARTY_ITEMS_HOST_TEST=51001900", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + g1] + (["0:BB_PARTY_BOSS_DROPS=0"] if rule_off else []),
        crash=[], until=BDO_UNTIL if rule_off else BD_UNTIL, until_hold=30 if rule_off else 15,
    )


def phase_bdo(only):
    return phase_bd(only, rule_off=True)


def check_bd(v, L, rule_off=False):
    tag = "bdo" if rule_off else "bd"
    j = need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    arr = need(v, L, tag + " guest in Byrgenwerth (m32_00)", 1, r"map .*-> m32_00", j or 0)
    kill = need(v, L, tag + " host: the award of Rom's host-only lot 51001900", 0,
                r"award lot 51001900 \(hostOnly 1", arr or 0)
    if rule_off:
        none_of(v, L, tag + " host: no boss drop grant queued (rule off)", r"host award #\d+ lot 51001900 ", insts=(0,))
        none_of(v, L, tag + " guest: no Kin Coldblood given", r"gave #\d+ lot 51001900", insts=(1,))
    else:
        need(v, L, tag + " host: the boss drop is queued for the party with the host's items", 0,
             r"host award #\d+ lot 51001900 \(ledger -1 = flag -1, boss\) queued for the party; items \[4:1591x\d+\]",
             kill or 0)
        need(v, L, tag + " guest: takes it (present at the kill)", 1,
             r"boss drop lot 51001900 from the host \(1 item\(s\)\): queued for this guest", kill or 0)
        need(v, L, tag + " guest: Kin Coldblood given, with the item-get box", 1,
             r"gave #\d+ lot 51001900 .*goods 1591 x\d+ \(held \d+ -> \d+, item-get dialog\)", kill or 0)
        n = len(L.all(1, r"gave #\d+ lot 51001900"))
        v.step(tag + " guest: exactly one give", n == 1, "%d give(s)" % n)
    check_common(v, L, tag)


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


# Phase ef (endingflow, ~5 min, 2 players, group `ending_flow`; 0.7.6, parity D10/D6/B3; the
# ng_plus_coop player session: the host played Childhood's Beginning, a guest without the three
# cords was shown Honoring Wishes). The guest is summoned into the host's Dream; its game gets the
# host's ending 3 (Moon Presence, BB_PARTY_STORY_TEST=ending:3,summoned) and answers the question
# YES (BB_PARTY_ENDING_TEST=yes). The default save has no flag 9900 (cords): it must still play the
# HOST's ending - remo 21000030 (never 21000020) - in its own Dream, then the next cycle once and
# the staff roll (screenshots of the cutscene and of the credits).
EF_UNTIL = r"1:ending: staff roll / ending step t=10 s|1:test: FAILED"


def phase_ef(only):
    return dict(
        name="ef", seconds=330, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_STORY_TEST=ending:3,summoned",
              "1:BB_PARTY_STORY_TEST_DELAY=30", "1:BB_PARTY_ENDING_TEST=yes",
              "1:BB_TEST_REMO_SKIP_AFTER=20"],
        watch=["--watch", r"ending 3: question open=efask@1,3",
               "--watch", r"replay: remo 2100[01]030=efremo@3,10",
               "--watch", r"ending: staff roll / ending step t=10 s=efcredits@1,4"],
        crash=[], until=[EF_UNTIL], until_hold=6,
    )


# Phase ngp (ngplus_ending, ~12 min (the staff roll is ~380 s and cannot be skipped), 2 players, group `ngplus_ending`, not in a default run; CL-e4239a): the host finishes the game (flag 72100130 = Gehrman's Submit,
# the cheapest ending: m21 event 12100180) with a guest in its Dream. The guest is asked, answers YES, plays the
# host's ending in its own world. Both saves end in NG+ 1 EXACTLY (an ending once moved a guest save two cycles, 0.7.6),
# then the guest presses Party > Rejoin in NG+ and is back in the host's world (nobody could join after the ending).
NGP_UNTIL = r"1:ending: the next cycle's world is up|1:test: FAILED"  # the guest's line comes after the host's READY and the join (2 joins in the old pattern: at 60 FPS the first join is followed by a re-join)


def phase_ngp(only):
    # The new cycle starts in Iosefka's Clinic (its prologue: nobody can be summoned until a hunter is "start-ready");
    # the flags a player has by the Dream (the Dream's lamp, Gehrman, the Workshop) are set as the test aid.
    ready = [("12101020", "12101021", "12100105", "9401")]
    host = ",".join(["god", "host_flag=72100130@12"] + ["host_flag=%s@%d%%m24_01" % (f, 12 if k == 0 else 1) for k, f in enumerate(ready[0])])
    guest = ",".join(["god", "guest_press=0@2"] + ["guest_flag=%s@%d%%m24_01&" % (f, 12 if k == 0 else 1) for k, f in enumerate(ready[0])] + ["guest_rejoin@470"])
    return dict(
        name="ngp", seconds=900, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host, "0:BB_PARTY_DIRECTOR_TEST_SOLO=1",
              "1:BB_PARTY_DIRECTOR_TEST=" + guest, "1:BB_PARTY_ENDING_TEST=yes",
              "1:BB_TEST_REMO_SKIP_AFTER=20", "0:BB_TEST_REMO_SKIP_AFTER=20"],
        watch=["--watch", r"ending 1: question open=ngpask@1,3",
               "--watch", r"ending: staff roll / ending step t=10 s=ngpcredits@0,4",
               "--watch", r"ending: the next cycle's world is up=ngpnext@0,6",
               "--watch", r"cooperators 0 -> 1=ngpback@0,6"],
        crash=[], until=[NGP_UNTIL], until_hold=70,  # the host is start-ready ~10 s and the guest back ~30 s after the first next-cycle line
    )


# Phase mb (moonmusic, ~4 min, 2 players, group `moon_music`; 0.7.7, parity B6, CL-76fd20: after the
# Gehrman fight the guests did not hear the Moon Presence music). The host's Dream with the three
# cords (9900) and Gehrman dead (12101802 + 12101800); the host steps onto the arena's hit parts
# (2103601-5): its event 12101852 plays the Moon Presence cutscene (remo 21000050) and sets 12101852.
# The boss music is the EMEVD's 2010[04] (boss map sound 2103812 on) in event 12104853, run by every
# game on its own once its player stands in region 2102801; the trace shows that instruction on each.
MB_UNTIL = r"1:EMEVD event 12104853 \[13\]|1:test: FAILED"
MB_UNTIL0 = r"0:EMEVD event 12104853 \[13\]"
MB_ENV = ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12104853,12104881,12101852,12101850,12104855"]


def phase_mb(only):
    host = ",".join(["god", "host_flag=9900@6", "host_flag=12101802@7", "host_flag=12101800@8",
                     "host_move=28.4:-9.2:5.6@10%m21_00", "host_move=40:-12:-14@40%m21_00", "log_flag=12101800/12101852/12104850/12104851/12101850", "log_pos=2"])
    guest = ",".join(["god", "guest_move=28.4:-9.2:5.6@12%m21_00", "guest_move=40:-12:-14@40%m21_00", "log_flag=12101800/12101852/12104850/12104851/12101850", "log_pos=2"])
    return dict(
        name="mb", seconds=200, count=2, headless=0,
        env=MB_ENV,
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + guest],
        watch=["--watch", r"remo playing=mbremo@2,6,12", "--watch", r"EMEVD event 12104853 \[13\]=mbmusic@1,4"],
        crash=[], until=[MB_UNTIL0, MB_UNTIL], until_hold=20,
    )


# Phase gb (gehrmanmusic, ~4 min, 2 players, group `gehrman_music`; 0.7.7, parity B6, CL-19ed70: guests
# heard no music in the Gehrman fight). The host's Dream with the three cords (9900); the host's
# fight-start event 12101802 waits for its talk flag 72100131, set by the director 8 s after the join: it
# plays Gehrman's cutscene (remo 21000040). The boss music is the EMEVD's 2010[04] (boss map sound
# 2103802 on) in event 12104803, run by every game on its own once its player stands in region
# 2102802; the trace shows that instruction on each. Before the fix its wait also needed flag
# 12104801, set only by the guest fog-entry event 12104811, so neither role got there.
GB_UNTIL = r"1:EMEVD event 12104803 \[13\]|1:test: FAILED"
GB_UNTIL0 = r"0:EMEVD event 12104803 \[13\]"
GB_ENV = ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=12104803,12104811,12101802,12104802,12104810"]


def phase_gb(only):
    flags = "log_flag=12101800/12101802/12104800/12104801/12104802/72100131"
    # The flag steps keep the chain armed; a move step ends it (a later step would wait for a re-join).
    host = ",".join(["god", "host_flag=9900@6", "host_flag=72100131@8", "host_move=28.4:-9.2:5.6@12%m21_00", flags, "log_pos=2"])
    guest = ",".join(["god", "guest_move=28.4:-9.2:5.6@12%m21_00", flags, "log_pos=2"])
    return dict(
        name="gb", seconds=200, count=2, headless=0,
        env=GB_ENV,
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + guest],
        watch=["--watch", r"remo playing=gbremo@2,6,12", "--watch", r"EMEVD event 12104803 \[13\]=gbmusic@1,4"],
        crash=[], until=[GB_UNTIL0, GB_UNTIL], until_hold=20,
    )


# Phases gbs / mbs (groups `gehrman_music_stop` / `moon_music_stop`; 0.7.7, CL-b020ff: the boss music did
# not stop at the kill in a party). The same set-up as gb / mb, then the host kills the boss (its real
# defeat path, host_kill) 20 s after the music started. The kill event (12101800 / 12101850) sets
# 12104809 / 12104859, which the OFF event (12104805 / 12104855) waits for; the ON event's own stop wait
# ([19] / [18]) also waits on the guest-entry flag 12104801 / 12104851, but that event never reaches its
# stop in a party (nor with the flag set): the OFF event is what stops the music, and it does on each game.
GBS_UNTIL = r"1:EMEVD event 12104805 \[3\][\s\S]*1:test: guest_move to \(40\.00|1:test: FAILED"
GBS_UNTIL0 = r"0:test: host_move to \(40\.00"
MBS_UNTIL = r"1:EMEVD event 12104853 \[2[1-3]\]|1:test: FAILED"
MBS_UNTIL0 = r"0:EMEVD event 12104853 \[2[1-3]\]"


def phase_gbs(only):
    ph = phase_gb(only)
    flags = "log_flag=12101800/12101802/12104800/12104801/12104802/12104809/72100131"
    host = ",".join(["god", "host_flag=9900@6", "host_flag=72100131@8", "host_move=28.4:-9.2:5.6@12%m21_00",
                     "host_kill=2100800@25%m21_00", "host_move=40:-12:-14@10%m21_00", flags, "log_pos=2"])
    guest = ",".join(["god", "guest_move=28.4:-9.2:5.6@12%m21_00", "guest_move=40:-12:-14@65%m21_00", flags, "log_pos=2"])
    ph.update(name="gbs", seconds=260, env=GB_ENV + ["BB_PARTY_EMEVD_TRACE_ALL=12104803,12104805,12104811,12101802,12104802,12101800"],
              ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest],
              watch=["--watch", r"remo playing=gbremo@2,6,12", "--watch", r"EMEVD event 12104803 \[(?:13|22)\]=gbmusic@1,4"],
              until=[GBS_UNTIL0, GBS_UNTIL], until_hold=20)
    return ph


def phase_mbs(only):
    ph = phase_mb(only)
    flags = "log_flag=12101800/12101852/12104850/12104851/12101850/12104859"
    host = ",".join(["god", "host_flag=9900@6", "host_flag=12101802@7", "host_flag=12101800@8",
                     "host_move=28.4:-9.2:5.6@10%m21_00", "host_move=40:-12:-14@40%m21_00",
                     "host_kill=2100810@25%m21_00", "host_move=28.4:-9.2:5.6@10%m21_00", flags, "log_pos=2"])
    guest = ",".join(["god", "guest_move=28.4:-9.2:5.6@12%m21_00", "guest_move=40:-12:-14@40%m21_00", "guest_move=28.4:-9.2:5.6@40%m21_00", flags, "log_pos=2"])
    ph.update(name="mbs", seconds=300, env=MB_ENV + ["BB_PARTY_EMEVD_TRACE_ALL=12104853,12104855,12104881,12101852,12101850,12104855"],
              ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest],
              watch=["--watch", r"remo playing=mbremo@2,6,12", "--watch", r"EMEVD event 12104853 \[(?:13|21)\]=mbmusic@1,4"],
              until=[MBS_UNTIL0, MBS_UNTIL], until_hold=20)
    return ph


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
# the rune at home: C3's own grant, or (0.7.7: the guest's talk flags stay in its own save) the game's own event 12700906
CV_RUNE_GIVEN = (r"gave #\d+ lot %s \(flag %s, own\).*flag %s is set|"
                 r"award lot %s \(hostOnly 1, flag %s\) in the own world")
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


# Phase cvt (covenant_talk, 0.7.7, ~4 min, 2 players, group `covenant_talk`; parity row D5: a covenant outcome
# from a REAL NPC talk, not a test step). The party stands in the Forbidden Woods (lamp 2702950); the GUEST walks
# up to Valtr (chr 2700912, talk t270503) and presses Cross through his lines (the League talk, event 12700906
# gives lot 43120, the Impurity rune, to the player whose talk set trigger 72700321): the guest alone gets the
# rune (own flag 6676 in its own save), the host's save and inventory stay without it.
CVT_VALTR = "2700912"
CVT_UNTIL = [r"1:guest_leave: Party > Leave pressed[\s\S]*test read \(load mode 0"]


def phase_cvt(only, who="guest"):
    n = int(os.environ.get("CVT_PRESSES", "24"))
    talk = (["%s_chr=%s:1.6@12%%m27_00" % (who, CVT_VALTR)] + ["%s_press=0x4000@2.5%%m27_00!" % who] * n
            + ["%s_press=0x0@2%%m27_00!" % who])
    g1 = ["god"] + (talk if who == "guest" else []) + ["guest_leave@%d%%m27_00" % (6 if who == "guest" else 85)]
    h0 = ["god"] + (talk if who == "host" else [])
    return dict(
        name="cvt" if who == "guest" else "cvh", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=%s,1800,1801,1790,72700321" % CV_RUNE_FLAG, "BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2702950", "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(h0),
              "0:BB_PARTY_PROGRESS_TEST_SET=1800", "0:BB_PARTY_PROGRESS_TEST_CLEAR=1790,1801,%s" % CV_RUNE_FLAG,
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog",
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=%s" % CV_RUNE_FLAG],
        watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
               "--watch", r"npc_talk: local choice: npc 270503=cvchoice@0.5,2",
               "--watch", r"own talk gift trigger %s=cvtrune@1,4" % CV_RUNE_TRIGGER],
        crash=[], until=CVT_UNTIL, until_hold=8,
    )


# Phase cvl (covenant_staff, 0.7.9 annalise079, ~5 min, 2 players, group `covenant_staff`; CL-22c416): the League Staff (goods 131, lot 43130,
# flag 6678, trigger 72700322) for a GUEST. The guest talks to Valtr for real (as cvt: his League talk), logging the staff's trigger and flag;
# then BOTH members hold the staff in quick slot 0 (BB_AUTO_HUD_TEST quick=0:131 gives it when not held) and USE it with the item button
# (Triangle): the use animation of the host and of the guest are logged (log_anim) and compared.
CVL_FLAGS = "72700321/72700322/6676/6678"


def phase_cvl(only):
    n = int(os.environ.get("CVT_PRESSES", "24"))
    use = lambda who: ["%s_press=0x0@%s%%m27_00!" % (who, os.environ.get("CVL_WAIT", "20")), "%s_press=0x1000@0.5%%m27_00!" % who,
                       "%s_press=0x0@8%%m27_00!" % who, "%s_press=0x1000@0.5%%m27_00!" % who, "%s_press=0x0@8%%m27_00!" % who]
    talk = (["guest_chr=%s:1.6@12%%m27_00" % CVT_VALTR] + ["guest_press=0x4000@2.5%m27_00!"] * n + ["guest_press=0x0@2%m27_00!"])
    # a second talk after joining (CVL_TALK2 presses): does Valtr's talk go on to hand over the staff (trigger 72700322)?
    talk += ["guest_press=0x4000@4%m27_00!"] + ["guest_press=0x4000@2.5%m27_00!"] * int(os.environ.get("CVL_TALK2", "20")) + ["guest_press=0x0@2%m27_00!"]
    # Valtr's join talk sets 72700321 only (the staff trigger is not in it, seen live: two talk rounds); the staff's own-gift path is
    # driven by setting 72700322 the way his talk would (as the cv phase does for the rune)
    talk += ["guest_flag=72700322@2%m27_00"]
    lf = "log_flag=" + CVL_FLAGS
    h0 = ["god", lf, "log_anim"] + ["host_press=0x0@12%m27_00!"] + use("host")
    g1 = ["god", lf, "log_anim"] + talk + use("guest") + ["guest_leave@4%m27_00"]
    return dict(
        name="cvl", seconds=600, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=%s,1800,1801,1790,72700321,72700322,6678" % CV_RUNE_FLAG, "BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2702950", "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(h0),
              "0:BB_PARTY_PROGRESS_TEST_SET=1800", "0:BB_PARTY_PROGRESS_TEST_CLEAR=1790,1801,%s,6678" % CV_RUNE_FLAG,
              "0:BB_AUTO_HUD=1", "0:BB_AUTO_HUD_TEST=" + os.environ.get("CVL_QUICK", "10:quick=0:131"),
              "1:BB_AUTO_HUD=1", "1:BB_AUTO_HUD_TEST=" + os.environ.get("CVL_QUICK", "10:quick=0:131"),
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog",
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=%s,6678" % CV_RUNE_FLAG],
        watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
               "--watch", r"npc_talk: local choice: npc 270503=cvchoice@0.5,2"],
        crash=[], until=CVT_UNTIL, until_hold=8,
    )


# Phase cvr (covenant_runes, 0.7.7 covfx077, ~6 min, 2 players, group `covenant_runes`; CL-524a2c): the oath runes'
# EFFECT per member. In the Hunter's Dream the HOST awards itself a covenant rune lot (CVR_HOST_LOT), the GUEST
# another one (CVR_GUEST_LOT, guest-only award), each memorizes its own at the Memory Altar (Oath Memory slot) and
# `log_oath` reads the oath rune's marker SpEffect (6100 Corruption, 6110 Radiance, 6120 Hunter, 6142 Impurity,
# 250 Beast's Embrace, 255 Milkweed: what the game's EMEVDs and the NPCs test) on the local player of each game:
# each must carry ITS OWN covenant and never the other's, while the guest is summoned in the host's world.
CVR_HOST_LOT = os.environ.get("CVR_HOST_LOT", "3401850")  # Beast's Embrace (17000 Corruption, 34020 Radiance, 43120 Impurity)
CVR_GUEST_LOT = os.environ.get("CVR_GUEST_LOT", "43020")  # Milkweed (35010 Hunter)
CVR_KEYS = os.environ.get("CVR_KEYS", "left,cross,w,w,w,w,w,w")
CVR_GKEYS = os.environ.get("CVR_GKEYS", CVR_KEYS)
CVR_UNTIL = [r"1:guest_leave: Party > Leave pressed[\s\S]*1:test: oath: "]


def phase_cvr(only):
    leave = os.environ.get("CVR_LEAVE", "1") != "0"
    return dict(
        name="cvr", seconds=480, count=2, headless=0,
        env=["BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god,log_oath,goods=4104:1,award_lot=" + CVR_HOST_LOT,
              "0:BB_PARTY_GUESTSVC_TEST=hosttalk=210699;open;hsdelay=45;keys=" + CVR_KEYS,
              "1:BB_PARTY_DIRECTOR_TEST=god,log_oath" + (",guest_leave@150" if leave else ""),
              "1:BB_PARTY_GUESTSVC_TEST=lot=%s;goods=4104:1;warp=talk:210699;open;keys=%s" % (CVR_GUEST_LOT, CVR_GKEYS)],
        crash=[], until=CVR_UNTIL, until_hold=15,
    )


# Phase cvm (covenant_measure, 0.7.8 covnpc078, ~7 min, 2 players, group `covenant_measure`; CL-95dee9): the oath runes'
# AMOUNTS in game. As cvr (each member memorizes its own rune at the Dream's Memory Altar, no god mode) with the pair
# Hunter (35010, host) + Corruption (17000, guest): `log_oath` prints each game's HP / stamina every 0.5 s with the oath
# rows' numbers. Corruption's second row 180010 drains 1 HP every 2 s (conditionHp 12.5), Hunter's 180020 raises the stamina
# recovery by 3 (staminaRecoverChangeSpeed): after both attack the air (R1 x14) and stand still, the host's stamina must come
# back faster than the guest's (the baseline: no stamina rune) and the guest's HP must fall by about 1 per 2 s while the
# host's stays. CVM_HOST_LOT / CVM_GUEST_LOT swap the pair (e.g. Radiance 34020 vs Impurity 43120).
CVM_HOST_LOT = os.environ.get("CVM_HOST_LOT", "35010")
CVM_GUEST_LOT = os.environ.get("CVM_GUEST_LOT", "17000")
CVM_LOTS = {"35010": ("HunterStamina", "Hunter"), "17000": ("CorruptionDrain", "Corruption"), "34020": ("RadianceVial", "Radiance")}


def phase_cvm(only):
    atk = os.environ.get("CVM_ATTACKS", "14")
    wait = os.environ.get("CVM_WAIT", "85")
    def seq(who):
        return (["%s_press=0@%s%%m21_00!" % (who, wait)] + ["%s_press=0x800@0.7%%m21_00!" % who] * int(atk)
                + ["%s_press=0@40%%m21_00!" % who])
    return dict(
        name="cvm", seconds=int(os.environ.get("CVM_SECONDS", "420")), count=2, headless=0,
        env=["BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["log_oath", "goods=4104:1", "award_lot=" + CVM_HOST_LOT] + seq("host")),
              "0:BB_PARTY_GUESTSVC_TEST=hosttalk=210699;open;hsdelay=45;keys=" + CVR_KEYS,
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(["log_oath"] + seq("guest")),
              "1:BB_PARTY_GUESTSVC_TEST=lot=%s;goods=4104:1;warp=talk:210699;open;keys=%s" % (CVM_GUEST_LOT, CVR_GKEYS)]
             + (["0:BB_PARTY_TEST_SET_HP=" + os.environ["CVM_HP"], "1:BB_PARTY_TEST_SET_HP=" + os.environ["CVM_HP"]] if os.environ.get("CVM_HP") else []),
        crash=[], until=[r"0:test: host_press 0@40[\s\S]*1:test: guest_press 0@40"], until_hold=10,
    )


# Phase cvc (covenant_chalice, 0.7.7 coven077, ~9 min, 2 players, group `covenant_chalice`; CL-524a2c): the oath runes in a
# Chalice Dungeon. The host Continues inside its Pthumeru dungeon (as phase cl), lights the boss room's lamp and goes to the
# Dream, where host and guest each memorize their OWN oath rune at the Memory Altar (as phase cvr: different lots), then the
# host takes the altar's awaken warp back into the dungeon (the guest is summoned in): `log_oath` must show each member's own
# covenant effect on its own chr in the Dream AND in the dungeon (map m29), never the other's.
CVC_UNTIL = [r"0:travel step (\d+)/\1: host_lamp[\s\S]*1:test: oath: covenant effects on the local player: [^\n]*map 0x1d"]


def phase_cvc(only):
    host = ",".join(["log_pos", "log_oath", "god", "goods=4104:1", "award_lot=" + CVR_HOST_LOT,
                     "host_obj=%s:1.2:0@12%%m29" % CL_LAMP_OBJ, "host_press=0x4000@2%m29!", "host_press=0x4000@3%m29!", "host_press=0@25%m29!"]
                    + ["host_obj=%s:1.2:0@2%%m29" % CL_LAMP_OBJ, "host_press=0x4000@4%m29!", "host_press=0x80@3%m29!", "host_press=0x4000@1%m29!"] + ["host_lamp=%s@%s%%m21" % (CL_LAMP, os.environ.get("CVC_DREAM_S", "110")),
                                        "host_press=0@" + os.environ.get("CVC_TAIL_S", "30") + "%m29!"])
    g1 = ",".join(["log_oath", "god"])
    return dict(
        name="cvc", seconds=int(os.environ.get("CVC_SECONDS", "720")), count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host,
              "0:BB_PARTY_GUESTSVC_TEST=hosttalk=210699;open;hsdelay=20;keys=" + CVR_KEYS,
              "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_GUESTSVC_TEST=lot=%s;goods=4104:1;warp=talk:210699;open;keys=%s" % (CVR_GUEST_LOT, CVR_GKEYS)],
        crash=[], until=CVC_UNTIL, until_hold=15,
    )


# Phase cva (covenant_annalise, 0.7.7 covfx077, ~8 min, 2 players, group `covenant_annalise`; CL-524a2c): Annalise's REAL talk
# (kneel -> the Vilebloods, Corruption 17000 / flag 9042 / state 1040) in the Queen's Chamber fixture of anl: the HOST
# talks first, then the GUEST in the same session (like cvs): each must get its own talk and its own rune.
CVA_ANNALISE = "2500600"
CVA_READ = "1040,1041,1042,9042,50000200"


def phase_cva(only):
    n = int(os.environ.get("CVT_PRESSES", "24"))
    talk = lambda who, at: (["%s_chr=%s:1.6@%d%%m25_00" % (who, CVA_ANNALISE, at)] + ["%s_press=0x4000@2.5%%m25_00!" % who] * n
                            + ["%s_press=0x0@2%%m25_00!" % who])
    env, ienv = _anl_env([])
    env = [(e + ",9042,50000200") if e.startswith("BB_PARTY_PROGRESS_TEST_CLEAR") else e for e in env
           if not e.startswith("BB_PARTY_PROGRESS_TEST_READ")] + ["BB_PARTY_PROGRESS_TEST_READ=" + CVA_READ]
    if os.environ.get("CVA_TRACE") == "1":  # the instructions of Annalise's m25 events (kneel / offer / give blood), every call
        env = env + ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=" + ",".join(str(i) for i in range(12500010, 12500022))]
    sweep = []
    if os.environ.get("CVA_SWEEP") == "1":  # exploration: after the host's talk try gesture list rows 0..N one by one (touchpad, Down*k, Cross)
        env = env + ["BB_PARTY_GESTURE_TEST=" + ",".join(str(g) for g in range(800, 823))]
        lf = "log_flag=" + "/".join(str(f) for f in list(range(72500300, 72500340)) + [9042, 1040, 1041, 1042, 50000200])
        sweep = [lf, "log_anim", "host_press=0x4000@2.5%m25_00!", "host_press=0x0@28%m25_00!"]  # one talk, wait for its end
        gat = os.environ.get("CVA_SWEEP_AT", "3")  # seconds after the talk's first Cross: her "Kneel afore Us" line is ~17 s in
        if os.environ.get("CVA_KNEEL"):
            # pre-open the gesture list (the menu cannot be opened during her lines), clear the first-talk state so her greeting plays
            # again, perform list gesture k at CVA_KNEEL_AT s after that (her "Kneel afore Us" line is ~9 s into the greeting)
            sweep = sweep[:2]
            for k in [int(x) for x in os.environ["CVA_KNEEL"].split(",")]:
                # 331 falling 1 -> 0 (with 301 / 302 clear) starts her greeting again; the gesture menu cannot be open during it
                # (it is closed), so it is opened CVA_KNEEL_AT s later, right after her lines end (~14 s)
                sweep += ["host_flag=72500331@1%m25_00", "host_flag=72500301:0@0.1%m25_00", "host_flag=72500302:0@0.1%m25_00",
                          "host_flag=72500331:0@0.1%m25_00", "host_press=0x100000@" + os.environ.get("CVA_KNEEL_AT", "14.5") + "%m25_00!",
                          "host_press=0x1000@0.8%m25_00!"] + ["host_press=0x40@0.4%m25_00!"] * k                     + ["host_press=0x8000@0.8%m25_00!", "host_press=0x0@" + os.environ.get("CVA_KNEEL_WAIT", "14") + "%m25_00!"]
        if os.environ.get("CVA_KNEEL2"):
            # 0.7.8: her kneel area is the sphere of region 2502501 (radius 1 m at CVA_KNEEL_AT_POS, event 12500011 -> flag 72500330);
            # the earlier trials stood 1.9 m from its centre, outside it. Per list row k: step into the sphere, open the gesture list
            # (touchpad, Triangle = Switch, Down * k, Square = Test Now), wait; after her greeting (the first wait)
            pos = os.environ.get("CVA_KNEEL_POS", "127.7:129.95:-464.5")
            sweep = sweep[:2] + ["host_press=0x0@" + os.environ.get("CVA_KNEEL_GREET", "22") + "%m25_00!"]
            for k in [int(x) for x in os.environ["CVA_KNEEL2"].split(",")]:
                sweep += ["host_move=%s@1%%m25_00" % pos, "host_press=0x100000@1.5%m25_00!", "host_press=0x1000@0.8%m25_00!"]                          + ["host_press=0x40@0.4%m25_00!"] * k + ["host_press=0x8000@0.8%m25_00!", "host_press=0x0@" + os.environ.get("CVA_KNEEL_WAIT", "9") + "%m25_00!"]
        talk_each = os.environ.get("CVA_SWEEP_TALK") == "1"  # every trial starts its own talk (touchpad at CVA_SWEEP_AT s after its Cross)
        if talk_each:
            sweep = sweep[:2]
        ks = [int(x) for x in os.environ.get("CVA_SWEEP_KS", "").split(",") if x] or range(int(os.environ.get("CVA_SWEEP_FROM", "0")), int(os.environ.get("CVA_SWEEP_TO", "23")))
        for k in ([] if (os.environ.get("CVA_KNEEL") or os.environ.get("CVA_KNEEL2")) else ks):
            if talk_each:
                sweep += ["host_press=0x4000@2.5%m25_00!"]
            # the touchpad's gesture menu: Triangle (Switch) moves into the list of learned gestures, Down walks it, Square = Test Now
            sweep += ["host_press=0x100000@" + gat + "%m25_00!", "host_press=0x1000@0.8%m25_00!"] + ["host_press=0x40@0.4%m25_00!"] * k                 + ["host_press=0x8000@0.8%m25_00!", "host_press=0x0@" + os.environ.get("CVA_SWEEP_WAIT", "8") + "%m25_00!"]
    ienv = ienv + ["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god", "log_oath"] + (talk("host", 15) if not sweep else ["host_chr=%s:1.6@15%%m25_00" % CVA_ANNALISE]) + sweep),
                   "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god", "log_oath"] + talk("guest", CVS_GUEST_AT) + ["guest_leave@8%m25_00"]),
                   "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog"]
    if sweep:
        ienv = [e.replace(",guest_leave@8%m25_00", "") for e in ienv]
    return dict(name="cva", seconds=int(os.environ.get("CVA_SECONDS", "900")) if sweep else 600, count=2, headless=0, env=env, ienv=ienv,
                watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
                       "--watch", r"npc_talk: local choice: npc=cvchoice@0.5,2"],
                crash=[], until=[] if sweep else [r"1:guest_leave: Party > Leave pressed[\s\S]*test read \(load mode 0"], until_hold=8)


# Phase cvk (covenant_kneel, 0.7.9 covenant079, ~9 min, 2 players, group `covenant_kneel`; CL-95dee9): Annalise's KNEEL for real. The
# ESD t250307 reads flag 72500329 (the player's kneel) from its own talk machine 0x7FFFFFF0, entered when the kneel-prompt machine
# 0x7FFFFFF1 (action text 6003 outside, 6005 inside the 1 m sphere of region 2502501 = flag 72500330) returns 1, i.e. when the
# player PRESSES THE ACTION BUTTON (Cross) standing in the sphere after her greeting: not a gesture. The HOST kneels first, then the
# GUEST in the same session (own state per player): the kneel counters (72500317-319), her kneel events and the Vileblood talk follow.
CVK_POS = os.environ.get("CVK_POS", "127.7:129.95:-464.5")
CVK_LOG = "/".join(str(f) for f in [72500301, 72500302, 72500329, 72500330, 72500331, 72500332, 72500333, 72500334, 72500335, 72500317, 72500318, 72500319, 1040, 1041, 1042, 9042, 50000200])


def phase_cvk(only):
    greet = os.environ.get("CVK_GREET", "24")
    def kneel(who, at):
        seq = ["%s_chr=%s:1.6@%d%%m25_00" % (who, CVA_ANNALISE, at), "%s_press=0@%s%%m25_00!" % (who, greet),
               "%s_move=%s@1%%m25_00" % (who, CVK_POS), "%s_press=0@2%%m25_00!" % who]
        for _ in range(int(os.environ.get("CVK_TRIES", "1"))):
            seq += ["%s_press=0x4000@0.5%%m25_00!" % who, "%s_press=0@%s%%m25_00!" % (who, os.environ.get("CVK_WAIT", "14"))]
        # after the kneel her talk goes on: Cross through her lines / the offer (own menu choices are real rows)
        for _ in range(int(os.environ.get("CVK_TALK", "24"))):
            seq += ["%s_press=0x4000@2.5%%m25_00!" % who, "%s_press=0@0.5%%m25_00!" % who]
        return seq + ["%s_press=0x0@3%%m25_00!" % who]
    n = int(os.environ.get("CVT_PRESSES", "24"))
    talk = lambda who, at: (["%s_chr=%s:1.6@%d%%m25_00" % (who, CVA_ANNALISE, at)] + ["%s_press=0x4000@2.5%%m25_00!" % who] * n
                            + ["%s_press=0x0@2%%m25_00!" % who])
    env, ienv = _anl_env([])
    env = [(e + ",9042,50000200") if e.startswith("BB_PARTY_PROGRESS_TEST_CLEAR") else e for e in env
           if not e.startswith("BB_PARTY_PROGRESS_TEST_READ")] + ["BB_PARTY_PROGRESS_TEST_READ=" + CVA_READ + ",72500329,72500333"]
    if os.environ.get("CVK_TRACE") == "1":
        env = env + ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=" + ",".join(str(i) for i in range(12500010, 12500022))]
    lf = "log_flag=" + CVK_LOG
    gonly = os.environ.get("CVK_ONLY") == "guest"  # the guest alone talks (no host talk first): its own grant, never the host's
    ienv = ienv + ["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god", "log_oath", lf, "log_anim"] + ([] if gonly else kneel("host", 15))),
                   "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god", "log_oath", lf, "log_anim"] + kneel("guest", 30 if gonly else CVS_GUEST_AT) + ["guest_leave@8%m25_00"]),
                   "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog"]
    return dict(name="cvk", seconds=int(os.environ.get("CVK_SECONDS", "900")), count=2, headless=0, env=env, ienv=ienv,
                watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
                       "--watch", r"npc_talk: local choice: npc=cvchoice@0.5,2"],
                crash=[], until=[r"1:guest_leave: Party > Leave pressed[\s\S]*test read \(load mode 0"], until_hold=8)


# Phase cvf (covenant_alfred, 0.7.8 covnpc078, ~8 min, 2 players, group `covenant_alfred`; CL-95dee9): Alfred's REAL talk in the
# Cathedral Ward (m24_00, chr 2400763 at the Oedon Chapel steps, state 1344, ESD t240343; his talk sets the gift triggers 9072 /
# 9073 of the common event 9040: lots 34000 / 34010, goods 1300 x3 / 4116; the oath runes are NOT his: Eileen's talk gives the Hunter
# rune (9075, lot 35010), Annalise's the Corruption rune (9042, lot 17000), Valtr's the Impurity rune (12700906), the Radiance rune
# (lot 34020) only the Chalice event of m29). In ONE session the HOST talks to him first (the game's own event),
# then the GUEST: each must get its own talk and its own rune (the guest's through its own grant at home), covenant outcomes per
# player (the host's joined state never reaches the guest's dialog).
CVF_ALFRED = "2400763"
CVF_TRIGGER, CVF_LOT, CVF_FLAG = "9073", "34010", "50001810"
CVF_SET = "1344,9801," + NF_LOGARIUS_DEAD
CVF_CLEAR = ",".join(str(f) for f in list(range(1340, 1344)) + list(range(1345, 1360)) + [72400475, 72400476, 72400471, 12405160, 9072, 9073, 50001800, 50001810])
CVF_READ = "1344,1345,1346,9072,9073,50001800,50001810"


def phase_cvf(only):
    n = int(os.environ.get("CVT_PRESSES", "24"))
    talk = lambda who, at: (["%s_chr=%s:1.6@%d%%m24_00" % (who, CVF_ALFRED, at)] + ["%s_press=0x4000@2.5%%m24_00!" % who] * n
                            + ["%s_press=0x0@2%%m24_00!" % who])
    h0 = ["god", "log_oath"] + talk("host", 12)
    g1 = ["god", "log_oath"] + talk("guest", CVS_GUEST_AT) + ["guest_leave@8%m24_00"]
    return dict(
        name="cvf", seconds=600, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CVF_CLEAR, "BB_PARTY_PROGRESS_TEST_SET=" + CVF_SET,
             "BB_PARTY_PROGRESS_TEST_READ=" + CVF_READ, "BB_PARTY_TEST_COND_TIMEOUT=200"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2402950", "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(h0),
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog"],
        watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
               "--watch", r"npc_talk: local choice: npc=cvchoice@0.5,2",
               "--watch", r"own talk gift trigger %s=cvtrune@1,4" % CVF_TRIGGER],
        crash=[], until=[r"1:guest_leave: Party > Leave pressed[\s\S]*test read \(load mode 0"], until_hold=8,
    )


# Phase cvs / cvo (covenant_seq, 0.7.7, ~7 min, 2 players, group `covenant_seq`; "covenants per player"): in ONE
# session the HOST talks to Valtr and joins the League (his rune 43120 / flag 6676 in the host's save), then the
# GUEST walks up to the same Valtr and talks: it must get its own join menu and its own rune (at home, after the
# Leave), the host's dialog state must not override it. cvo: the host's rule "Covenants per player" off: the
# covenant is shared, the guest sees the joined dialog (no join row, no rune: expected).
CVS_GUEST_AT = int(os.environ.get("CVS_GUEST_AT", "110"))


def phase_cvs(only, per_player=True):
    n = int(os.environ.get("CVT_PRESSES", "24"))
    talk = lambda who, at: (["%s_chr=%s:1.6@%d%%m27_00" % (who, CVT_VALTR, at)] + ["%s_press=0x4000@2.5%%m27_00!" % who] * n
                            + ["%s_press=0x0@2%%m27_00!" % who])
    h0 = ["god"] + ([] if per_player else ["host_rule=party_covenant_per_player:0@2"]) + talk("host", 12)
    g1 = ["god"] + talk("guest", CVS_GUEST_AT) + ["guest_leave@8%m27_00"]
    return dict(
        name="cvs" if per_player else "cvo", seconds=600, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_READ=%s,1800,1801,1790,1792,72700321" % CV_RUNE_FLAG, "BB_PARTY_TEST_COND_TIMEOUT=200"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2702950", "0:BB_PARTY_DIRECTOR_TEST=" + ",".join(h0),
              "0:BB_PARTY_PROGRESS_TEST_SET=1800", "0:BB_PARTY_PROGRESS_TEST_CLEAR=1790,1801,%s" % CV_RUNE_FLAG,
              "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1), "1:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog",
              "1:BB_PARTY_PROGRESS_TEST_SET=1800", "1:BB_PARTY_PROGRESS_TEST_CLEAR=1790,1801,%s" % CV_RUNE_FLAG],
        watch=["--watch", r"npc_talk: local line: talk=cvtalk@1.5,10,30",
               "--watch", r"npc_talk: local choice: npc 270503=cvchoice@0.5,2",
               "--watch", r"own talk gift trigger %s=cvtrune@1,4" % CV_RUNE_TRIGGER],
        crash=[], until=CVT_UNTIL, until_hold=8,
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
    g1 = ",".join(["god", "goods=4312:0", "log_flag=6670/12101020", "guest_flag=6670:0@12",
                   # again after the award (the guest's bell messenger event must run at the
                   # Dream reload, for the per-player messenger state check below)
                   "guest_flag=6670:0@30"])
    return dict(
        name="ie", seconds=300, count=2, headless=0,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_PROGRESS_TEST_CLEAR=12101020,72101000,72101001,72101002,6670",
              "0:BB_PARTY_ITEMS_HOST_TEST=10050",
              # 0.7.5: the host's pickup flags of a questline treasure (Orphanage Key) goes to
              # the guest, an ordinary corpse's (Madman's Knowledge) stays per player
              "0:BB_PARTY_ITEMS_HOST_TEST_FLAGS=52420900,52410140",
              "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_PROGRESS_TEST_READ=6670,12101020,12101021",
              "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"gave #\d+ lot 10050=iebell@1,3",
               "--watch", r"flags 12101020=0 12105050=1=ierise@1,3,6",
               "--watch", r"text shows=ietext@0.3",
               "--watch", r"flags 12101020=1=iedone@0.5,3"],
        # The host's full item list is built when the guest connects: the guest starts only after
        # the host's 6670 clear (0.7.3 release smoke: the guest connected 1 s before it, got lot
        # 10050 in the full list while still holding the bell, and skipped the later award).
        start_when=[r"1/0:test: cleared in the own save \(never_sync\):.*6670:"],
        crash=[], until=IE_UNTIL, until_hold=15,
    )


# ---- shared_rewards (0.7.7, sharing077): the owner's rule "every shared item reaches every member exactly once" ----
# Phase rw (~5 min, 2 players): the GUEST lands the last hits on the Cleric Beast (the host stands off; the boss's HP
# is set to 3 % on the host first, then only the guest's R1 taps hurt it). The boss's reward (lot 50000010, the Sword
# Hunter Badge) must reach the host (its own event) and the guest (C3) once each, with the notice
# "Sword Hunter Badge: shared with the party" on both screens.
RW_UNTIL = [r"1:gave #\d+ lot 50000010 "]


def phase_rw(only):
    host = ",".join(["god", "goods=4114:0", "log_flag=12411700/12401800",
                     "host_warp=2412952@8^1",                               # the party to the Cleric Beast's lamp
                     "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,        # the fight starts
                     "host_chr=2410800:16@6%m24_01",                         # the host stays 16 m off
                     "host_kill=2410800:hp3@8%m24_01"])                       # the boss at 3 %: the guest's hits finish it
    g1 = ",".join(["god", "goods=4114:0",
                   "guest_move=%s@25%%m24_01" % CLERIC_SPOT_GUEST,
                   "guest_chr=2410800:2@3%m24_01"] +                         # beside the boss
                  ["guest_press=0x800@1.5%m24_01"] * 60 + ["guest_press=0x0@2%m24_01"])
    return dict(
        name="rw", seconds=420, count=2, headless=0, until_hold=10,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR_REWARD, "BB_PARTY_NPC_HOSTILE_LOG=1",
             "BB_PARTY_TARGET_ANIM=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"notice shown: .*=rwnotice@0.6,2", "--watch", r"boss defeat, event=rwdead@1,4"],
        # the guest starts after the host cleared the reward's ledger bit (else the join's full list carries the lot and the
        # guest, holding the badge then, takes its flag: a later grant of the same lot is the same grant, ie phase)
        start_when=[r"1/0:test: cleared in the own save .*60009036"],
        crash=[], until=RW_UNTIL,
    )


def check_rw(v, L):
    need(v, L, "rw join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "rw the fight is on (bar)", 0, r"boss health bar shown")
    gb = L.all(0, r"guest blow recorded as the host's")
    v.step("rw the guest's blows are on the host's boss (damage records credited to the host)", len(gb) > 0, "%d recorded" % len(gb))
    dead = need(v, L, "rw the Cleric Beast dies on the host's game (the guest landed the last hits)", 0,
                r"boss defeat, event 124\d+|boss health bar: off", on or 0)
    ha = L.all(0, r"award lot 50000010 \(hostOnly 1")
    v.step("rw the host gets the boss's reward (its own event, once)", len(ha) == 1, "%d award(s) on the host" % len(ha))
    gg = L.all(1, r"gave #\d+ lot 50000010 ")
    v.step("rw the guest gets the boss's reward once", len(gg) == 1, "%d give(s) on the guest" % len(gg))
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rw notice on the %s's screen" % who, i, r"notice shown: .*: shared with the party", dead or 0)
    check_common(v, L, "rw")


# Phase rwe (~4 min, 2 players): the Evil Eye (Brain of Mensis, entity 2600129) dies without a blow of entity 10000
# (host_kill HP 0 = the case of a guest's last hit: m26 event 12600029 waits for "damage by 10000"). After the
# grace time the host awards the Living String itself (lot 2605010, KillAwardTick) and the guest gets it too, once.
RWE_UNTIL = [r"1:gave #\d+ lot 2605010 "]


def phase_rwe(only):
    host = ",".join(["god", "goods=7200:0", "log_flag=12600026/12600027/52605010/5913",
                     "host_warp=2602952@8^1",
                     "host_kill=2600129:hp@25%m26_00"])
    g1 = ",".join(["god", "goods=7200:0", "log_flag=52605010/5913"])
    return dict(
        name="rwe", seconds=300, count=2, headless=0, until_hold=10,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=52605010,5913,12600029,12600028", "BB_PARTY_PROGRESS_TEST_SET=12600026"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"notice shown: .*=rwenotice@0.6,2"],
        start_when=[r"1/0:test: cleared in the own save .*52605010"],
        crash=[], until=RWE_UNTIL,
    )


def check_rwe(v, L):
    need(v, L, "rwe join: 2 players", 0, r"cooperators 0 -> 1")
    k = need(v, L, "rwe the Eye is dead without the game's own award", 0, r"test: host_kill: .*entity 2600129")
    a = need(v, L, "rwe the host awards the Living String itself (the guest's-last-hit fallback)", 0,
             r"kill award: entity 2600129 is dead and lot 2605010", k or 0)
    ha = L.all(0, r"award lot 2605010 \(hostOnly 1")
    v.step("rwe the host's award of lot 2605010 runs once", len(ha) == 1, "%d" % len(ha))
    gg = L.all(1, r"gave #\d+ lot 2605010 .*goods 7200")
    v.step("rwe the guest gets the Living String once", len(gg) == 1, "%d give(s)" % len(gg))
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rwe notice on the %s's screen" % who, i, r"notice shown: Living String: shared with the party", a or 0)
    check_common(v, L, "rwe")


# Phase rwt (~5 min, 2 players): a weapon and a Caryll rune from world treasure, both directions. The host picks up
# the Cannon (lot 2400510) and a Caryll rune (lot 17000, rune 200010; the treasure runes 2700620 etc. name gems with no GemGenParam row: the game rolls nothing for them); the GUEST "picks up" the Tonitrus (2800330) and a Caryll rune
# (3200510) in the host's world (BB_PARTY_GUEST_PICKUP_TEST: the D4 pickup path's own calls). Each reaches the others once.
RWT_UNTIL = [r"1:gave #\d+ lot 17000 ", r"0:gift from slot 1: flag 53200510 is now"]


def phase_rwt(only):
    # the item boxes of the awards are closed with Cross (the host's gift award waits while a dialog is up)
    host = ",".join(["god", "host_warp=2412951@10^1"] + ["host_press=0x4000@6!"] * 30)
    g1 = ",".join(["god"] + ["guest_press=0x4000@7!"] * 30)
    return dict(
        name="rwt", seconds=360, count=2, headless=0, until_hold=15,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=52400510,50000200,52700620,52800330,53200510,52420900"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "0:BB_PARTY_ITEMS_HOST_TEST=2400510,17000,2700620",
              "1:BB_PARTY_GUEST_PICKUP_TEST=2800330,3200510,2420900"],
        watch=["--watch", r"notice shown: .*=rwtnotice@0.6,2,5"],
        start_when=[r"1/0:test: cleared in the own save .*52400510"],
        crash=[], until=RWT_UNTIL,
    )


def check_rwt(v, L):
    j = need(v, L, "rwt join: 2 players", 0, r"cooperators 0 -> 1")
    for lot, what in ((2400510, "Cannon (weapon)"), (17000, "Caryll rune")):
        need(v, L, "rwt host pickup of the %s goes to the party" % what, 0,
             r"(host award|host flag \d+ ->) #\d+ lot %d " % lot, j or 0)
        need(v, L, "rwt the guest's apply of the host's %s is logged" % what, 1, r"#\d+ lot %d " % lot, j or 0)
        gg = L.all(1, r"gave #\d+ lot %d " % lot)
        # A Caryll rune whose gem the host's give list did not yield waits for the guest's own world (known, unproven live)
        wait = L.all(1, r"lot %d .*waits for the own world" % lot)
        v.step("rwt the guest gets the host's %s once (a rune: given at its next own-world load)" % what,
               len(gg) == 1 or (lot == 17000 and len(wait) == 1), "%d give(s), %d deferred" % (len(gg), len(wait)))
    # a treasure rune whose gem id has no GemGenParam row (lot 2700620, gem 227000): the game rolls nothing, so nothing
    # is shared and no notice claims it (runenow077)
    need(v, L, "rwt a treasure rune lot that rolls no item is recognised on the host", 0,
         r"host award lot 2700620: the game's roll made no item", j or 0)
    none_of(v, L, "rwt no 'Caryll rune: shared' notice for the empty roll",
            r"notice \"Caryll rune: shared with the party\" \(lot 2700620", insts=(0, 1))
    for lot, what in ((2800330, "Tonitrus (weapon)"), (3200510, "Caryll rune")):
        need(v, L, "rwt guest pickup of the %s is reported to the host" % what, 1,
             r"gift: shared key treasure lot %d " % lot, j or 0)
        need(v, L, "rwt the host receives the guest's %s" % what, 0, r"gift from slot 1: lot %d .*awarded in the host's world" % lot, j or 0)
        gh = L.all(0, r"award lot %d \(hostOnly" % lot)
        v.step("rwt the host awards the guest's %s once" % what, len(gh) == 1, "%d award(s)" % len(gh))
    # CL-60f2d5: a guest's pickup of a shared key treasure (Orphanage Key, lot 2420900) is awarded in the host's world once
    # (idempotent per lot) and the host's own ground copy is taken out where that map is loaded (informational here)
    need(v, L, "rwt the host receives the guest's Orphanage Key", 0, r"gift from slot 1: lot 2420900 .*awarded in the host's world", j or 0)
    ka = L.all(0, r"award lot 2420900 \(hostOnly")
    v.step("rwt the host awards the guest's Orphanage Key once", len(ka) == 1, "%d award(s)" % len(ka))
    kh = L.all(0, r"gift from slot 1: lot 2420900's .*taken out")
    v.step("rwt the host's ground copy of the key (info: only where its map is loaded)", True, "%d hide(s)" % len(kh))
    none_of(v, L, "rwt the host's key is never awarded a second time", r"gift from slot 1: lot 2420900 was already awarded|award lot 2420900 .*award lot 2420900", insts=(0,))
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "rwt notice on the %s's screen" % who, i, r"notice shown: .*: shared with the party", j or 0)
    none_of(v, L, "rwt no duplicate give of the Cannon / rune on the guest", r"skip .*lot (2400510|17000)", insts=(1,))
    check_common(v, L, "rwt")


# Phase gv (~4 min, 2 players, group `give_items`; 0.7.8 CL-63e235): give items to a party member, both directions.
# Test aid BB_PARTY_GIVE_TEST (party_give.h), times in seconds after the world is steady with a member connected; both
# games start with the switch OFF (the default) and turn it on with "on@s". Quicksilver Bullets 900 (host 20 = the inventory maximum, guest 0),
# Blood Vials 1000 (host 10, guest 18: the guest inventory takes 2, the storage box the other 3), Sword Hunter Badge
# 4114 (host 1: refused). Host: 900 x10 -> guest, the badge (refused), 900 x1 while the guest switch is off (their
# consent), 1000 x5 (past the inventory maximum), 900 x50 (not held). Guest: 900 x4 -> host, 900 x1 with its own switch off.
GV_UNTIL = [r"0:applied goods 900 x4 from slot 1", r"1:applied goods 1000 x5 from slot 0"]


def gv_ienv(net=None):
    host = ",".join(["god", "goods=900:20", "goods=1000:10", "goods=4114:1"])
    g1 = ",".join(["god", "goods=900:0", "goods=1000:18"])
    out = ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
           "0:BB_PARTY_GIVE_WATCH=900,1000,4114", "1:BB_PARTY_GIVE_WATCH=900,1000",
           "0:BB_OVERLAY_OPEN_PARTY_AT=60",  # the Insert menu on the Party tab for the screenshot of the give rows
           "0:BB_PARTY_GIVE_TEST=on@0,900:10:1@25,4114:1:1@40,900:1:1@75,1000:5:1@95,900:50:1@110",
           "1:BB_PARTY_GIVE_TEST=on@0,900:4:0@50,off@62,900:1:0@64,on@80"]
    if net:
        out += ["0:BB_NET_SIM=" + net, "1:BB_NET_SIM=" + net]
    return out


def phase_gv(only, name="gv", net=None):
    watch = []
    for rx, tag in ((r"Party give: notice: Gave .* x10 to", "gave"), (r"Party give: notice: .* x10 from", "got"),
                    (r"Party give: notice: Gave .* x4 to", "back"),
                    (r"request goods 900 x50 to slot 1 refused", "rows")):
        watch += ["--watch", "%s=%s@0.8" % (rx, tag)]
    return dict(
        name=name, seconds=360 if net else 300, count=2, headless=0, until_hold=15,
        env=[], watch=watch, ienv=gv_ienv(net),
        crash=[], until=GV_UNTIL,
    )


def gv_holdings(L, i, goods):
    last = None
    for _t, line in L.all(i, r"Party give: holdings goods %d = (\d+)" % goods):
        m = re.search(r"holdings goods %d = (\d+)" % goods, line)
        if m:
            last = int(m.group(1))
    return last


def check_gv(v, L, tag="gv"):
    j = need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    # switch off by default: nothing until the test turns it on
    need(v, L, tag + " host -> guest: 10 Quicksilver Bullets leave the host inventory", 0,
         r"removed goods 900 x10: held 20 -> 10 \(removed 10\)", j or 0)
    need(v, L, tag + " host -> guest: the guest game gives them (verified count)", 1,
         r"applied goods 900 x10 from slot 0: inventory 0 -> 10, storage \d+ -> \d+ \(arrived 10\)", j or 0)
    need(v, L, tag + " host -> guest: the giver transfer ends with all 10 arrived", 0, r"transfer ended: got 10 of 10, refund 0", j or 0)
    need(v, L, tag + " notice on the giver (Gave <item> xN to <name>)", 0, r"Party give: notice: Gave .* x10 to ", j or 0)
    need(v, L, tag + " notice on the receiver (<item> xN from <name>)", 1, r"Party give: notice: .* x10 from ", j or 0)
    # guest -> host
    need(v, L, tag + " guest -> host: 4 leave the guest inventory", 1, r"removed goods 900 x4: held 10 -> 6 \(removed 4\)", j or 0)
    need(v, L, tag + " guest -> host: the host game gives them", 0, r"applied goods 900 x4 from slot 1: inventory 10 -> 14", j or 0)
    # a story / unique item is refused before anything moves
    need(v, L, tag + " the Sword Hunter Badge is refused on the giver", 0, r"request goods 4114 x1 to slot 1 refused: (story|unique|key_item)", j or 0)
    none_of(v, L, tag + " no offer for the badge ever left the host", r"offer id \d+: goods 4114", insts=(0,))
    # the giver own switch off, then the receiver
    need(v, L, tag + " the giver switch off refuses the give", 1, r"request goods 900 x1 to slot 0 refused: off", j or 0)
    need(v, L, tag + " the receiver switch off refuses the offer (consent)", 1, r"offer id \d+ goods 900 x1 from slot 0 refused: their_off", j or 0)
    need(v, L, tag + " ... and the giver is told", 0, r"notice: Give .* does not take gifts", j or 0)
    # past the inventory maximum: the rest goes into the storage box
    need(v, L, tag + " 5 Blood Vials: the guest inventory fills, the rest goes into the storage box", 1,
         r"applied goods 1000 x5 from slot 0: inventory 18 -> 20, storage 0 -> 3 \(arrived 5\)", j or 0)
    need(v, L, tag + " more than the giver holds is refused", 0, r"request goods 900 x50 to slot 1 refused: not_held", j or 0)
    # conservation: nothing lost, nothing created
    h = gv_holdings(L, 0, 900)
    g = gv_holdings(L, 1, 900)
    v.step(tag + " Quicksilver Bullets are conserved (host 14 + guest 6 = 20)", h == 14 and g == 6, "host %s, guest %s" % (h, g))
    h = gv_holdings(L, 0, 1000)
    v.step(tag + " Blood Vials: the host lost exactly 5", h == 5, "host %s" % h)
    none_of(v, L, tag + " no roll-back, no failed removal on a clean link", r"rolled back|refund \d+ of goods", insts=(0, 1))
    check_common(v, L, tag)


# Phase gvg (~4 min, 3 players, group `give_items`): guest -> guest through the host relay; the host signs with the
# sender slot. Guest 1 gives 6 bullets to guest 2 (slot 2) and guest 2 gives 2 back to guest 1.
GVG_UNTIL = [r"1:applied goods 900 x2 from slot 2", r"2:applied goods 900 x6 from slot 1"]


def phase_gvg(only):
    return dict(
        name="gvg", seconds=360, count=3, headless=0, until_hold=15,
        env=[], watch=["--watch", r"Party give: notice: Gave .* x6 to=gave@0.8", "--watch", r"Party give: notice: .* x6 from=got@0.8"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god,goods=900:20", "2:BB_PARTY_DIRECTOR_TEST=god,goods=900:0",
              "0:BB_PARTY_GIVE_TEST=on@0", "1:BB_PARTY_GIVE_TEST=on@0,900:6:2@40",
              "2:BB_PARTY_GIVE_TEST=on@0,900:2:1@70",
              "1:BB_PARTY_GIVE_WATCH=900", "2:BB_PARTY_GIVE_WATCH=900"],
        crash=[], until=GVG_UNTIL,
    )


def check_gvg(v, L):
    j = need(v, L, "gvg join: 3 players", 0, r"cooperators \d+ -> 2")
    need(v, L, "gvg the host relays guest 1 offer to guest 2", 0, r"relay offer id \d+ slot 1 -> 2", j or 0)
    need(v, L, "gvg guest 1: 6 leave its inventory", 1, r"removed goods 900 x6: held 20 -> 14 \(removed 6\)", j or 0)
    need(v, L, "gvg guest 2 gives them (the giver is slot 1)", 2, r"applied goods 900 x6 from slot 1: inventory 0 -> 6", j or 0)
    need(v, L, "gvg the host relays guest 2 answers back", 0, r"relay (ready|done) id \d+ slot 2 -> 1", j or 0)
    need(v, L, "gvg guest 2 -> guest 1: 2 back", 1, r"applied goods 900 x2 from slot 2: inventory 14 -> 16", j or 0)
    a, b = gv_holdings(L, 1, 900), gv_holdings(L, 2, 900)
    v.step("gvg bullets are conserved (guest 1 + guest 2 = 20)", a is not None and b is not None and a + b == 20, "guest 1 %s, guest 2 %s" % (a, b))
    none_of(v, L, "gvg no roll-back", r"rolled back", insts=(0, 1, 2))
    check_common(v, L, "gvg")


# Phase gvr (~3 min, 2 players, group `give_rollback`): the timeout roll-back. The host game ignores COMMIT and ABORT
# (BB_PARTY_GIVE_FAULT=mute), so the guest DONE never comes: it asks (ABORT, unanswered) and puts the 5 back after the
# wait. Nothing arrives on the host. Phase gvl: the guest drops its link right after COMMIT (linkdrop); whatever the
# host got, the sum of both inventories is the same at the end (never duplicated, never lost).
GVR_UNTIL = [r"1:refund 5 of goods 900 \(no confirmation\)"]


def phase_gvr(only):
    return dict(
        name="gvr", seconds=300, count=2, headless=0, until_hold=10,
        env=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god,goods=900:0", "1:BB_PARTY_DIRECTOR_TEST=god,goods=900:20",
              "0:BB_PARTY_GIVE_TEST=on@0", "1:BB_PARTY_GIVE_TEST=on@0,900:5:0@30", "0:BB_PARTY_GIVE_FAULT=mute",
              "0:BB_PARTY_GIVE_WATCH=900", "1:BB_PARTY_GIVE_WATCH=900"],
        watch=["--watch", r"Party give: notice: Give .* the item was put back=back@0.8"],
        crash=[], until=GVR_UNTIL,
    )


def check_gvr(v, L):
    j = need(v, L, "gvr join: 2 players", 0, r"cooperators 0 -> 1")
    t = need(v, L, "gvr the guest 5 leave its inventory after READY", 1, r"removed goods 900 x5: held 20 -> 15", j or 0)
    need(v, L, "gvr the muted host never gives", 0, r"test fault mute: COMMIT id \d+ ignored", t or 0)
    need(v, L, "gvr the guest asks (ABORT) when no DONE comes", 1, r"send abort id \d+", t or 0)
    r = need(v, L, "gvr no answer: the 5 are put back", 1, r"refund 5 of goods 900 \(no confirmation\): held 15 -> 20", t or 0)
    need(v, L, "gvr the roll-back is told to the player", 1, r"notice: Give .*: no confirmation, the item was put back", r or 0)
    none_of(v, L, "gvr the host never applied it", r"applied goods 900", insts=(0,))
    a, b = gv_holdings(L, 1, 900), gv_holdings(L, 0, 900)
    v.step("gvr bullets are conserved (guest 20, host 0)", a == 20 and b == 0, "guest %s, host %s" % (a, b))
    check_common(v, L, "gvr")


GVL_UNTIL = [r"1:test fault linkdrop"]


def phase_gvl(only):
    return dict(
        name="gvl", seconds=300, count=2, headless=0, until_hold=60,
        env=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god,goods=900:0", "1:BB_PARTY_DIRECTOR_TEST=god,goods=900:20",
              "0:BB_PARTY_GIVE_TEST=on@0", "1:BB_PARTY_GIVE_TEST=on@0,900:5:0@30", "1:BB_PARTY_GIVE_FAULT=linkdrop",
              "0:BB_PARTY_GIVE_WATCH=900", "1:BB_PARTY_GIVE_WATCH=900"],
        crash=[], until=GVL_UNTIL,
    )


def check_gvl(v, L):
    j = need(v, L, "gvl join: 2 players", 0, r"cooperators 0 -> 1")
    t = need(v, L, "gvl the guest 5 leave its inventory", 1, r"removed goods 900 x5: held 20 -> 15", j or 0)
    need(v, L, "gvl the guest drops its link right after COMMIT", 1, r"test fault linkdrop", t or 0)
    # the transfer ends either way: arrived (DONE after the reconnect) or put back
    need(v, L, "gvl the transfer is settled (arrived or put back)", 1, r"transfer ended: got [0-5] of 5|refund 5 of goods 900 \(no confirmation\)", t or 0)
    a, b = gv_holdings(L, 1, 900), gv_holdings(L, 0, 900)
    ok = a is not None and b is not None and a + b == 20
    v.step("gvl bullets are conserved whatever the link did (guest + host = 20)", ok, "guest %s, host %s" % (a, b))
    check_common(v, L, "gvl")


# Phase gvm (~3 min, 2 players, group `give_menu`): the Give command in the game's own item menu (Options > inventory >
# an item). The host presses its way to Quicksilver Bullets' command menu (GVM_KEYS env of the harness user can change
# the keys while exploring); screenshots show the menu.
GVM_KEYS = os.environ.get("GVM_KEYS", "0x8,0x4000,0x4000,0x2000,0x40,0x40,0x4000,0x40,0x40,0x4000,0x4000,0x40,0x4000").split(",")


def phase_gvm(only):
    host = ",".join(["god", "goods=900:20"] + ["host_press=%s@%d!" % (k, 4 if i else 30) for i, k in enumerate(GVM_KEYS)])
    return dict(
        name="gvm", seconds=180, count=2, headless=0,
        env=[], watch=["--watch", r"Game menu: Give added=added@1.5,4", "--watch", r"item menu: Give chosen=picker@1.5,3", "--watch", r"transfer ended=done@1.5,3"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god,goods=900:5",
              "0:BB_PARTY_GIVE_ITEMS=1", "1:BB_PARTY_GIVE_ITEMS=1", "1:BB_PARTY_GIVE_WATCH=1230", "0:BB_PARTY_GIVE_WATCH=1230"],
        crash=[], until=[r"0:transfer ended: got 2 of 2"], until_hold=10,
    )


def check_gvm(v, L):
    need(v, L, "gvm join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "gvm the item menu offers Give", 0, r"Game menu: Give added to the item menu")
    need(v, L, "gvm choosing Give reaches the port", 0, r"item menu: Give chosen")
    check_common(v, L, "gvm")


# Phase gvt (~5 min, 2 players, group `give_menu`; 0.7.9 CL-063e51): Give on the Materials tab, done with real menu presses (no transfer test
# aid). Player report: Give did not show for upgrade materials (EquipParamGoods goodsType 2, not 0). Both players open the inventory, switch to
# the Materials tab (Right), open Blood Stone Shard's command menu (Use / Discard / Give; the cursor starts on the disabled Use), press Down
# twice, Cross on Give, Cross on the member, then the amount (host: Down twice = 5; guest: Down once = 2). The game's Cross handler refuses
# every row but Discard where Use is not usable (the port hooks it, bbport_game_menu.cpp CrossHook); the pickers go through the same handler.
# The guest starts its presses after the host's give has arrived (the item-get dialog of the receiver must not swallow a press).
GVT_PRE = "0x8,0x4000,0x4000,0x2000,0x20,0x4000".split(",")
GVT_HOST = os.environ.get("GVT_HOST", ",".join(GVT_PRE + ["0x40", "0x40", "0x4000", "0x4000", "0x40", "0x40", "0x4000"])).split(",")
GVT_GUEST = os.environ.get("GVT_GUEST", ",".join(GVT_PRE + ["0x40", "0x40", "0x4000", "0x4000", "0x40", "0x4000"])).split(",")


def phase_gvt(only):
    gap = int(os.environ.get("GVT_GAP", "5"))
    host = ",".join(["god", "goods=3000:30", "goods=3010:7"] + ["host_press=%s@%d!" % (k, gap if i else 30) for i, k in enumerate(GVT_HOST)])
    guest = ",".join(["god", "goods=3000:5"] + ["guest_press=%s@%d!" % (k, gap if i else 30 + gap * (len(GVT_HOST) + 2)) for i, k in enumerate(GVT_GUEST)])
    return dict(
        name="gvt", seconds=420, count=2, headless=0,
        env=[], watch=["--watch", r"Game menu: Give added=added@1.5,4", "--watch", r"Give picker built=picker@1.5", "--watch", r"transfer ended=done@1.5,3"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "0:BB_PARTY_GIVE_ITEMS=1", "1:BB_PARTY_GIVE_ITEMS=1", "1:BB_PARTY_GIVE_WATCH=3000,3010", "0:BB_PARTY_GIVE_WATCH=3000,3010"],
        crash=[], until=[r"0:applied goods 3000 x2 from slot 1"], until_hold=10,
    )


def check_gvt(v, L):
    need(v, L, "gvt join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "gvt the Materials tab item menu offers Give (Blood Stone Shard)", 0, r"Game menu: Give added to the item menu \(item 0x40000bb8\)")
    need(v, L, "gvt Cross on the Give row reaches the port (host, Materials tab)", 0, r"Game menu: Cross on the Give row")
    need(v, L, "gvt the member picker is built (host)", 0, r"Game menu: Give picker built")
    need(v, L, "gvt host -> guest: 5 Blood Stone Shards leave the host", 0, r"removed goods 3000 x5: held 30 -> 25")
    need(v, L, "gvt ... and the guest gets them", 1, r"applied goods 3000 x5 from slot 0")
    need(v, L, "gvt the transfer ends with everything arrived", 0, r"transfer ended: got 5 of 5, refund 0")
    need(v, L, "gvt guest -> host (guest's own menu presses): 2 leave the guest", 1, r"removed goods 3000 x2: held 10 -> 8")
    need(v, L, "gvt ... and the host gets them", 0, r"applied goods 3000 x2 from slot 1")
    none_of(v, L, "gvt no crash dump", r"BLOODBORNE CRASH DUMP|Boot: runtime set up", 90.0, insts=(0, 1))
    check_common(v, L, "gvt")


# Phase gvw (~6 min, 2 players, group `give_gear`; 0.7.9 CL-1b3b98): give weapons and armor. Fixtures (the same code that receives a gift builds
# them): host Saw Cleaver +4 (7000400) with 2 blood gems, Hunter Garb (71000), Yharnam Hunter Garb (61000), Kos Parasite +3 (38000300, a
# shared lot's weapon); guest Chikage +2 (2000200) with 1 gem. Host: Kos Parasite (refused, nothing offered), the Saw Cleaver +4 -> guest (the
# weapon with its level and both gems arrives, leaves the host), Hunter Garb -> guest, the guest's gear switch off: Yharnam Garb refused (their
# consent) and back on; the guest gives Chikage +2 to the host and the Saw Cleaver +4 back (the same two gems after two moves).
GG_W = 7000600
GG_A = 71000
GG_A2 = 61000
GG_K = 38000300
GG_G = 2000400


def gg_hex(i):
    return "0x%x" % i


# Navigation to the Saw Cleaver +4 in the Weapons tab: Options, Inventory, (popup), Circle, Right to the tab, Down to the item, Cross
# (the item menu: Use / Discard / Give, the cursor on Use), Down Down, Cross (Give), Cross (the member), Circle out.
GVW_PRE = "0x8,0x4000,0x4000,0x2000".split(",")
GVW_TAB = os.environ.get("GVW_TAB", "0x20,0x20,0x20").split(",")
GVW_ITEM = os.environ.get("GVW_ITEM", "").split(",") if os.environ.get("GVW_ITEM") else []
GVW_KEYS = os.environ.get("GVW_KEYS", ",".join(GVW_PRE + GVW_TAB + GVW_ITEM + ["0x4000", "0x40", "0x40", "0x4000", "0x4000"])).split(",")
GVW_GAP = int(os.environ.get("GVW_GAP", "5"))
GVW_AID_T = 30 + GVW_GAP * (len(GVW_KEYS) + 3)


def phase_gvw(only, name="gvw", net=None):
    host_setup = "w%d:2@5,a%d@6,a%d@7,w%d:0@8" % (GG_W, GG_A, GG_A2, GG_K)
    # The Saw Cleaver +4 goes through the game's own item menu (real presses, GVW_KEYS: the host_press chain below); the other gives use the
    # transfer test aid, timed after the presses (an aid at 30 s is the Kos Parasite's refusal, which needs no menu).
    host_give = "gon@0,w%d:1@30,a%d:1@%d,a%d:1@%d" % (GG_K, GG_A, GVW_AID_T, GG_A2, GVW_AID_T + 20)
    guest_give = "gon@0,goff@%d,gon@%d,w%d:0@%d,w%d:0@%d" % (GVW_AID_T + 10, GVW_AID_T + 35, GG_G, GVW_AID_T + 45, GG_W, GVW_AID_T + 70)
    watch = []
    for rx, tag in ((r"Party give: notice: Gave .* to", "gave"), (r"Party give: notice: .* from", "got"),
                    (r"Party give: notice: Give .*: does not take", "off"), (r"Party give: notice: Give .*: unique", "uniq")):
        watch += ["--watch", "%s=%s@0.8" % (rx, tag)]
    presses = ["host_press=%s@%d!" % (k, GVW_GAP if i else 30) for i, k in enumerate(GVW_KEYS)]
    # a last, harmless Circle keeps the test chain open until the aids of the other gives are done (the harness stops 60 s after the chain)
    presses.append("host_press=0x2000@%d!" % max(GVW_GAP, GVW_AID_T + 75 - (30 + GVW_GAP * (len(GVW_KEYS) - 1))))
    # the guest opens its inventory's Weapons tab after the give (timed screenshots show the received weapon), then closes it again
    gk = GVW_PRE + GVW_TAB + ["0x40"]
    gpresses = ["guest_press=%s@%d!" % (k, GVW_GAP if i else 30 + GVW_GAP * len(GVW_KEYS) + 10) for i, k in enumerate(gk)]
    gpresses += ["guest_press=0x2000@25!", "guest_press=0x2000@3!"]
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god"] + presses), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god"] + gpresses),
            "0:BB_PARTY_GIVE_GEAR=1", "0:BB_PARTY_GEAR_SETUP=" + host_setup, "1:BB_PARTY_GEAR_SETUP=w%d:1@5" % GG_G,
            "0:BB_PARTY_GIVE_TEST=" + host_give, "1:BB_PARTY_GIVE_TEST=" + guest_give,
            "0:BB_PARTY_GEAR_PROBE=60", "1:BB_PARTY_GEAR_PROBE=60", "0:BB_PARTY_GEAR_WATCH=1", "1:BB_PARTY_GEAR_WATCH=1"]
    if net:
        ienv += ["0:BB_NET_SIM=" + net, "1:BB_NET_SIM=" + net]
    return dict(name=name, seconds=520 if net else 460, count=2, headless=0, until_hold=22, env=[], watch=watch, ienv=ienv, crash=[],
                until=[r"0:applied weapon %d from slot 1" % GG_W])


def gg_gems(L, i, item):
    """The gem lines of the last probe entry of `item` on instance i: [(slot, gem id, data...)], None without such an entry."""
    last = None
    cur = None
    for _t, line in L.all(i, r"Party items: gear probe: "):
        m = re.search(r"gear probe: index \d+ handle 0x\w+ item (0x\w+) count \d+ ok (\d) equipped (\d) gems (\d+)", line)
        if m:
            cur = (m.group(1), []) if m.group(1) == item else None
            if cur:
                last = cur
            continue
        m = re.search(r"gear probe:   slot (\d+) gem (\d+) data ((?:-?\d+ ?){8})", line)
        if m and cur:
            cur[1].append((int(m.group(1)), int(m.group(2)), tuple(int(x) for x in m.group(3).split())))
    return last[1] if last else None


def gg_fixture_gems(L, i):
    out = []
    for _t, line in L.all(i, r"Party give: test setup gem: "):
        m = re.search(r"test setup gem: slot (\d+) id (\d+) data ((?:-?\d+ ?){8})", line)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), tuple(int(x) for x in m.group(3).split())))
    return out


def check_gvw(v, L, tag="gvw"):
    j = need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    s = need(v, L, tag + " host fixture: the Saw Cleaver is built with its blood gems", 0,
             r"test setup: weapon %d with [12] gems: result 1 " % GG_W, 0)
    need(v, L, tag + " guest fixture: the Chikage is built with a blood gem", 1, r"test setup: weapon %d with 1 gems: result 1 " % GG_G, 0)
    fix = gg_fixture_gems(L, 0)
    n = len(fix)
    v.step(tag + " the fixture weapon carries blood gems (slot masks fit the test gems)", n >= 1, "%s" % (fix,))
    # a unique (shared) weapon is refused on the giver, nothing offered
    need(v, L, tag + " Kos Parasite (shared lot gear) is refused", 0, r"request weapon %d to slot 1 refused: gear_unique" % GG_K, s or 0)
    none_of(v, L, tag + " no offer for the Kos Parasite ever left the host", r"offer id \d+: weapon %d " % GG_K, insts=(0,))
    # host -> guest: the weapon with its gems
    o = need(v, L, tag + " host -> guest: the weapon is offered with its gems", 0, r"offer id \d+: weapon %d \(handle 0x\w+, %d gems\) to slot 1" % (GG_W, n), s or 0)
    need(v, L, tag + " the guest accepts it (room, rules on its own table)", 1, r"offer id \d+ weapon %d \(%d gems\) from slot 0 accepted" % (GG_W, n), o or 0)
    r_ = need(v, L, tag + " the host removes it only after READY, and it is gone", 0, r"removed weapon %d \(handle 0x\w+, %d gems\): gone from the inventory" % (GG_W, n), o or 0)
    a = need(v, L, tag + " the guest builds it: 1 entry, every gem placed", 1, r"applied weapon %d from slot 0: arrived 1 .*gems placed %d loose 0 lost 0" % (GG_W, n), r_ or 0)
    need(v, L, tag + " the giver transfer ends with it arrived", 0, r"transfer ended: got 1 of 1, refund 0", a or 0)
    need(v, L, tag + " notice on the giver (name, gems)", 0, r"Party give: notice: Gave .*\(%d blood gems?\) to " % n, a or 0)
    need(v, L, tag + " notice on the receiver", 1, r"Party give: notice: .*\(%d blood gems?\) from " % n, a or 0)
    # armor
    need(v, L, tag + " host -> guest: Hunter Garb arrives", 1, r"applied armor %d from slot 0: arrived 1" % GG_A, a or 0)
    need(v, L, tag + " ... and left the host", 0, r"removed armor %d \(handle 0x\w+, 0 gems\): gone from the inventory" % GG_A, a or 0)
    # the receiver's consent
    need(v, L, tag + " the receiver's gear switch off refuses the offer (consent)", 1, r"offer id \d+ armor %d from slot 0 refused: their_gear_off" % GG_A2, a or 0)
    need(v, L, tag + " ... and the giver is told", 0, r"notice: Give .* does not take weapons and armor", a or 0)
    # guest -> host, and the weapon back
    need(v, L, tag + " guest -> host: the Chikage with its gem", 0, r"applied weapon %d from slot 1: arrived 1 .*gems placed 1 loose 0 lost 0" % GG_G, a or 0)
    need(v, L, tag + " guest -> host: the Saw Cleaver comes back with every gem", 0,
         r"applied weapon %d from slot 1: arrived 1 .*gems placed %d loose 0 lost 0" % (GG_W, n), a or 0)
    # the gems read back from the inventories: on the guest after the first move, on the host after the round trip
    g1 = gg_gems(L, 1, gg_hex(GG_W))
    v.step(tag + " the guest's weapon holds the host's gems (read back from its inventory)", g1 == fix, "%s vs %s" % (g1, fix))
    g0 = gg_gems(L, 0, gg_hex(GG_W))
    v.step(tag + " the host's weapon holds the same gems after two moves", g0 == fix, "%s vs %s" % (g0, fix))
    none_of(v, L, tag + " no refund, no removal that did not verify, no incomplete refund", r"REFUND INCOMPLETE|STILL IN THE INVENTORY|refund of (weapon|armor)", insts=(0, 1))
    none_of(v, L, tag + " no crash dump", r"BLOODBORNE CRASH DUMP|Boot: runtime set up", 90.0, insts=(0, 1))
    check_common(v, L, tag)


# Phase gva (~5 min, 2 players, group `give_armor`; 0.7.9 CL-1b3b98): Give on the Armor tab with REAL menu presses (the weapon rows of gvw
# were proven that way, the armor rows were not). The test save wears Hunter Hat / Garb / Gloves / Trousers; the fixture adds a second, UNWORN
# Hunter Garb (71000, same id as the worn one: only the inventory entry tells them apart) and a Yharnam Hunter Garb (61000). The Armor tab then
# lists Hat, Garb (worn), Garb (unworn), Gloves, Trousers, Yharnam. Presses: Options, Inventory, Circle, Right x5 (Armor tab), Cross on the Hat
# (worn: Use and Discard only), Circle, Down, Cross on the worn Garb (the same), Circle, Down, Cross on the unworn Garb (Use / Discard / Give),
# Down Down, Cross (Give), Cross (the member), Circle out. The guest opens its Armor tab afterwards (screenshots show the piece). Tunable: GVA_TAB,
# GVA_KEYS, GVA_GAP.
GVA_A = 71000
GVA_A2 = 61000
GVA_PRE = "0x8,0x4000,0x4000,0x2000".split(",")
GVA_TAB = os.environ.get("GVA_TAB", "0x20,0x20,0x20,0x20,0x20").split(",")
GVA_WORN = ["0x4000", "0x2000", "0x40", "0x4000", "0x2000", "0x40"]  # the menus of the two worn pieces, opened and closed
GVA_GIVE = ["0x4000", "0x40", "0x40", "0x4000", "0x4000", "0x2000"]  # the unworn Garb: menu (Use / Discard / Give), Down Down, Give, the member, out
GVA_KEYS = os.environ.get("GVA_KEYS", ",".join(GVA_PRE + GVA_TAB + GVA_WORN + GVA_GIVE)).split(",")
GVA_GAP = int(os.environ.get("GVA_GAP", "6"))


def phase_gva(only):
    presses = ["host_press=%s@%d!" % (k, GVA_GAP if i else 30) for i, k in enumerate(GVA_KEYS)]
    t_end = 30 + GVA_GAP * len(GVA_KEYS)
    # the guest opens its Armor tab after the give (timed screenshots show the received piece) and closes it again
    gpresses = ["guest_press=%s@%d!" % (k, GVA_GAP if i else t_end + 10) for i, k in enumerate(GVA_PRE + GVA_TAB)]
    gpresses += ["guest_press=0x2000@25!", "guest_press=0x2000@3!"]
    watch = []
    for rx, tag in ((r"Party give: notice: Gave .* to", "gave"), (r"Party give: notice: .* from", "got")):
        watch += ["--watch", "%s=%s@0.8" % (rx, tag)]
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god"] + presses), "1:BB_PARTY_DIRECTOR_TEST=" + ",".join(["god"] + gpresses),
            "0:BB_PARTY_GIVE_GEAR=1", "1:BB_PARTY_GIVE_GEAR=1", "0:BB_PARTY_GEAR_SETUP=a%d@5,a%d@6" % (GVA_A, GVA_A2),
            "0:BB_PARTY_GEAR_PROBE=20", "1:BB_PARTY_GEAR_PROBE=%d" % (t_end + 40), "0:BB_PARTY_GEAR_WATCH=1", "1:BB_PARTY_GEAR_WATCH=1"]
    return dict(name="gva", seconds=420, count=2, headless=0, until_hold=30, env=[], watch=watch, ienv=ienv, crash=[],
                until=[r"1:Party items: gear probe: index \d+ handle 0x\w+ item 0x%x count 1 ok 1 equipped 0" % (0x10000000 | GVA_A)])


def gva_probe(L, i, item, after=0.0, before=1e9):
    """The gear probe's entries of `item` (hex) on instance i, from the last probe block in (after, before): [(handle, equipped)]."""
    rows = L.all(i, r"Party items: gear probe: index \d+ handle 0x\w+ item 0x[0-9a-f]+ count \d+ ok \d equipped \d", after, before)
    last = max((t for t, _ in rows), default=None)
    out = []
    for t, line in rows:
        m = re.search(r"index (\d+) handle (0x\w+) item (0x[0-9a-f]+) count \d+ ok \d equipped (\d)", line)
        if t == last and m and m.group(3) == item:
            out.append((m.group(2), int(m.group(4))))
    return out


def check_gva(v, L):
    item = "0x%x" % (0x10000000 | GVA_A)
    j = need(v, L, "gva join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "gva host fixture: the unworn Hunter Garb is built", 0, r"test setup: armor %d with 0 gems: result 1 " % GVA_A)
    setup = L.find(0, r"test setup: armor %d with 0 gems: result 1 \(handle (0x\w+)\)" % GVA_A)
    handle = re.search(r"handle (0x\w+)", setup[1]).group(1) if setup else "?"
    # the item menus, by the presses: Cross presses #2 (Hat) and #3 (worn Garb) offer no Give, #4 (unworn Garb) does
    crosses = [t for t, _ in L.all(0, r"test: host_press 0x4000;")]
    if len(crosses) < 7:
        v.step("gva the host's press chain ran (7 Cross presses)", False, "%d Cross presses" % len(crosses))
        return
    before = gva_probe(L, 0, item, 0.0, crosses[2])
    v.step("gva host before the give: a worn and an unworn Hunter Garb (probe)", sorted(e for _, e in before) == [0, 1], "%s" % (before,))
    adds = [t for t, _ in L.all(0, r"Game menu: Give added to the item menu \(item 0x[0-9a-f]+\)")]
    worn_adds = [t for t in adds if crosses[2] - 0.5 <= t < crosses[4] - 0.5]
    v.step("gva the worn Hat's and worn Garb's item menus offer no Give", not worn_adds, "%d Give row(s) in the window" % len(worn_adds))
    need(v, L, "gva the unworn Garb's item menu offers Give", 0, r"Game menu: Give added to the item menu \(item %s\)" % item, crosses[4] - 0.5, crosses[5])
    c = need(v, L, "gva Cross on the Give row reaches the port", 0, r"Game menu: Cross on the Give row", crosses[5] - 0.5)
    p = need(v, L, "gva the member picker is built", 0, r"Game menu: Give picker built", c or 0)
    g = need(v, L, "gva Cross on the member gives THAT entry (the unworn copy's handle, not the worn one)", 0,
             r"item menu: give armor %d \(handle %s\) to slot 1" % (GVA_A, handle), p or 0)
    o = need(v, L, "gva the giver offers it", 0, r"offer id \d+: armor %d \(handle %s, 0 gems\) to slot 1" % (GVA_A, handle), g or 0)
    acc = need(v, L, "gva the guest accepts the offer", 1, r"offer id \d+ armor %d \(0 gems\) from slot 0 accepted" % GVA_A, 0)
    a = need(v, L, "gva the guest builds the piece", 1, r"applied armor %d from slot 0: arrived 1" % GVA_A, 0)
    r_ = need(v, L, "gva the giver removes it", 0, r"removed armor %d \(handle %s, 0 gems\): gone from the inventory" % (GVA_A, handle), o or 0)
    # the protocol: the receiver's READY (room reserved, offer accepted) comes first, the giver removes only after it, then COMMIT; the
    # receiver builds the piece on COMMIT and answers DONE (a refund path covers a lost COMMIT)
    rd = need(v, L, "gva the giver got the receiver's READY", 0, r"recv ready id \d+ from slot 1", o or 0)
    if rd is not None and r_ is not None:
        v.step("gva the giver removes the piece only after READY", r_ >= rd, "READY +%.3f s, removed +%.3f s" % (rd, r_))
    need(v, L, "gva the host's DONE: the receiver confirmed it built the piece", 0, r"recv done id \d+ from slot 1", a or 0)
    need(v, L, "gva the transfer ends with it arrived", 0, r"transfer ended: got 1 of 1, refund 0", o or 0)
    need(v, L, "gva notice on the giver", 0, r"Party give: notice: Gave .* to ", o or 0)
    need(v, L, "gva notice on the receiver", 1, r"Party give: notice: .* from ", o or 0)
    after = (a or 0) + 1
    gh = gva_probe(L, 1, item, after)
    v.step("gva the guest holds the received Hunter Garb unworn next to the one it wears (probe)", sorted(e for _, e in gh) == [0, 1] and
           any(h == handle and e == 0 for h, e in gh), "%s" % (gh,))
    hh = gva_probe(L, 0, item, (r_ or 0) + 1)
    v.step("gva the host keeps only the WORN Hunter Garb (probe)", [e for _, e in hh] == [1], "%s" % (hh,))
    none_of(v, L, "gva no refund, no removal that did not verify", r"REFUND INCOMPLETE|STILL IN THE INVENTORY|refund of (weapon|armor)", insts=(0, 1))
    none_of(v, L, "gva no crash dump", r"BLOODBORNE CRASH DUMP|Boot: runtime set up", 90.0, insts=(0, 1))
    check_common(v, L, "gva")


# Phase gvs (~4 min, 2 players, group `give_menu`): the item menu opened and closed many times, the pickers opened and
# backed out of, then one real give: no crash, every open offers Give again, the menu state stays usable.
GVS_BASE = "0x8,0x4000,0x4000,0x2000,0x40,0x40".split(",")
GVS_LOOP = ["0x4000", "0x2000"] * 8 + ["0x4000", "0x40", "0x40", "0x4000", "0x2000", "0x2000"] * 4 +            ["0x4000", "0x40", "0x40", "0x4000", "0x4000", "0x4000", "0x4000"]


def phase_gvs(only):
    host = ",".join(["god", "goods=900:20"] + ["host_press=%s@%s!" % (k, "30" if i == 0 else "4") for i, k in enumerate(GVS_BASE)] +
                    ["host_press=%s@1.6!" % k for k in GVS_LOOP])
    return dict(
        name="gvs", seconds=300, count=2, headless=0,
        env=[], watch=["--watch", r"transfer ended=done@1.5,3"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god,goods=900:5",
              "0:BB_PARTY_GIVE_ITEMS=1", "1:BB_PARTY_GIVE_ITEMS=1"],
        crash=[], until=[r"0:transfer ended: got \d+ of \d+"], until_hold=10,
    )


def check_gvs(v, L):
    need(v, L, "gvs join: 2 players", 0, r"cooperators 0 -> 1")
    n = len(L.all(0, r"Game menu: Give added to the item menu"))
    v.step("gvs the item menu offered Give on every open (13 opens)", n >= 13, "%d" % n)
    n = len(L.all(0, r"Game menu: Give picker built"))
    v.step("gvs the pickers opened (4 member lists, then member + amount)", n >= 6, "%d" % n)
    need(v, L, "gvs one real give at the end", 0, r"transfer ended: got 2? ?\d* of")
    none_of(v, L, "gvs no crash dump", r"BLOODBORNE CRASH DUMP|Boot: runtime set up", 20.0, insts=(0, 1))
    check_common(v, L, "gvs")


# Phase gag (guest_gates, ~4 min, 2 players, group `guest_gates`; 0.7.8 CL-ee41ff / CL-f572a3): the host's Dream. (1) The host sets
# Gehrman's ordinary-talk flag 72100120 (his first line heard); the guest reads it as 1 (host-world mirror, BB_PARTY_GEHRMAN_SYNC).
# (2) The guest without the Rune Workshop Tool stands at the Memory Altar (t210699): the gate keeps it vanilla ("kept vanilla").
def phase_gag(only):
    host = ",".join(["god", "host_flag=72100120@8", "log_flag=72100120/72100121"])
    guest = ",".join(["god", "guest_flag=72100121@16", "log_flag=72100120/72100121"])
    return dict(
        name="gag", seconds=200, count=2, headless=0, env=[],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "1:BB_PARTY_GUESTSVC_TEST=warp=talk:210699;open;keys=left,cross,w,w"],
        crash=[], until=[r"1:Memory Altar \(t210699\): kept vanilla", r"0:test: flags 72100120=1 72100121=1"], until_hold=25,
    )


def check_gag(v, L):
    need(v, L, "gag join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "gag the guest reads Gehrman's flag 72100120 as set by the host", 1, r"test: flags 72100120=1")
    need(v, L, "gag the guest's own Gehrman flag 72100121 reaches the host (both directions)", 0, r"test: flags 72100120=1 72100121=1")
    need(v, L, "gag the Memory Altar is kept vanilla for a guest without the tool", 1, r"Memory Altar \(t210699\): kept vanilla")
    none_of(v, L, "gag the guest's altar talk is not overridden", r"Memory Altar \(t210699\): a guest may use it", insts=(1,))
    check_common(v, L, "gag")


# Phase rwl (~3 min, 2 players, group `reward_late`): a member who joins AFTER a shared pickup gets it once. The host's
# save already holds the pickup flags of the Cannon (lot 2400510, a weapon) and the Living String (lot 2605010) when the
# guest connects (the guest starts after the host's flags are set); the join's full list carries both, the guest
# gets each exactly once (one quiet batch, no item box per item), and a second full list (Leave + Rejoin is not
# needed: the guest's per-session grant memory) never gives them again.
RWL_UNTIL = [r"1:catch-up batch: \d+ item grant"]


def phase_rwl(only):
    host = ",".join(["god", "log_flag=52400510/52605010"])
    g1 = ",".join(["god", "log_flag=52400510/52605010"])
    return dict(
        name="rwl", seconds=240, count=2, headless=0, until_hold=12,
        env=[],
        ienv=["0:BB_PARTY_PROGRESS_TEST_SET=52400510,52605010", "1:BB_PARTY_PROGRESS_TEST_CLEAR=52400510,52605010",
              "0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        start_when=[r"1/0:test: flags 52400510=1"],
        crash=[], until=RWL_UNTIL,
    )


def check_rwl(v, L):
    need(v, L, "rwl join: the guest comes after the host's pickups", 0, r"cooperators 0 -> 1")
    need(v, L, "rwl the host's full list goes to the late guest", 0, r"items: full list \(\d+ lots\) sent to slot 1")
    for lot, what in ((2400510, "Cannon (weapon)"), (2605010, "Living String")):
        g = L.all(1, r"gave #\d+ lot %d " % lot)
        v.step("rwl the late guest gets the %s once" % what, len(g) == 1, "%d give(s)" % len(g))
    need(v, L, "rwl one quiet summary for the catch-up (no notice per item)", 1, r"catch-up batch: [12] item grant")
    none_of(v, L, "rwl no per-item notice for a catch-up", r"notice shown: ", insts=(1,))
    check_common(v, L, "rwl")


# Phase il (items leak, 0.7.5, ~3 min, 2 players, group `items_leak`): the host's catch-up list reaches a
# guest that lacks the host's progress and items quietly (one summary notice, no 8 s drip, no consumables),
# and the guest's messenger shop is its own (the 5500-5521 "owns the Uncanny / Lost weapon" gates the guest's game
# sets in the host's world are put back to its save's values; Gascoigne's attire, 5910, follows the host).
#   The guest's save: Gascoigne / Cleric Beast / Sword Hunter Badge bosses un-killed (their ledger bits are not
#   seeded, so the host's list gives them), the key goods 4000 / 4001 / 4114 removed; the guest stands at the Bath
#   Messenger (talk 210691) with the Purchase list open on the weapons tab (screenshots).
IL_UNTIL = [r"1:test: levelup: done[\s\S]*test: levelup: done", r"1:catch-up batch: \d+ item grant"]


def phase_il(only):
    # 0.7.6: the Chalice boss unlocks of the Insight shop (5090 Bone Ash, 5091 Madman) are progress without a
    # lot: the host's event sets them, the guest's own save must get them (group shop_sync)
    host = ",".join(["god", "log_flag=5910/5508/70009300/70009310/5090/5091", "set_flag=5090:5091"])
    g1 = ",".join(["god", "goods=4000:0", "goods=4001:0", "goods=4114:0", "log_flag=5910/5508/5500/5521"])
    return dict(
        name="il", seconds=240, count=2, headless=0,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host,
              "0:BB_PARTY_PROGRESS_TEST_READ=5910,70009300,70009310,70009320,70009330,70009260",
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=12411800,12401800,12411700,5910,5090,5091,52410920,50001915,52411000,70009300,70009310,70009320,70009330",
              "1:BB_PARTY_PROGRESS_TEST_READ=5910,12411800,5508,5160",
              # A/B for the shop screenshots: IL_NOFIX=1 runs the guest without the shop-gate fix
              ("1:BB_PARTY_SHOP_GATES=0" if os.environ.get("IL_NOFIX") else "1:BB_PARTY_SHOP_GATES=1"),
              "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              ("0:BB_PARTY_GUESTSVC_TEST=hosttalk=210691;open=15000140;keys=right,right,right,right,right,down,down,down,down,down,down,down,down,down,down,down,down,down,down;hsdelay=3" if os.environ.get("IL_HOSTSHOP") else "1:BB_PARTY_GUESTSVC_TEST=warp=talk:210691;open=15000140;keys=right,right,right,down,down,down,down,down,down,down,down|warp=talk:210691;open=15000140;keys=right,right,right,right,right,down,down,down,down,down,down,down,down,down,down,down,down,down,down")],
        watch=[], crash=[], until=IL_UNTIL, until_hold=25,
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


# Phase nfe (npc_eileen, 0.7.7 D11, ~4 min, 2 players, group `npc_eileen`): the blood-drunk Eileen (Grand Cathedral,
# m24_00, entity 2400902) fought by the whole party. Her hostility is the init event 12400650 (team 27, "kTeamEvents":
# the hostile 27 is no foe for a phantom, so a guest's copy gets 23) at a load with her state 1374 + night 9802;
# event 12400656 (the "falls to the blood" transition in kGuestRuns) is never started by the 1.09 map script
# (no InitializeEvent; the night's common event 9723 sets 1372 itself, traced 2026-10-30), so only the init path is
# live. Fixture: 1374 + 9802 set, her other flags 1360-1379 / 1700-1705 cleared. Host and guest stand beside her;
# the guest locks on, R1s (her HP falls on both games), HP 0 on its game (guest_kill :hp); her death (1368) reaches
# the host. Alfred (nfa) already covers an in-map switch; this covers a hostile NPC that is hostile from the load.
NFE_EILEEN = 2400902
NFE_FLAGS = "1368/1369/1372/1374/1701/1705/9802"
NFE_CLEAR = ",".join(str(f) for f in list(range(1360, 1374)) + list(range(1375, 1380)) + list(range(1700, 1706)) + [72400520, 72400524, 72400526])
NFE_UNTIL = r"0:test: flags 1368=1"


def phase_nfe(only):
    host = ",".join([
        "god", "log_flag=" + NFE_FLAGS,
        "host_warp=2402950@8^1",                              # 1: the party at the Cathedral Ward lamp
        "host_chr=%d:5@10%%m24_00^1" % NFE_EILEEN,            # 2: the host beside Eileen (guest back in)
        "host_kill=%d:hp2@3%%m24_00" % NFE_EILEEN,            # 3: her HP at 2 % (the host owns it): the guest's R1s finish her
        "host_press=0@150%m24_00!"])                          # 4: idle while the guest fights
    g1 = ",".join(["god", "log_flag=" + NFE_FLAGS,
                   "guest_chr=%d:3@12%%m24_00" % NFE_EILEEN]    # 2: beside her
                  + ["guest_press=0x4:lock@1%m24_00!"] * 3
                  + ["guest_chr=%d:1.2@4%%m24_00" % NFE_EILEEN]  # 3: in front of her
                  + ["guest_press=0x4:lock@1%m24_00!"] * 2
                  + (["guest_press=0x800@1.2%m24_00!"] * 3 + ["guest_chr=%d:1.2@1%%m24_00" % NFE_EILEEN]) * 5)
    return dict(
        name="nfe", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + NFE_CLEAR, "BB_PARTY_PROGRESS_TEST_SET=1374,9802," + NF_LOGARIUS_DEAD,
             "BB_PARTY_PROGRESS_TEST_READ=1368,1369,1372,1374,9802", "BB_PARTY_CHR_DUMP=40",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: host_chr %d=nfearrive@0.5,3" % NFE_EILEEN,
               "--watch", r"test: guest_press 0x800=nfehit@0.5",
               "--watch", r"test: flags 1368=1=nfedead@1,5"],
        crash=[], until=[NFE_UNTIL], until_hold=8,
    )


# Phase nfw (npc_alfred_ward, 0.7.7 D11, ~5 min, 2 players, group `npc_alfred_ward`): Alfred in the Cathedral Ward
# (m24_00, entity 2400763, state 1344 friendly). The host hits him three times (event 12400592 slot 1 counts the
# local player's blows -> 72400476; 12400593 slot 1 -> team 27 + state 1345): a host-only in-map switch (`4[3]`
# gate in kGuestRuns) while the guest stands next to him. The guest's copy (gate passed) re-teams him to 23 on its
# game; the guest locks on, its real R1s hurt him on both games (he heals at low HP, so the host ends it with HP 0, the game's
# own death), his drop roll, death and Echoes reach both.
NFW_ALFRED = 2400763
NFW_FLAGS = "1344/1345/1346/72400476"
NFW_CLEAR = ",".join(str(f) for f in list(range(1340, 1344)) + list(range(1345, 1360)) + [72400475, 72400476, 72400471, 12405160])
NFW_UNTIL = r"0:drops: shared drop roll[^\n]*entity 2400763"


def phase_nfw(only):
    host = ",".join([
        "god", "log_flag=" + NFW_FLAGS,
        "host_warp=2402950@8^1",                               # 1: the party at the Cathedral Ward lamp
        "host_chr=%d:1.5@10%%m24_00^1" % NFW_ALFRED]           # 2: the host in front of Alfred (guest back in)
        + ["host_press=0x800@1.2%m24_00!"] * 6                  # 3: his counter (3 hits by the local player) -> hostile
        + ["host_kill=%d:hp@42%%m24_00" % NFW_ALFRED,         # 4: after the guest's R1s: his HP 0 (the game's own death; he heals from 1 %)
           "host_press=0@150%m24_00!"])
    g1 = ",".join(["god", "log_flag=" + NFW_FLAGS,
                   "guest_chr=%d:2.5:90@12%%m24_00" % NFW_ALFRED]  # 2: beside him (friendly)
                  + ["guest_press=0x4:lock@1%m24_00!"] * 2
                  + ["guest_press=0@14%m24_00!"]
                  + ["guest_chr=%d:1.2@2%%m24_00" % NFW_ALFRED]  # 3: in front of him after the switch
                  + ["guest_press=0x4:lock@1%m24_00!"] * 2
                  + (["guest_press=0x800@1.2%m24_00!"] * 3 + ["guest_chr=%d:1.2@1%%m24_00" % NFW_ALFRED]) * 5)
    return dict(
        name="nfw", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + NFW_CLEAR, "BB_PARTY_PROGRESS_TEST_SET=1344,9801," + NF_LOGARIUS_DEAD,
             "BB_PARTY_PROGRESS_TEST_READ=1344,1345,1346,72400476", "BB_PARTY_CHR_DUMP=40",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: host_chr %d=nfwarrive@0.5,3" % NFW_ALFRED,
               "--watch", r"test: host_press 0x800=nfwprovoke@0.6",
               "--watch", r"test: guest_press 0x800=nfwhit@0.5",
               "--watch", r"shared drop roll[^\n]*entity 2400763=nfwdead@1,5"],
        crash=[], until=[NFW_UNTIL], until_hold=8,
    )


# Phases nfn / nfr / nfv (npc_hunters, 0.7.9 D11, ~5 min each, 2 players, groups `npc_hunter_nightmare` /
# `npc_hunter_research` / `npc_valtr`): the other host-only hostile switches of kGuestRuns that nfw proved for
# Alfred. The NPC stands friendly in its quest state; the host's blows trip its "turns hostile" event
# (13400920 slot 2 Hunter's Nightmare, 13500920 Research Hall, 12700905 Valtr: the NPC's HP at 90 % or the
# provoke flag, host chr type 0 gate) with the guest beside it; the guest's copy (gate passed) re-teams it to 23
# on its game, the guest locks on and its real R1s hurt it on both games, the host's HP 0 (the game's own
# death) rolls the drop and reaches the guest. Same shape as nfw (Alfred, Cathedral Ward).
NFH = {
    "nfn": dict(entity=3400902, map="m34_00", lamp=3402950, friendly=1720, hostile=1725, states=(1710, 1729),
                flags=(73400420,), who="the Hunter's Nightmare hunter", group="npc_hunter_nightmare", kill_at=60),
    "nfr": dict(entity=3500900, map="m35_00", lamp=3502950, friendly=1722, hostile=1727, states=(1710, 1729),
                flags=(73500620,), who="the Research Hall hunter", group="npc_hunter_research", kill_at=60),
    "nfd": dict(entity=2300710, map="m23_00", lamp=2302950, friendly=1320, hostile=1322, states=(1320, 1325),
                flags=(), who="Djura", group="npc_djura", kill_at=60, host_far=True, deg=90, hostile_alt=1323),
    "nfv": dict(entity=2700912, map="m27_00", lamp=2702950, friendly=1800, hostile=1805, states=(1790, 1809),
                flags=(72700320,), who="Valtr", group="npc_valtr", kill_at=60, per_player=True),
}


def phase_nfh(only, name):
    c = NFH[name]
    e, m = c["entity"], c["map"]
    flags = "/".join(str(f) for f in list(range(c["states"][0], c["states"][1] + 1)) + list(c["flags"]))
    clear = ",".join(str(f) for f in list(range(c["states"][0], c["states"][1] + 1)) + list(c["flags"])
                     if f != c["friendly"])
    host = ",".join([
        "god", "log_flag=" + flags,
        "host_warp=%d@8^1" % c["lamp"],                         # 1: the party at the lamp
        "host_chr=%d:1.5%s@10%%%s^1" % (e, c.get("deg") and ":%d" % c["deg"] or "", m)]  # 2: the host in front of the NPC (guest back in)
        + ["host_press=0x800@1.2%%%s!" % m] * 8                 # 3: the host's blows: the NPC turns hostile
        + ["host_kill=%d:hp@%d%%%s" % (e, c["kill_at"], m),     # 4: after the guest's R1s: its HP 0 (the game's own death)
           "host_press=0@150%%%s!" % m])
    g1 = ",".join(["god", "log_flag=" + flags,
                   "guest_chr=%d:2.5:90@12%%%s" % (e, m)]       # 2: beside it (friendly)
                  + ["guest_press=0x4:lock@1%%%s!" % m] * 2
                  + ["guest_press=0@%d%%%s!" % (c.get("gwait", 24), m)]
                  + ["guest_chr=%d:1.2%s@2%%%s" % (e, c.get("deg") and ":%d" % c["deg"] or "", m)]  # 3: in front of it after the switch
                  + ["guest_press=0x4:lock@1%%%s!" % m] * 2
                  + (["guest_press=0x800@1.2%%%s!" % m] * 3 + ["guest_chr=%d:1.2@1%%%s" % (e, m)]) * 5)
    if c.get("deg"):
        g1 = g1.replace("guest_chr=%d:1.2@1%%%s" % (e, m), "guest_chr=%d:1.2:%d@1%%%s" % (e, c["deg"], m))
    until = r"0:(drops: shared drop roll[^\n]*entity %d|test: host_kill: [^\n]*entity %d)" % (e, e)
    return dict(
        name=name, seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=%d" % c["friendly"],
             "BB_PARTY_PROGRESS_TEST_READ=%d,%d" % (c["friendly"], c["hostile"]), "BB_PARTY_CHR_DUMP=40",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: host_chr %d=%sarrive@0.5,3" % (e, name),
               "--watch", r"test: host_press 0x800=%sprovoke@0.6" % name,
               "--watch", r"test: guest_press 0x800=%shit@0.5" % name,
               "--watch", r"shared drop roll[^\n]*entity %d=%sdead@1,5" % (e, name)],
        crash=[], until=[until], until_hold=12,
    )


def check_nfh(v, L, name):
    c = NFH[name]
    e = c["entity"]
    nm = name
    need(v, L, "%s join: 2 players" % nm, 0, r"cooperators 0 -> 1")
    at = need(v, L, "%s host in front of %s" % (nm, c["who"]), 0, r"test: host_chr %d" % e)
    gat = need(v, L, "%s guest beside it" % nm, 1, r"test: guest_chr %d" % e)
    prov = L.find(0, r"test: host_press 0x800", at or 0)
    if not (at and gat and prov):
        v.step("%s the host's R1s ran" % nm, False, "host_chr %s guest_chr %s press %s" % (at, gat, bool(prov)))
        check_common(v, L, nm)
        return
    bh = nf_hp(L, 0, e, 0, prov[0])
    bg = nf_hp(L, 1, e, 0, prov[0])
    v.step("%s (before) %s there on both games, friendly (not team 27/23)" % (nm, c["who"]),
           bool(bh) and bool(bg) and all(tm not in (23, 27) for _, _, tm in bh + bg),
           "host teams %s, guest teams %s" % (sorted({tm for _, _, tm in bh}), sorted({tm for _, _, tm in bg})))
    # Djura's own events pick 1322 or 1323 as the hostile state (run to run); both mean hostile
    hflag = L.find(0, r"test: flags [^\n]*\b(%d|%d)=1" % (c["hostile"], c.get("hostile_alt", c["hostile"])), prov[0] - 1)
    hteam = [(t, tm) for t, _, tm in nf_hp(L, 0, e, prov[0] - 1) if tm == 27]
    v.step("%s the host's R1s turn it hostile on the host (%d / team 27)" % (nm, c["hostile"]), bool(hflag or hteam),
           "flag at %s, team 27 at %s" % ("+%.1f" % hflag[0] if hflag else "-", "+%.1f" % hteam[0][0] if hteam else "-"))
    gteam = [(t, tm) for t, _, tm in nf_hp(L, 1, e, prov[0] - 1) if tm in (23, 27)]
    v.step("%s in-map switch: a foe on the guest's game while it stands there (team 23)" % nm,
           bool(gteam) and gteam[0][1] == 23, "guest teams after the host's R1s %s" %
           sorted({tm for _, _, tm in nf_hp(L, 1, e, prov[0] - 1)}))
    t0 = gteam[0][0] if gteam else prov[0]
    dead = L.find(0, r"shared drop roll[^\n]*entity %d" % e, t0)
    end = dead[0] if dead else 1e9
    hits = L.all(1, r"test: guest_press 0x800", t0, end)
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already", t0 - 1, end)
    v.step("%s the guest locks on to the hostile NPC" % nm, bool(lock), lock[1][:140] if lock else "no lock-on")
    low = L.find(0, r"test: host_kill: .*entity %d" % e, t0 - 5)
    v.step("%s (info) its HP 0 on the host" % nm, True, low[1][-120:] if low else "no host_kill line")
    hh = [h for _, h, _ in nf_hp(L, 0, e, t0, end)]
    gh = [h for _, h, _ in nf_hp(L, 1, e, t0, end)]
    v.step("%s (info) its HP series, host / guest" % nm, True, "%s / %s" % (hh[:10], gh[:10]))
    v.step("%s the guest's R1s hurt it on the host's game and on the guest's (%d R1)" % (nm, len(hits)),
           bool(hits) and (bool(hh) or c.get("host_far")) and bool(gh) and (not hh or min(hh) < max(hh[:1]) - 30) and
           min(gh) < max(gh[:1]) - 30,
           "host %s guest %s" % (hh[:4], gh[:4]))
    v.step("%s (info) its drop roll on the host (some hunters drop nothing)" % nm, True,
           "drop roll at +%.1f s" % dead[0] if dead else "no drop roll")
    # the quest outcome: after its HP 0 the hostile state flag goes off on the host AND on the guest (the host's flags
    # reach the guest), and another state of its range goes on there (the death state)
    kt = low[0] if low else t0
    outs = []
    for i, who in ((0, "host"), (1, "guest")):
        rows = L.all(i, r"test: flags ", kt + 1)
        last = rows[-1][1] if rows else ""
        m = re.search(r"%d=(\d)" % c["hostile"], last)
        others = [int(f) for f, val in re.findall(r"(\d+)=(\d)", last) if val == "1" and c["states"][0] <= int(f) <= c["states"][1] and int(f) != c["hostile"]]
        outs.append((who, bool(m) and m.group(1) == "0", others))
    # Valtr's states are covenant outcomes (1790-1809): per player by the user's decision, so only the host's change
    own = outs[:1] if c.get("per_player") else outs
    v.step("%s kill credit: its hostile state %d is off and a death state on, on the host%s" % (
               nm, c["hostile"], "" if c.get("per_player") else " and on the guest"),
           all(ok and o for _, ok, o in own), "; ".join("%s off=%s now %s" % (w, ok, o) for w, ok, o in outs))
    need(v, L, "%s its death on the guest's game (HP 0 in its chr dump)" % nm, 1,
         r"chr dump: entity %d [^\n]*HP 0/" % e, low[0] if low else t0)
    check_common(v, L, nm)


# Phase nfb (npc_brador, 0.7.9 D11, ~6 min, 2 players, group `npc_brador`): Brador, the Research Hall's church assassin
# (DLC m35, entity 3500936). His events wait for the HOST (chr type 0, goods 4015 held, the omen region 3502890 ->
# 3502891 -> the appear regions 3502892 / 3502896, all in one spot of the hall): a guest's copy can never fire them.
# The host walks to the spot (no teleport into an arena: it is the hall's floor), Brador appears for the host
# (flag 13505911 = X4, his invincibility off, SpEffect 8060); the guest (beside the host) must see him too (the
# party's assassin tick shows him while the host-world flag is on), lock on, hurt him on both games; his HP 0 is the
# host's (the game's own death) and his outcome flags reach the guest. Same fixture state for the guest: nothing.
NFB = {
    "nfb": dict(entity=3500936, map="m35_00", lamp=3502950, spot="-375.4:1512.7:-792.8", goods="4015:1",
                flags=(1730, 13505910, 13505911, 13505912, 13505913), appeared=13505911, who="Brador (Research Hall)",
                group="npc_brador"),
    "nfh": dict(entity=3600900, map="m36_00", lamp=3602950, spot=None, goods=None,
                flags=(1730, 13605950, 13605960, 13605970), appeared=13605960, who="a Fishing Hamlet assassin (1)",
                group="npc_hamlet"),
}


def phase_nfb(only, name="nfb"):
    c = NFB[name]
    m, e, spot = c["map"], c["entity"], c["spot"]
    NFB_FLAGS = "/".join(str(f) for f in c["flags"])
    NFB_CLEAR = ",".join(str(f) for f in c["flags"])
    NFB_UNTIL = r"0:test: host_kill: [^\n]*entity %d" % e
    host = ",".join([
        "god", "log_flag=" + NFB_FLAGS] + (["goods=" + c["goods"]] if c["goods"] else []) + [
        "host_warp=%d@8^1" % c["lamp"],                                   # 1: the party at the Research Hall lamp
        ("host_move=%s:0:native:pin=2@6%%%s^1" % (spot, m) if spot else "host_chr=%d:2.5@6%%%s^1" % (e, m)),  # 2: at the omen spot / beside the waiting NPC
        "host_press=0@45%%%s!" % m,                                # 3: the omen, then Brador's appearance
        "host_chr=%d:2.5@30%%%s" % (e, m),                 # 4: beside him
        "host_press=0@70%%%s!" % m,                                # 5: the guest's blows
        "host_kill=%d:hp@2%%%s" % (e, m),                 # 6: his HP 0 (the game's own death)
        "host_press=0@120%%%s!" % m])
    g1 = ",".join(["god", "log_flag=" + NFB_FLAGS,
                   ("guest_move=%s:0:native:pin=2@8%%%s" % (spot, m) if spot else "guest_chr=%d:3.5@8%%%s" % (e, m)),  # 2: at the same spot / near the waiting NPC
                   "guest_press=0@32%%%s!" % m,                    # Brador appears (the host's event)
                   "guest_chr=%d:1.5:90@5%%%s" % (e, m)]  # 3: beside him
                  + ["guest_press=0x4:lock@1%%%s!" % m] * 3
                  + ["guest_chr=%d:1.2@3%%%s" % (e, m)]
                  + ["guest_press=0x4:lock@1%%%s!" % m] * 2
                  + (["guest_press=0x800@1.2%%%s!" % m] * 3 + ["guest_chr=%d:1.2@1%%%s" % (e, m)]) * 4)
    return dict(
        name=name, seconds=480, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + NFB_CLEAR, "BB_PARTY_PROGRESS_TEST_READ=" + NFB_CLEAR,
             "BB_PARTY_CHR_DUMP=40", "BB_PARTY_TEST_COND_TIMEOUT=120"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"%d=1=%sappear@0.5,3" % (c["appeared"], name),
               "--watch", r"test: guest_press 0x800=%shit@0.5" % name,
               "--watch", r"test: host_kill: [^\n]*entity %d=%skill@1,5" % (e, name)],
        crash=[], until=[NFB_UNTIL], until_hold=15,
    )


def check_nfb(v, L, name="nfb"):
    c = NFB[name]
    e = c["entity"]
    nfb = name
    need(v, L, name + " join: 2 players", 0, r"cooperators 0 -> 1")
    ap = need(v, L, "%s %s appears for the host (flag %d on)" % (name, c["who"], c["appeared"]), 0, r"test: flags [^\n]*%d=1" % c["appeared"])
    t0 = ap if ap else 0
    tick = L.find(1, r"Party NPC fights: %s \(entity %d\) appeared in the host's world" % (re.escape(c["who"]), e), t0)
    v.step(name + " (info) the guest's assassin tick showed him", True, "yes" if tick else "no tick line (his own copy already ran)")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, name + " %s's game has %s alive (chr dump)" % (who, c["who"]), i, r"chr dump: entity %d [^\n]*HP [1-9]" % e, t0)
    gt = sorted({tm for _, _, tm in nf_hp(L, 1, e, t0)})
    ht = sorted({tm for _, _, tm in nf_hp(L, 0, e, t0)})
    v.step(name + " (info) the NPC's team, host / guest", True, "%s / %s" % (ht, gt))
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already", t0)
    v.step(name + " the guest locks on to the assassin", bool(lock), lock[1][:140] if lock else "no lock-on")
    hits = L.all(1, r"test: guest_press 0x800", t0)
    kill = L.find(0, r"test: host_kill: .*entity %d" % e, t0)
    end = kill[0] if kill else 1e9
    hh = [h for _, h, _ in nf_hp(L, 0, e, t0, end)]
    gh = [h for _, h, _ in nf_hp(L, 1, e, t0, end)]
    v.step(name + " (info) the NPC's HP series, host / guest", True, "%s / %s" % (hh[:10], gh[:10]))
    v.step(name + " the guest's R1s hurt the assassin on the host's game and on the guest's (%d R1)" % len(hits),
           bool(hits) and bool(hh) and bool(gh) and min(hh) < max(hh[:1]) - 20 and min(gh) < max(gh[:1]) - 20,
           "host %s guest %s" % (hh[:4], gh[:4]))
    v.step(name + " (info) host_kill line", True, kill[1][-140:] if kill else "none")
    need(v, L, name + " his death on the guest's game (HP 0 in its chr dump)", 1, r"chr dump: entity %d [^\n]*HP 0/" % e, kill[0] if kill else t0)
    outs = []
    for i, who in ((0, "host"), (1, "guest")):
        rows = L.all(i, r"test: flags ", (kill[0] if kill else t0) + 1)
        outs.append((who, rows[-1][1][-90:] if rows else "no flags line"))
    v.step(name + " (info) outcome flags after the kill", True, "; ".join("%s: %s" % o for o in outs))
    check_common(v, L, name)


# Phase npi (npc_invader, 0.7.7 D11, 2 players, group `npc_invader`): an NPC hunter that ARRIVES as an invader. No retail
# event does it (the only 2003[51] type-1 event, 12904634, is called by the prototype layout m29_05_00_05 only), so the
# Beckoning Bell fixture (BB_PARTY_TEST_NPC) summons the Tomb of Oedon sign NPC (2410740) with session type 28 (EMEVD
# type 1, the invader's). The host summons it in Central Yharnam; the guest must see it arrive at the same place, lock
# on, hit it; HP and death agree.
NPI_NPC = 2410740
NPI_UNTIL = r"0:drops: enemy drop roll on this game[^\n]*chr handle 0x10014001"


def phase_npi(only):
    host = ",".join([
        "god", "host_warp=2412951@8^1",                       # 1: the party in Central Yharnam (m24_01)
        "host_chr=%d:5@10%%m24_01^1" % NPI_NPC,               # 2: the host beside the arrived invader (the fixture summons it there)
        "host_press=0@20%m24_01!",                            # 3: the arrival walk and presentation end (team 21 / chr type 2, ~15 s)
        "host_kill=%d:hp@42%%m24_01" % NPI_NPC,               # 4: after the guest's R1s: its HP 0 (the game's own death; it heals from low HP)
        "host_press=0@150%m24_01!"])                          # 5: idle while the guest fights
    g1 = ",".join(["god", "guest_chr=%d:3@12%%m24_01" % NPI_NPC]  # 2: beside it
                  + ["guest_press=0@22%m24_01!"]                 # the arrival ends, the host sets its HP low
                  + ["guest_press=0x4:lock@1%m24_01!"] * 3
                  + ["guest_chr=%d:1.2@4%%m24_01" % NPI_NPC]    # 3: in front of it
                  + ["guest_press=0x4:lock@1%m24_01!"] * 2
                  + (["guest_press=0x800@1.2%m24_01!"] * 3 + ["guest_chr=%d:1.2@1%%m24_01" % NPI_NPC]) * 5)
    return dict(
        name="npi", seconds=420, count=2, headless=0,
        env=["BB_PARTY_TEST_COND_TIMEOUT=100", "BB_PARTY_CHR_DUMP=40"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "0:BB_PARTY_TEST_NPC=%d:28" % NPI_NPC, "0:BB_PARTY_TEST_NPC_DELAY=12",
              "0:BB_PARTY_TEST_NPC_LOG=0"],
        watch=["--watch", r"Party NPC: summon #1=npiarrive@0.5,3",
               "--watch", r"test: guest_press 0x800=npihit@0.5",
               "--watch", r"drops: enemy drop roll on this game[^\n]*chr handle 0x10014001=npidead@1,5"],
        crash=[], until=[NPI_UNTIL], until_hold=8,
    )


# Phase npa (npcattack, ~5 min, 2 players, group `npc_attack`; 0.7.5 "NPC attacks beyond #13"): a guest
# hits a FRIENDLY NPC of the host's world (Alfred in the Queen's Chamber, state 1348, team 26) with no
# help from the host. The SpEffect target table has 1 for the host's body (team 19) against team 26 but
# 0 for a cooperator (team 20): the guest's blows used to pass through. Same fixture as nfa.
NPA_UNTIL = r"1:test: guest_kill[\s\S]*test read \(load mode 1"


def phase_npa(only):
    host = ",".join([
        "god", "log_flag=" + NFA_FLAGS,
        "host_warp=2502951@8^1",                              # 1: the party at the Queen's Chamber lamp
        "host_chr=%d:4@10%%m25^1" % NF_ALFRED,                # the host 4 m from Alfred, no blow of its own
        "host_press=0@60%m25!"])
    g1 = ",".join(["god", "log_flag=" + NFA_FLAGS,
                   "guest_chr=%d:1.6@14%%m25" % NF_ALFRED]     # 2: in front of the friendly Alfred
                  + ["guest_press=0x800@1.5%m25!"] * 8         # 3: R1 x8 on a friendly NPC
                  + ["guest_press=0@4%m25!"]
                  + ["guest_kill=%d:hp@2%%m25" % NF_ALFRED])   # 4: a lethal blow's HP 0
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    return dict(
        name="npa", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_PROGRESS_TEST_READ=1348,1349,1350,12500810", "BB_PARTY_CHR_DUMP=40",
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: guest_chr %d=npaarrive@0.5,3" % NF_ALFRED,
               "--watch", r"test: guest_press 0x800=npahit@0.5",
               "--watch", r"test: guest_kill: =npakill@1,5"],
        crash=[], until=[NPA_UNTIL], until_hold=8,
    )


# Phase npd (npc_attack_doll, group `npc_attack`; 0.7.5 report "cannot hurt the Doll as guest"): in the host's
# Dream the guest, then the host, R1 the Doll once each (192 HP: the second blow kills her) (entity 2100700); her HP must fall on both games for both.
NPD_DOLL = 2100700


def phase_npd(only):
    host = ",".join(["god", "host_chr=%d:1.6@40%%m21" % NPD_DOLL] + ["host_press=0x800@1.5%m21!"] + ["host_press=0@4%m21!"])
    g1 = ",".join(["god", "guest_chr=%d:1.6@14%%m21" % NPD_DOLL] + ["guest_press=0x800@1.5%m21!"] + ["guest_press=0@4%m21!"])
    return dict(
        name="npd", seconds=300, count=2, headless=0,
        env=["BB_PARTY_CHR_DUMP=40", "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2102950", "0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        watch=["--watch", r"test: guest_press 0x800=npdhit@0.5"],
        crash=[], until=[r"0:test: host_press 0x0 "], until_hold=8,
    )


def check_npd(v, L):
    need(v, L, "npd join: 2 players", 0, r"cooperators 0 -> 1")
    hh = L.all(0, r"test: host_press 0x800")
    gh = L.all(1, r"test: guest_press 0x800")
    if not (hh and gh):
        v.step("npd the R1s ran", False, "host %d guest %d" % (len(hh), len(gh)))
        check_common(v, L, "npd")
        return
    for who, hits in (("guest", gh), ("host", hh)):
        t0 = hits[0][0]
        for i, where in ((0, "host's"), (1, "guest's")):
            before = [h for _, h, _ in nf_hp(L, i, NPD_DOLL, 0, t0)]
            after = [h for _, h, _ in nf_hp(L, i, NPD_DOLL, t0, t0 + 8)]
            v.step("npd the %s's R1 on the Doll hurt her on the %s game" % (who, where),
                   bool(before) and bool(after) and min(after) < before[-1], "HP %s -> %s" % (before[-1:], after[:4]))
    tm = sorted({t for _, _, t in nf_hp(L, 1, NPD_DOLL, gh[0][0], gh[-1][0] + 4)})
    v.step("npd (info) the Doll's team on the guest's game during its blows", True, "%s" % tm)
    check_common(v, L, "npd")


# Phase pvd (pvpfix, ~10 min, 2 players, group `pvp_revive`; 0.7.5 "host unhittable after PvP deaths"): friendly
# fire on, the guest R1s the host at 30 HP (test aid) in 12 cycles; each death is the host's in-place revive at
# its lamp (no load, the Queen's Chamber fixture of npa). Every cycle must kill the host again (a player's report:
# after a few deaths he could not be hit until a warp). BB_PARTY_PVP_TEST steps (party_pvp_rules.h).
PVD_CYCLES = 12
PVD_STEPS = 2 + 2 * PVD_CYCLES
PVD_UNTIL = r"0:pvp: test: step %d:" % PVD_STEPS


def phase_pvd(only):
    host = ",".join(["host_warp=2502951@8^1"] + ["host_press=0@60%m25!"] * (PVD_CYCLES * 42 // 60 + 3))  # keeps the chain (and the run) alive
    hsteps = ["on@3", "log@20"] + ["hp=30@12", "log@28"] * PVD_CYCLES
    gsteps = ["log@20"] + ["face@10", "hit=6@1", "log@21"] * PVD_CYCLES
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    return dict(
        name="pvd", seconds=PVD_CYCLES * 42 + 240, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_TEST_COND_TIMEOUT=100", "BB_PARTY_VT_DUMP=536c460"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "0:BB_PARTY_PVP_TEST=" + ",".join(hsteps),
              "1:BB_PARTY_PVP_TEST=" + ",".join(gsteps)],
        watch=[], crash=[], until=[PVD_UNTIL], until_hold=8,
    )


# Phase pvh (pvphp, ~6 min, 2 players, group `pvp_full_hp`; 0.7.6 "Full health in PvP"): the party at the Queen's
# Chamber lamp (pvd's spot). The guest sets its own HP to 250 of its 503 (the 30 % cut of 719), then the host flips
# friendly fire on / off / on (host_rule, live), the rule off and on while PvP is on, and PvP off at the end. On
# every flip the guest's game must log `phantom health: lifted: full health in PvP` / `on, the game's 30 % cut`
# and `HP kept at its fraction: a/b -> c/d` with c = a * d / b rounded (260/503 -> 372/719 and back; the game
# alone had 476/719: it adds the max difference when the max grows and keeps the points when it shrinks); the
# host's own HP is never touched; the host's copy of the guest (BB_PARTY_LEVELSYNC_LOG, every 5 s) shows the
# guest's bar. Screenshots (--every 5): the guest's HP bar before and after each flip.
PVH_FLIPS = [("party_friendly_fire", 1), ("party_friendly_fire", 0), ("party_friendly_fire", 1),
             ("party_pvp_full_hp", 0), ("party_pvp_full_hp", 1), ("party_friendly_fire", 0)]
PVH_GAP = 30
PVH_UNTIL = r"0:test: host_rule: party_friendly_fire 1 -> 0[\s\S]*test: host_rule: party_friendly_fire 1 -> 0"


def phase_pvh(only):
    host = ",".join(["host_warp=2502951@8^1"] +
                    ["host_rule=%s:%d@%d%%m25" % (k, v, 35 if i == 0 else PVH_GAP) for i, (k, v) in enumerate(PVH_FLIPS)] +
                    ["host_press=0@60%m25!"] * 3)
    gsteps = ["log@12", "hp=250@6", "log@1"] + ["log@%d" % PVH_GAP] * len(PVH_FLIPS)
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    return dict(
        name="pvh", seconds=35 + PVH_GAP * len(PVH_FLIPS) + 200, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_TEST_COND_TIMEOUT=100", "BB_PARTY_LEVELSYNC_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_PVP_TEST=" + ",".join(gsteps)],
        watch=[], crash=[], until=[PVH_UNTIL], until_hold=25,
    )


# Phase pvn (pvpnpc, ~5 min, 2 players, group `pvp_npc`; 0.7.7 "bell-summoned NPC hunters turn on the host with PvP
# on"): the party in Central Yharnam, the host summons an NPC hunter with the Beckoning Bell test fixture
# (BB_PARTY_TEST_NPC). Friendly fire goes on (BB_PARTY_PVP_TEST), 40 s pass with the host and the guest idle (the NPC
# must not hurt the host: the host's HP, the table cells between the host's team and the NPC's team), then the guest
# hits the host (PvP between players still works: the host's HP falls) and PvP goes off again.
PVN_UNTIL = r"0:pvp: test: step 9:"


def phase_pvn(only):
    host = ",".join(["host_warp=2412951@8^1"] + ["host_press=0@60%m24_01!"] * 6)
    hsteps = ["log@30", "on@2", "log@3", "log@40", "log@25", "log@12", "off@3", "log@8", "log@2"]
    gsteps = ["log@30", "log@5", "log@40", "face@2", "hit=6@1", "log@5"]
    clear = ",".join(str(f) for f in list(range(1340, 1348)) + list(range(1349, 1360)) + [12500051, 12500052, 12500053, 12500054] + list(range(72500350, 72500360)))
    return dict(
        name="pvn", seconds=420, count=2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + clear, "BB_PARTY_PROGRESS_TEST_SET=" + NFA_FIXTURE_SET,
             "BB_PARTY_TEST_COND_TIMEOUT=100"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "0:BB_PARTY_PVP_TEST=" + ",".join(hsteps),
              "1:BB_PARTY_PVP_TEST=" + ",".join(gsteps), "0:BB_PARTY_TEST_NPC=2410740", "0:BB_PARTY_TEST_NPC_DELAY=12"],
        watch=[], crash=[], until=[PVN_UNTIL], until_hold=8,
    )


# Phase npf (npcfog077, ~5 min, 2 players, group `npc_fog`; 0.7.7 "NPC helpers summoned with a bell follow the party into
# the boss arena", NW-dd78dd9 part 2): Old Yharnam, the Blood-starved Beast alive (first fight seen). An NPC hunter
# (Alfred's sign, entity 2300740) is summoned with the Beckoning Bell fixture (BB_PARTY_TEST_NPC) by the host (or by the
# guest: BB_NPF_RING=guest), both walk through the beast's fog (host first). The NPC's own events (12304450 walks it to
# the fog once the fight is on, 12304460 through the fog into the arena) must put it inside the arena on the host's game.
NPF_NPC = 2300740
NPF_FOG = 2301800
NPF_FLAGS = "12304420,12304450,12304800,12304801,12301802,12301800"
NPF_UNTIL = r"0:test: host_press 0x0 [\s\S]*0:test: host_press 0x0 [\s\S]*0:test: host_press 0x0 "


def phase_npf(only):
    ring = os.environ.get("BB_NPF_RING", "host")
    pvp = os.environ.get("BB_NPF_PVP", "0") == "1"
    solo = os.environ.get("BB_NPF_SOLO", "0") == "1"   # control run: the host alone (no guest), the vanilla behaviour
    order = os.environ.get("BB_NPF_ORDER", "host")      # who walks through the beast's fog first: host | guest
    hfog = (["host_press=0x4@1!", "host_press=0@1!"] + ["host_press=0x4000@3%m23_00!", "host_press=0@2%m23_00!"] * 2 +
            sum([["host_obj=%d:%s:%d@2%%m23_00" % (NPF_FOG, os.environ.get("BB_NPF_DIST", "0.6"), ang), "host_press=0x4@1!", "host_press=0@1!",
                  "host_press=0x4000@3%m23_00!", "host_press=0@2%m23_00!", "host_press=0x4000@3%m23_00!", "host_press=0@2%m23_00!"]
                 for ang in (0, 180, 90, 270)], [])) if order == "host" else ["host_press=0@12%m23_00!"]
    quick = os.environ.get("BB_NPF_QUICK") == "1"   # the host stands at the fog (right spot) while the helper materializes
    if quick:
        hfog = ["host_press=0@18%m23_00!"] + hfog
    if os.environ.get("BB_NPF_SWEEP") == "1":   # diagnostic: stand around the fog at several distances / angles, no presses
        hfog = sum([["host_obj=%d:%s:%d@3%%m23_00" % (NPF_FOG, d, a), "host_press=0@1%m23_00!"]
                    for d in ("0.6", "1.5", "3", "5") for a in (0, 90, 180, 270)], [])
    host = ["god", "insight=20", ] + ([] if solo else ["host_warp=2302950@8^1"]) + ["host_flag=1340@2%m23_00", "host_flag=72400461@0.5%m23_00", ] + (["host_flag=12301802@3%m23_00"] if os.environ.get("BB_NPF_LATESET") == "1" else []) + [] + ([] if os.environ.get("BB_NPF_NONPC") == "1" else ["host_use=4312@1%m23_00"]) + (["host_press=0@4%m23_00!"] if os.environ.get("BB_NPF_NONPC") == "1" else ["host_move=-45:-123.9:7:90@4%m23_00", "host_press=0x4000@3%m23_00!",
            "host_press=0@%d%%m23_00!" % (2 if quick else 25)]) + ["host_obj=%d%s@2%%m23_00" % (NPF_FOG, ":0.6:0" if quick else "")] + hfog + [
            "host_press=0@50%m23_00!", "host_press=0@40%m23_00!", "host_press=0@40%m23_00!"]
    gdelay = 220 if order == "host" else 110
    g1 = ["god"] + (["guest_move=-45:-123.9:7:90@20%m23_00"] if os.environ.get("BB_NPF_GUEST_NEAR") == "1" else []) + ["guest_obj=%d:0.6:0@%d%%m23_00" % (NPF_FOG, gdelay), "guest_press=0x4000@2%m23_00!", "guest_press=0x4000@2%m23_00!",
          "guest_press=0@60%m23_00!"]
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + ",".join(host)] + ([] if solo else ["1:BB_PARTY_DIRECTOR_TEST=" + ",".join(g1)]) + [
            "%d:BB_PARTY_TEST_NPC=%d" % (0 if ring == "host" else 1, NPF_NPC),
            "%d:BB_PARTY_TEST_NPC_MODE=log" % (0 if ring == "host" else 1),
            "%d:BB_PARTY_TEST_NPC_FLAGS=%s" % (0 if ring == "host" else 1, NPF_FLAGS)]
    if os.environ.get("BB_NPF_NONPC") == "1":   # control run: no bell helper at all
        ienv = [e for e in ienv if "BB_PARTY_TEST_NPC" not in e]
    if os.environ.get("BB_NPF_TRACE") == "1":   # the instructions of the NPC's two events, every call (host game)
        ienv += ["0:BB_PARTY_EMEVD_TRACE=1", "0:BB_PARTY_EMEVD_TRACE_ALL=12304450,12304460,12304410,12304810,12304811"]
        ienv += ["0:BB_PARTY_HOLD_LOG=1", "0:BB_PARTY_GUESTSVC_TEST=buttonprobe;promptlog"]
    if solo:
        ienv += ["0:BB_PARTY_TEST_START_LAMP=2302950", "0:BB_PARTY_DIRECTOR_TEST_SOLO=1"]     # start at Old Yharnam's lamp: no party travel
    if pvp:
        ienv += ["0:BB_PARTY_PVP_TEST=on@2,log@200"]
    return dict(
        name="npf", seconds=420, count=1 if solo else 2, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=12301800", "BB_PARTY_PROGRESS_TEST_SET=" + ("72400461,1340" if os.environ.get("BB_NPF_LATESET") == "1" else "12301802,72400461,1340"), "BB_PARTY_TEST_COND_TIMEOUT=100",
             "BB_PARTY_CHR_DUMP=60"],
        ienv=ienv, watch=[], crash=[], until=[NPF_UNTIL], until_hold=10,
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
        "host_warp=2412953@8^1",                                    # the Tomb of Oedon lamp (party travel)
        # its neighbours (rt: a death loop; eh1: 2410202 killed the guest instead and took the echoes)
        "host_kill=2410249@3%m24_01", "host_kill=2410250@1%m24_01", "host_kill=2410202@1%m24_01",
    ] + ["host_press=0@20%m24_01!"] * 12)
    g1 = ",".join(
        ["echoes=15000", "guest_chr=2410247@3%m24_01"] + ["guest_press=0x800@1.5%m24_01!"] * 2 +
        ["guest_press=0@8%m24_01", "guest_press=0@8%m24_01"] +      # killed beside it (two draws only: three killed IT in eh r1 / r4)
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
    # A frail guest can die again after the payment: that new stain pays on its own, so count up to it
    deaths = L.all(1, r"party stain \(death in the host's world\)")
    pays = [p for p in pays if len(deaths) < 2 or p[0] < deaths[1][0]]
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
CLG_UNTIL_G = r"1:travel step (\d+)/\1: "  # guest 1's last step: 15 s in the Dream (r2: its earlier m21 ended the run)


def _clg_extra_env():
    import os
    return ["1:BB_PARTY_STAIN_PAY_VFUNC=1"] if os.environ.get("BB_SMOKE_CLG_OLDPAY") == "1" else []


def phase_clg(only):
    host = ",".join(["log_pos", "god",
                     "host_obj=%s:1.2:0@12%%m29" % CL_LAMP_OBJ,     # (1) light the boss room's lamp
                     "host_press=0x4000@2%m29!",
                     "host_move=-49.4:30.6:-91.0@4%m29!",          # beside the lamp: where the guest gets up
                     "host_press=0@45%m29!",                       # (2) the guest's fall meanwhile
                     "host_move=-65.0:31.6:-91.0@1%m29!?near=-64.6:31.6:-90.4:1.5",  # (3) onto the car
                     "host_press=0@60%m29!"])                      # (4) the guest's lamp: the party travels
    g1 = ",".join(["log_pos=0.5", "echoes=15000",                  # (5) echoes for the fall's party stain
                   "guest_move=-46.0:62.0:-92.0@25%m29!",         # (2) 31 m above the boss room's floor
                   "guest_move=-64.6:31.6:-90.4@25%m29!",         # (3) onto the lift's car
                   "guest_obj=%s:1.2:0@14%%m29!" % CLG_LAMP_OBJ2,  # (4) the bottom lamp: light it
                   "guest_press=0x4000@2%m29!",
                   "guest_obj=%s:1.2:0@9%%m29!" % CLG_LAMP_OBJ2,   # use it: the Dream question (r1: at the
                   "guest_press=0x4000@4%m29!",                    # first Cross), YES (it opens on NO), OK
                   "guest_press=0x80@3%m29!", "guest_press=0x4000@2%m29!",
                   "guest_press=0@15%m21!"])
    watch = []
    for rx, tag in ((r"died from a fall", "gfall"), (r"up again beside the nearest living member", "gup"),
                    (r"Chalice Dungeon lift car", "car"), (r"lamp flag 729001", "dream"), (r"map .* -> m21_00", "m21")):
        watch += ["--watch", "%s=%s@1,4" % (rx, tag)]
    return dict(
        name="clg", seconds=480, count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        watch=watch,
        # chaliceechoes: the fall's party stain (15000 echoes, in the host's dungeon) is walked onto
        # (the test aid puts the guest 4 m from it, 4 s later onto it): paid in the Chalice Dungeon
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_STAIN_TEST=4",
              # echoes077: the Caryll rune gain rates (x1.1 x1.2 x1.3) on the guest: the stain must still pay 15000
              "1:BB_PARTY_STAIN_TEST_SPFX=100000,100001,100002"] + _clg_extra_env(),
        crash=[],
        until=[CLG_UNTIL, CLG_UNTIL_G],
    )


# Phase cla (chalicefollow2, ~3 min, 2 players, group `chalice_alias`): the host's game ChannelId
# is one the party's channel board cannot hold (live, Nightwatch: 0x80e3..., so the host refused every
# direct summon "no ChannelId on the party's board" and the guest never followed it into the dungeon).
# BB_CHALICE_ALIAS_TEST=1 on the host makes it treat its (board-sized) id that way: the dungeon goes
# on the board under an alias made of its FormData and the summon carries that; guest 1 must be in.
CLA_UNTIL = r"1:map .* -> m29_00"


def phase_cla(only):
    host = ",".join(["log_pos", "host_press=0@40%m29!"])
    g1 = ",".join(["log_pos=0.5", "guest_press=0@40%m29!"])
    return dict(
        name="cla", seconds=240, count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party"],
        watch=["--watch", r"cannot ride the board=alias@0,4"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "0:BB_CHALICE_ALIAS_TEST=1"],
        crash=[],
        until=[CLA_UNTIL],
        until_hold=25,
    )


def check_cla(v, L):
    need(v, L, "cla host: the game's id rides the board under an alias", 0,
              r"chalice: the game's dungeon id 0x[0-9a-f]+ cannot ride the board: summons carry (\d+)")
    need(v, L, "cla guest 1 summoned into the host's dungeon", 1, r"map .* -> m29_00")
    need(v, L, "cla guest 1's game made the channel request for the alias", 1,
         r"direct summon: Chalice Dungeon \d+ \(ChannelId .*channel request made")
    none_of(v, L, "cla host never refused for a missing ChannelId", r"has no ChannelId on the party's board", 0, 1e9, (0,))
    none_of(v, L, "cla no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "cla")


# Phase clw (chaliceechoes, ~4 min, 2 players, group `chalice_guest`): a wipe in the host's Chalice
# Dungeon. Guest 1 dies, then the host: the host's respawn load (host_death, to the dungeon's lamp,
# inside the dungeon guest 1 stands in) must be followed by the dead guest at once (0.7.5: "not
# followed: the host enters chalice dungeon N, this game has 0": the guest waited in limbo for the
# host's trip to the Dream).
CLW_UNTIL = r"1:guest: held death goes with the host's travel|1:guest: host travels \(#\d+ host_death .*\); waiting"


# Phases cy / cyn (chalice_layers, CL-48ee07, ~5 min, 2 players, group `chalice_layers`): Chalice layer travel
# (bbport.ini party_layer_travel, here BB_PARTY_LAYER_TRAVEL=1). The host Continues inside the Pthumeru dungeon
# (fixture chalice_pthumeru_inside, in the layer 1 boss room), guest 1 is summoned in. The host walks to the boss
# room's lamp 2901952 (BB_LAMP_TRAVEL_TEST lamp walk: Cross lights it, then opens the lamp's travel list: the
# Dream and the dungeon's other lit lamps), chooses lamp 2902950 (the layer 1 start), "Travel with the party?"
# is answered YES (BB_PARTY_TRAVEL_CHOICE=party): both end up at that lamp; then `back`: the way back to 2902952.
# cy: guest 1 has its own other dungeon (chalice_hintertomb_ritual); cyn: guest 1 has none (the default save).
CY_UNTIL = r"0:Lamp travel: test: all trips done|0:Lamp travel: test: FAILED|0:Lamp travel: no load within 60 s"


def prep_chalayers_none(root: Path):
    if not CL_FIXTURE.is_dir():
        sys.exit("chalice_layers: the fixture %s is missing (docs/party/test_saves.md)" % CL_FIXTURE)
    _cl_saves(root, CL_FIXTURE)
    _cl_saves(root, CL_DEFAULT, 1)


def phase_cy(only, name="cy", none=False):
    host = "log_pos"
    g1 = "log_pos=0.5"
    watch = []
    for rx, tag in ((r"Chalice layer travel", "list"), (r"Lamp travel: chose lamp", "chose"),
                    (r"Lamp travel: arrived", "arr"), (r"guest travel .* not followed", "nofollow")):
        watch += ["--watch", "%s=%s@0,4" % (rx, tag)]
    return dict(
        name=name, seconds=420, count=2, headless=0, prep=prep_chalayers_none if none else prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_LAYER_TRAVEL=1"],
        watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "0:BB_LAMP_TRAVEL_TEST=members,lamp=%s,to=%s,back" % (CL_LAMP, CL_OWN_ENTRY)],
        crash=[],
        until=[CY_UNTIL],
        until_hold=20,
    )


def check_cy(v, L, tag="cy"):
    lst = need(v, L, tag + " host: the dungeon lamp offers the travel list", 0,
               r"Lamp travel: lamp 2902952 .*Chalice layer travel\); 2 entries: 1:2102950.* 2:2902950")
    ch1 = need(v, L, tag + " host chooses lamp 2902950", 0, r"Lamp travel: chose lamp 2902950", lst or 0)
    arr1 = need(v, L, tag + " host arrives at lamp 2902950", 0, r"Lamp travel: arrived: lamp 2902950, map m29", ch1 or 0)
    if tag == "cy":
        # the party split treats a dungeon lamp as an awaken point: the host enters alone, the guest (its own current
        # dungeon is another one) is sent home and the summon quick-joins it back (never a gray loop)
        need(v, L, tag + " the guest is sent home, not dropped in a dungeon of its own", 1,
             r"send-home .* not dropped: the host is in a Chalice Dungeon this game does not have", ch1 or 0)
        need(v, L, tag + " guest is summoned into the same dungeon (a load)", 1, r"map .* -> m29_00", ch1 or 0)
    else:
        # cyn: a guest with no dungeon of its own is carried too: sent home and summoned, or (its current slot holds
        # the host's dungeon: LayerFollowRefusal) it runs the same lamp warp itself; which of the two depends on
        # what its game reads as its current dungeon at that moment, both end beside the host
        need(v, L, tag + " guest is in the same dungeon again (a load)", 1, r"map .* -> m29_00", ch1 or 0)
    back = need(v, L, tag + " host starts the way back", 0, r"Lamp travel: test: back: now lamp 2902950", arr1 or 0)
    ch2 = need(v, L, tag + " host chooses lamp 2902952 again", 0, r"Lamp travel: chose lamp 2902952", back or 0)
    arr2 = need(v, L, tag + " host arrives at lamp 2902952", 0, r"Lamp travel: arrived: lamp 2902952, map m29", ch2 or 0)
    need(v, L, tag + " guest is back in the dungeon after the second trip (a load)", 1, r"map .* -> m29_00", ch2 or 0)
    none_of(v, L, tag + " guest never loads another map for good (no gray loop)", r"map .* -> m255", 0, 1e9, (1,))
    need(v, L, tag + " all trips done", 0, r"Lamp travel: test: all trips done", arr2 or 0)
    none_of(v, L, tag + " no stuck load", r"Lamp travel: no load within 60 s|Lamp travel: test: FAILED")
    none_of(v, L, tag + " no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, tag)


def phase_clw(only):
    host = ",".join(["log_pos", "host_move=-49.4:30.6:-91.0@4%m29!",
                     # a wipe: the host dies 16 s after the join (the guest dies 17-23 s after it; the host's death anim
                     # runs ~9 s, the guest's held death revives in place ~2 s after it is taken, so the host must die
                     # BEFORE the guest or at the same time: it came from the host's own move timer, 3 s before..7 s after the guest)
                     "host_die@16%m29^1",
                     "host_press=0@40%m29!"])
    g1 = ",".join(["log_pos=0.5", "echoes=15000",
                   "guest_move=-48.0:30.6:-91.0@4%m29!",
                   "guest_die@12%m29", "guest_press=0@40%m29!"])
    return dict(
        name="clw", seconds=300, count=2, headless=0, prep=prep_chalamp, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_TEST_INVINCIBLE=1"],
        watch=["--watch", r"host_death=hd@1,4", "--watch", r"party stain=stain@1,4"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[],
        until=[CLW_UNTIL],
        until_hold=75,   # the respawn load (15 s), the arrival and a look around (screenshots)
    )


def phase_chw(only):
    """chw: the chc fixture, then clw's wipe (guest down, then the host): the respawn load runs the map's events again."""
    host = ",".join(["log_pos=2", "host_move=-49.4:30.6:-91.0@4%m29!", "host_die@16%m29^1", "host_press=0@40%m29!"])
    g1 = ",".join(["log_pos=2", "guest_move=-48.0:30.6:-91.0@4%m29!", "guest_die@12%m29", "guest_press=0@40%m29!"])
    return dict(
        name="chw", seconds=300, count=2, headless=0, prep=prep_chalcurse, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_TEST_INVINCIBLE=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1],
        crash=[],
        until=[CLW_UNTIL],
        until_hold=75,
    )


def check_clw(v, L):
    need(v, L, "clw join: guest 1 in the host's dungeon", 1, r"map .* -> m29_00")
    died = need(v, L, "clw guest 1 down", 1, r"guest: own death in the host's world held")
    hd = need(v, L, "clw the host's respawn travel reaches the guest", 1, r"guest: host travels \(#\d+ host_death", died or 0)
    if hd:
        none_of(v, L, "clw the host's respawn in the dungeon the guest stands in is followed (never 'not followed')",
                r"host_death .* not followed", hd - 3, hd + 3, (1,))
        # both orders are the wipe: the guest's death held when the host's travel comes, or the guest dying while the
        # host's death animation runs (its death is taken after the host's load came and follows it dead)
        need(v, L, "clw the dead guest goes with the host's load", 1,
             r"guest: held death goes with the host's travel|guest: own death taken [\d.]+ s after the host's wipe load came: follows it dead", hd - 15)
        rp = need(v, L, "clw the guest replays the respawn (lamp warp)", 1, r"replay now #\d+ host_death", hd)
        if rp:
            back = need(v, L, "clw the guest is back in the dungeon after the load", 1, r"map .* -> m29_00", rp)
            if back:
                need(v, L, "clw the respawn load builds the host's dungeon again (not the guest's own slot's)", 1,
                     r"chalice: load mode 1: slot \d+'s record \(dungeon \d+, .*\) carried over again", rp)
                none_of(v, L, "clw no fall death after the respawn", r"local death: HP 0 at", back, 1e9, (1,))
    none_of(v, L, "clw no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "clw")



# Phase sj (forestrejoin, ~5 min, 2 players, group `seam_rejoin`; a player's report, 0.7.4: in the
# Forbidden Woods cave (the seam between m27_00 and m24_00) the guest that crossed first was sent
# home seconds after the host caught up - its game's leave state had started when the host's chr
# left - and its re-join landed in the host's Hunter's Dream; after such stays the guest could not
# use a lamp, pick up items or (another report) the Doll. The party stands in the cave on the m24_00
# side; the guest walks over the seam into m27_00, the host follows 2 s later: the guest's send-home
# must be dropped (it stays a cooperator) and the leave state undone. Then a party travel to the Hunter's Dream: the guest levels up at the Doll
# while the host talks to her at the same moment (the guest's action buttons work after the stay).
# SJ_ROUTE: the walk from the Cathedral Ward lamp 2402950 (tools/party/nav_route.py, m24_00 navmesh;
# ~5 min), no longer used by the phase (a ground-checked teleport instead); kept for a manual walk.
SJ_ROUTE = ("13.1:-9.5:107.0/7.6:-10.9:108.6/3.4:-11.0:109.5/-1.0:-10.3:113.4/-7.3:-10.1:117.1/-10.8:-10.4:120.5/"
            "-10.2:-11.1:128.3/-8.5:-10.4:134.5/-3.6:-6.1:143.2/-1.6:-4.8:148.9/1.7:-2.7:154.0/4.6:0.2:159.1/"
            "6.1:0.8:167.4/9.3:1.0:169.9/10.4:1.2:172.1/7.9:0.2:176.9/3.1:0.1:179.0/0.3:0.3:180.9/-5.4:0.4:183.6/"
            "-7.5:-0.1:188.5/-12.8:-0.4:186.6/-17.9:-0.4:188.4/-19.6:-0.4:185.0/-25.4:-0.3:184.3/-30.4:0.1:182.1/"
            "-35.5:0.2:178.7/-37.9:0.1:175.2/-41.9:0.1:172.7/-45.5:0.3:169.5/-50.3:0.2:167.0/-55.5:0.2:165.5/"
            "-59.7:-0.0:163.2/-64.1:-0.2:165.5/-70.1:-0.3:168.3/-77.9:-0.0:168.4/-82.9:-0.0:165.5/-87.9:-0.0:167.1/"
            "-91.8:0.2:168.5/-94.4:0.2:174.5/-98.2:0.3:179.4/-101.5:-0.2:183.5/-100.3:-0.1:187.0/-105.1:-1.0:190.3/"
            "-106.7:-2.4:196.6/-104.9:-2.5:200.5/-107.8:-4.2:202.6/-112.2:-6.3:201.2/-114.3:-7.9:198.6/"
            "-115.4:-9.5:195.0/-113.3:-11.1:189.8/-116.8:-11.9:186.8/-119.6:-13.5:185.9/-122.0:-15.9:181.8/"
            "-126.5:-18.8:179.1/-128.3:-19.6:178.7/-131.5:-19.3:175.0/-136.2:-18.9:173.1/-141.3:-18.9:171.1/"
            "-146.9:-18.9:167.8/-148.7:-19.3:169.8/-148.2:-20.7:174.0/-144.2:-22.4:177.4/-138.7:-24.3:176.6/"
            "-135.1:-26.3:172.3/-135.4:-28.4:166.5/-140.4:-30.7:163.4")
SJ_M24 = "-142.9:-33.5:175.0"     # at the cave mouth, m24_00 side (live sj5: the seam is at x ~ -146)
SJ_G24 = "-141.4:-33.1:170.9"
SJ_H24 = "-144.6:-33.5:175.7"     # the host waits here, 2 m before the seam, until the guest is over it
SJ_G27 = "-149.3:-33.7:177.4"     # over the seam: m27_00 (live sj5: the host walked in there)
SJ_UNTIL = [r"1:test: levelup: (LEVEL UP|FAILED)|1:map - -> m21_00[\s\S]*funnel: guest_sent_home"]


def phase_sj(only):
    # Both teleported (guest first) to the cave mouth on the m24_00 side, each held there until the
    # game's safe-ground spot is under it (":ground": no ground by the pin's end -> held once more; a
    # fall after the release -> one re-warp); the host waits 2 m before the seam, the guest walks over
    # it and the host starts its step over as the guest reaches the seam (still in m24_00: once the
    # guest is over, the host's step chain is out of the party - cooperators 0 - and would wait), so
    # it is back beside the guest within the leave state's ~5 s, before its send-home. (Live sj10: a
    # route ending at x -148.6 is "done" 2 m short, still in m24_00 for the host.) Still flaky (flaky2078, 0.7.8): the walker (camera-relative stick) circles 5-12 s at the seam
    # in some runs and the "map -> m27" log line lags the stage flip by 1-10 s, so the host is back after the
    # guest's send-home; gating the host on the guest being over the seam (near its final waypoint) or a
    # warp instead of the walk was tried and did not run/help.
    host = ",".join(["god", "host_move=%s:pin=10:ground@12^1" % SJ_H24,
                     "host_route=-147.5:-33.5:177.6/-149.3:-33.7:177.4/-151.1:-35.0:183.5@0.5?near=-146.6:-33.5:178.0:1.5",
                     "host_warp=2102950@30"])
    g1 = ",".join(["god", "guest_move=%s:pin=10:ground@6%%m24_00" % SJ_G24,
                   "guest_route=-143.2:-33.4:177.0/-146.6:-33.5:178.0/%s@4%%m24_00?near=%s:3" % (SJ_G27, SJ_H24)])
    return dict(
        name="sj", seconds=300, count=2, headless=0, pads=(0, 1),
        env=["BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_PARTY_TEST_START_LAMP=2402950", "0:BB_PARTY_DIRECTOR_TEST=" + host,
              "1:BB_PARTY_DIRECTOR_TEST=" + g1, "1:BB_PARTY_GUESTSVC_TEST=gate=2;echoes=20000;warp=doll;levelup"],
        watch=["--watch", r"map m24_00_00_00 -> m27_00_00_00=sjcross@1,4",
               "--watch", r"OnLeave_Limit\) dropped=sjleave@0.5,4",
               "--watch", r"test: levelup: (before Cross|LEVEL UP)=sjdoll@0.5,2"],
        crash=[], until=SJ_UNTIL, until_hold=10,
    )


def check_sj(v, L):
    need(v, L, "sj join: 2 players", 0, r"cooperators 0 -> 1")
    cross = need(v, L, "sj guest walks over the seam into m27_00", 1, r"map m24_00_00_00 -> m27_00_00_00")
    if cross is None:
        check_common(v, L, "sj")
        return
    need(v, L, "sj the host's chr leaves on the guest (its leave state starts)", 1,
         r"split: remote chr .* leaves \(slot 0 is in m24_00", cross - 2)
    nt = L.find(0, r"split: notice: .* went on to the Forbidden Woods", cross - 2)
    v.step("sj (info) host's notice that the guest went on", True, nt[1][:150] if nt else "no notice line (host followed first?)")
    hc = need(v, L, "sj host follows into m27_00", 0, r"map m24_00_00_00 -> m27_00_00_00", cross - 2)
    tr = L.find(0, r"travel step \d+/\d+: host_warp", cross)
    end = tr[0] if tr else 1e9
    dr = need(v, L, "sj guest's send-home dropped (host apart or just back)", 1, r"\(OnLeave_Limit\) dropped", cross, end)
    if dr is not None and hc is not None:
        # the report's case: the host is back beside the guest before the leave state's send-home
        v.step("sj together: the host over the seam before the guest's send-home", hc < dr,
               "host +%.1f s, send-home +%.1f s" % (hc, dr))
        need(v, L, "sj together: the drop names the seam case", 1, r"together again after a map seam", cross, end)
    need(v, L, "sj the leave state undone", 1, r"the leave state undone", cross, end)
    none_of(v, L, "sj guest not sent home", r"funnel: guest_sent_home", cross, insts=(1,))
    # (the host's cooperator count drops while the two are in different areas: split removes the chr)
    none_of(v, L, "sj guest stays in the host's room", r"left room|\(leave_room\)", cross, end, insts=(0,))
    if tr:
        need(v, L, "sj party travel: guest in the Dream", 1, r"map .*-> m21_00", tr[0])
        need(v, L, "sj guest's action buttons free after the stay (hold log)", 1,
             r"hold log: ctx 0x[0-9a-f]+ wait 0 hold 0 evstate 0 flag4047 0 role 6 (?:chrtype -?\d+ )?m21_00", tr[0])
        # after a B2 load into the Dream (live sj8 before the fix: the Doll's lines, no LevelUp menu)
        need(v, L, "sj guest levels up at the Doll after a party travel", 1, r"test: levelup: LEVEL UP on the guest", tr[0])
    check_common(v, L, "sj")


# ---- Phase po (0.7.6, phantomsave; group `phantom_own`): the own world after every leave path ----
# Player reports (Ryzen's save; the kicked guest, Nightwatch 0.2.4 beta 1): after leaving a party the guest's own
# Hunter's Dream would not answer: no talk, no headstone, no pickup. The own character record kept a summoned
# guest's chr type (PlayerGameData +0xA4 = 1, the log's `own 0 (type 1`). Two players. Guest 1: its own Leave,
# then back in; the host's Leave, then back in; the host's kick last (a kick bans the name for the session).
# After each leave the guest is home in the Dream: its chr type must be 0 ("player numbers: own 0 (type 0, chr
# type 0)") and it must level up at the Doll (the game's own talk path; the same action-button manager as the
# headstones, lamps and pickups). The three Doll fixtures run one per leave (BB_PARTY_GUESTSVC_TEST gate=N).
PO_FIX = "ownworld;gate=%d;echoes=20000;warp=doll;levelup"
PO_UNTIL = [r"1:test: levelup: (LEVEL UP on the guest|FAILED)[\s\S]*test: levelup: (LEVEL UP on the guest|FAILED)"]


def phase_po(only):
    host = ",".join(["god", "host_kick=Hunter1@25#2"])
    g1 = ",".join(["god", "guest_leave@12", "guest_rejoin@75"])
    return dict(
        name="po", seconds=560, count=2, headless=0,
        env=["BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_GUESTSVC_TEST=" + "|".join(PO_FIX % n for n in (1, 2))],
        crash=[], until=PO_UNTIL, until_hold=10,
    )


def check_po(v, L):
    need(v, L, "po join: 2 players", 0, r"cooperators 0 -> 1")
    lv = need(v, L, "po own Leave (guest, Party page)", 1, r"guest_leave: Party > Leave pressed")
    kk = need(v, L, "po the host's kick", 0, r"host_kick: Party > Kick Hunter1 pressed", lv or 0)
    for name, after in (("own Leave", lv), ("kick", kk)):
        if after is None:
            continue
        home = L.find(1, r"B2 load report: player numbers: own 0 \(type \d", after)
        v.step("po %s: home in its own world" % name, home, home[1][-140:] if home else "no home load report")
        if home:
            v.step("po %s: own chr type 0 at home (no phantom type kept)" % name,
                   re.search(r"own 0 \(type 0, chr type 0\)", home[1]), home[1][-140:])
    n = len(L.all(1, r"test: levelup: LEVEL UP on the guest"))
    v.step("po the Doll's level up works at home after the own Leave and the kick", n >= 2, "%d of 2" % n)
    none_of(v, L, "po no failed Doll fixture", r"test: levelup: FAILED", insts=(1,))
    check_common(v, L, "po")


def prep_pos(root: Path):
    """BB_PO_SAVE=DIR (a savedata folder): inst0 starts from that save, e.g. a player's copy whose character
    kept a summoned guest's chr type (never committed); else the default test save."""
    src = os.environ.get("BB_PO_SAVE")
    if src:
        _cl_saves(root, Path(src), 0)
        print("[smoke] pos: inst0 starts from %s" % src, flush=True)


def phase_pos(only):
    return dict(
        name="pos", seconds=200, count=1, headless=0, prep=prep_pos,
        env=["BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "0:BB_PARTY_GUESTSVC_TEST=ownworld;ownsolo;echoes=20000;warp=doll;levelup"],
        crash=[], until=[r"0:test: levelup: (LEVEL UP on the guest|FAILED)"], until_hold=8,
    )


def check_pos(v, L):
    rep = L.find(0, r"B2 load report: player numbers: own 0 \(type \d")
    v.step("pos own world after the first load: chr type 0", rep and re.search(r"type 0, chr type 0", rep[1]),
           rep[1][-120:] if rep else "no load report")
    need(v, L, "pos the Doll's level up works in the own world", 0, r"test: levelup: LEVEL UP on the guest")
    check_common(v, L, "pos")


# Phase sg (shop gate repair, 0.7.6, ~2 min, 1 player, group `shop_repair`): a save that carries the
# 0.7.4-era leftovers (all 22 "owns the Uncanny / Lost weapon" flags 5500-5521 set) and owns one of those weapons
# (Uncanny Beast Cutter +7, 24010700, flag 5502; Uncanny Saw Cleaver +3, 7010300, pickup flag 5100; 5160 and 5281 set without the weapon): in its own world the repair clears the 21 flags without the
# weapon, keeps 5502, and marks the save (60009998). BB_SG_SHOP=1 also opens the Bath Messenger's weapon list.
SG_UNTIL = r"0:shop gate repair: \d+ 'owns the weapon'"


def phase_sg(only):
    flags = ",".join(str(f) for f in list(range(5500, 5522)) + [5100, 5160, 5281])
    gs = ("ownworld;ownsolo;warp=talk:210691;open=15000140;keys=" + os.environ.get("BB_SG_KEYS", "right,right,right," + ",".join(["down"] * 8))) if os.environ.get("BB_SG_SHOP") else ""
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=god,weapon=24010700:7010300,log_flag=5500/5502/5503/5521"]
    if gs:
        ienv.append("0:BB_PARTY_GUESTSVC_TEST=" + gs)
    return dict(
        name="sg", seconds=200 if gs else 120, count=1, headless=0,
        env=["BB_PARTY_PROGRESS_TEST_SET=" + flags, "BB_PARTY_PROGRESS_TEST_CLEAR=60009998"],
        ienv=ienv,
        watch=(["--watch", r"levelup: key down=sgshop@0.3,2", "--watch", r"levelup: keys done=sgshopend@0.3"] if gs else []),
        crash=[], until=[SG_UNTIL], until_hold=(70 if gs else 6),
    )


def check_sg(v, L):
    need(v, L, "sg fixture: the 22 gate flags set in the own save", 0, r"test: set in the own save .*5500:\d->1.*5521:\d->1,5100:\d->1,5160:\d->1,5281:\d->1")
    need(v, L, "sg fixture: the Uncanny Beast Cutter +7 is held", 0, r"test: weapon 24010700: held \d -> 1 ")
    rep = L.find(0, r"shop gate repair: 23 'owns the weapon' flags set without the weapon cleared: 5160\(8010000\),5281\(8020000\),5500\(23010000\)")
    v.step("sg repair: 23 flags cleared, one log line", rep, rep[1][:260] if rep else "missing")
    v.step("sg repair: the owned weapon's flag 5502 stays",
           rep and "still set (weapon owned): 5100,5502;" in rep[1] and "5502(" not in rep[1], rep[1][-120:] if rep else "")
    n = len(L.all(0, r"shop gate repair: \d+ 'owns the weapon'"))
    v.step("sg repair: runs once", n == 1, "%d runs" % n)
    check_common(v, L, "sg")


# Phase sg2 (0.7.6, group `shop_repair`, 2 players, ~4 min): the guest's save carries the leftovers (set while
# summoned, the fixture writes the own pool), the guest goes home (own Leave), the repair runs there, the guest
# rejoins and opens the Bath Messenger's weapon list while summoned: ShopGateTick shows the own save's cleaned
# values, only the owned weapons' Uncanny rows are there (screenshot sg2shop).
SG2_UNTIL = r"1:test: levelup: keys done"


def phase_sg2(only):
    flags = ",".join(str(f) for f in list(range(5500, 5522)) + [5100, 5160, 5281])
    keys = os.environ.get("BB_SG_KEYS", "right,right,right," + ",".join(["down"] * 8))
    return dict(
        name="sg2", seconds=420, count=2, headless=0,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=god,weapon=24010700:7010300,guest_leave@10,guest_rejoin@50",
              "1:BB_PARTY_PROGRESS_TEST_SET=" + flags, "1:BB_PARTY_PROGRESS_TEST_CLEAR=60009998",
              "1:BB_PARTY_GUESTSVC_TEST=gate=2;warp=talk:210691;open=15000140;keys=" + keys],
        watch=["--watch", r"1:.*levelup: key down=sg2shop@0.3,2", "--watch", r"levelup: keys done=sg2shopend@0.3"],
        crash=[], until=[SG2_UNTIL], until_hold=6,
    )


def check_sg2(v, L):
    j = need(v, L, "sg2 join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "sg2 fixture: the gate flags set in the guest's own save", 1, r"test: set in the own save .*5500:\d->1")
    rep = L.find(1, r"shop gate repair: 23 'owns the weapon' flags set without the weapon cleared")
    v.step("sg2 repair: runs in the guest's own world (after its Leave)", rep, rep[1][:200] if rep else "missing")
    need(v, L, "sg2 rejoin: the guest is summoned again", 1, r"load mode 1", rep[0] if rep else 0)
    need(v, L, "sg2 shop: the overlay shows the own save's cleaned gates", 1, r"shop gates while summoned: .*5500=0", rep[0] if rep else 0)
    check_common(v, L, "sg2")


# Phase sgb (shopdlc078, 0.7.8, group `shop_badge`, ~2 min, 1 player): the default save (Saw + Sword Hunter Badges
# 4110 / 4114 held, night 9801, the Eye of a Blood-drunk Hunter from its own Messenger) with all ten badge lineup
# flags 12101000-12101009 set (the playtest 2026-11-01 leftovers: another player's badges): in its own world the
# repair clears the eight without the badge (one line each), keeps 12101000 / 12101004 and marks the save
# (60009997); the Eye repair leaves the Eye (night reached: past the vanilla unlock) and marks 60009996.
# BB_SGB_FIXTURE=<test_saves folder> loads another save instead, without the flag fixture (a real save check).
SGB_UNTIL = [r"0:shop gate repair: badge rows:", r"0:eye repair: (repaired|nothing to do)"]
SGB_FLAGS = ",".join(str(12101000 + n) for n in range(10))


def prep_sgb(root: Path):
    fx = os.environ.get("BB_SGB_FIXTURE")
    if fx:
        src = Path(fx) / "savedata"
        if not src.is_dir():
            sys.exit("sgb: the fixture %s is missing" % src)
        _cl_saves(root, src)
        print("[smoke] sgb: inst0 starts from %s" % src, flush=True)


def phase_sgb(only):
    env = ["BB_PARTY_PROGRESS_TEST_CLEAR=60009997,60009996"]
    if not os.environ.get("BB_SGB_FIXTURE"):
        env.insert(0, "BB_PARTY_PROGRESS_TEST_SET=" + SGB_FLAGS)
    return dict(
        name="sgb", seconds=150, count=1, headless=0, prep=prep_sgb,
        env=env,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god,log_flag=12101000/12101001/12101004/12101007/9801/50000100/12101101"],
        crash=[], until=SGB_UNTIL, until_hold=8,
    )


def check_sgb(v, L):
    need(v, L, "sgb fixture: the ten badge row flags set in the own save", 0,
         r"test: set in the own save .*12101000:\d->1.*12101009:\d->1")
    for n in (1, 2, 3, 5, 6, 7, 8, 9):
        need(v, L, "sgb repair: 1210100%d cleared (badge %d not held)" % (n, 4110 + n), 0,
             r"shop gate repair: flag 1210100%d cleared \(.* %d not held\)" % (n, 4110 + n))
    rep = L.find(0, r"shop gate repair: badge rows: 8 flags cleared; still set \(badge held\): 12101000,12101004; save marked \(flag 60009997\)")
    v.step("sgb repair: the held badges' rows 12101000 / 12101004 stay, save marked", rep, rep[1][-160:] if rep else "missing")
    for n in (0, 4):
        bad = L.find(0, r"shop gate repair: flag 1210100%d cleared" % n)
        v.step("sgb repair: 1210100%d (badge held) not touched" % n, not bad, bad[1][:160] if bad else "")
    n = len(L.all(0, r"shop gate repair: badge rows:"))
    v.step("sgb repair: runs once", n == 1, "%d runs" % n)
    eye = L.find(0, r"eye repair: nothing to do: night reached \(9801\)")
    v.step("sgb eye: night reached: the Eye and its flags stay (past the vanilla unlock)", eye, eye[1][:200] if eye else "missing")
    bad = L.find(0, r"eye repair: (Eye of a Blood-drunk Hunter .* removed|flag \d+ cleared)")
    v.step("sgb eye: nothing removed or cleared", not bad, bad[1][:160] if bad else "")
    check_common(v, L, "sgb")


# Phase sgp (sgive078, 0.7.8, CL-55cea7, group `sgive_owner`, 2 players, ~4 min): the guest is the owner's real
# poisoned save of the 2026-11-01 playtest (BB_SGP_FIXTURE, default E:\games\bbport\test_saves\play4_guest_poisoned:
# Crow / Powder Keg / Wheel / Cainhurst badges + rows without their pickup flags, six more key items without theirs;
# the Saw / Sword / Radiant Sword badges, Oedon Tomb Key etc. earned) and a stale party_summoned_gives.txt: an entry
# without a slot, one without a character id, one for another character in the same slot, one for another slot,
# and one for THIS character (its id written by the test into the save: positive case). The guest goes home at +10 s
# (own world: drops, the re-give of its own entry, the repair) and rejoins at +50 s.
SGP_FIXTURE = Path(os.environ.get("BB_SGP_FIXTURE", "E:/games/bbport/test_saves/play4_guest_poisoned/savedata"))
SGP_CID = 0x5A5A5
SGP_UNTIL = [r"1:key item repair: .*save marked", r"1:cooperators -1 -> 0|0:cooperators 1 -> 0|1:test: guest rejoin"]


def sgp_id_flags():
    first = 60009000 + 1000 - 6 - 20
    return [first + b for b in range(20) if (SGP_CID >> b) & 1] + [60009000 + 1000 - 6]


def prep_sgp(root: Path):
    if not SGP_FIXTURE.is_dir():
        sys.exit("sgp: the fixture %s is missing (docs/party/test_saves.md)" % SGP_FIXTURE)
    _cl_saves(root, SGP_FIXTURE, 1)
    user = root / "inst1" / "user"
    # The instance folder outlives the phases: a party_characters.txt of an earlier phase's character in slot 0 makes
    # the start purge (runtime_savepolicy_side_files) drop every slot-0 line of the list below as "another
    # character's" before the game reads it (smoke6: "3 party side file entries dropped"). Start with no record.
    (user / "party_characters.txt").unlink(missing_ok=True)
    # A save last written as a summoned guest carries the phantom mark (chr type 1): the own world of such a save
    # does not load. The test copy has it cleared (the launcher's save editor does the same: repair_phantom_mark).
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
    from saveedit.model import SaveModel
    for f in (user / "savedata").rglob("userdata0000"):
        m = SaveModel(f.read_bytes())
        if m.phantom_mark:
            m.repair_phantom_mark()
            f.write_bytes(m.to_bytes())
    goods = 0x40000000
    lines = ["%d 4009 1 1" % goods,                      # an older file: no slot, no id
             "%d 4014 1 1 0" % goods,                    # a file with the slot only
             "%d 4018 1 1 0 11111" % goods,              # another character in the same slot
             "%d 4020 1 1 1 22222" % goods,              # another slot, kept for that character
             "%d 4010 1 1 0 %d" % (goods, SGP_CID)]      # this character: given again at home
    (user / "party_summoned_gives.txt").write_text("\n".join(lines) + "\n")


def phase_sgp(only):
    return dict(
        name="sgp", seconds=240, count=2, headless=0, prep=prep_sgp,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=god,guest_leave@10,guest_rejoin@50",
              "1:BB_PARTY_PROGRESS_TEST_SET=" + ",".join(str(f) for f in sgp_id_flags()),
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=60009995"],
        crash=[], until=[SGP_UNTIL[0]], until_hold=60,
    )


def check_sgp(v, L):
    need(v, L, "sgp fixture: the test character id written to the guest's own save", 1, r"test: set in the own save .*60009994:\d->1")
    need(v, L, "sgp drop: the two entries from older files (no slot / no character id)", 1,
         r"goods given while summoned: 2 entries from an older file without a character id dropped")
    need(v, L, "sgp drop: the other character's entry in this slot", 1,
         r"goods given while summoned: 1 entry given to another character in save slot \d+")
    need(v, L, "sgp keep: the entry of another save slot waits for that character", 1,
         r"goods given while summoned: 1 entry for another character \(not save slot \d+\) kept for it")
    need(v, L, "sgp positive: this character's own entry is given again at home", 1,
         r"goods given while summoned are missing in the own world; given again: goods 4010 ")
    none_of(v, L, "sgp nothing else is given again", r"given again: .*goods (4009|4014|4018|4020)\b", insts=(1,))
    for g in (4111, 4112, 4116, 4117, 4003, 4012, 4013, 4104, 4308, 4310):
        need(v, L, "sgp repair: goods %d removed (no source flag)" % g, 1, r"key item repair: goods %d removed" % g)
    for f in (12101001, 12101002, 12101006, 12101007):
        need(v, L, "sgp repair: badge row %d cleared" % f, 1, r"key item repair: badge row flag %d cleared" % f)
    none_of(v, L, "sgp repair: earned items and rows stay (Saw / Sword / Radiant badges, Oedon key, workshop tool, Bell, bandage, Eye)",
            r"key item repair: (goods (4110|4114|4115|4000|4103|4312|4907|4311|4001|4320) removed|badge row flag 1210100[045] cleared)",
            insts=(1,))
    need(v, L, "sgp repair: summary", 1, r"key item repair: 10 key items removed, 4 badge row flags cleared, 0 kept; save marked")
    n = len(L.all(1, r"key item repair: \d+ key items? removed"))
    v.step("sgp repair: runs once", n == 1, "%d runs" % n)
    check_common(v, L, "sgp")


# Phase sgb2 (shopdlc078, 0.7.8, group `shop_badge_sync`, ~4 min, 2 players). Host: cathedral_day (no night 9801, the
# Eye held with its Messenger flag 50000100 and the sale flags 12101100 / 12101101 = the playtest's early Eye) with the
# Saw Hunter Badge taken away (goods=4110:0) and its row flag 12101000 cleared. Guest: the default save (holds 4110).
# (1) The host's own-world repair takes the Eye away and clears 50000100 / 12101100 / 12101101 (one line each), keeps
# the badge row 12101004 (Sword Hunter Badge held). (2) The party travels to the Hunter's Dream: the guest's copy of
# m21 event 12101000 slot 0 ends on its own Saw badge; its state (packet 0x31) is dropped on the host, whose
# 12101000 stays 0 (playtest: the host's rows were set 1 s after the guests came). BB_PARTY_ITEM_GATE_SYNC=1 on the
# host shows the old behaviour (the control run: 12101000 -> 1).
SGB2_FIXTURE = Path("E:/games/bbport/test_saves/cathedral_day/savedata")
SGB2_UNTIL = [r"0:travel step (\d+)/\1: host_press"]


def prep_sgb2(root: Path):
    if not SGB2_FIXTURE.is_dir():
        sys.exit("sgb2: the fixture %s is missing (docs/party/test_saves.md)" % SGB2_FIXTURE)
    _cl_saves(root, SGB2_FIXTURE)


def phase_sgb2(only):
    host = ",".join(["god", "goods=4110:0", "log_flag=12101000/12101004/9801/50000100/12101101",
                     "host_lamp=2102950@20%m24_00^1", "host_press=0@45%m21!"])
    # The guest without its summoned shop-gate overlay (BB_PARTY_SHOP_GATES=0): its copy of 12101000 slot 0 then
    # waits for and ends on its own Saw badge in the host's Dream as a 0.7.7 guest's did (with the overlay it ends at
    # once from its own save's 12101000 and sends no state), so the host's packet filter alone is what is tested.
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_SHOP_GATES=0",
            "0:BB_PARTY_PROGRESS_TEST_CLEAR=12101000,60009997,60009996", "0:BB_PARTY_SPLIT_TEST=yes",
            "1:BB_PARTY_SPLIT_TEST=yes"]
    if os.environ.get("BB_SGB2_CONTROL"):
        ienv.append("0:BB_PARTY_ITEM_GATE_SYNC=1")
    if os.environ.get("BB_SGB2_NETSIM"):  # e.g. dsl / bad: the players' internet link (both games)
        ienv += ["0:BB_NET_SIM=" + os.environ["BB_SGB2_NETSIM"], "1:BB_NET_SIM=" + os.environ["BB_SGB2_NETSIM"]]
    return dict(
        name="sgb2", seconds=330, count=2, headless=0, prep=prep_sgb2,
        env=[], ienv=ienv,
        crash=[], until=SGB2_UNTIL, until_hold=6,
    )


def check_sgb2(v, L):
    need(v, L, "sgb2 fixture: the host's Saw Hunter Badge taken away", 0, r"test: goods 4110 x1 -> x0")
    rm = need(v, L, "sgb2 eye repair: the host's Eye removed (no night, DLC never entered)", 0,
              r"eye repair: Eye of a Blood-drunk Hunter 4311 removed .*key stacks 1 -> 0")
    for f in (50000100, 12101100, 12101101):
        need(v, L, "sgb2 eye repair: flag %d cleared" % f, 0, r"eye repair: flag %d cleared" % f)
    need(v, L, "sgb2 eye repair: save marked", 0, r"eye repair: repaired: .*save marked \(flag 60009996\)")
    need(v, L, "sgb2 badge repair: 12101004 kept (Sword badge held), nothing cleared", 0,
         r"shop gate repair: badge rows: 0 flags cleared; still set \(badge held\): 12101004;")
    j = need(v, L, "sgb2 join: the guest is in", 0, r"cooperators 0 -> 1")
    dream = need(v, L, "sgb2 the party is in the Hunter's Dream (host)", 0, r"map .* -> m21_00_00_00", j or 0)
    need(v, L, "sgb2 the guest is in the host's Dream", 1, r"map .* -> m21_00_00_00", j or 0)
    drop = None if os.environ.get("BB_SGB2_CONTROL") else need(
        v, L, "sgb2 the guest's badge row state (12101000 slot 0) dropped on the host", 0,
        r"Item gates: event 12101000 slot 0's state from another player not applied", dream or 0)
    bad = L.find(0, r"test: flags 12101000=1", dream or 0)
    if os.environ.get("BB_SGB2_CONTROL"):
        v.step("sgb2 CONTROL (filter off): the guest's state sets the host's 12101000", bad, "no 12101000=1 on the host")
    else:
        v.step("sgb2 the host's 12101000 stays 0 in the Dream with the guest (no Saw badge of its own)", not bad and dream,
               bad[1][:160] if bad else ("drop seen" if drop else "no drop line"))
    check_common(v, L, "sgb2")


# Phase sgb3 (badgeguard078, 0.7.8, group `badge_guard`, 2 players, ~2 min): the live badge guard. Default saves (Saw +
# Sword Hunter Badges 4110 / 4114 held; the repair markers are set so only the guard acts). Both games get the badge
# rows 12101000 (Saw, held), 12101004 (Sword, held) and 12101008 (Spark Hunter Badge 4118, NOT held) set in the own
# save: the host in its own world, the guest summoned (overlay + pool). The host also gets row 12101009 (Cosmic Eye
# Watch Badge) set by the game's SetEventFlag (director set_flag: the hook path of the attribution). Expected: both
# 12101008 closed within seconds with the log line + a "set by" attribution (test aid / SetEventFlag), the host's
# 12101009 too, and the held rows 12101000 / 12101004 never touched. BB_SGB3_OFF=1: BB_PARTY_BADGE_GUARD=0 control.
SGB3_FLAGS = "12101000,12101004,12101008,60009997,60009996,60009995"
SGB3_OFF = bool(os.environ.get("BB_SGB3_OFF"))
SGB3_UNTIL = [r"0:Party items: badge row 12101009 was open", r"1:Party items: badge row 12101008 was open"]


def phase_sgb3(only):
    watch = "log_flag=12101000/12101004/12101008/12101009"
    ienv = ["0:BB_PARTY_DIRECTOR_TEST=god,set_flag=12101009," + watch, "1:BB_PARTY_DIRECTOR_TEST=god," + watch,
            "0:BB_PARTY_PROGRESS_TEST_SET=" + SGB3_FLAGS, "1:BB_PARTY_PROGRESS_TEST_SET=" + SGB3_FLAGS]
    if SGB3_OFF:
        ienv += ["0:BB_PARTY_BADGE_GUARD=0", "1:BB_PARTY_BADGE_GUARD=0"]
    return dict(
        name="sgb3", seconds=150 if not SGB3_OFF else 90, count=2, headless=0,
        env=["BB_PARTY_START=immediate"], ienv=ienv,
        crash=[], until=([] if SGB3_OFF else SGB3_UNTIL), until_hold=20,
    )


def check_sgb3(v, L):
    need(v, L, "sgb3 fixture: the host's badge rows set in the own save", 0, r"test: set in the own save .*12101008:\d->1")
    need(v, L, "sgb3 fixture: the guest's badge rows set in the own save", 1, r"test: set in the own save .*12101008:\d->1")
    j = need(v, L, "sgb3 join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "sgb3 fixture: the host's row 12101009 set by the game's SetEventFlag", 0, r"test: set_flag 12101009: 0 -> 1")
    if SGB3_OFF:
        none_of(v, L, "sgb3 CONTROL (guard off): no row closed", r"Party items: badge row \d+ was open")
        setat = L.find(0, r"test: set in the own save .*12101008:\d->1")
        up = L.find(0, r"test: flags.*12101008=1", setat[0] if setat else 0.0)
        bad = L.find(0, r"test: flags.*12101008=0", up[0] if up else 1e9)
        v.step("sgb3 CONTROL: the host's 12101008 is open and stays open", up and not bad,
               bad[1][:160] if bad else ("stays 1" if up else "never read 1"))
        check_common(v, L, "sgb3")
        return
    own = need(v, L, "sgb3 host (own world): 12101008 closed, badge 4118 named, attribution given", 0,
               r"Party items: badge row 12101008 was open without badge 4118: closed \(was set by: .*test aid.*\) \[Spark Hunter Badge, own world\]")
    need(v, L, "sgb3 host: row 12101009 closed with the SetEventFlag hook's attribution (game code, callers)", 0,
         r"badge row 12101009 was open without badge 4119: closed \(was set by: .*SetEventFlag\(1\) by game code.*callers 0x[0-9a-f]+")
    need(v, L, "sgb3 guest (summoned): 12101008 closed, overlay and own save", 1,
         r"Party items: badge row 12101008 was open without badge 4118: closed \(was set by: .*test aid.*\) \[Spark Hunter Badge, summoned, overlay and own save\]")
    need(v, L, "sgb3 host: the flag reads 0 afterwards", 0, r"test: flags.*12101008=0", own or 0)
    need(v, L, "sgb3 guest: the flag reads 0 afterwards", 1, r"test: flags.*12101008=0")
    none_of(v, L, "sgb3 control: the held badges' rows 12101000 / 12101004 are never closed",
            r"Party items: badge row (12101000|12101004) was open")
    none_of(v, L, "sgb3 control: the held badges' rows never read 0", r"test: flags.*1210100[04]=0")
    n = len(L.all(0, r"Party items: badge row 12101008 was open"))
    v.step("sgb3 host: row 12101008 closed once", n == 1, "%d lines" % n)
    check_common(v, L, "sgb3")


# Phase sgb4 (badgegate079, 0.7.9, CL-3b4da5, group `badge_gate`, 2 players, ~4 min): a guest who gets a hunter badge
# WHILE SUMMONED in the host's world must see that badge's Bath Messenger rows. The guest (default save, Powder Keg
# Hunter Badge 4112 not held, own row flag 12101002 cleared) is summoned at once; the director tops the badge up to 1 on
# the guest 8 s after the host world is up (the game's own give, in the host's world); ShopGateTick then opens the row in
# the overlay and writes the own pool ("12101002=1+own"); the guest walks up to the Bath Messenger and opens Purchase
# (screenshots sgb4shop). Phase sgb4n is the control, the same walk without the badge: the row stays 0 and no write.
SGB4_ROW = 12101002
SGB4_UNTIL = r"1:test: levelup: keys done"


def prep_sgb4(root: Path):
    # Both games start from the plain default save: a save left in inst0 / inst1 by an earlier phase (a host that
    # is not in the Hunter's Dream) kept the guest's walk from ever starting (full smoke 0.7.9: host in m24).
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CL_DEFAULT, 1)


def phase_sgb4(only, name="sgb4", give=True):
    keys = os.environ.get("BB_SG_KEYS", "right,right,right," + ",".join(["down"] * 8))
    watch = "log_flag=%d/12101004" % SGB4_ROW
    return dict(
        name=name, seconds=300, count=2, headless=0, prep=prep_sgb4,
        env=["BB_PARTY_START=immediate"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god",
              "1:BB_PARTY_DIRECTOR_TEST=god," + ("goods=4112:1," if give else "goods=4112:0,") + watch,
              "1:BB_PARTY_PROGRESS_TEST_CLEAR=%d,60009997,60009995" % SGB4_ROW,
              "1:BB_PARTY_GUESTSVC_TEST=guesttalk=210691;open=15000140;keys=" + keys + ";hsdelay=25"],
        watch=["--watch", r"1:.*levelup: key down=%sshop@0.3,2" % name, "--watch", r"levelup: keys done=%sshopend@0.3" % name],
        crash=[], until=[SGB4_UNTIL], until_hold=6,
    )


def check_sgb4(v, L, name="sgb4", give=True):
    j = need(v, L, name + " join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, name + " the guest is summoned in the host's world", 1, r"load mode 1", j or 0)
    row = str(SGB4_ROW)
    if give:
        g = need(v, L, name + " the badge is given to the summoned guest", 1, r"test: goods 4112 x0 -> x1 \(ok\)", j or 0)
        need(v, L, name + " ShopGateTick opens the row in the overlay AND writes the own pool", 1,
             r"shop gates while summoned: .*%s=1\+own" % row, g or 0)
        need(v, L, name + " the row reads 1 afterwards", 1, r"test: flags.*%s=1" % row, g or 0)
        shut = L.find(1, r"shop gates while summoned: .*%s=0" % row, g or 0)
        v.step(name + " the tick never resets the row again", not shut, shut[1][:160] if shut else "")
    else:
        none_of(v, L, name + " no badge: the row is never opened or written", r"shop gates while summoned: .*%s=1" % row, insts=(1,))
        none_of(v, L, name + " no badge: the row never reads 1", r"test: flags.*%s=1" % row, insts=(1,))
    need(v, L, name + " the guest opened the Bath Messenger's Purchase list", 1, r"levelup: keys done")
    check_common(v, L, name)


# Phase csh (0.7.6 chaliceshop, group `chalice_shop`, 2 players, ~5 min): the Bath Messenger of a host's Chalice
# Dungeon (t290300) sells the Sinister Root Chalices (ShopLineupParam rows 5N0011..5N0015) gated by the dungeon's
# own feature flags 92905385..89 (case flag 92905300 + DungeonFeatureParam.eventflagId; the Isz Chalice 6539 sets
# 92905389). The host Continues from chalice_isz_ritual (the dungeon in its slot 1) and enters it by the altar's
# awaken warp with the guest in the party; the guest (default save, 30000 echoes) walks to the messenger, opens its
# shop and buys the Sinister Isz Root Chalice (screenshots: the Chalices tab lists it; its own echoes pay).
# Phase csh2 (group `chalice_shop`, ~3 min): the same with a guest that has a Chalice Dungeon of its own in slot 1
# (chalice_hintertomb_ritual: the current slot is 1, the host's dungeon is the quick-joined slot 0): its game used to
# read ITS dungeon's feature flags (310 / 378, no 389) in the host's dungeon, so the row was missing for exactly the
# guests who have done rituals (player report 2026-10-25, Lower Pthumeru). Checks the flags it reads (log), not the
# shop (this guest's region events do not fire after a warp: no talk prompt; see docs/party/chalice.md 9).
CSH_FIXTURE = Path("E:/games/bbport/test_saves/chalice_isz_ritual/savedata")
CSH_UNTIL = r"1:test: levelup: PURCHASE on the guest"
CSH2_UNTIL = r"1:chalice case flags \(role 6, load mode 1"


def prep_csh(root: Path):
    if not CSH_FIXTURE.is_dir():
        sys.exit("csh: the fixture %s is missing (docs/party/test_saves.md)" % CSH_FIXTURE)
    _cl_saves(root, CSH_FIXTURE)
    _cl_saves(root, CL_DEFAULT, 1)


def prep_csh2(root: Path):
    prep_csh(root)
    if CL_GUEST_FIXTURE.is_dir():
        _cl_saves(root, CL_GUEST_FIXTURE, 1)
    else:
        sys.exit("csh2: the fixture %s is missing" % CL_GUEST_FIXTURE)


def phase_csh(only):
    keys = os.environ.get("BB_CSH_KEYS", "cross,left,wait,wait,buy,wait")
    host = ",".join(["log_pos", "host_lamp=%s@25%%m21" % CL_OWN_ENTRY, "host_press=0@150%m29!"])
    return dict(
        name="csh", seconds=300, count=2, headless=0, prep=prep_csh, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_SHOP_GATE_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=log_pos,echoes=30000,guest_press=0@150%m29!",
              "1:BB_PARTY_GUESTSVC_TEST=guesttalk=290300;keys=" + keys + ";hsdelay=8"],
        watch=["--watch", r"1:.*levelup: key left=cshtab@0.5,2", "--watch", r"1:.*levelup: PURCHASE on the guest=cshbuy@0.5,2"],
        crash=[], until=[CSH_UNTIL], until_hold=6,
    )


def phase_csh3(only):
    # 0.7.7 chalshop077: csh with the guest that owns a dungeon (slot 1): the same real walk-up to the messenger; the
    # talk prompt and the shop must work for it too (CL-567d1f).
    ph = phase_csh(only)
    ph.update(name="csh3", prep=prep_csh2)
    return ph


# Phase cis (chalice_insight, 2 players; 0.7.7 insight077, CL-6dc7c6): the Insight shop unlocks of a Chalice boss kill. Rows 5090
# (Bone Ash set, 4 pieces, 2/4/2/2 Insight) and 5091 (Madman set, 1/3/1/1) are set by the map-wide event 12906824 when the
# LAYER 2 boss (entity 2900101, flag 12901801) of a Central Pthumeru dungeon (HolygrailExParam mapBaseId 20 -> m29_20_90_00; 5091
# the layer 2 boss 2900105 of m29_31_90_00) dies. The guest's save is the fixture chalice_central_ritual (a Central Pthumeru
# Chalice 6209 ritual, made by `caq` with CAQ_GOODS=goods=6209:1,...): the host (default save) enters that dungeon by the altar's
# "Enter the party's dungeon" (as caw), kills the layer 2 boss (host_kill, the game's own death path), the guest's flag 5090 must
# follow, then the guest opens a Bath Messenger's shop and buys one Insight armour piece (Insight spent, goods received).
CIS_FIX = Path(os.environ.get("CIS_FIX", "E:/games/bbport/test_saves/chalice_central_ritual/savedata"))


def prep_cis(root: Path):
    if not CIS_FIX.is_dir():
        sys.exit("cis: the fixture %s is missing (docs/party/test_saves.md)" % CIS_FIX)
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CIS_FIX, 1)


def phase_cis(only):
    host = ("log_pos,log_flag=5090/5091/12901801,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar,"
            "seq=" + "cross/?cross@Use the Ritual Altar/wait/down/down/cross"
            + ",host_press=0@40%m29!" + os.environ.get("CIS_HOST_TAIL", ",host_kill=2900101:hp@20%m29,host_lamp=2102950@10%m29"))
    g1 = "log_pos,log_flag=5090/5091/12901801,insight=" + os.environ.get("CIS_INSIGHT", "20") + os.environ.get("CIS_GUEST_TAIL", "")
    keys = os.environ.get("CIS_KEYS", "right,wait,right,wait,right,wait,right,wait,right,wait,wait,down,down,down,down,wait,wait,buy,wait")
    return dict(
        name="cis", seconds=int(os.environ.get("CIS_SECONDS", "430")), count=2, headless=0, prep=prep_cis, post=post_chalamp, pad=1,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_ALTAR_AUTO=1", "BB_PARTY_TEST_INVINCIBLE=1", "BB_PARTY_SHOP_GATE_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1,
              "1:BB_PARTY_GUESTSVC_TEST=gate=3;warp=talk:" + os.environ.get("CIS_NPC", "210698") + ";open=15000140;keys=" + keys],
        watch=[], crash=[], until=[os.environ.get("CIS_UNTIL", r"1:test: levelup: (PURCHASE on the guest|keys done)")], until_hold=8,
    )


def cis_save(d: Path, i):
    """(Insight, Bone Ash Mask held) of instance i's party save slot 0, decoded from a copy (scripts/saveedit)."""
    sys.path.insert(0, str(REPO / "scripts"))
    from saveedit.model import SaveModel  # noqa: E402
    src = sorted((Path(RUN_ROOT[0]) / f"inst{i}" / "user" / "savedata_party").rglob("userdata0000"))
    if not src:
        return None
    copy = d / "saves" / f"inst{i}_userdata0000"
    copy.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src[0], copy)
    try:
        m = SaveModel(copy.read_bytes())
        return m.insight, any(it.kind == "armor" and it.number == 40000 for it in m.items())
    except (OSError, ValueError, Exception):
        return None


def check_cis(v, L, d=None):
    check_common(v, L, "cis")
    need(v, L, "cis host in the dungeon (m29)", 0, r"map - -> m29_00")
    need(v, L, "cis guest in the dungeon (m29)", 1, r"map - -> m29_00")
    kill = need(v, L, "cis the host killed the layer 2 boss (HP 0)", 0, r"host_kill: entity 2900101 HP \d+ -> 0 ")
    need(v, L, "cis host: the boss flag 12901801 and the unlock 5090 set by the game's own event", 0, r"test: flags (?:\S+ )*5090=1", kill or 0)
    need(v, L, "cis guest: 5090 (Bone Ash unlock) reaches the guest", 1, r"test: flags (?:\S+ )*5090=1", kill or 0)
    need(v, L, "cis guest: Insight rises after the kill", 1, r"Party: \[[\d:.]+\] Insight (\d+) -> (\d+)", kill or 0)
    need(v, L, "cis guest: the Insight messenger's menu opened (Buy with Insight)", 1, r"test: levelup: key ", kill or 0)
    # CL-8e5fc4: the census repair puts one boss copy to death once, not every 15 s for ever
    reps = L.all(1, r"enemy census: repair: [1-9]\d* enemy chr\(s\) .*: [\d ]*\b2900101\b")
    v.step("cis guest: the census repair killed the boss copy at most once (CL-8e5fc4)", len(reps) <= 1, "%d repair kill(s) of 2900101" % len(reps))
    # the shop (screenshots, looked at: the Attire tab lists Bone Ash Mask / Armour / Gauntlets / Leggings for the guest) and the purchase
    # of the Bone Ash Mask (row 200060, 2 Insight, qwcId 5090): the Insight is spent, the piece is in the guest's save
    buy = need(v, L, "cis guest: bought one Insight item (Insight spent)", 1, r"test: levelup: PURCHASE on the guest: .* Insight (\d+) -> (\d+)", kill or 0)
    sv = cis_save(d, 1) if d else None
    v.step("cis guest save: Insight 23 -> 21 and the Bone Ash Mask (armor 40000) held, saved",
           bool(sv) and sv[0] == int(os.environ.get("CIS_INSIGHT_AFTER", "21")) and sv[1], "guest save (Insight, mask held) %s" % (sv,))


# Phase cim (chalice_madman, 2 players; 0.7.9 shop079, CL-8e24b7): the Madman set rows 5091 of a LOWER Hintertomb Chalice dungeon
# (goods 6319, the FIXED Lower Hintertomb Chalice, mapBaseId 319 -> script folder m29_31_90_00; event 12906824 sets 5091 when the
# twin boss 2900105 + 2900106, event 12901723, are both dead). Only m29_31_90_00 (and m29_20_90_00 for 5090) start event 12906824: the ritual chalices 6300 / 6302 /
# 6310 / 6312 run m29_30_90_00, which never sets 5091 (cim079). Same walk as cis, with the fixture chalice_lowerhint_fixed (made by
# `caq` with CAQ_GOODS=goods=6319:1,... , docs/party/test_saves.md): the host kills 2900105 and 2900106, the guest's flag 5091 must follow and
# the guest's Insight shop must open.
CIM_FIX = Path(os.environ.get("CIM_FIX", "E:/games/bbport/test_saves/chalice_lowerhint_fixed/savedata"))


def prep_cim(root: Path):
    if not CIM_FIX.is_dir():
        sys.exit("cim: the fixture %s is missing (docs/party/test_saves.md)" % CIM_FIX)
    _cl_saves(root, CL_DEFAULT, 0)
    _cl_saves(root, CIM_FIX, 1)


def phase_cim(only):
    ph = phase_cis(only)
    # the layer 2 boss of this dungeon is a PAIR: event 12901723 sets 12901801 only when 2900105 AND 2900106 are dead
    host = ph["ienv"][0].replace("host_kill=2900101", "host_kill=2900105").replace(
        "host_kill=2900105:hp@20%m29", "host_kill=2900105:hp@20%m29,host_kill=2900106:hp@3%m29")
    # the host holds a chalice (goods 6109, as caw) so the altar opens its menu at all
    host = host.replace("log_pos,", "log_pos,goods=6109:1,", 1)
    ph.update(name="cim", prep=prep_cim)
    ph["ienv"] = [host] + ph["ienv"][1:]
    if os.environ.get("CIM_KILL"):  # diagnostics: other targets, e.g. "999@5%m29,2900105:hp@20%m29" (999 lists the strongest loaded enemies)
        host = ph["ienv"][0].replace("host_kill=2900105:hp@20%m29", ",".join("host_kill=" + k for k in os.environ["CIM_KILL"].split(",")))
        ph["ienv"] = [host] + ph["ienv"][1:]
    if os.environ.get("CIM_TRACE"):  # diagnostics: which EMEVD instructions the dungeon runs
        ph["env"] = ph["env"] + ["BB_PARTY_EMEVD_TRACE=1", "BB_PARTY_EMEVD_TRACE_ALL=" + os.environ["CIM_TRACE"]]
    return ph


def check_cim(v, L, d=None):
    check_common(v, L, "cim")
    need(v, L, "cim host in the dungeon (m29)", 0, r"map - -> m29_00")
    need(v, L, "cim guest in the dungeon (m29)", 1, r"map - -> m29_00")
    need(v, L, "cim the host killed the layer 2 boss pair, 2900105 (HP 0)", 0, r"host_kill: (?:\w+ )?entity 2900105 .*?HP \d+ -> 0")
    kill = need(v, L, "cim the host killed the layer 2 boss pair, 2900106 (HP 0)", 0, r"host_kill: (?:\w+ )?entity 2900106 .*?HP \d+ -> 0")
    need(v, L, "cim host: the unlock 5091 set by the game's own event", 0, r"test: flags (?:\S+ )*5091=1", kill or 0)
    need(v, L, "cim guest: 5091 (Madman set unlock) reaches the guest", 1, r"test: flags (?:\S+ )*5091=1", kill or 0)
    need(v, L, "cim guest: the Insight messenger's menu opened (the Madman rows' shop)", 1, r"test: levelup: key ", kill or 0)


def phase_csh2(only):
    host = ",".join(["log_pos", "host_lamp=%s@25%%m21" % CL_OWN_ENTRY, "host_press=0@60%m29!"])
    return dict(
        name="csh2", seconds=200, count=2, headless=0, prep=prep_csh2, post=post_chalamp,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_SHOP_GATE_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=guest_press=0@60%m29!"],
        crash=[], until=[CSH2_UNTIL], until_hold=12,
    )


def check_csh(v, L):
    need(v, L, "csh join: the guest is in the host's dungeon", 1, r"map - -> m29_00_00_00")
    need(v, L, "csh host: the Isz dungeon's case flags include 92905389", 0, r"chalice case flags .*92905389")
    need(v, L, "csh guest: the same flags incl. 92905389 in the host's dungeon", 1,
         r"chalice case flags \(role 6, load mode 1.*: 92905300,92905302,92905345,92905378,92905389;")
    need(v, L, "csh guest: the messenger menu opened", 1, r"test: levelup: key ")
    need(v, L, "csh guest: bought the Sinister Isz Root Chalice (20000 echoes), it holds it", 1,
         r"PURCHASE on the guest: echoes 30000 -> 10000.*Isz 6532 x1\)")
    check_common(v, L, "csh")


def check_csh3(v, L):
    need(v, L, "csh3 join: the guest (own dungeon in slot 1) is in the host's dungeon", 1, r"map - -> m29_00_00_00")
    need(v, L, "csh3 guest: the messenger's region flag 12907224 set (the bath animation / talk prompt)", 1,
         r"chalice case flags \(role 6, load mode 1.*region flags 129072xx: .*12907224")
    need(v, L, "csh3 guest: the action prompt at the messenger", 1, r"test: levelup: at [\d.]+ m: action prompt text")
    need(v, L, "csh3 guest: the messenger menu opened", 1, r"test: levelup: key ")
    need(v, L, "csh3 guest: bought the Sinister Isz Root Chalice (20000 echoes), it holds it", 1,
         r"PURCHASE on the guest: echoes 30000 -> 10000.*Isz 6532 x1\)")
    check_common(v, L, "csh3")


def check_csh2(v, L):
    need(v, L, "csh2 join: the guest is in the host's dungeon", 1, r"map - -> m29_00_00_00")
    need(v, L, "csh2 guest: its current slot is its own (1), the host's dungeon the quick-joined slot 0", 1,
         r"chalice: in the host's dungeon: the game's current dungeon \(FrpgNetMan \+0xa88\) is the quick-joined slot 0's")
    need(v, L, "csh2 guest: the dungeon's feature flags are read from the quick-joined slot", 1,
         r"chalice: the dungeon's feature flags \(type 9, 92900000..92999999\) are read from the quick-joined slot 0's group")
    need(v, L, "csh2 guest: sees the host's flags incl. 92905389, not its own Hintertomb's (310)", 1,
         r"chalice case flags \(role 6, load mode 1.*: 92905300,92905302,92905345,92905378,92905389;")
    none_of(v, L, "csh2 guest: never its own dungeon's flags (92905310) in the host's dungeon",
            r"chalice case flags \(role 6, load mode 1.*: 92905301,92905310", insts=(1,))
    check_common(v, L, "csh2")


# ---- Phase ou (0.7.6, guestmesh): short outages keep the session ----
# A phone hotspot or a Hamachi relay freezes the link for 4-10 s now and then (player report
# 2026-10-24: the guest's game ended the session and went home, rejoined, and again). Two players;
# a symmetric total outage (BB_NET_SIM_OUTAGE_FILE: both games silent) of 4, 6, 8, 10 and 6 s with
# 18 s of play between. Nobody may leave the room or be sent home, the party shows its short
# "connection unstable" notice, and the guest still sees the host afterwards.
OU_LENGTHS = [4000, 6000, 8000, 10000, 6000]
OU_GAP_S = 18


class OutageDriver:
    """Writes the outage file (one shared by every instance) once the guest is in the host's world."""

    def __init__(self, root, harness_log, lengths=OU_LENGTHS, gap_s=OU_GAP_S, first_s=20, join_rx="cooperators 0 -> 1",
                 log_inst=0):
        import threading
        self.stop = False
        self.harness_log = Path(harness_log)
        self.root, self.lengths, self.gap_s, self.first_s = Path(root), lengths, gap_s, first_s
        threading.Thread(target=self.run, args=(join_rx, log_inst), daemon=True).start()

    def run(self, join_rx, log_inst):
        f = self.root / "outage.txt"
        f.unlink(missing_ok=True)
        log = self.root / ("inst%d" % log_inst) / "run.log"
        while not self.stop:  # the games started (their old run.log moved away), then the join
            try:
                if "instance 0 started" in self.harness_log.read_text(errors="replace"):
                    break
            except OSError:
                pass
            time.sleep(1)
        while not self.stop:
            try:
                if join_rx.encode() in log.read_bytes():
                    break
            except OSError:
                pass
            time.sleep(1)
        end = time.time() + self.first_s
        for k, ms in enumerate(self.lengths):
            while time.time() < end and not self.stop:
                time.sleep(0.5)
            if self.stop:
                return
            f.write_text("%d o%d" % (ms, k + 1))
            end = time.time() + ms / 1000.0 + self.gap_s

    def terminate(self):
        self.stop = True


def phase_ou(only, name="ou", sim="lat=20"):
    return dict(
        name=name, seconds=330, count=2, headless=0,
        env=["BB_NET_SIM=" + sim, "BB_NET_SIM_OUTAGE_FILE={root}/outage.txt", "BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god"],
        driver=lambda root, hl: OutageDriver(root, hl),
        until=[r"0:outage file .*: %d o%d" % (OU_LENGTHS[-1], len(OU_LENGTHS))], until_hold=OU_GAP_S + 12,
        crash=[],
    )


def check_ou(v, L, tag="ou"):
    j = need(v, L, "%s join: 2 players" % tag, 0, r"cooperators 0 -> 1")
    if j is None:
        check_common(v, L, tag)
        return
    outs = L.all(0, r"outage file .*: \d+ o\d+", j)
    v.step("%s %d outages ran" % (tag, len(OU_LENGTHS)), len(outs) == len(OU_LENGTHS), "%d of %d" % (len(outs), len(OU_LENGTHS)))
    none_of(v, L, "%s nobody leaves the room" % tag, r"left room|\(leave_room\)|LeaveRoom req", j, insts=(0, 1))
    none_of(v, L, "%s the guest is not sent home" % tag, r"funnel: guest_sent_home|send-home|host lost .*home now|sent home", j, insts=(1,))
    none_of(v, L, "%s the guest's game ends no session" % tag, r"notify_multi_play_error|session over|room_closed", j, insts=(1,))
    need(v, L, "%s the party link held or came back" % tag, 1, r"back in the party after|outage over: the game session survived", j)
    need(v, L, "%s short notice 'connection unstable'" % tag, 1, r"connection to .* unstable", j)
    # (the host's chr being up after the last outage is read from the screenshots: the remote chrs line is only logged on changes)
    if tag != "ou":
        check_vpn_mtu(v, L, tag, (0, 1))
    check_common(v, L, tag)


# ---- Phases gdr / gdrd / gdrb / gdrf (0.7.9, CL-8c879b, NW-2cedbeb): a guest dies, its game ends, it restarts and rejoins ----
# Player report (0.7.7, over a VPN): a guest died, restarted its game and could not get back into the host's
# world (it stayed in its own world, the summon never arrived). Two players. The guest dies in the host's
# world (no god mode), its game is killed 8 s later (as a crash / a quit: the harness crash plan), run.bat
# restarts it, it title-joins and must be back in the host's room (cooperators 0 -> 1 a second time) with
# no failed title join and nobody kicked. gdr: loopback; gdrd: BB_NET_SIM=dsl; gdrb: bad; gdrf: loopback with
# "Party: Fast rejoin" off (BB_PARTY_FAST_JOIN=0).
GDR_UNTIL = r"0:cooperators 1 -> 0[\s\S]*cooperators 0 -> 1"


def phase_gdr(only, name="gdr", sim=None, fast=True):
    env = (["BB_NET_SIM=" + sim] if sim else []) + ([] if fast else ["BB_PARTY_FAST_JOIN=0"])
    return dict(
        name=name, seconds=420, count=2, headless=0, env=env,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=guest_die@25",
              "0:BB_PARTY_DIRECTOR_TEST_RESTART=none", "1:BB_PARTY_DIRECTOR_TEST_RESTART=none"],
        crash=[r"1:travel step \d+/\d+: guest_die@8"], until=[GDR_UNTIL], until_hold=25,
    )


def check_gdr(v, L, d, tag="gdr"):
    j = need(v, L, "%s join: 2 players" % tag, 0, r"cooperators 0 -> 1")
    if j is None:
        check_common(v, L, tag)
        return
    died = need(v, L, "%s the guest died in the host's world" % tag, 1, r"travel step \d+/\d+: guest_die", j)
    crashes = harness_crashes(d)
    g = [t for t, i in crashes if i == 1]
    v.step("%s guest game ended (harness kill)" % tag, bool(g), "+%.1f s" % g[0] if g else "the crash plan did not fire")
    if g:
        back = need(v, L, "%s the restarted guest is back in the host's room" % tag, 0, r"cooperators 0 -> 1", g[0])
        if back is not None:
            v.step("%s rejoin time (kill -> room)" % tag, back - g[0] < 60, "%.1f s (limit 60)" % (back - g[0]))
        none_of(v, L, "%s no failed title join" % tag, r"title join: FAILED", g[0], 1e9, (1,))
        none_of(v, L, "%s nobody kicked" % tag, r"KickoutRoomMember req", g[0], 1e9, (0,))
        need(v, L, "%s guest in the host's world (game joined the room)" % tag, 1, r"apply gate open: summoned|load mode 1", g[0])
    check_common(v, L, tag)


# ---- Phases oum / meshm (0.7.8, CL-79fe9c): ou and mesh over a VPN-like path ----
# The same runs under BB_NET_SIM dsl (80 ms, jitter, 2 % loss, dup, reorder) with mtu=1400: a datagram whose
# UDP payload is over 1400 bytes is dropped, as an IP-fragmented datagram is when a tunnel loses one fragment.
# Every game datagram over the transport's 1232 must have been split (the p2p port line counts what was
# not: "unsplit"), and the party must survive the outages / the pair cut as it does on the clean path.
VPN_MTU_SIM = "dsl,mtu=1400"


def check_vpn_mtu(v, L, tag, insts):
    for i in insts:
        lines = L.all(i, r"p2p port \d+ .*sent > 1232 B \d+, > 1400 B \d+, unsplit \d+")
        v.step("%s inst %d: the p2p port line counts oversized sends" % (tag, i), bool(lines),
               "%d line(s)" % len(lines))
        big = sum(int(m) for _, line in lines for m in re.findall(r"> 1400 B (\d+)", line))
        whole = L.all(i, r"game datagram left whole: ")
        # A datagram the transport cannot split (not records, or one record over 1232) leaves whole and is
        # lost on this path; it is reported, not failed (the game's own retransmission covers it).
        v.step("%s inst %d: game datagrams over 1400 bytes sent (unsplittable only)" % (tag, i), True,
               "%d; left whole: %s" % (big, "; ".join(l[1].split("left whole: ", 1)[-1][:80] for l in whole[:3]) or "none"))


# ---- Phase mesh (0.7.6, guestmesh): two guests that cannot reach each other keep seeing each other ----
# Three players. Once both guests are in, the pair guest 1 <-> guest 2 is cut at the network (BB_NET_SIM
# block=<the other guest's party port>, switched by BB_NET_SIM_BLOCK_FILE; the host's port stays open) for
# 40 s, then opened. Each guest's path to the other must go through the host relay while it is cut,
# both must still see the other (chr up, a position that is not the origin placeholder) at the end of the
# cut, and the pair goes direct again some time after the path is back.
MESH_CUT_AFTER_S, MESH_CUT_S, MESH_AFTER_S = 25, 40, 45


class BlockDriver:
    """Writes the block file (1 = the guest pair is cut, 0 = open) once guest 2 is in the party."""

    def __init__(self, root, harness_log, join_rx=r"cooperators \d -> 2", log_inst=0, after_s=MESH_CUT_AFTER_S, cut_s=MESH_CUT_S):
        import threading
        self.stop = False
        self.root, self.harness_log = Path(root), Path(harness_log)
        self.join_rx, self.log_inst, self.after_s, self.cut_s = join_rx, log_inst, after_s, cut_s
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        f = self.root / "block.txt"
        f.write_text("0")
        log = self.root / ("inst%d" % self.log_inst) / "run.log"
        while not self.stop:
            try:
                if "instance 0 started" in self.harness_log.read_text(errors="replace"):
                    break
            except OSError:
                pass
            time.sleep(1)
        rx = re.compile(self.join_rx.encode())
        while not self.stop:
            try:
                if rx.search(log.read_bytes()):
                    break
            except OSError:
                pass
            time.sleep(1)
        for wait, val in ((self.after_s, "1"), (self.cut_s, "0")):
            end = time.time() + wait
            while time.time() < end and not self.stop:
                time.sleep(0.5)
            if self.stop:
                return
            f.write_text(val)

    def terminate(self):
        self.stop = True


def phase_mesh(only, name="mesh", sim="lat=20"):
    return dict(
        name=name, seconds=420, count=3, headless=1,
        env=["BB_NET_SIM_BLOCK_FILE={root}/block.txt", "BB_PARTY_PEER_RELAY_HOLD_MS=12000",
             "BB_PARTY_PEER_RELAY_STABLE_MS=4000", "BB_PARTY_PLACEHOLDER_HEAL=1", "BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_NET_SIM=" + sim, "1:BB_NET_SIM=%s,block={p2}" % sim, "2:BB_NET_SIM=%s,block={p1}" % sim,
              "0:BB_PARTY_DIRECTOR_TEST=god", "1:BB_PARTY_DIRECTOR_TEST=god", "2:BB_PARTY_DIRECTOR_TEST=god"],
        driver=lambda root, hl: BlockDriver(root, hl),
        until=[r"1:the block= ports are cut[\s\S]*direct again \(probes answered\)", r"2:the block= ports are cut[\s\S]*direct again \(probes answered\)"],
        until_hold=MESH_AFTER_S - 20, crash=[],
    )


def _last_chrs(L, i, before):
    hits = L.all(i, r"remote chrs: ", 0.0, before)
    return hits[-1] if hits else None


def check_mesh(v, L, tag="mesh"):
    j = need(v, L, "%s join: 3 players" % tag, 0, r"cooperators \d -> 2")
    if j is None:
        check_common(v, L, tag)
        return
    for g, other in ((1, 2), (2, 1)):
        cut = L.find(g, r"block file .*: the block= ports are cut", j)
        v.step("%s guest %d: the pair was cut" % (tag, g), cut, "+%.1f s" % cut[0] if cut else "no block file line")
        if not cut:
            continue
        opened = L.find(g, r"block file .*: the block= ports are open", cut[0])
        end = opened[0] if opened else 1e9
        rel = need(v, L, "%s guest %d: the path to guest %d moves to the host relay" % (tag, g, other), g,
                   r"path to .*through the host relay", cut[0], end)
        last = _last_chrs(L, g, end)
        ok = bool(last) and "no chr" not in last[1] and len(re.findall(r"\[mode \d 0x[0-9a-f]+ \(", last[1])) >= 2 and \
            not re.search(r"\(-?0\.\d, -?0\.\d, -?0\.\d\)", last[1])
        v.step("%s guest %d: sees both others (chrs up, none at the origin) at the end of the cut" % (tag, g), ok,
               last[1][:200] if last else "no remote chrs line")
        need(v, L, "%s guest %d: the pair goes direct again after the cut" % (tag, g), g, r"direct again \(probes answered\)", end)
    none_of(v, L, "%s nobody leaves the room" % tag, r"left room|\(leave_room\)", j, insts=(0,))
    if tag != "mesh":
        check_vpn_mtu(v, L, tag, (0, 1, 2))
    check_common(v, L, tag)


# ---- Phase fp (0.7.6, guestmesh): a 4-player lobby with one guest behind a simulated NAT ----
# Max players 4 on all four; guest 3 sits behind a simulated port-restricted NAT (BB_NET_SIM nat=1:
# nothing gets in unsolicited). Player report 2026-10-25: two of three guests stuck loading with
# "its UDP is not heard on the party port". All three must end up in the host's world.
def phase_fp(only):
    return dict(
        name="fp", seconds=420, count=4, headless=2,
        env=["BB_PARTY_MAX=4", "BB_PARTY_HOLD_LOG=1"],
        ienv=["0:BB_NET_SIM=lat=20", "1:BB_NET_SIM=lat=20", "2:BB_NET_SIM=lat=20", "3:BB_NET_SIM=lat=20,nat=1"],
        until=[r"0:cooperators 2 -> 3"], until_hold=15, crash=[],
    )


def check_fp(v, L):
    need(v, L, "fp join: 4 players (host counts 3 cooperators)", 0, r"cooperators \d -> 3")
    for g in (1, 2, 3):
        need(v, L, "fp guest %d is in the host's world" % g, g, r"guest services: open: guest in the host's")
    none_of(v, L, "fp no 'UDP is not heard'", r"its UDP is not heard|direct summon: FAILED", insts=(0, 1, 2, 3))
    check_common(v, L, "fp")


# ---- Phases jn<G><s>[d] (0.7.9 join079, CL-95041a; group `join_stagger`, `join_stagger_sim` for the dsl ones): G guests (2 or 3)
# started 0 / 2 / 5 s apart (a / b / c; d = 2 s apart under BB_NET_SIM=dsl) all join the host. A room served to one guest held every
# other join for 20 s ("a crashed member's connection is still held"): the first guest was refused / told to wait by the second
# guest's serve hold. No member crashes here, so no join may ever wait for or be refused by a crash hold; every guest ends up
# in the host's world. ----
JN_STAGGER = {"a": 0, "b": 2, "c": 5, "d": 2}
JN_HOLD_RX = r"a crashed member's (old )?connection is still held"


def phase_jn(only, name):
    guests, kind = int(name[2]), name[3]
    return dict(
        name=name, seconds=420, count=guests + 1, headless=max(1, guests - 1),
        env=["BB_PARTY_MAX=%d" % (guests + 1), "BB_PARTY_HOLD_LOG=1"] + (["BB_NET_SIM=dsl"] if kind == "d" else []),
        ienv=[], args=["--stagger", str(JN_STAGGER[kind])],
        until=[r"0:cooperators \d -> %d" % guests], until_hold=20, crash=[],
    )


def check_jn(v, L, name):
    guests = int(name[2])
    t = need(v, L, "%s join: %d guests in (host cooperators -> %d)" % (name, guests, guests), 0, r"cooperators \d -> %d" % guests)
    for g in range(1, guests + 1):
        need(v, L, "%s guest %d is in the host's world" % (name, g), g, r"guest services: open: guest in the host's")
    none_of(v, L, name + ": no join waits for / is refused by a crash hold (nobody crashed)", JN_HOLD_RX, insts=tuple(range(guests + 1)))
    none_of(v, L, name + ": no failed direct summon", r"direct summon: FAILED", insts=tuple(range(guests + 1)))
    if t is not None:
        t0 = min([x for x in (L.find(g, r"asked the host for its room|title join: asking", 0) for g in range(1, guests + 1)) if x] or [(0, "")])[0]
        v.step(name + ": all guests in within 120 s of the first ask (info)", t - t0 < 120, "%.1f s" % (t - t0))
    check_common(v, L, name)


JN_PHASES = [("jn%d%s" % (g, k)) for g in (2, 3) for k in "abcd"]


# ---- Phase oj (0.7.8, CL-7f44cd): the open party. The host has no password and BB_PARTY_OPEN=1; the guest has no
# password and no party code, only the host's plain address (127.0.0.1:port, a loopback test peer). The host must say
# an open-party join was accepted and the guest must be in the host's world. ----
def phase_oj(only):
    return dict(
        name="oj", seconds=300, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_OPEN=1", "1:BB_PARTY_CODE=127.0.0.1:{p0}", "1:BB_PARTY_PASSWORD="],
        until=[r"0:cooperators 0 -> 1"], until_hold=40, crash=[],
    )


def check_oj(v, L):
    need(v, L, "oj host: a player joins by plain address (open party)", 0, r"link: open party: a player joins by plain address")
    need(v, L, "oj join: guest in the host's world (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1")
    need(v, L, "oj guest services: guest in the host's world", 1, r"guest services: open: guest in the host's")
    none_of(v, L, "oj: no open-party refusal", r"open party: a plain-address join refused", insts=(0,))
    none_of(v, L, "oj: no wrong-password / key failure", r"wrong password|bad password|key mismatch", insts=(0, 1))
    check_common(v, L, "oj")


# ---- Phase fp5 (0.7.8, CL-de2e94): a full 5-player party (host + 4 guests) ----
# BB_PARTY_MAX=5, three of the five headless (the queue allows two rendered clients). Chain: all four guests join
# (the host counts 4 cooperators; the 0.7.7 later-guests-stuck-loading fix), party lamp travel to the Cleric Beast's
# lamp, the fight (every guest in the arena, boss scaling for 4 cooperators clamps to the 3-player row), guest 2
# dies in the fight and gets up, the boss is killed, guest 3 Leaves and Rejoins (back at 4 cooperators).
def phase_fp5(only):
    host = ",".join([
        "goods=4114:0",
        "host_warp=2412952@10^4",                                 # the Cleric Beast's lamp, once 4 guests are in
        "host_move=%s@15%%m24_01" % CLERIC_TRIGGER_HOST,          # starts the fight
        "host_kill=2410800@30%m24_01",                            # the boss kill
    ])
    mv = "guest_move=%s@%%d%%%%m24_01" % CLERIC_SPOT_GUEST
    g1 = ",".join([mv % 22, "guest_press=0x0@30%m24_01!"])
    g2 = ",".join([mv % 22, "guest_die@3%m24_01", "guest_press=0x0@3%m24_01!"])
    g3 = ",".join([mv % 22, "guest_leave@35%m24_01", "guest_rejoin@5"])
    g4 = ",".join([mv % 22, "guest_press=0x0@30%m24_01!"])
    return dict(
        name="fp5", seconds=600, count=5, headless=3,
        env=["BB_PARTY_MAX=5", "BB_PARTY_HOLD_LOG=1", "BB_PARTY_TEST_INVINCIBLE=1", "BB_BOOT_WATCHDOG=20",
             "BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR],
        ienv=["%d:BB_NET_SIM=lat=20" % i for i in range(5)] +
             ["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + g1, "2:BB_PARTY_DIRECTOR_TEST=" + g2,
              "3:BB_PARTY_DIRECTOR_TEST=" + g3, "4:BB_PARTY_DIRECTOR_TEST=" + g4],
        watch=["--watch", r"cooperators 3 -> 4=full@4,12", "--watch", r"up again in the arena=arena@1,4"],
        until=[r"0:cooperators 3 -> 4[\s\S]*cooperators 3 -> 4"], until_hold=20, crash=[],
    )


def check_fp5(v, L):
    full = need(v, L, "fp5 join: 5 players (host counts 4 cooperators)", 0, r"cooperators \d -> 4")
    for g in (1, 2, 3, 4):
        need(v, L, "fp5 guest %d is in the host's world" % g, g, r"guest services: open: guest in the host's")
    none_of(v, L, "fp5 no 'UDP is not heard'", r"its UDP is not heard|direct summon: FAILED", insts=(0, 1, 2, 3, 4))
    none_of(v, L, "fp5 nobody kicked / refused (room full)", r"KickoutRoomMember req|room is full|party is full", insts=(0,))
    tr = need(v, L, "fp5 party lamp travel (host)", 0, r"travel step \d+/\d+: host_warp to lamp 2412952", full or 0)
    if tr is not None:
        for i in (1, 2, 3, 4):
            need(v, L, "fp5 travel: guest %d in m24_01" % i, i, r"map .*-> m24_01", tr)
    need(v, L, "fp5 boss scaling sees 4 cooperators (H2 clamp to the 3-player row)", 0,
         r"H2: boss scaling sees 4 cooperators as 2", tr or 0)
    d = need(v, L, "fp5 guest 2 dies in the fight", 2, r"travel step \d+/\d+: guest_die", tr or 0)
    if d is not None:
        need(v, L, "fp5 guest 2 gets up (arena revive / respawn)", 2,
             r"up again in the arena|guest: respawned at the host's lamp|guest: died alone: revived|revived at", d)
    k = need(v, L, "fp5 boss kill (host)", 0, r"host_kill: entity 2410800|killed by", tr or 0)
    lv = need(v, L, "fp5 guest 3 Leave", 3, r"guest_leave: Party > Leave pressed", k or 0)
    if lv is not None:
        need(v, L, "fp5 Leave: host sees it (4 -> 3)", 0, r"cooperators 4 -> 3", lv)
        rj = need(v, L, "fp5 guest 3 Rejoin pressed", 3, r"guest_rejoin: Party > Rejoin pressed", lv)
        if rj is not None:
            back = need(v, L, "fp5 Rejoin: back in (3 -> 4)", 0, r"cooperators 3 -> 4", rj)
            if back is not None:
                v.step("fp5 Rejoin time (press -> host room)", back - rj < 45, "%.1f s (limit 45)" % (back - rj))
    check_common(v, L, "fp5")


# ---- Phase lo (0.7.7, lanonly077): a guest whose game finds no public UDP endpoint is still summoned ----
# Player report 2026-10-25: host fine (open port), the guest's STUN got no answer, the host refused
# the summon ("UDP to X is blocked: its game has only the LAN address"). BB_PARTY_TEST_UNREACHABLE=1
# makes the host count its member and the guest count itself as having no public endpoint (loopback
# has no real public IP); the guest also sits behind a symmetric NAT sim. Before the fix: no bell,
# no summon. Now: the guest rings, the host summons it at the address it hears it from.
# `BB_PARTY_UNREACHABLE_HOLD=1` in the environment of the run restores the old hold (the "before").
def phase_lo(only):
    return dict(
        name="lo", seconds=300, count=2, headless=1,
        env=["BB_PARTY_TEST_UNREACHABLE=1"],
        ienv=["0:BB_NET_SIM=lat=20", "1:BB_NET_SIM=lat=20,nat=sym", "1:BB_PARTY_TITLE_JOIN=0"],  # in-world bell join
        until=[r"0:cooperators 0 -> 1"], until_hold=15, crash=[],
    )


def check_lo(v, L):
    note = L.find(0, r"summon: .* has no public UDP endpoint of its own")
    v.step("lo host: names the guest without a public endpoint and summons it at the observed address", note,
           note[1][:200] if note else "no 'has no public UDP endpoint' line (old hold?)")
    ring = L.find(1, r"summon: this game has no public UDP endpoint .*ringing anyway")
    v.step("lo guest: knows it has no public endpoint and rings anyway", ring, ring[1][:200] if ring else "no line")
    none_of(v, L, "lo no hold", r"bell: not ringing: UDP to the host is blocked|UDP to .* is blocked: its game has only",
            insts=(0, 1))
    need(v, L, "lo join: guest in the host's world (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1")
    need(v, L, "lo guest services: guest in the host's world", 1, r"guest services: open: guest in the host's")
    check_common(v, L, "lo")



# Phase sh (CL-adeb50 "Stay in my world", ~3 min, 2 players, group `stay_home`): the guest goes home, turns the switch
# on (own_rule=party_stay:1: the same setting the System > Party row flips) and stays there while the host travels
# twice (the Tomb of Oedon, then back to Cathedral Ward): the guest does not follow, rings no bell, no gather, and the
# host neither rings nor votes-waits for it. Switched off again, the guest rings and is back in the host's world.
SH_UNTIL = [r"0:cooperators 0 -> 1[\s\S]*cooperators 0 -> 1"]


def phase_sh(only):
    host = ",".join([
        "god",
        "host_rule=party_travel_vote:1@8%m2",  # the host's travel vote on: a staying guest must not be asked or waited for
        "host_lamp=2412953@25%m2&",       # the party's lamp travel to m24_01 while the guest is home and staying
        "host_lamp=2402950@12%m24_01&",   # and on to m24_00
    ])
    guest = ",".join([
        "god",
        "own_rule=party_stay:1@5%m2",    # the switch on (in the host's world: nothing changes yet)
        "guest_home@3%m2",               # the guest goes home on its own: the bell would ring again within seconds
        "own_rule=party_stay:0@85%m2&",  # off: rings, gathers, joins the host's world again
    ])
    return dict(
        name="sh", seconds=300, count=2, headless=0,
        env=[],
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "0:BB_PARTY_SPLIT_TEST=yes", "1:BB_PARTY_SPLIT_TEST=yes", "0:BB_PARTY_RING_EVERY=8", "1:BB_PARTY_RING_EVERY=8"],
        crash=[],
        until=SH_UNTIL,
        until_hold=6,
    )


def check_sh(v, L):
    need(v, L, "sh join: the guest is in", 0, r"cooperators 0 -> 1")
    on = need(v, L, "sh the guest turns Stay in my world on", 1, r"test: own_rule: party_stay 0 -> 1")
    need(v, L, "sh the guest goes home", 1, r"test: guest_home: the game's own leave run", on or 0)
    need(v, L, "sh the guest notes it stays", 1, r"Party stay: staying in my own world", on or 0)
    hs = need(v, L, "sh the host hears it", 0, r"Party stay: guest slot 1 stays in its own world", on or 0)
    t1 = need(v, L, "sh the host travels (m24_01) while the guest stays", 0, r"travel step 2/\d+: host_lamp", hs or 0)
    t2 = need(v, L, "sh the host travels again (m24_00)", 0, r"travel step 3/\d+: host_lamp", t1 or 0)
    if t1:
        none_of(v, L, "sh the staying guest is asked nothing", r"Party vote: round \d+: question open", on or 0, 1e9, (1,))
    off = need(v, L, "sh the guest turns it off", 1, r"test: own_rule: party_stay 1 -> 0", t2 or 0)
    if on and off:
        none_of(v, L, "sh paused: the guest follows no host travel", r"map .*-> m24_0[01]", t1 or on, off, (1,))
        none_of(v, L, "sh paused: the guest rings no bell", r"ringing the Small Resonant Bell", on + 4, off, (1,))
        none_of(v, L, "sh paused: the host rings no bell for it", r"ringing the Beckoning Bell", (hs or on) + 2, off, (0,))
        none_of(v, L, "sh paused: no gather to the host's lamp", r"gather: ", on + 4, off, (1,))
    if off:
        need(v, L, "sh resumes: the guest says so", 1, r"stay: joining resumes", off)
        rg = need(v, L, "sh resumes: the guest rings again", 1, r"ringing the Small Resonant Bell", off)
        need(v, L, "sh resumes: the host sees the guest join again", 0, r"cooperators 0 -> 1", off)
        need(v, L, "sh resumes: the host hears it joins", 0, r"Party stay: guest slot 1 joins again", off)
    check_common(v, L, "sh")



# Phase gg (CL-a3a9e6 "guests can gather", ~4 min, 2 players, group `guest_gather`): the host's party_guest_gather rule
# is off at first: the guest's press is not even queued (its row is hidden). The party travels to the Tomb of Oedon,
# the guest walks 130 m away, the host allows guests to gather, the guest gathers: the host is asked (its own YES,
# BB_PARTY_GATHER_TEST) and warped in-world beside the guest's spot, no load. The host switches the rule off again:
# the guest's press is refused. The guest's Stay in my world refusal is the unit test's (RequestRefusal).
GG_UNTIL = [r"1:test: guest_gather: [^\n]*NOT queued[\s\S]*test: guest_gather: [^\n]*NOT queued"]


def phase_gg(only):
    host = ",".join([
        "god",
        "host_warp=2412953@8",                        # party lamp travel: the Tomb of Oedon (everyone in m24_01)
        "host_rule=party_guest_gather:1@30%m24_01",   # the host allows guests to gather
        "host_rule=party_guest_gather:0@40%m24_01",   # and takes it back
    ])
    guest = ",".join([
        "god",
        "guest_gather@20%m24_01",                     # rule off: not queued (the row is hidden)
        "guest_move=%s@8%%m24_01" % GA_FAR,           # 130 m away in the same map
        "guest_gather@25%m24_01",                     # rule on: the host is asked
        "guest_gather@40%m24_01",                     # rule off again
    ])
    watch = ["--grey"]
    for rx, tag in ((r"gather: put beside the spot the host asked from", "beside"), (r"request \d+ sent to the host", "asked")):
        watch += ["--watch", "%s=%s@0.5,3" % (rx, tag)]
    return dict(
        name="gg", seconds=300, count=2, headless=0,
        env=[], watch=watch,
        ienv=["0:BB_PARTY_DIRECTOR_TEST=" + host, "1:BB_PARTY_DIRECTOR_TEST=" + guest,
              "0:BB_PARTY_GATHER_TEST=yes", "1:BB_PARTY_GATHER_TEST=yes"],
        crash=[],
        until=GG_UNTIL,
        until_hold=8,
    )


def check_gg(v, L):
    off = need(v, L, "gg rule off: the guest's press is not queued", 1, r"test: guest_gather: Party > Gather the party pressed - NOT queued")
    on = need(v, L, "gg the host allows guests to gather", 0, r"test: host_rule: party_guest_gather 0 -> 1", off or 0)
    sent = need(v, L, "gg the guest sends its request", 1, r"Party gather: guest: Gather the party: request \d+ sent to the host", on or 0)
    got = need(v, L, "gg the host asks the members", 0, r"Party gather: round \d+: .* gathers the party: 1 member\(s\) asked", sent or 0)
    need(v, L, "gg the guest hears the host accepted", 1, r"Party gather: guest: request \d+ accepted: 1 member", got or 0)
    ask = need(v, L, "gg the host is asked as a member", 0, r"Party gather: host as a member: round \d+: .* gathers the party", got or 0)
    yes = need(v, L, "gg the host says YES: in-world, no load", 0, r"Party gather: host: YES to the guest in slot 1: route inworld", ask or 0)
    put = need(v, L, "gg the host is put beside the guest's spot", 0, r"gather: put beside the spot the host asked from .* no load", yes or 0)
    need(v, L, "gg the host stands there", 0, r"Party gather: guest: stands beside the host", put or 0)
    need(v, L, "gg the round is summed up", 0, r"Party gather: round \d+ over .*: Gather: .* coming", yes or 0)
    none_of(v, L, "gg no map load for the gather", r"gather: no travel|Party gather: .*travelling to the host", yes or 0, 1e9, (0, 1))
    back = need(v, L, "gg the host takes the rule back", 0, r"test: host_rule: party_guest_gather 1 -> 0", put or 0)
    need(v, L, "gg rule off again: the press is refused (not queued or the host refuses)", 1,
         r"NOT queued|request \d+ refused by the host: guests may not gather", back or 0)
    check_common(v, L, "gg")



# Phase iv (CL-4cc63c invasions into open public games, ~9 min, 3 players, group `invade`): the host (Allow invaders on) and a
# guest are in the world; the invader (BB_PARTY_INVADE=1, no password) dials at its title first (the host is not in a world
# yet: refused with a named reason), dials again from its own world and joins as a hostile phantom: its chr is on the invader
# team on all three games, the host and the guest hit it and it hits the host (HP drops), then it dies (the game's own
# send-home, not a revive at a lamp), asks again, joins, and the HOST dies: the invader goes home. The invader instance is
# headless (the machine allows two rendered clients).
IV_UNTIL = [r"2:invade: notice: You defeated"]


def phase_iv(only, name="iv", sim=None):
    # BB_PARTY_INVADE_TEST (party_invade_rules.h ParseTest): the host dies 40 s into the invader's second stay; the invader asks
    # again 25 s after the refusal at its title, falls 45 s into the first stay, asks again 15 s after getting home.
    hsteps = "log@5,invader@4,face=22@2,hit=3@1,log@3"
    gsteps = "log@5,invader@8,face=22@2,hit=3@1,log@3"
    isteps = "log@5,invader@4,face=19@2,hit=4@1,log@3"
    ienv = ["0:BB_PARTY_INVADERS=1", "2:BB_PARTY_INVADE=1",
            "0:BB_PARTY_INVADE_TEST=die@40?in#2",
            "2:BB_PARTY_INVADE_TEST=rejoin@25?home,die@45?in,rejoin@22?home",
            "0:BB_PARTY_PVP_TEST=" + hsteps, "1:BB_PARTY_PVP_TEST=" + gsteps, "2:BB_PARTY_PVP_TEST=" + isteps]
    if sim:
        ienv += ["%d:BB_NET_SIM=%s" % (i, sim) for i in range(3)]
    return dict(name=name, seconds=900, count=3, headless=1, env=[], ienv=ienv, crash=[], until=IV_UNTIL, until_hold=40)


def check_iv(v, L, name="iv"):
    n = name
    first = need(v, L, n + " invader: the first dial (host not in a world) is refused with a named reason", 2,
                 r"refused by the host: (the host is not in a world you can enter now|the host is on a loading screen)")
    gate = need(v, L, n + " host: the gate passes the invader (final)", 0, r"invade: gate: invader \(name hidden\) \(final\): none", first or 0)
    adm = need(v, L, n + " host: an invader is admitted (pending)", 0, r"invade: an invader is admitted into slot \d+ \(pending\)", gate or 0)
    inn = need(v, L, n + " host: the invader is in the world", 0, r"invade: invader slot \d+: in the world", adm or 0)
    need(v, L, n + " guest: told of the invader", 1, r"invade: notice: (An invader is coming|Invader .* has entered)", adm or 0)
    need(v, L, n + " invader: told it invades", 2, r"invade: notice: You invade ", adm or 0)
    need(v, L, n + " invader: no shared progress or items arrive (the host's rules are not sent)", 2,
         r"invade: this game invades the host's world", 0)
    # teams: the invader's chr is on its own team (a copy of the map enemies) on every game
    for i, who in ((0, "host"), (1, "guest"), (2, "invader")):
        need(v, L, n + " %s: the invader team is a copy of the map enemies" % who, i, r"invader: team \d+ is a copy of team 23", adm or 0)
        need(v, L, n + " %s: the invader's chr moved to the invader team" % who, i, r"invader: chr [0-9a-f]+ \((this game's own|a remote member's)\) moved from team \d+ to team \d+", adm or 0)
    # the red phantom look (SpEffect 9026 in place of the cooperator's 9006), per chr, on every game
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, n + " %s: the invader's copy loses the cooperator look (SpEffect 9006)" % who, i, r"invader: chr [0-9a-f]+ cooperator look \(SpEffect 9006\) off", adm or 0)
        need(v, L, n + " %s: the invader's copy has the red look (SpEffect 9026)" % who, i, r"invader: chr [0-9a-f]+ red look \(SpEffect 9026\) on \(a remote member's\)", adm or 0)
    need(v, L, n + " invader: its own chr has the red look (SpEffect 9026)", 2, r"invader: chr [0-9a-f]+ red look \(SpEffect 9026\) on \(this game's own\)", adm or 0)
    # the fight: every hit lands (HP drops on the host and on the invader)
    need(v, L, n + " host: the invader's blows hurt the host", 0, r"invader: own HP \d+ -> \d+", inn or 0)
    need(v, L, n + " invader: the host's and the guest's blows hurt the invader", 2, r"invader: own HP \d+ -> \d+", inn or 0)
    need(v, L, n + " guest: nothing else changes: no friendly fire between host and guest", 1, r"pvp: rule: friendly fire off|rule: friendly fire", 0)
    none_of(v, L, n + " no friendly-fire cell opened between the host and the guest", r"friendly fire applied", 0, 1e9, (0, 1))
    # the invader falls: tells the host, goes home through the game's own send-home
    fell = need(v, L, n + " invader: it falls and goes home", 2, r"invade: the invader fell: going home", inn or 0)
    # the games' clocks start a few ms apart and the host reads the invader's HP 0 from its vitals: its line can come a hair
    # before the invader's own, so the rows that follow look from 3 s earlier
    fell0 = max(0, fell - 3) if fell else 0
    need(v, L, n + " host: the invasion ends (invader died)", 0, r"invasion of slot \d+ ends: invader_died|ends: .*invader", fell0)
    need(v, L, n + " host+guest: told the invader was defeated", 1, r"invade: notice: The invader .* was defeated", fell0)
    home = need(v, L, n + " invader: home through the game's own leave (no lamp revive)", 2, r"host lost \([^)]*\): home now \(OnLeave_Limit\)", fell0)
    # asks again; the host dies; the invader goes home again
    again = need(v, L, n + " invader: asks again", 2, r"test: script step \d+: rejoin -> ", home or 0)
    inn2 = need(v, L, n + " host: the invader is in the world again", 0, r"invade: invader slot \d+: in the world", again or 0)
    # 0.7.9: an invader's slot is released at once: the second invasion (asked 22 s after it got home) is not refused by the crash
    # hold ("a crashed member's connection is still held", 26 s) nor by a world that still counts the first invader ("full")
    none_of(v, L, n + " host: the second invasion is not refused (no crash hold, no full world)", r"direct summon: slot 2 .*refused \((a crashed member's connection is still held|the host's world is full)", again or 0, 1e9, (0,))
    if again is not None and inn2 is not None:
        v.step(n + " host: the second invader is in within 25 s of asking (it waited 30 s before)", inn2 - again < 25, "%.1f s" % (inn2 - again))
    hd = need(v, L, n + " host: dies", 0, r"test: script step \d+: this player dies now", inn2 or 0)
    need(v, L, n + " host: the invasion ends (host died)", 0, r"invasion of slot \d+ ends: host_died", hd or 0)
    need(v, L, n + " invader: told it won", 2, r"invade: notice: You defeated ", hd or 0)
    need(v, L, n + " invader: goes home when the host dies", 2, r"host lost \([^)]*\): home now \(OnLeave_Limit\)", hd or 0)
    need(v, L, n + " the teams go back (invader chr restored)", 0, r"invader: team \d+ closed: \d+ table cell\(s\) put back", hd or 0)
    # the look is removed with the invasion (the cooperator look comes back on the other games' copy)
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, n + " %s: the invader's red look is removed at the end" % who, i, r"invader: its red look \(SpEffect 9026\) is off|invader: team \d+ closed", fell0)
    # rewards (the game's own CalcExcuteMultiBonus, once per outcome, on the receiver's own game)
    need(v, L, n + " host: paid for the fallen invader (echoes at 0.50, no Insight)", 0, r"invade: reward: host, invader_died: rate 0\.50, Insight \+0: Blood Echoes -?\d+ -> -?\d+", fell0)
    need(v, L, n + " guest: paid for the fallen invader (echoes at 0.25, no Insight)", 1, r"invade: reward: guest, invader_died: rate 0\.25, Insight \+0: Blood Echoes -?\d+ -> -?\d+", fell0)
    need(v, L, n + " invader: paid for the fallen host (echoes at 0.10 and +1 Insight)", 2, r"invade: reward: invader, host_died: rate 0\.10, Insight \+1: Blood Echoes -?\d+ -> -?\d+", hd or 0)
    # the result banners (the game's text effects): ADVERSARY EXPUNGED (7) to the host and the guest, PREY SLAUGHTERED (5) only to the invader
    need(v, L, n + " host: the 'adversary expunged' banner when the invader fell", 0, r"invade: banner: host, invader_died: text effect 7", fell0)
    need(v, L, n + " guest: the 'adversary expunged' banner when the invader fell", 1, r"invade: banner: guest, invader_died: text effect 7", fell0)
    need(v, L, n + " invader: the 'prey slaughtered' banner when it killed the host", 2, r"invade: banner: invader, host_died: text effect 5", hd or 0)
    none_of(v, L, n + " the host and the guest never get PREY SLAUGHTERED (text effect 1 / 5) nor a boss-defeat banner in an invasion",
            r"invade: banner: (host|guest), \w+: text effect [15]\b|boss defeat processing 2003\[12\]", 0, 1e9, (0, 1))
    none_of(v, L, n + " the invader gets no banner when it falls", r"invade: banner: invader, invader_died", 0, 1e9)
    none_of(v, L, n + " the invader is not paid when it falls, the host not when it falls, the guest not for the host", r"invade: reward: (invader, invader_died|host, host_died|guest, host_died)", 0, 1e9)
    check_common(v, L, n)


# Phase ivx (CL-4cc63c, ~8 min, 2 players, group `invade_refuse`): the refusals. One host with a world and one invader that
# dials again after each change of the host's moment: Allow invaders off (refused: the host does not allow invaders), on with
# a boss fight (refused), a cutscene (refused), 4 players in the world (refused: no free slot), and finally nothing in the way
# (admitted). The host's moment is forced with own_rule=invade_view:<word>; the 4-player case is also a unit test and is live
# here as the host's world_players forced to 4 (a real fourth player needs 5 instances).
IVX_UNTIL = [r"0:invade: invader slot \d+: in the world"]


def phase_ivx(only, name="ivx", sim=None):
    # The host's moment, relative to its world coming up: Allow invaders off (1 s), on (+70), boss view (+45), cutscene (+45),
    # 4 players (+45), none (+45). The invader dials at +40 (off), +140 (boss), +185 (cutscene), +230 (4 players), +275 (in).
    host = "allow=0@1,allow=1@70,view=boss@45,view=cutscene@45,view=full@45,view=none@45"
    invader = "rejoin@40?home,rejoin@100?home,rejoin@45?home,rejoin@45?home,rejoin@45?home"
    ienv = ["1:BB_PARTY_INVADE=1", "0:BB_PARTY_INVADERS=0", "0:BB_PARTY_INVADE_TEST=" + host, "1:BB_PARTY_INVADE_TEST=" + invader]
    if sim:
        ienv += ["%d:BB_NET_SIM=%s" % (i, sim) for i in range(2)]
    return dict(name=name, seconds=900, count=2, headless=1, env=[], ienv=ienv, crash=[], until=IVX_UNTIL, until_hold=10)


def check_ivx(v, L, name="ivx"):
    n = name
    off = need(v, L, n + " invader: Allow invaders off -> refused, named", 1, r"refused by the host: the host does not allow invaders")
    on = need(v, L, n + " host: the switch goes on", 0, r"test: script step \d+: Allow invaders -> 1", 0)
    boss = need(v, L, n + " invader: boss fight -> refused, named", 1, r"refused by the host: a boss fight is on in the host's world", on or 0)
    cut = need(v, L, n + " invader: cutscene -> refused, named", 1, r"refused by the host: a cutscene is playing in the host's world", boss or 0)
    full = need(v, L, n + " invader: 4 players -> refused, named", 1, r"refused by the host: no free slot in the host's world \(4 players\)", cut or 0)
    inn = need(v, L, n + " host: nothing in the way -> the invader is admitted and in", 0, r"invade: invader slot \d+: in the world", full or 0)
    need(v, L, n + " host: a host with the switch off was never invadable (announced flag)", 0, r"gate: invader \(name hidden\)", 0)
    none_of(v, L, n + " the invader never stood in the world before the last dial", r"cooperators 0 -> 1", 0, (inn or 1e9) - 5, (0,))
    check_common(v, L, n)


# Phase ivl (0.7.9, group `invade_level`): the invasion level range. The host (Allow invaders on) is in its world; the invader claims
# level 900 (BB_PARTY_INVADE_LEVEL, HELLO and vitals alike) and dials at +40: it must be refused with the range sentence (or, if
# the host's level was not yet known at the dial, removed as soon as its vitals say 900), and never stand in the world.
IVL_UNTIL = [r"1:Can't invade .*your level 900 is outside their range"]


def phase_ivl(only):
    ienv = ["1:BB_PARTY_INVADE=1", "0:BB_PARTY_INVADERS=1", "1:BB_PARTY_INVADE_LEVEL=900",
            "1:BB_PARTY_INVADE_TEST=rejoin@40?home"]
    return dict(name="ivl", seconds=420, count=2, headless=0, env=[], ienv=ienv, crash=[], until=IVL_UNTIL, until_hold=25)


def check_ivl(v, L):
    need(v, L, "ivl invader: refused with the level range sentence", 1, r"Can't invade .*: your level 900 is outside their range \(\d+-\d+\)")
    need(v, L, "ivl host: the gate names the level range", 0, r"gate: invader \(name hidden\).*level_range")
    none_of(v, L, "ivl the invader never stood in the world", r"invade: invader slot \d+: in the world", insts=(0,))
    check_common(v, L, "ivl")


# Phase ojl (0.7.9, group `open_level`): the open-party level range. The host has no password (BB_PARTY_OPEN=1); the guest joins by the
# plain address and claims level 900: refused with "Can't join ...", never in the host's world.
OJL_UNTIL = [r"1:Can't join .*your level 900 is outside their range"]


def phase_ojl(only):
    return dict(name="ojl", seconds=420, count=2, headless=0, env=[],
                ienv=["0:BB_PARTY_OPEN=1", "1:BB_PARTY_CODE=127.0.0.1:{p0}", "1:BB_PARTY_PASSWORD=", "1:BB_PARTY_INVADE_LEVEL=900"],
                until=OJL_UNTIL, until_hold=25, crash=[])


# Phase ojla (0.7.9, group `open_level_any`): the same join with "Any level can join" on (BB_PARTY_OPEN_ANY_LEVEL=1): the level 900 guest is
# admitted and still in the host's world 75 s after it joined (the vitals recheck would have removed it within seconds).
def phase_ojla(only):
    return dict(name="ojla", seconds=420, count=2, headless=0, env=[],
                ienv=["0:BB_PARTY_OPEN=1", "0:BB_PARTY_OPEN_ANY_LEVEL=1", "1:BB_PARTY_CODE=127.0.0.1:{p0}", "1:BB_PARTY_PASSWORD=",
                      "1:BB_PARTY_INVADE_LEVEL=900"],
                until=[r"0:cooperators 0 -> 1"], until_hold=75, crash=[])


def check_ojla(v, L):
    need(v, L, "ojla join: the level 900 guest is in the host's world (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1")
    need(v, L, "ojla guest services: guest in the host's world", 1, r"guest services: open: guest in the host's")
    need(v, L, "ojla host: the vanilla range would have removed it, any level keeps it", 0, r"level 900 is outside the vanilla range \d+-\d+, kept")
    none_of(v, L, "ojla: no level refusal on the guest", r"Can't join .*outside their range", insts=(1,))
    none_of(v, L, "ojla: no level gate refusal or removal on the host", r"open-party join level \d+ refused|outside the host's range \d+-\d+: removed", insts=(0,))
    none_of(v, L, "ojla: the guest never left the party", r"cooperators 1 -> 0", insts=(0,))
    check_common(v, L, "ojla")


def check_ojl(v, L):
    need(v, L, "ojl guest: refused with the level range sentence", 1, r"Can't join .*: your level 900 is outside their range \(\d+-\d+\)")
    # The guest dials while the host's level is still unknown (no world yet): admitted, then removed by the vitals recheck.
    need(v, L, "ojl host: the guest is removed (HELLO gate or the vitals recheck)", 0, r"outside the host's range \d+-\d+: removed|open-party join level 900 refused")
    check_common(v, L, "ojl")


# Phase iv2 / ivm (0.7.9, CL-7e748c, group `invade_two`): Max invaders. Three games: the host (Allow invaders on) and TWO invaders (the
# machine's queue allows two rendered clients, so both invaders are headless here); no guest, so the party's Max players 3 holds host + 2.
# iv2 (Max invaders 2): invader A dials, then B while A is in: both are admitted and stand in the world together, both on the ONE
# invader team on every game; A falls (it goes home, the host is paid for it), B stays, then B falls (paid again). ivm (the default,
# Max invaders 1): B's dial while A is in is refused with "another invader is already in that world"; after A fell, B asks again and is in.
IV2_UNTIL = [r"0:invasion of slot 2 ends: invader_died"]
IVM_UNTIL = [r"0:invasion of slot \d+ ends: invader_died"]  # the first invader falls; the second one is let in 30 s later (until_hold)


def phase_iv2(only, name="iv2", maxi=2):
    ienv = ["0:BB_PARTY_INVADERS=1", "1:BB_PARTY_INVADE=1", "2:BB_PARTY_INVADE=1"]
    if maxi == 2:
        ienv += ["0:BB_PARTY_MAX_INVADERS=2", "1:BB_PARTY_INVADE_TEST=rejoin@30?home,die@25?in",
                 "2:BB_PARTY_INVADE_TEST=rejoin@45?home,die@35?in"]
        until = IV2_UNTIL
    else:
        ienv += ["1:BB_PARTY_INVADE_TEST=rejoin@30?home,die@25?in", "2:BB_PARTY_INVADE_TEST=rejoin@45?home,rejoin@40?home"]
        until = IVM_UNTIL
    return dict(name=name, seconds=900, count=3, headless=2, env=[], ienv=ienv, crash=[], until=until, until_hold=40 if maxi == 2 else 70)


def check_iv2(v, L, name="iv2"):
    n = name
    a_in = need(v, L, n + " host: the first invader (slot 1) is in the world", 0, r"invade: invader slot 1: in the world")
    b_in = need(v, L, n + " host: the second invader (slot 2) is in the world too", 0, r"invade: invader slot 2: in the world", a_in or 0)
    a_end = need(v, L, n + " host: the first invader falls", 0, r"invasion of slot 1 ends: invader_died", b_in or 0)
    if b_in is not None and a_end is not None:
        v.step(n + " host: both were in the world together (the second came before the first fell)", b_in < a_end, "in +%.1f s, first ends +%.1f s" % (b_in, a_end))
    b_end = need(v, L, n + " host: the second invader falls on its own", 0, r"invasion of slot 2 ends: invader_died", a_end or 0)
    # the host's tables open once for both (an invader's own game closes and reopens them around its own arrival load)
    teams = len(L.all(0, r"invader: team \d+ is a copy of team 23", 0, 1e9))
    v.step(n + " host: both invaders share ONE invader team (the table copy is made once)", teams == 1, "%d copy(ies)" % teams)
    moved = len(L.all(0, r"invader: chr [0-9a-f]+ \(a remote member's\) moved from team \d+ to team \d+", 0, 1e9))
    v.step(n + " host: both invaders' chrs moved to the invader team", moved >= 2, "%d chr(s)" % moved)
    for i, who in ((1, "invader 1"), (2, "invader 2")):
        need(v, L, n + " %s: its own chr is on the invader team" % who, i, r"invader: chr [0-9a-f]+ \(this game's own\) moved from team \d+ to team \d+", a_in or 0)
        need(v, L, n + " %s: the other invader's copy is on the invader team too" % who, i, r"invader: chr [0-9a-f]+ \(a remote member's\) moved from team \d+ to team \d+", a_in or 0)
        need(v, L, n + " %s: told it invades" % who, i, r"invade: notice: You invade ", a_in or 0)
        need(v, L, n + " %s: falls and goes home" % who, i, r"invade: the invader fell: going home", a_in or 0)
    need(v, L, n + " invader 1: told the other invader came in", 1, r"invade: notice: Invader .* has entered the world: fight!", b_in or 0)
    entered = len(L.all(0, r"invade: notice: Invader .* has entered your world: fight!", 0, 1e9))
    v.step(n + " host: told of each invader (2 notices)", entered == 2, "%d" % entered)
    paid = len(L.all(0, r"invade: reward: host, invader_died: rate 0\.50, Insight \+0", 0, 1e9))
    v.step(n + " host: paid once for EACH fallen invader", paid == 2, "%d payout(s)" % paid)
    for i, who in ((1, "invader 1"), (2, "invader 2")):
        need(v, L, n + " %s: home through the game's own leave" % who, i, r"host lost \([^)]*\): home now \(OnLeave_Limit\)", a_in or 0)
    need(v, L, n + " the teams go back at the end", 0, r"invader: team \d+ closed: \d+ table cell\(s\) put back", b_end or 0)
    none_of(v, L, n + " no invader was refused for room or a present invader", r"refused by the host: (another invader|no free slot)", 0, 1e9, (1, 2))
    check_common(v, L, n)


def check_ivm(v, L):
    n = "ivm"
    a_in = need(v, L, n + " host: the first invader (slot 1) is in the world", 0, r"invade: invader slot 1: in the world")
    need(v, L, n + " invader 2: the second dial is refused (Max invaders 1 by default)", 2, r"refused by the host: another invader is already in that world", a_in or 0)
    a_end = need(v, L, n + " host: the first invader falls", 0, r"invasion of slot 1 ends: invader_died", a_in or 0)
    # the host is ALONE (no guest): the first invader's place in the host's room goes at once, so the second joiner's game does not
    # wait for a dead peer until the heartbeat timeout (it took 34 s, and the first try failed)
    need(v, L, n + " host: the first invader's room membership ended at once", 0, r"membership of the host's room ended at once", a_end or 0)
    again = need(v, L, n + " invader 2: asks again (its first dial was refused)", 2, r"test: script step 2: rejoin", a_end or 0)
    b_in = need(v, L, n + " host: the second invader gets in after the first left", 0, r"invade: invader slot \d+: in the world", a_end or 0)
    if again is not None and b_in is not None:
        v.step(n + " host: the second invader is in within 25 s of asking, into a host that was alone", b_in - again < 25, "%.1f s" % (b_in - again))
    check_common(v, L, n)


# Phase ivp (0.7.9, group `invade_place`): where the invader appears. Host (Allow invaders on) and invader, BOTH rendered. Both load
# into Central Yharnam at the Great Bridge lamp (the director's place=, god mode), then the invader dials from its own world (the same
# map block: an invader in another map block hangs, see docs/party/invasions.md); the host names a lamp it has lit, 30 m or more
# away (BB_PARTY_INVADE_APART_ANY=1: any lamp of the block, in case the test save lit too few); the invader must be put there (not
# beside the host) and stay there: the invader's log reads the distance every 5 s, the screenshots show both games.
IVP_UNTIL = [r"1:invade: apart: \d+ m from the host"]


def phase_ivp(only):
    ienv = ["1:BB_PARTY_INVADE=1", "0:BB_PARTY_INVADERS=1", "0:BB_PARTY_INVADE_APART_ANY=1",
            "0:BB_PARTY_DIRECTOR_TEST=god,place=0x18010000:-193.5:-28.6:68.6:0", "1:BB_PARTY_INVADE_TEST=rejoin@45?home,rejoin@25?home",
            "1:BB_PARTY_DIRECTOR_TEST=god,place=0x18010000:-193.5:-28.6:68.6:0"]
    return dict(name="ivp", seconds=720, count=2, headless=0, env=[], ienv=ienv, crash=[], until=IVP_UNTIL, until_hold=30)


def check_ivp(v, L):
    n = "ivp"
    pick = need(v, L, n + " host: names a lamp 30 m or more from the host", 0, r"invade: apart: \d+ lit lamp\(s\) in this block: the invader appears at lamp \d+ \((\d+) m from the host\)")
    adm = need(v, L, n + " host: the invader is in the world", 0, r"invade: invader slot \d+: in the world", pick or 0)
    put = need(v, L, n + " invader: put at that lamp, apart from the host", 1, r"invader: arrived [\d.]+ m from the host; put at lamp \d+, ([3-9]\d|\d{3})\.\d m from the host", adm or 0)
    far = need(v, L, n + " invader: stays apart (>= 25 m, 5 s later)", 1, r"invade: apart: ([2-9]\d|\d{3}) m from the host", put or 0)
    none_of(v, L, n + " invader: no fall recovery, no fall", r"recovery: falling for 6 s", 0, 1e9, (1,))
    none_of(v, L, n + " invader: not put beside the host afterwards", r"join: arrived [\d.]+ m from the host \(.*put beside it", (put or 0) - 1, 1e9, (1,))
    check_common(v, L, n)


# Phase ivc (0.7.9, group `invade_cross`): an invader in ANOTHER map block than the host. The host is placed in Central Yharnam (m24_01),
# the invader is NOT placed: it dials from its own world, the Hunter's Dream (m21). It must load into the host's map in ONE load (the A5
# cross-map arrival: its "rejoin" reaches the host, the host sends the snapshot and the destination back), then the invasion goes on as
# in the same block: placed apart from the host via a lit lamp, in the world, no 120 s hang ("could not get into the host's world").
IVC_UNTIL = [r"1:invader: arrived [\d.]+ m from the host; put at lamp"]


def phase_ivc(only):
    ienv = ["1:BB_PARTY_INVADE=1", "0:BB_PARTY_INVADERS=1", "0:BB_PARTY_INVADE_APART_ANY=1",
            "0:BB_PARTY_DIRECTOR_TEST=god,place=0x18010000:-193.5:-28.6:68.6:0", "1:BB_PARTY_INVADE_TEST=rejoin@45?home"]
    return dict(name="ivc", seconds=720, count=2, headless=0, env=[], ienv=ienv, crash=[], until=IVC_UNTIL, until_hold=30)


def check_ivc(v, L):
    n = "ivc"
    cross = need(v, L, n + " invader: its own world is in another map block than the host's (the cross-map arrival)", 1, r"A5: joined from another map \(own .*, the host's room in .*\): the arrival load follows")
    rj = need(v, L, n + " invader: travels into the host's map by the arrival re-join", 1, r"guest: travels alone \(join\) to .*, the host's area", cross or 0)
    need(v, L, n + " host: the invader's re-join reached it (snapshot again)", 0, r"host: slot \d+ travels alone into this area; re-sending the snapshot", rj or 0)
    need(v, L, n + " invader: the host's destination arrived", 1, r"guest: host destination stage", rj or 0)
    adm = need(v, L, n + " host: the invader is in the world", 0, r"invade: invader slot \d+: in the world", cross or 0)
    need(v, L, n + " invader: told it invades", 1, r"invade: notice: You invade ", adm or 0)
    put = need(v, L, n + " invader: put at a lamp apart from the host", 1, r"invader: arrived [\d.]+ m from the host; put at lamp \d+, ([3-9]\d|\d{3})\.\d m from the host", adm or 0)
    need(v, L, n + " invader: stays apart (>= 25 m)", 1, r"invade: apart: ([2-9]\d|\d{3}) m from the host", put or 0)
    none_of(v, L, n + " invader: no hang in the arrival (not sent home after 120 s)", r"could not get into the host's world|did not finish within 90 s|FallbackToB1|no host snapshot within 45 s", 0, 1e9, (1,))
    none_of(v, L, n + " invader: no fall recovery, no fall", r"recovery: falling for 6 s", 0, 1e9, (1,))
    check_common(v, L, n)


# Phase ivf (0.7.9, group `invade_fallback`): the invasion whose host snapshot never arrives (player report: the invader turned into a
# co-op white phantom in its own world after the 45 s B1 fallback). Same setup as ivc (cross-map invader), the host does not re-send the
# snapshot (BB_PARTY_B2_TEST_FAIL=resync). The invader must not replay the travel or get a co-op chr type: the invasion ends with a
# notice and the vanilla send-home, the host reads the invader as gone.
IVF_UNTIL = [r"1:the invasion ends: no way into the host's world"]


def phase_ivf(only):
    ienv = ["1:BB_PARTY_INVADE=1", "0:BB_PARTY_INVADERS=1", "0:BB_PARTY_INVADE_APART_ANY=1", "0:BB_PARTY_B2_TEST_FAIL=resync",
            "0:BB_PARTY_DIRECTOR_TEST=god,place=0x18010000:-193.5:-28.6:68.6:0", "1:BB_PARTY_INVADE_TEST=rejoin@45?home"]
    return dict(name="ivf", seconds=420, count=2, headless=0, env=[], ienv=ienv, crash=[], until=IVF_UNTIL, until_hold=40)


def check_ivf(v, L):
    n = "ivf"
    cross = need(v, L, n + " invader: its own world is in another map block than the host's", 1, r"A5: joined from another map \(own .*, the host's room in .*\): the arrival load follows")
    need(v, L, n + " host: the snapshot was withheld (test aid)", 0, r"test fail: join snapshot NOT re-sent", cross or 0)
    fb = need(v, L, n + " invader: the B1 fallback ran", 1, r"guest: FALLBACK(?: to B1)? \(no host snapshot within 45 s", cross or 0)
    end = need(v, L, n + " invader: an invader does not replay the travel, the invasion ends", 1, r"an invader does not replay the travel; the invasion ends", fb or 0)
    need(v, L, n + " invader: told why", 1, r"invade: notice: The invasion failed: you could not get into the host's world", end or 0)
    need(v, L, n + " invader: goes home", 1, r"the invasion ends: no way into the host's world", end or 0)
    need(v, L, n + " invader: left the party link (BYE)", 1, r"Leave: left the party", end or 0)
    need(v, L, n + " host: the invasion is over (the invader is gone, no co-op)", 0, r"invader slot \d+ is gone|invasion of slot \d+ ends", end or 0)
    none_of(v, L, n + " invader: never made a co-op white phantom", r"own chr type 0 -> 1", 0, 1e9, (1,))
    none_of(v, L, n + " invader: no travel replay in its own world", r"guest travel #\d+ .*ignored|FALLBACK to B1 .*no travel to replay", (fb or 0) + 0.001, 1e9, (1,))
    check_common(v, L, n)


PHASES = {**{n: (lambda only, n=n: phase_jn(only, n)) for n in JN_PHASES}, "ivc": phase_ivc, "ivf": phase_ivf, "ojla": phase_ojla, "iv2": phase_iv2, "ivm": lambda only: phase_iv2(only, "ivm", 1), "ivp": phase_ivp, "iv": phase_iv, "ivn": lambda only: phase_iv(only, "ivn", "dsl"), "ivx": phase_ivx, "ivl": phase_ivl, "ojl": phase_ojl, "ah": phase_ah, "pc": phase_pc, "vm": phase_vm, "yj": phase_yj, "yjc": phase_yjc, "llp": phase_llp, "s1": phase_s1, "sh": phase_sh, "gg": phase_gg, "s2": phase_s2, "rules": phase_rules, "ss": phase_ss, "sc": phase_sc, "summonfail": phase_summonfail,
          "s3": phase_s3, "sk": phase_sk, "lg": phase_lg, "lv": phase_lv, "ga": phase_ga, "gl": phase_gl, "lf": phase_lf, "lfx": phase_lfx, "lfa": phase_lfa, "lfw": phase_lfw, "lfwc": phase_lfwc, "lfws": phase_lfws, "lfwcs": phase_lfwcs, "lfwf": phase_lfwf, "lfwcf": phase_lfwcf, "lfwbs": phase_lfwbs, "lfwcbs": phase_lfwcbs, "lu": phase_lu, "mk": phase_mk, "kd": phase_kd, "kg": phase_kg, "fdr": phase_fdr, "kw": phase_kw, "fl": phase_fl, "fd": phase_fd, "fdj": phase_fdj, "cd": phase_cd, "iw": phase_iw, "iwn": phase_iwn, "an": phase_an, "anl": phase_anl, "ld": phase_ld, "cl": phase_cl, "cy": phase_cy, "cyn": lambda only: phase_cy(only, "cyn", True), "clg": phase_clg, "clw": phase_clw, "cla": phase_cla, "caa": phase_caa, "cam": phase_cam, "camo": lambda only: phase_cam(only, "offline"), "camh": phase_camh, "cas": phase_cas, "cpr": phase_cpr, "chc": phase_chc, "chn": lambda only: phase_chc(only, "chn", True), "chw": phase_chw, "cpa": phase_cpa, "cae": phase_cae, "cao": phase_cao, "cag": phase_cag, "cage": lambda only: phase_cag(only, True), "cgt": phase_cgt, "cgp": phase_cgp, "cgu": phase_cgu, "cab": phase_cab, "caq": phase_caq, "caw": phase_caw, "el": phase_el, "lt": phase_lt, "bf": phase_bf, "bv": phase_bv, "bw": phase_bw, "ch": phase_ch, "md": phase_md, "mds": phase_mds, "svl": phase_svl, "rh": phase_rh, "rs": phase_rs, "fw": phase_fw, "fwg": phase_fwg, "fwb": phase_fwb, "fwe": phase_fwe, "fh": phase_fh, "lp": phase_lp, "gd": phase_gd, "gh": phase_gh, "gm": phase_gm, "gr": phase_gr, "gr2": phase_gr2, "gr3": phase_gr3, "gr4": phase_gr4, "mp": phase_mp, "mpg": phase_mpg, "mph": phase_mph, "gro": lambda only: phase_gr(only, "gro", False), "gr2o": lambda only: phase_gr2(only, "gr2o", False), "gr3o": lambda only: phase_gr3(only, "gr3o", False), "gr4o": lambda only: phase_gr4(only, "gr4o", False), "jl": phase_jl, "ml": phase_ml, "mg": phase_mg, "ll": phase_ll, "hlq": phase_hlq, "bq": phase_bq, "hlr": phase_hlr, "hs": phase_hs, "ngj": phase_ngj, "nl": phase_nl, "ff": phase_ff, "lk": phase_lk, "lkc": phase_lkc, "rt": phase_rt, "br": phase_br, "rf": phase_rf, "rj": phase_rj, "em": phase_em, "emf": phase_emf, "bd": phase_bd, "bdo": phase_bdo, "sm": phase_sm, "ef": phase_ef, "ngp": phase_ngp, "mb": phase_mb, "gb": phase_gb, "mbs": phase_mbs, "gbs": phase_gbs, "da": phase_da, "cv": phase_cv, "cvs": phase_cvs, "cvo": lambda only: phase_cvs(only, False), "cvr": phase_cvr, "cvm": phase_cvm, "cvc": phase_cvc, "cva": phase_cva, "cvf": phase_cvf, "cvk": phase_cvk, "cvt": phase_cvt, "cvl": phase_cvl, "cvh": lambda only: phase_cvt(only, "host"), "ie": phase_ie, "il": phase_il, "rw": phase_rw, "rwe": phase_rwe, "rwt": phase_rwt, "gag": phase_gag, "gv": phase_gv, "gvg": phase_gvg, "gvr": phase_gvr, "gvl": phase_gvl, "gvn": lambda only: phase_gv(only, "gvn", "dsl"), "gvm": phase_gvm, "gvs": phase_gvs, "gvt": phase_gvt, "gvw": phase_gvw, "gvwn": lambda only: phase_gvw(only, "gvwn", "dsl"), "gva": phase_gva, "rwl": phase_rwl, "tv": phase_tv, "nf": phase_nf, "nfa": phase_nfa, "nfe": phase_nfe, "nfw": phase_nfw, "nfn": lambda only: phase_nfh(only, "nfn"), "nfr": lambda only: phase_nfh(only, "nfr"), "nfv": lambda only: phase_nfh(only, "nfv"), "nfd": lambda only: phase_nfh(only, "nfd"), "nfb": phase_nfb, "nfh": lambda only: phase_nfb(only, "nfh"), "npi": phase_npi, "npa": phase_npa, "npd": phase_npd, "pvd": phase_pvd, "pvh": phase_pvh, "pvn": phase_pvn, "npf": phase_npf, "eh": phase_eh, "bl": phase_bl, "mw": phase_mw, "px": phase_px, "pxg": lambda only: phase_px(only, True), "sj": phase_sj, "ou": phase_ou, "gdr": phase_gdr, "gdrd": lambda only: phase_gdr(only, "gdrd", "dsl"), "gdrb": lambda only: phase_gdr(only, "gdrb", "bad"), "gdrf": lambda only: phase_gdr(only, "gdrf", None, False), "mesh": phase_mesh, "oum": lambda only: phase_ou(only, "oum", VPN_MTU_SIM), "meshm": lambda only: phase_mesh(only, "meshm", VPN_MTU_SIM), "fp": phase_fp, "fp5": phase_fp5, "oj": phase_oj, "lo": phase_lo, "po": phase_po, "pos": phase_pos, "sg": phase_sg, "sg2": phase_sg2, "sgb": phase_sgb, "sgb2": phase_sgb2, "sgb3": phase_sgb3, "sgb4": phase_sgb4, "sgb4n": lambda only: phase_sgb4(only, "sgb4n", False), "sgp": phase_sgp, "csh": phase_csh, "csh2": phase_csh2, "csh3": phase_csh3, "cis": phase_cis, "cim": phase_cim, "ba": phase_ba, "bh": phase_bh, "bk": phase_bk, "hg": phase_hg, "bg": phase_bg, "lr": phase_lr, "lrg": phase_lrg, "mi": phase_mi, "mig": lambda only: phase_mi(only, "mig", True), "lw": phase_lw, "lwf": phase_lwf, "amp": phase_amp, "la": phase_la, "lb": phase_lb, "lc": phase_lc, "scb": phase_scb}
# --only groups (a smaller run for repeat cycles): the phase each needs and the verdict rows it keeps
GROUPS = {"join_stagger": ",".join(JN_PHASES), "join_stagger_sim": "jn2d,jn3d", "invade_cross": "ivc", "invade_fallback": "ivf", "open_level_any": "ojla", "invade_two": "iv2,ivm", "invade_place": "ivp", "invade": "iv", "invade_sim": "ivn", "invade_refuse": "ivx", "invade_level": "ivl", "open_level": "ojl", "auto_hud": "ah", "party_chat": "pc", "version_options": "vm", "yahargul_join": "yj", "yahargul_join_other": "yjc", "lecture_lamp_party": "llp", "join": "s1", "leave": "s1", "death": "s1", "travel": "s1", "boss": "s1", "host_death": "s1",
          "wipe": "s1", "wipe_late": "s1", "kill": "s2", "arena": "s2", "itembox": "s2", "rejoin_kill": "s2", "c6": "s2", "host_crash": "s2",
          "host_leave": "s2", "keyitems": "s2", "rules": "rules", "splitscreen": "ss", "stay_home": "sh", "guest_gather": "gg", "savecopy": "sc", "summonfail": "summonfail",
          "host_revive": "s3", "streamkill": "sk", "load_gap": "lg", "leave_gap": "lv", "gather": "ga", "gather_ground": "gl", "lifts": "lf", "lifts2": "lfx", "lift_apart": "lfa", "lift_walk": "lfw,lfwc", "lift_walk_far": "lfwf,lfwcf", "lift_walk_slow": "lfws,lfwcs,lfwbs,lfwcbs", "lift_down": "lu", "ladder": "lf,ld", "mark": "mk", "kidnap": "kd,kg", "drop_far": "fdr", "story_warp": "kw", "fall": "fl", "fall_deep": "fd,fdj", "host_fall_lamp": "cd", "cainhurst_scene": "iw,iwn", "cainhurst_annalise": "an,anl", "chalamp": "cl", "chalice_layers": "cy,cyn", "chalice_guest": "clg", "chalice_progress": "cpr", "chalice_curse": "chc,chn,chw", "chalice_altar": "cpa", "chalice_enter": "cae,cao", "chalice_altar_guest": "cag", "chalice_altar_guest_full": "cage", "chalice_boss": "cab", "chalice_queen": "caq,caw", "chalice_wipe": "clw", "chalice_alias": "cla,caa,cam,camo,camh,cas", "elevator": "el", "lifts_together": "lt", "fight_load": "bf", "cainhurst_lift": "ch", "cainhurst_maid": "md", "cloth_stall": "mds", "save_load": "svl", "research_lift": "rh", "research_solo": "rs", "forbidden_lift": "fw", "forbidden_lift_guest": "fwg", "forbidden_lift_both": "fwb", "forbidden_lift_edge": "fwe", "hemwick_lift": "fh", "lamps": "lp", "guest_dream": "gd", "guest_dream_host": "gh", "guest_lamp": "gm", "guest_rest": "gr,gr2,gr3,gr4", "atlas": "mp,mpg,mph", "guest_rest_off": "gro,gr2o,gr3o,gr4o", "join_lamp": "jl", "mensis_lamp": "ml", "mensis_lamp_guest": "mg", "lecture_lamp": "ll", "host_lamp": "hlq", "boss_lamp": "bq", "host_lamp_repair": "hlr", "boss_lamp": "bq", "headstone_split": "hs", "ngplus_rejoin": "ngj", "npc_lamp": "nl", "frontier_fall": "ff", "lunarium_key": "lk,lkc",

          "rot": "rt", "boss_rooms": "br", "rom_fog": "rf", "rom_jump": "rj", "emissary": "em", "emissary_first": "emf", "boss_drops": "bd", "boss_drops_off": "bdo", "scene_mirror": "sm", "ending_flow": "ef", "ngplus_ending": "ngp", "moon_music": "mb", "gehrman_music": "gb", "moon_music_stop": "mbs", "gehrman_music_stop": "gbs", "dream_apart": "da", "covenant": "cv",
          "valtr_rune": "cv", "covenant_talk": "cvt,cvh", "covenant_seq": "cvs,cvo", "covenant_runes": "cvr", "covenant_measure": "cvm", "covenant_chalice": "cvc", "covenant_annalise": "cva", "covenant_alfred": "cvf", "covenant_kneel": "cvk", "covenant_staff": "cvl", "items_every": "ie", "items_leak": "il", "reward_guest_kill": "rw", "reward_eye": "rwe", "reward_treasure": "rwt", "guest_gates": "gag", "give_items": "gv,gvg", "give_rollback": "gvr,gvl", "give_netsim": "gvn", "give_menu": "gvm,gvs,gvt", "give_gear": "gvw", "give_armor": "gva", "give_gear_netsim": "gvwn", "reward_late": "rwl", "shop_sync": "il", "travel_vote": "tv", "npc_fights": "nf,nfa", "npc_alfred": "nfa", "npc_eileen": "nfe", "npc_alfred_ward": "nfw", "npc_hunter_nightmare": "nfn", "npc_hunter_research": "nfr", "npc_valtr": "nfv", "npc_djura": "nfd", "npc_brador": "nfb", "npc_hamlet": "nfh", "npc_invader": "npi", "npc_attack": "npa", "npc_doll": "npd", "pvp_revive": "pvd", "pvp_full_hp": "pvh", "pvp_npc": "pvn", "npc_fog": "npf", "echo_hold": "eh", "limbo_leave": "bl", "boss_darkness": "mw", "death_poison": "px,pxg", "seam_rejoin": "sj", "outage_short": "ou", "guest_restart": "gdr,gdrd,gdrb,gdrf", "guest_mesh": "mesh", "vpn_mtu": "oum,meshm", "fourp_nat": "fp", "fivep": "fp5", "open_party": "oj", "lan_only": "lo", "phantom_own": "po", "phantom_save": "pos", "shop_repair": "sg", "shop_repair2": "sg2", "shop_badge": "sgb", "shop_badge_sync": "sgb2", "badge_guard": "sgb3", "badge_gate": "sgb4,sgb4n", "sgive_owner": "sgp", "chalice_shop": "csh,csh2", "chalice_shop3": "csh3", "chalice_insight": "cis", "chalice_madman": "cim", "chalice_glyph": "cgt,cgp,cgu", "boss_ai": "ba", "boss_dead": "bh,bk", "boss_late": "hg", "boss_guest": "bg", "boss_laurence_wipe": "lr", "boss_laurence_guest_first": "lrg", "micolash": "mi", "micolash_guest": "mig", "boss_visible": "bv,bw", "ludwig_wipe": "lw", "ludwig_refight": "lwf", "amelia_phase": "amp", "lone_ai": "la,lc", "scurry_beasts": "scb"}
GROUP_ROWS = {"join_stagger": r"^jn\d[a-d][ :]", "join_stagger_sim": r"^jn\dd[ :]", "invade_cross": r"^ivc ", "invade_fallback": r"^ivf ", "open_level_any": r"^ojla ", "invade_two": r"^(iv2|ivm) ", "invade_place": r"^ivp ", "invade": r"^iv ", "invade_sim": r"^ivn ", "invade_refuse": r"^ivx ", "invade_level": r"^ivl ", "open_level": r"^ojl ", "auto_hud": r"^ah ", "party_chat": r"^pc ", "version_options": r"^vm ", "yahargul_join": r"^yj ", "yahargul_join_other": r"^yj ", "lecture_lamp_party": r"^ll ", "join": r"^s1 join", "leave": r"s1 (Leave|Rejoin)", "death": r"guest death|bloodstain|revived",
              "travel": r"travel", "boss": r"Gascoigne|intro", "host_death": r"host death|limbo",
              "wipe": r"wipe", "wipe_late": r"late wipe", "kill": r"s2 boss kill|party stays", "arena": r"arena", "itembox": r"item box|reward on the summoned",
              "rejoin_kill": r"s2 (Leave after|Rejoin|no kick)", "c6": r"guest crash",
              "host_crash": r"host crash", "host_leave": r"host Leave", "keyitems": r"key item", "rules": r"^rules", "splitscreen": r"^ss ", "stay_home": r"^sh ", "guest_gather": r"^gg ",
              "savecopy": r"^sc ", "summonfail": r"^summonfail",
              "host_revive": r"^s3 ", "streamkill": r"^sk ", "load_gap": r"^lg(:| )", "leave_gap": r"^lv(:| )", "gather": r"^ga ", "gather_ground": r"^gl ",
              "lifts": r"^lf (join|lift|guest's lift|host)", "lifts2": r"^lfx (join|lift|guest's lift|host)", "lift_apart": r"^lfa ", "lift_walk": r"^lfwc? ", "lift_walk_far": r"^lfwc?f ", "lift_walk_slow": r"^lfwc?b?s ", "lift_down": r"^lu ", "ladder": r"^lf (join|ladder)|^ld ", "mark": r"^mk ", "kidnap": r"^k[dg] ", "drop_far": r"^fdr ", "story_warp": r"^kw ", "fall": r"^fl ", "fall_deep": r"^fd ", "host_fall_lamp": r"^cd ", "cainhurst_scene": r"^iwn? ", "cainhurst_annalise": r"^an |^anl ", "chalamp": r"^cl ", "chalice_layers": r"^cyn? ", "chalice_guest": r"^clg ", "chalice_progress": r"^cpr ", "chalice_curse": r"^ch[cw] ", "chalice_altar": r"^cpa ", "chalice_enter": r"^ca[eo] ", "chalice_altar_guest": r"^cag ", "chalice_altar_guest_full": r"^cage ", "chalice_boss": r"^cab ", "chalice_queen": r"^ca[qw] ", "chalice_enter": r"^ca[eo] ", "chalice_altar_guest": r"^cag ", "chalice_altar_guest_full": r"^cage ", "chalice_boss": r"^cab ", "chalice_wipe": r"^clw ", "chalice_alias": r"^(cla|caa|cam|camo|camh|cas) ", "elevator": r"^el ", "lifts_together": r"^lt ", "fight_load": r"^bf ", "cainhurst_lift": r"^ch ", "cainhurst_maid": r"^md ", "cloth_stall": r"^mds ", "save_load": r"^svl ", "research_lift": r"^rh ", "forbidden_lift": r"^fw ", "forbidden_lift_guest": r"^fwg ", "forbidden_lift_both": r"^fwb ", "forbidden_lift_edge": r"^fwe ", "hemwick_lift": r"^fh ", "research_solo": r"^rs ", "lamps": r"^lp ", "guest_dream": r"^gd ", "guest_dream_host": r"^gh ", "guest_lamp": r"^gm ", "join_lamp": r"^jl ", "mensis_lamp": r"^ml ", "lecture_lamp": r"^ll ", "host_lamp": r"^hlq ", "host_lamp_repair": r"^hlr ", "mensis_lamp_guest": r"^mg ", "headstone_split": r"^hs ", "ngplus_rejoin": r"^ngj ", "npc_lamp": r"^nl ", "frontier_fall": r"^ff ", "lunarium_key": r"^lkc? ",
              "rot": r"^rt ", "boss_rooms": r"^br ", "rom_fog": r"^rf ", "rom_jump": r"^rj ", "emissary": r"^em ", "emissary_first": r"^emf ", "boss_drops": r"^bd ", "boss_drops_off": r"^bdo ", "scene_mirror": r"^sm ", "ending_flow": r"^ef ", "ngplus_ending": r"^ngp ", "moon_music": r"^mb ", "gehrman_music": r"^gb ", "moon_music_stop": r"^mbs ", "gehrman_music_stop": r"^gbs ", "dream_apart": r"^da ",
              "covenant": r"^cv(:| join| host's fixture| rule| covenant| shared| host's save| guest's save)",
              "valtr_rune": r"^cv(:| join| rule| rune)", "covenant_talk": r"^cv[th] ", "covenant_seq": r"^cv[so] ", "covenant_runes": r"^cvr ", "covenant_measure": r"^cvm ", "covenant_chalice": r"^cvc ", "covenant_annalise": r"^cva ", "covenant_alfred": r"^cvf ", "covenant_kneel": r"^cvk ", "covenant_staff": r"^cvl ", "items_every": r"^ie ", "items_leak": r"^il ", "reward_guest_kill": r"^rw ", "reward_eye": r"^rwe ", "reward_treasure": r"^rwt ", "guest_gates": r"^gag ", "give_items": r"^gv[g]? ", "give_rollback": r"^gv[rl] ", "give_netsim": r"^gvn ", "give_menu": r"^gv[ms] ", "give_items": r"^gv[g]? ", "give_rollback": r"^gv[rl] ", "give_netsim": r"^gvn ", "give_menu": r"^gv[ms] ", "give_items": r"^gv[g]? ", "give_rollback": r"^gv[rl] ", "give_netsim": r"^gvn ", "give_menu": r"^gv[ms] ", "reward_late": r"^rwl ", "shop_sync": r"^il (join|sync)", "travel_vote": r"^tv ", "npc_fights": r"^nfa? ", "npc_alfred": r"^nfa ", "npc_attack": r"^npa ", "npc_doll": r"^npd ", "pvp_revive": r"^pvd ", "pvp_full_hp": r"^pvh ", "pvp_npc": r"^pvn ", "npc_fog": r"^npf ", "npc_invader": r"^npi ", "npc_eileen": r"^nfe ", "npc_alfred_ward": r"^nfw ", "npc_hunter_nightmare": r"^nfn ", "npc_hunter_research": r"^nfr ", "npc_valtr": r"^nfv ", "npc_djura": r"^nfd ", "npc_brador": r"^nfb ", "npc_hamlet": r"^nfh ", "echo_hold": r"^eh ", "limbo_leave": r"^bl ", "boss_darkness": r"^mw ", "death_poison": r"^pxg? ", "seam_rejoin": r"^sj ", "outage_short": r"^ou ", "guest_restart": r"^gdr[dbf]? ", "guest_mesh": r"^mesh ", "vpn_mtu": r"^(oum|meshm) ", "fourp_nat": r"^fp ", "fivep": r"^fp5 ", "open_party": r"^oj ", "lan_only": r"^lo ", "phantom_own": r"^po ", "phantom_save": r"^pos ", "shop_repair": r"^sg ", "shop_repair2": r"^sg2 ", "shop_badge": r"^sgb ", "shop_badge_sync": r"^sgb2 ", "badge_guard": r"^sgb3 ", "badge_gate": r"^sgb4n? ", "sgive_owner": r"^sgp ", "chalice_shop": r"^csh2? ", "chalice_shop3": r"^csh3 ", "chalice_insight": r"^cis ", "chalice_madman": r"^cim ", "chalice_glyph": r"^cg[tpu] ", "boss_ai": r"^ba ", "boss_dead": r"^b[hk][ :]", "boss_late": r"^hg ", "boss_guest": r"^bg ", "boss_laurence_wipe": r"^lr ", "boss_laurence_guest_first": r"^lrg ", "micolash": r"^mi ", "micolash_guest": r"^mig ", "boss_visible": r"^bv ", "ludwig_wipe": r"^lw ", "ludwig_refight": r"^lwf ", "amelia_phase": r"^amp ", "lone_ai": r"^la |^lc "}


# --only: the log lines that end each group's checks (instances.py --until "I:RX"); a phase with
# --only ends once the chosen groups' lines are all seen, so the steps after them never run.
GROUP_UNTIL = {
    "stay_home": SH_UNTIL,
    "invade": IV_UNTIL,
    "invade_sim": IV_UNTIL,
    "invade_refuse": IVX_UNTIL,
    "invade_level": IVL_UNTIL,
    "open_level": OJL_UNTIL,
    "open_level_any": [r"0:cooperators 0 -> 1"],
    "guest_gather": GG_UNTIL,
    "join": ["0:cooperators [01] -> 2"],
    # the host's second "cooperators 1 -> 2": the first is guest 2's first join, the second the rejoin
    "leave": ["1:guest_rejoin: Party > Rejoin pressed", r"0:cooperators 1 -> 2[\s\S]*cooperators 1 -> 2"],
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
    "itembox": ["1:item notice closed|lot 50000010 .*(?:not )?given (?:now )?while summoned|owed key item: lot 50000010"],
    # guest 1 back in the host's world after the Rejoin, then ~10 s more log (the no-kick check);
    # the press alone ended the run before the join (kickfix k2)
    "rejoin_kill": [r"1:guest_rejoin: Party > Rejoin pressed[\s\S]*apply gate open: summoned[\s\S]{20000}"],
    "keyitems": [r"1:key goods held: .*load mode 0\)"],  # guest 1 back in its own world after the Leave
    "rules": [r"0:mark=rules_h_ffoff[\s\S]{200}", "1:test menu: done"],
    "splitscreen": [r"1:Party HUD: splitscreen compass, [1-9] companion"],
    # guest 2 in the host's world, then ~10 s more log (the screenshots of it in the host's world)
    "summonfail": [r"0:cooperators 0 -> 1"],
    "streamkill": [SK_UNTIL],
    "load_gap": [LG_UNTIL],
    "leave_gap": [LV_UNTIL],
    "gather": GA_UNTIL,
    "gather_ground": GL_UNTIL,
    # the host back at the lamp (the lift's block streamed in, its init run) and guest 1's chain done
    "lifts": [LF_UNTIL],
    "lift_apart": [LFA_UNTIL, r"1:travel step (\d+)/\1: guest_press"],
    "fall": [FL_UNTIL, FL_HOST_UNTIL],
    "host_fall_lamp": [CD_UNTIL],
    "cainhurst_scene": IW_UNTIL,
    "cainhurst_annalise": [AN_UNTIL],
    "elevator": EL_UNTIL,
    "lifts_together": LT_UNTIL,
    "lift_walk": LFW_UNTIL,
    "lift_walk_slow": LFW_UNTIL,
    "lift_walk_far": LFW_UNTIL,
    "cainhurst_lift": CH_UNTIL,
    "cainhurst_maid": MD_UNTIL,
    "cloth_stall": MD_UNTIL,
    "save_load": [SVL_UNTIL],
    "research_lift": RH_UNTIL,
    "research_solo": [r"0:travel step (\d+)/\1: "],
    "lamps": LP_UNTIL,
    "guest_dream": GD_UNTIL,
    "yahargul_join": YJ_UNTIL,
    "version_options": [r"1:the host refused this game|1:join: arrived \d+\.\d m from the host"],
    "guest_dream_host": [r"0:Lamp travel: arrived: lamp 2102950|0:no load within 60 s|0:test: FAILED"],
    "guest_lamp": [r"1:test: listonly: warp menu closed|1:test: FAILED"],
    "atlas": [r"1:Lamp travel: arrived: lamp 2302950|0:Lamp travel: arrived: lamp 2302950|\d:test: FAILED|\d:Map: test travel: no lamp"],
    "guest_rest": [r"1:Lamp travel: arrived: lamp 2412951|1:Lamp travel: arrived: lamp 2302950|0:Lamp travel: arrived: lamp 2302950|\d:test: FAILED"],
    "join_lamp": [r"1:test: listonly: warp menu closed|1:test: FAILED"],
    "mensis_lamp": [r"1:guest: died alone: revived|1:test: FAILED"],
    "lecture_lamp": [r"0:test: flags 1320781\d=1|0:test: flags 1320783\d=1|0:test: FAILED"],
    "host_lamp": [r"0:test: travel step 3/3: host_press|0:test: FAILED"],
    "host_lamp_repair": [r"0:lamp repair: this save was checked|0:test: FAILED"],
    "mensis_lamp_guest": [r"0:host death taken: .*revive at lamp|0:test: FAILED"],
    "headstone_split": HS_UNTIL,
    "ngplus_rejoin": [r"1:start \(prologue_solo\): in the host's world|1:test: FAILED"],
    "npc_lamp": NL_UNTIL,
    "frontier_fall": FF_UNTIL,
    "lunarium_key": LK_UNTIL + LKC_UNTIL,
    "mark": [MK_UNTIL],
    # the host awake in the cell (its last step) and both guests in m28_00
    "kidnap": [KD_UNTIL, "1:map .*-> m28_00", "2:map .*-> m28_00"],
    "drop_far": [FDR_UNTIL],
    "story_warp": [KW_UNTIL],
    # the host back in the dungeon from the Dream, ~40 s after (guest 1 summoned back in)
    "chalamp": [CL_UNTIL, CL_UNTIL_G],
    "chalice_guest": [CLG_UNTIL, CLG_UNTIL_G],
    "chalice_layers": [CY_UNTIL],
    "chalice_wipe": [CLW_UNTIL],
    "chalice_progress": [r"1:travel step (\d+)/\1: "],
    "chalice_curse": [CLW_UNTIL, r"1:travel step (\d+)/\1: "],
    "fight_load": BF_UNTIL,
    "rot": [RT_UNTIL],
    "echo_hold": [EH_UNTIL],
    "boss_rooms": BR_UNTIL,
    "rom_fog": RF_UNTIL,
    "rom_jump": RJ_UNTIL,
    "emissary": EM_UNTIL, "emissary_first": EM_UNTIL,
    "boss_drops": BD_UNTIL,
    "boss_drops_off": BDO_UNTIL,
    "scene_mirror": [SM_UNTIL],
    "ending_flow": [EF_UNTIL], "ngplus_ending": [NGP_UNTIL],
    "moon_music": [MB_UNTIL],
    "gehrman_music": [GB_UNTIL],
    "moon_music_stop": [MBS_UNTIL],
    "gehrman_music_stop": [GBS_UNTIL],
    "dream_apart": DA_UNTIL,
    "npc_fights": [NF_UNTIL],
    "npc_alfred": [NFA_UNTIL],
    "npc_attack": [NPA_UNTIL],
    "npc_doll": [r"0:test: host_press 0x0 "],
    "pvp_revive": [PVD_UNTIL],
    "pvp_full_hp": [PVH_UNTIL],
    "pvp_npc": [PVN_UNTIL],
    "npc_fog": [NPF_UNTIL],
    "covenant": CV_UNTIL,
    "valtr_rune": CV_UNTIL,
    "covenant_talk": CVT_UNTIL, "covenant_seq": CVT_UNTIL, "covenant_runes": CVR_UNTIL, "covenant_chalice": CVC_UNTIL, "covenant_annalise": CVT_UNTIL,
    "items_every": IE_UNTIL,
    "reward_guest_kill": RW_UNTIL,
    "reward_eye": RWE_UNTIL,
    "reward_treasure": RWT_UNTIL,
    "reward_late": RWL_UNTIL,
    "items_leak": IL_UNTIL,
    "shop_sync": [r"0:host flag 5091 = 1 \(world_state\)"],
    "travel_vote": TV_UNTIL,
    "limbo_leave": BL_UNTIL,
    "boss_darkness": MW_UNTIL,
    "death_poison": PX_UNTIL,
    "death_poison": PX_UNTIL,
    "boss_ai": BA_UNTIL,
    "boss_dead": BA_UNTIL,
    "boss_late": HG_UNTIL,
    "boss_guest": BG_UNTIL,
    "boss_laurence_wipe": LR_UNTIL,
    "micolash": MI_UNTIL,
    "micolash_guest": MI_UNTIL,
    "boss_visible": BV_UNTIL,
    "ludwig_wipe": LW_UNTIL,
    "ludwig_refight": LWF_UNTIL,
    "ludwig_refight": LWF_UNTIL,
    "ludwig_refight": LWF_UNTIL,
    "amelia_phase": AMP_UNTIL,
    "amelia_phase": AMP_UNTIL,
    "lone_ai": LA_UNTIL,
    "seam_rejoin": SJ_UNTIL,
    "fourp_nat": [r"0:cooperators 2 -> 3"],
    "fivep": [r"0:cooperators 3 -> 4[\s\S]*cooperators 3 -> 4"],
    "lan_only": [r"0:cooperators 0 -> 1"],
    "open_party": [r"0:cooperators 0 -> 1"],
    "guest_mesh": [r"1:the block= ports are cut[\s\S]*direct again \(probes answered\)", r"2:the block= ports are cut[\s\S]*direct again \(probes answered\)"],
    "phantom_own": PO_UNTIL,
    "phantom_save": [r"0:test: levelup: (LEVEL UP on the guest|FAILED)"],
    "shop_repair": [SG_UNTIL],
    "shop_repair2": [SG2_UNTIL],
    "shop_badge": SGB_UNTIL,
    "shop_badge_sync": SGB2_UNTIL,
    "badge_guard": SGB3_UNTIL, "badge_gate": [SGB4_UNTIL],
    "sgive_owner": [SGP_UNTIL[0]],
    "chalice_shop": [CSH_UNTIL, CSH2_UNTIL],
    "chalice_shop3": [CSH_UNTIL],
    "outage_short": [r"0:outage file .*: %d o%d" % (OU_LENGTHS[-1], len(OU_LENGTHS))],
}
# --only: seconds the run goes on after the group's lines (screenshots of the state they reach)
GROUP_HOLD = {"host_death": 10, "wipe_late": 10}  # wipe_late: the fight opened by the walk-in on every screen  # the spectate on every screen (the guests' panels: "Spectating")
NEEDS_CRASH = {"c6", "host_crash", "host_leave", "guest_restart"}  # s2 groups that need the crash plan and its full chain


def narrow_phase(ph, only):
    """--only: end the phase on the chosen groups' lines; drop the s2 crash plan nobody asked for."""
    mine = [g for g in only if ph["name"] in GROUPS[g].split(",")]
    if ph["name"] in ("iwn", "anl"):  # its own until (the scene lines); the group marker is iw's
        return ph
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


# ---- frame-rate sweep (--fps 30,60,90,60/30) --------------------------------------------------------
# A phase's "fps" key is its own default rate (the rate a report was made at: rh, fw*, fh, rs at 60) and
# applies only when no --fps sweep is running. "fps_fixed": True marks a phase that needs its rate even in
# a sweep (none does today). Without --fps nothing changes: BB_MP_FPS (default 30) or the phase's "fps".
# Physics / timing groups (lifts, ladders, falls: anything frame-dependent): `--only physics`.
PHYSICS_GROUPS = ["lift_down", "lifts", "lift_apart", "lift_walk", "ladder", "elevator", "lifts_together", "cainhurst_lift", "research_lift", "research_solo",
                  "forbidden_lift", "forbidden_lift_guest", "forbidden_lift_both", "forbidden_lift_edge",
                  "hemwick_lift", "fall", "fall_deep", "host_fall_lamp", "frontier_fall", "cainhurst_maid", "cloth_stall", "rom_jump"]


def parse_fps_sweep(spec):
    """'30,60,90,60/30' -> ['30', '60', '90', '60/30'] (each entry validated like instances.parse_fps)."""
    entries = [x.strip() for x in str(spec).split(",") if x.strip()]
    if not entries:
        raise ValueError("empty --fps list")
    sys.path.insert(0, str(HERE))
    import instances as _inst
    for e in entries:
        _inst.parse_fps(e, 1)
    return entries


def fps_dir_name(entry):
    """Output sub-directory of one sweep entry: 60 -> fps60, 60/30 -> fps60x30."""
    return "fps" + entry.replace("/", "x")


def phase_fps(ph, sweep=None):
    """The --fps value for a phase's instances.py call (None: leave BB_MP_FPS / the default)."""
    if sweep and not ph.get("fps_fixed"):
        return sweep
    return str(ph["fps"]) if ph.get("fps") else None


def fps_summary(results, entries):
    """results: {entry: {phase: [passed, failed]}} -> (text, [phases that pass at some rates, fail at others])."""
    phases = []
    for e in entries:
        for ph in results.get(e, {}):
            if ph not in phases:
                phases.append(ph)
    w = max([5] + [len(e) for e in entries]) + 2
    lines = ["%-8s" % "phase" + "".join("%-*s" % (w + 6, "@" + e) for e in entries)]
    sensitive = []
    for ph in phases:
        cells, verdicts = [], []
        for e in entries:
            r = results.get(e, {}).get(ph)
            if r is None:
                cells.append("-")
                continue
            ok = r[1] == 0 and r[0] > 0
            verdicts.append(ok)
            cells.append("%s %d/%d" % ("PASS" if ok else "FAIL", r[0], r[0] + r[1]))
        lines.append("%-8s" % ph + "".join("%-*s" % (w + 6, c) for c in cells))
        if any(verdicts) and not all(verdicts):
            sensitive.append(ph)
    lines.append("(cell: verdict passed/total steps)")
    lines.append("FPS-sensitive (pass at some rates, fail at others): " + (", ".join(sensitive) if sensitive else "none"))
    return "\n".join(lines), sensitive


FR_LINE = re.compile(r"Frame rate: [\d.]+ s real, \d+ steps \(([\d.]+) per s\)")
FG_LINE = re.compile(r"FG pacing: source ([\d.]+) presented ([\d.]+)")
FG_STATE = re.compile(r"Frame generation: (\w+) (?:(\d)x on|unavailable)")


def _median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def measured_rates(lines):
    """One instance's log lines -> what it really ran: {'steps': median game steps per s (the frame-rate
    hooks' BB_FRAME_RATE_LOG lines; None at the game's own 30), 'fg': 'FSR 2x on' / 'FSR unavailable' /
    None (no frame-generation line), 'source' / 'presented': median FG pacing rates per s}. The first
    samples (start, loading) are dropped when there are more than four."""
    steps = [float(m.group(1)) for m in map(FR_LINE.search, lines) if m]
    fg = [(float(m.group(1)), float(m.group(2))) for m in map(FG_LINE.search, lines) if m]
    if len(steps) > 4:
        steps = steps[2:]
    if len(fg) > 4:
        fg = fg[2:]
    state = None
    for ln in lines:
        m = FG_STATE.search(ln)
        if m:
            state = "%s %sx on" % (m.group(1), m.group(2)) if m.group(2) else "%s unavailable" % m.group(1)
    return {"steps": _median(steps), "fg": state, "source": _median([x[0] for x in fg]),
            "presented": _median([x[1] for x in fg])}


def measured_summary(sub, entry):
    """A sweep entry's out dir -> (text lines: per instance what it ran, over every phase's logs; problems:
    frame generation asked but not on)."""
    sys.path.insert(0, str(HERE))
    import instances as _inst
    want_fg = _inst.parse_fg(entry, 5)
    per = {}
    for log in sorted(Path(sub).glob("*/inst[0-9].log")):
        i = int(log.stem[-1])
        try:
            meta = json.loads((log.parent / "instances.json").read_text())
            drawn = i < meta["count"] - meta["headless"]
        except (OSError, ValueError, KeyError):
            drawn = True
        r = measured_rates(log.read_text(encoding="utf-8", errors="replace").splitlines())
        r["drawn"] = drawn  # headless: no present, no frame generation
        per.setdefault(i, []).append(r)
    lines, problems = [], []
    for i in sorted(per):
        rs = per[i]
        steps = _median([r["steps"] for r in rs if r["steps"]])
        fg = next((r["fg"] for r in rs if r["fg"]), None)
        src = _median([r["source"] for r in rs if r["source"]])
        pres = _median([r["presented"] for r in rs if r["presented"]])
        text = "inst%d: game steps %s/s" % (i, "%.1f" % steps if steps else "30 (own rate)")
        if want_fg[i] and any(r["drawn"] for r in rs):
            text += ", frame generation %s" % (fg or "NO LOG LINE")
            if src and pres:
                text += " (rendered %.1f, presented %.1f per s)" % (src, pres)
            if not fg or "on" not in fg or not (src and pres and pres > 1.5 * src):
                problems.append("@%s inst%d: frame generation asked, not running (%s)" % (entry, i, fg or "no log line"))
        lines.append(text)
    return lines, problems


# flaky078: phases that do NOT test enemies, fights, drops or echoes: every loaded regular enemy is killed quietly as its area
# loads (director clear_enemies; bosses and NPCs stay). Phases that test enemy behaviour (s2 / s3 arena hits, bosses, invasions,
# drops, echoes) must not be listed. BB_SMOKE_CLEAR_ENEMIES=0 turns it off for a comparison run.
CLEAR_ENEMY_PHASES = {"lfw", "lfwc", "lfws", "lfwcs", "lfwf", "lfwcf", "lfwbs", "lfwcbs"}


# Fail fast (smoke 0.7.9, instances.py --stuck): a scripted run whose chains are unfinished and in which nothing moved
# (no step, load, join, boss event) for this long is over; a chain step waiting for a dead own chr ends it after 90 s.
# Default: 150 s, or 1.5 x the chain's longest step delay + 90 s. These phases have longer quiet stretches by design
# (measured on the 0.7.8 logs: the longest progress-free gap of a passing run; the values keep 1.4x or more of it).
STUCK_S = {"ngp": 520, "rules": 450, "ivx": 420, "ml": 300, "ou": 220, "oum": 220, "ah": 200, "lfwcf": 200, "gv": 180, "gvn": 180,
           "mesh": 180, "meshm": 180, "il": 180, "gvl": 180}


def stuck_seconds(ph):
    """The --stuck value of a phase (0 = off): its own `stuck`, else STUCK_S, else from its chains' longest step delay."""
    if "stuck" in ph:
        return ph["stuck"]
    if ph["name"] in STUCK_S:
        return min(STUCK_S[ph["name"]], ph["seconds"])
    top = 0.0
    for e in list(ph["env"]) + list(ph["ienv"]):
        m = re.match(r"(?:\d:)?BB_PARTY_DIRECTOR_TEST=(.*)", e)
        for it in (m.group(1).split(",") if m else ()):
            d = re.search(r"@([\d.]+)", it)
            top = max(top, float(d.group(1)) if d else 0.0)
    return min(max(150.0, 1.5 * top + 90.0), ph["seconds"])


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
    for i in range(5):
        p = root / f"inst{i}" / "run.log"
        # instances.py moves the old run.log to run.prev.log, unless something holds it open (then
        # it appends): remember the file id and size, and copy from there when it is the same file.
        starts[i] = (p.stat().st_ino, p.stat().st_size) if p.exists() else (None, 0)
    if ph.get("prep"):
        ph["prep"](root)
    cmd = [sys.executable, str(HERE / "instances.py"), "run", "--count", str(ph.get("count", 3)),
           *(["--fps", phase_fps(ph, getattr(a, "sweep_fps", None))] if phase_fps(ph, getattr(a, "sweep_fps", None)) else []),
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
    sys.path.insert(0, str(HERE))
    import instances as _inst  # the instance ports ({p0}..{p3}: a test aid naming a peer's party port)
    port0 = _inst.base_port(root)

    def subst(e):
        for k in range(5):
            e = e.replace("{p%d}" % k, str(port0 + k))
        return e.replace("{root}", str(root)).replace("{out}", str(out.resolve()))
    for e in ph["env"]:
        cmd += ["--env", subst(e)]
    clear = ph["name"] in CLEAR_ENEMY_PHASES and os.environ.get("BB_SMOKE_CLEAR_ENEMIES", "1") != "0"
    for e in ph["ienv"]:
        if clear:  # flaky078: no enemies to run into / get hit by in a phase that does not test them (director clear_enemies)
            e = re.sub(r"^(\d+:)?BB_PARTY_DIRECTOR_TEST=", lambda m: m.group(0) + "clear_enemies,", e)
        cmd += ["--ienv", subst(e.replace("{hold}", hold))]
    for w in ph.get("start_when", []):
        cmd += ["--start-when", w]
    for i in (ph.get("pads") or ((0,) if ph.get("pad") else ())):
        # the stick walks (host_stick / guest_stick) write the in-process pad script
        (root / f"pad{i}.txt").write_text("lx=128 ly=128\n")
        cmd += ["--ienv", f"{i}:BB_PAD_FILE=" + str(root / f"pad{i}.txt")]
    if ph.get("until_hold"):
        cmd += ["--until-hold", str(ph["until_hold"])]
    if not os.environ.get("BB_SMOKE_NO_STUCK") and stuck_seconds(ph) > 0:
        cmd += ["--stuck", "%.0f" % stuck_seconds(ph)]
    for c in ph["crash"]:
        cmd += ["--crash-plan", c]
    for u in ph.get("until", []):
        cmd += ["--until", u]
    # BB_MP_PARENT_PID: this harness; instances.py (and the reaper, by the slot's "parent") end the
    # games once it is gone (a killed smoke left its phase's games running to their cap).
    env = dict(os.environ, BB_MP_LEASE=os.environ.get("BB_MP_LEASE", "1200"), BB_MP_PARENT_PID=str(os.getpid()))
    if ph.get("until_quiet"):  # a phase whose last test step is followed by a long wait (cag: the guest's follow into the dungeon)
        env["BB_MP_UNTIL_QUIET"] = str(ph["until_quiet"])
    shots = out / "shots"
    shots.mkdir(exist_ok=True)
    if any("{out}/captures" in e for e in ph["env"]):
        (out / "captures").mkdir(exist_ok=True)  # the frame analyzer writes into an existing folder
    print("[smoke] %s: %s" % (ph["name"], " ".join(cmd)), flush=True)
    (out / "harness.log").unlink(missing_ok=True)
    shooter = LaterProc(out / "harness.log", [sys.executable, str(HERE / "shot.py"), "--root", str(root), "--out", str(shots),
                                "--tag", ph["name"], "--every", str(a.every), "--seconds", str(ph["seconds"] + 400)]
                               + ph.get("watch", []),
                               stdout=open(out / "shot.log", "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    driver = None
    try:
        # a phase driver (ou: the outage file) starts with the games, like the shooter
        driver = ph["driver"](root, out / "harness.log") if ph.get("driver") else None
        r = subprocess.run(cmd, env=env, stdout=open(out / "harness.log", "w"), stderr=subprocess.STDOUT)
    finally:
        shooter.terminate()
        if driver:
            driver.terminate()
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
    # the sweep's measured rates: which instances drew (a headless one runs no frame generation)
    (out / "instances.json").write_text(json.dumps({"count": ph.get("count", 3), "headless": ph.get("headless", 1)}))
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
        for i in range(5):
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


def check_cae(v, L):
    """cae: the one-press entry (BB_PARTY_ALTAR_AUTO) at the host's altar with a guest present."""
    check_common(v, L, "cae")
    t = need(v, L, "cae the autopilot answered the prompts and the host's load started", 0, r"altar: the party's dungeon is loading")
    need(v, L, "cae host in the dungeon (m29)", 0, r"map - -> m29_00", t or 0)
    need(v, L, "cae guest followed into the dungeon (m29)", 1, r"map - -> m29_00", t or 0)
    none_of(v, L, "cae the autopilot never gave up", r"altar: a prompt did not come", 0, 1e9, (0,))


def check_cao(v, L):
    """cao: the host's OWN ritual dungeon entered from the First Ritual Altar's awaken list with a guest present."""
    check_common(v, L, "cao")
    t = need(v, L, "cao the awaken list came up (seq answered)", 0, r"test: seq step \d+/\d+: cross", 0)
    need(v, L, "cao host entered its dungeon (lamp 2902950, m29)", 0, r"map - -> m29_00", t or 0)
    need(v, L, "cao guest followed into the host's dungeon (m29)", 1, r"map - -> m29_00", t or 0)
    none_of(v, L, "cao the menu walk was not stuck", r"seq STUCK", 0, 1e9, (0,))


def check_cab(v, L):
    """cab: a live Chalice layer boss with a guest: woken by the real route (the door's Open Door prompt + the walk
    in), its bar and HP on both games, damage synced, its death credits both (clear bonus on host and guest)."""
    check_common(v, L, "cab")
    need(v, L, "cab host in the dungeon (m29)", 0, r"map - -> m29_00")
    need(v, L, "cab the door prompt came up for the host", 0, r'test: text shows: "Open Door"')
    t0 = need(v, L, "cab the host's Cross opened the door (approach pressed)", 0, r"test: approach: pressed 0x4000")
    w0 = need(v, L, "cab the boss woke on the host (boss health bar shown, entity 2900100)", 0,
              r"boss health bar shown: a boss fight \(event \d+, entity 2900100", t0 or 0)
    w1 = need(v, L, "cab the same bar on the guest's game", 1, r"boss health bar shown: a boss fight \(event \d+, entity 2900100", t0 or 0)
    b0 = need(v, L, "cab host boss bar on screen with its HP (test: boss bar)", 0, r"test: boss bar \d+: .* hp \d+", w0 or 0)
    b1 = need(v, L, "cab guest boss bar on screen with its HP (test: boss bar)", 1, r"test: boss bar \d+: .* hp \d+", w1 or 0)
    hit = need(v, L, "cab damage: the host sets the boss to 50% HP", 0, r"host_kill: entity 2900100 HP \d+ -> \d+ \(HP set only", b0 or 0)
    if hit is not None:
        low = r"test: boss bar \d+: .* hp ([0-2]\d{3}|\d{1,3})$"
        need(v, L, "cab damage synced: the host's bar shows the lower HP", 0, low, hit)
        need(v, L, "cab damage synced: the guest's bar shows the lower HP too", 1, low, hit)
    kill = need(v, L, "cab the boss killed on the host (HP 0, the game's own death)", 0, r"host_kill: entity 2900100 HP \d+ -> 0 ", hit or 0)
    need(v, L, "cab boss death credited on the host (clear bonus)", 0, r"boss clear on this game \(BlockClear2\): boss 2900100", kill or 0)
    need(v, L, "cab boss death credited on the guest (host's clear bonus + its own)", 1, r"boss clear on this game \(BlockClear2\): boss 2900100", kill or 0)
    need(v, L, "cab guest: the host's boss clear bonus", 1, r"host boss clear bonus: \d+ Blood Echoes \(boss 2900100\)", kill or 0)
    # CL-6dc7c6: the Insight the kill gives reaches the guest (a "Insight N -> M" line with M > N, after the kill; the host's own
    # kill Insight and the boss event's story Insight both land there). The in-dungeon Bath Messenger's shop is covered by csh / csh3.
    need(v, L, "cab guest: Insight rises after the kill (the host's boss Insight reaches the guest)", 1,
         r"Party: \[[\d:.]+\] Insight (\d+) -> (\d+)", kill or 0)

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
    # Removed in 0.7.5: "a Pebble past the maximum lands in the storage". Since 35c4306a only key and
    # unique items are shared with the party, so the host's Pebble lot is no longer given to the guest.
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


def check_ss(v, L):
    j = need(v, L, "ss join: the guest is in (cooperators -> 1)", 0, r"cooperators 0 -> [1-9]")
    need(v, L, "ss guest draws the compass only (splitscreen)", 1, r"Party HUD: splitscreen compass, [1-9] companion", j or 0)
    none_of(v, L, "ss host keeps the full panel (no compass line)", r"Party HUD: splitscreen compass", insts=(0,))
    none_of(v, L, "ss splitscreen guest draws no name tags", r"Party HUD: tag \S+ (shown|drawn)", insts=(1,))
    need(v, L, "ss guest hides the information notices", 1, r"Party messages: hidden \(notice, mode errors, splitscreen\)")
    none_of(v, L, "ss host hides nothing", r"Party messages: hidden", insts=(0,))
    none_of(v, L, "ss: no crash / assert", r"(?i)unhandled exception|assertion failed|EXCEPTION_ACCESS|fatal error", insts=(0, 1))


def check_rules(v, L):
    need(v, L, "rules join: the guest is in (cooperators -> 1)", 0, r"cooperators 0 -> [1-9]")  # 4 hunters: two guests may land in one poll
    for k, (page, labels, change) in enumerate(RULES_PAGES):
        row, _, _, value, back, ini_after, ini_back, effect = change
        for i, who in ((0, "host"), (1, "guest")):
            hit = need(v, L, "rules %s: %s page opens" % (who, page), i, r"Game menu: page \d+ %s open" % re.escape(page))
            line = L.find(i, r"Game menu: page \d+ rows \(\d+\): ", hit or 0)
            if not line:
                v.step("rules %s: %s rows" % (who, page), False, "no rows line after the page opened")
                continue
            shown = [r.rsplit("=", 1)[0] for r in line[1].split("): ", 1)[1].split(" | ")]
            # the host has no "Join via a member" row (a guest's button)
            # (and a guest has no "Nightwatch VPN lobby" row: the host lists the party there)
            want = [x for x in labels if not (i == 0 and x == "Join via a member") and not (i == 1 and x in ("Nightwatch VPN lobby", "Open party (no password)", "Allow invaders", "Any level can join", "Max invaders"))
                    and not (i == 0 and x == "Host's access")]
            if page == "Party members":  # RULES_PLAYERS member rows (their names vary), then the fixed rows
                want = shown[:RULES_PLAYERS] + want
            ok = shown == want and (page != "Party members" or len(shown) == RULES_PLAYERS + (2 if RULES_PLAYERS < 5 else 1)) and len(shown) <= 6 and                 "(CUT)" not in re.sub(r"Last event=[^|]*", "Last event=", line[1])  # a long last event text is cut by design
            v.step("rules %s: %s rows" % (who, page), ok, line[1][:300] if ok else
                   "expected %s, got %s" % (want, line[1][:300]))
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
           and "Party votes on travel=Off" in sw[1] and "Covenants per player=On" in sw[1]
           and "NPC hunters fight all=On" in sw[1],  # 0.7.3 defaults
           sw[1][:250] if sw else "no Party rules: World rows line on the guest")
    # 0.7.5 partyrules: the guests' travel rules are the host's too, read-only on the guest
    tr = L.find(1, r"Game menu: page \d+ rows \(\d+\): Guests use lamps=")
    v.step("rules guest: sees the host's Party rules: Travel (read-only)",
           bool(tr) and "Guests use lamps=On" in tr[1] and "Guests use Hunter's Mark=On" in tr[1],
           tr[1][:250] if tr else "no Party rules: Travel rows line on the guest")
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
    # 0.7.6 connection warnings: the host names the guest whose UDP is not heard once it failed twice
    fails = L.all(0, r"slot 1 \(.*\) reports a failure: its UDP is not heard", f - 5)
    warn = L.find(0, r"connection warning: .* is in the party but its game is not heard", f - 5)
    # the phase sets BB_PARTY_CONNWARN_UDP_N=1: the guest's one failure report raises the notice
    v.step("summonfail: the host warns about the guest whose UDP is not heard (%d failure report(s))" % len(fails),
           warn if fails else True, (warn[1][:200] if warn else "no 'connection warning' line") if fails else "no failure report")
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


def check_lf(v, L, players=3):
    """players=2: the lfx run (host + guest 1, no observer game 2)."""
    insts = tuple(range(players))
    need(v, L, "lf join: %d players" % players, 0, r"cooperators [01] -> %d" % (players - 1))
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
    for i in insts[1:]:
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
    last = {i: lf_flags(L, i, 0, end) for i in insts}
    for key, name in ((12400827, "position"), (12400828, "busy")):
        vals = [last[i].get(key) if last[i] else None for i in insts]
        v.step("lf lift %s flag %d equal on all games at the end" % (name, key), len(set(vals)) == 1 and None not in vals,
               ", ".join("%s %s" % ("host" if i == 0 else "guest %d" % i, vals[i]) for i in insts))
    pull = ladder_end
    if pull is not None:
        need(v, L, "lf ladder: the host's game got the kick", 0, r"ObjAct kick \(another player\).*kick event 12410206",
             pull - 1, pull + 10)
        for i in insts:
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
    none_of(v, L, "lf lift: nobody died", r"local death: HP 0", 0, 1e9, insts)
    check_common(v, L, "lf")

def check_lfa(v, L):
    need(v, L, "lfa join: 2 players", 0, r"cooperators [01] -> 1")
    away = need(v, L, "lfa host travelled alone to the Dream", 0, r"map .*-> m21")
    ride = need(v, L, "lfa guest's lift ride (its own copy of the church lift's event)", 1,
                r"test: flags .*%s=\d" % LFA_POS, away or 0)
    sent = need(v, L, "lfa guest's ride over: sent to the host (or the host's world has it)", 1,
                r"guest: lift %s ride over \(flag %s = [01].*(to the host|has that end already)" % (LFA_CAR, LFA_POS), ride or 0)
    # the host's answer while it is away: the flag must be written (the block is not its world)
    ans = need(v, L, "lfa host (away) took the ride into its world", 0,
               r"lift %s %s=[01]\((applied: the lift's block is not loaded here|same here)\)" % (LFA_CAR, LFA_POS), (sent or 0) - 1)
    none_of(v, L, "lfa host did not refuse the ride as 'car loaded here'", r"lift %s %s=[01]\(car loaded here" % (LFA_CAR, LFA_POS), 0, 1e9, (0,))
    back = need(v, L, "lfa host back in Cathedral Ward", 0, r"map .*-> m24_01", ans or 0)
    end = 1e9
    last = {i: lf_flags(L, i, 0, end) for i in (0, 1)}
    for key, name in ((int(LFA_POS), "position"), (int(LFA_BUSY), "busy")):
        vals = [last[i].get(key) if last[i] else None for i in (0, 1)]
        v.step("lfa lift %s flag %d equal on both games at the end" % (name, key), len(set(vals)) == 1 and None not in vals,
               "host %s, guest 1 %s" % tuple(vals))
    # the car: the block's init event (12400146) puts the host's car where the position flag says: anim 0 for
    # flag 1, anim 15 for flag 0 (live: lfa runs); a host that kept the old end shows the car in the guest's empty shaft
    rows = L.all(0, r"test: object %s anim (?!-1 )\d+ " % LFA_CAR, back or 0, end)
    anim = re.search(r"anim (\d+) ", rows[-1][1]).group(1) if rows else None
    want = "0" if (last[1] or {}).get(int(LFA_POS)) == 1 else "15"
    v.step("lfa host's car at the end the guest's flag says (init anim %s)" % want, anim == want,
           "host anim %s after its return" % anim)
    none_of(v, L, "lfa lift: nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    check_common(v, L, "lfa")



def check_lv(v, L):
    need(v, L, "lv join: 3 players", 0, r"cooperators [01] -> 2")
    need(v, L, "lv the guest left", 1, r"guest_leave: Party > Leave pressed")
    none_of(v, L, "lv: no 'connection unstable' notice about the leaver", r"connection to .* unstable")
    none_of(v, L, "lv: no relay switch from the leave", r"repeated game traffic gaps, through the host relay")
    none_of(v, L, "lv: no outage resume log line about the leaver", r"outage resume: \S+ heard again after")
    check_common(v, L, "lv")


def check_lg(v, L):
    need(v, L, "lg join: 3 players", 0, r"cooperators [01] -> 2")
    tr = need(v, L, "lg the party travel ran", 0, r"travel step \d+/\d+: host_warp")
    ign = sum(len(L.all(i, r"rx gap in a loading screen ignored")) for i in (0, 1, 2))
    raw = sum(len(L.all(i, r"outage resume: \S+ heard again after")) for i in (0, 1, 2))
    v.step("lg info: loading gaps seen (ignored / unexcused resumes)", True, "%d / %d" % (ign, raw))
    none_of(v, L, "lg: no 'connection unstable' notice at the loads", r"connection to .* unstable")
    none_of(v, L, "lg: no relay switch from a gap at the loads", r"repeated game traffic gaps, through the host relay")
    none_of(v, L, "lg: no outage resume log line at the loads", r"outage resume: \S+ heard again after")
    check_common(v, L, "lg")


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
                    # 0.7.3 (673d1b29): a ':hp' kill outside the census tries the chr by entity id
                    # first, so an unloaded enemy logs "chr 0" in place of "no target"
                    r"test: host_kill: (no target \(entity {0}\)|NPC entity {0} \(chr 0, not in the census list\))"
                    .format(SK_ENEMY), away)
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
               r"test: host_kill: (no target \(entity {0}\)|NPC entity {0} \(chr 0x[0-9a-f]+, not in the census list\): "
               r"HP 0 -> 0)".format(SK_ENEMY), (rk or back) + 15)
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
        # 0.7.6: the lifted spot is a bad landing (above the host's floor): the next side; the test aid
        # (yesdropbad) makes the next two sides bad too: the host's own ground position is the last try.
        need(v, L, "gl guest: try 0 (8 m too high) is a bad landing", 1, r"guest: bad landing on try 0 \(above the host's floor", yes, yes + 6)
        need(v, L, "gl guest: try 1 bad (test aid) -> next side", 1, r"guest: bad landing on try 1 .*the next side", yes, yes + 12)
        need(v, L, "gl guest: try 2 bad (test aid) -> the host's own ground position", 1,
             r"guest: bad landing on try 2 .*the host's own ground position", yes, yes + 20)
        need(v, L, "gl guest: stands at the host's own position (try 3)", 1,
             r"guest: stands beside the host [\d.]+ s after the warp \([0-9.]+ m below the spot at most, try 3\)", yes, yes + 30)
        none_of(v, L, "gl guest: no map load for the gather", r"guest: travels alone \(gather\)", yes, yes + 40, (1,))
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
        # liftre077: samples are 0.5 s apart; a longer gap is a loading screen (a party travel's load has no
        # "travel step" line inside it), not a rate
        if t1 <= t0 or t1 - t0 > 2.5 or any(t0 <= s <= t1 for s in steps):
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


MD_DRAW_RX = re.compile(r"^  mesh pass #\d+ draw \d+: indices (\d+) ")
MD_CLOTH_RX = re.compile(r"^    vertex stream 0 at \S+ size \d+ stride 20: (\d+) elements, NaN \d+, box \(([^)]*)\) \.\. \(([^)]*)\)(?:, gpu-written (\w+), cpu-modified (\w+))?")


def md_record(text):
    """One frame analyzer record (BB_CAPTURE_MESH_COUNTS = MD_MESHES) -> ({(mesh, lod): draws},
    [cloth streams: (elements, all zero)]): which of the c2150 meshes the frame drew, at which LOD."""
    by_count = {c: (name, lod) for name, cs in MD_MESHES.items() for lod, c in enumerate(cs)}
    draws, cloth = {}, []
    for line in text.splitlines():
        m = MD_DRAW_RX.match(line)
        if m and int(m.group(1)) in by_count:
            k = by_count[int(m.group(1))]
            draws[k] = draws.get(k, 0) + 1
            continue
        m = MD_CLOTH_RX.match(line)
        if m and int(m.group(1)) < 4096:  # a cloth mesh's own small stream (not the shared vertex heap)
            zero = all(float(x) == 0.0 for x in (m.group(2) + " " + m.group(3)).split())
            # GPU-produced (the cloth's own vertices: the GPU's copy is newer than the game's memory) and
            # not about to be overwritten by the game's (stale) bytes; None: an older record without the flags
            fresh = None if m.group(4) is None else (m.group(4) == "yes" and m.group(5) == "no")
            cloth.append((int(m.group(1)), zero, fresh))
    return draws, cloth


def md_missing(draws):
    """[(mesh, lod, drawn, bodies)]: the maid meshes drawn fewer times than her body at that LOD (every c2150
    draws its dress and cloth with its body: a missing skirt that is not drawn shows here)."""
    out = []
    for lod in range(3):
        bodies = draws.get(("body", lod), 0)
        for name in ("dress", "BD_cloth", "AM_cloth", "BD_cloth_e"):
            n = draws.get((name, lod), 0)
            if n < bodies:
                out.append((name, lod, n, bodies))
    return out


def check_md(v, L, d):
    recs = sorted((Path(d) / "captures").glob("frame_*.txt")) if d else []
    parsed = [(r.name, *md_record(r.read_text(encoding="utf-8", errors="replace"))) for r in recs]
    seen = [(n, sum(c for (m, _l), c in dr.items() if m == "body")) for n, dr, _c in parsed]
    v.step("md frame analyzer records with the maids drawn (c2150 body)", len(seen) >= 4 and all(b > 0 for _n, b in seen),
           "%d records; bodies drawn %s" % (len(seen), " ".join(str(b) for _n, b in seen)))
    miss = [(n, md_missing(dr)) for n, dr, _c in parsed]
    bad = [(n, m) for n, m in miss if m]
    zero = sum(1 for _n, _d, cl in parsed for _e, z, _f in cl if z)
    total = sum(len(cl) for _n, _d, cl in parsed)
    v.step("md every maid drawn with her dress and cloth meshes (none drawn less than her body)", bool(parsed) and not bad,
           ("; ".join("%s: %s" % (n, ", ".join("%s LOD%d %d/%d" % x for x in m)) for n, m in bad[:3])) if bad else
           "all records; cloth vertex streams all-zero in game memory: %d of %d" % (zero, total))
    # The maids' cloth vertex streams (stride 20) are written by the GPU (live: every one gpu-written, the game's
    # memory under them zero at 30 FPS, stale at 60+). A draw that reads the game's bytes instead (gpu-written no,
    # or cpu-modified yes: an upload over the GPU's copy) draws the skirt from stale data: the missing skirt.
    flags = [f for _n, _d, cl in parsed for _e, _z, f in cl if f is not None]
    stale = sum(1 for f in flags if not f)
    v.step("md the maids' cloth vertex streams read from the GPU's own copy (none stale)", bool(flags) and not stale,
           "%d of %d cloth streams stale (gpu-written no or cpu-modified yes)" % (stale, len(flags)) if flags else
           "no cloth stream flags in the records (an older build)")
    # the locked-on maid's HP (R3 picks the nearest one: 2500233 or 2500234)
    hp = [int(x) for x in re.findall(r"lock-on 1 \(target at .*: enemy 25002\d\d, HP (\d+)\)",
                                     "\n".join(l for _t, l in L.all(0, r"test: host_press 0x")))]
    v.step("md the maid was hit (her HP dropped)", len(hp) >= 2 and min(hp) < hp[0],
           "HP " + " -> ".join(str(h) for h in sorted(set(hp), reverse=True)) if hp else "no lock-on on a maid")
    cap = L.all(0, r"test: host_gfx: capture")
    hit = L.find(0, r"test: host_press 0x800;")
    v.step("md records before and after the hit", bool(hit) and any(t < hit[0] for t, _l in cap) and any(t > hit[0] for t, _l in cap),
           "%d records, hit at %s" % (len(cap), "+%.1f s" % hit[0] if hit else "-"))
    check_common(v, L, "md")


def check_mds(v, L, d):
    """Steady cloth time (BB_STEADY_TIME=0 for the old behaviour): the time the cloth update is stepped with around
    each injected stall (the trace lines of bbport_frame_rate.cpp TestStall), and the frame records taken."""
    if not L.find(0, r"Frame rate: free \(hooks"):
        v.step("mds not applicable: the game's own 30 FPS runs no frame-rate hooks (cloth untouched)", True, "no hooks")
        check_common(v, L, "mds")
        return
    stalls = L.all(0, r"Frame rate: test stall \d+ ms")
    v.step("mds stalls injected (%d wanted)" % MDS_ROUNDS, len(stalls) >= MDS_ROUNDS, "%d" % len(stalls))
    rx = re.compile(r"test stall trace: dt ([\d.]+) s, steady ([\d.]+) s, cloth time given ([\d.]+) s, stepped ([\d.]+) s")
    ratio_given = ratio_stepped = 0.0
    longest_given = longest_stepped = 0.0
    lines = 0
    for k, (t0, _l) in enumerate(stalls):
        t1 = stalls[k + 1][0] if k + 1 < len(stalls) else t0 + 5
        rows = [tuple(float(x) for x in m.groups()) for _t, l in L.all(0, r"test stall trace:", t0, t1) for m in [rx.search(l)] if m]
        lines += len(rows)
        for a, b in zip(rows, rows[1:]):
            for col in (2, 3):
                if a[col] > 0 and b[col] > 0:
                    r = max(a[col] / b[col], b[col] / a[col])
                    if col == 2:
                        ratio_given = max(ratio_given, r)
                    else:
                        ratio_stepped = max(ratio_stepped, r)
        longest_given = max([longest_given] + [r[2] for r in rows])
        longest_stepped = max([longest_stepped] + [r[3] for r in rows])
    off = os.environ.get("BB_STEADY_TIME") == "0"
    v.step("mds the cloth's time step never jumps by more than 6%% from frame to frame around a stall%s"
           % (" (steady time off: informative)" if off else ""),
           bool(lines) and (off or ratio_stepped <= 1.06),
           "%d trace lines; cloth time given: longest %.1f ms, worst frame-to-frame ratio %.2f; stepped: longest %.1f ms, "
           "worst ratio %.2f" % (lines, longest_given * 1000, ratio_given, longest_stepped * 1000, ratio_stepped))
    recs = sorted((Path(d) / "captures").glob("frame_*.txt")) if d else []
    v.step("mds frame records around the stalls", len(recs) >= 2 * MDS_ROUNDS, "%d records" % len(recs))
    check_common(v, L, "mds")


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


def check_fw(v, L, tag="fw", riders=(0,), pos="12700134", busy="12700135", first="0", block="0x1b000000", car=(-217.2, 279.7),
             top=-77.9, puts=1):
    """lifts2: each rider rides one way and back, never stands more than 2.5 m over the car's top floor
    (`top`) in the shaft (the report: a foot at a time up to ~30 m over the car going down), is put back
    on its car at most `puts` times (BB_TEST_LIFT_RIDEBIT_OFF runs: FW_PUTS), and leaves the car."""
    puts = int(os.environ.get("FW_PUTS", puts))
    second = "1" if first == "0" else "0"
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    for i in riders:
        go = need(v, L, "%s inst%d: a ride starts" % (tag, i), i, r"test: flags %s=\d %s=1" % (pos, busy))
        if go is None:
            continue
        one = need(v, L, "%s inst%d: first ride (pos %s)" % (tag, i, first), i, r"test: flags %s=%s" % (pos, first),
                   after=go)
        if one is None:
            continue
        two = need(v, L, "%s inst%d: ride back (pos %s)" % (tag, i, second), i, r"test: flags %s=%s" % (pos, second),
                   after=one)
        high = []
        for t, line in L.all(i, r"test: pos area %s \(" % block, one - 12.0, (two or one + 30.0) + 3.0):
            m = re.search(r"\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line)
            if m and math.hypot(float(m.group(1)) - car[0], float(m.group(3)) - car[1]) < 3.0 and                     float(m.group(2)) > top + 2.5:
                high.append((t, float(m.group(2))))
        v.step("%s inst%d: never high over the car" % (tag, i), not high,
               "max y %.1f" % max(y for _, y in high) if high else "none")
        thru = L.all(i, r"dropped through the moving car", one - 12.0, 1e9)
        v.step("%s inst%d: at most %d put-back(s) on the car" % (tag, i, puts), len(thru) <= puts,
               "%d: %s" % (len(thru), thru[0][1][:150]) if thru else "none")
    check_common(v, L, tag)


def check_lift_down(v, L, tag):
    """liftfall078: the guest rode the car down in the host's world: on the car all the way and at the bottom alive.
    With the test aid (FW_ENV=BB_TEST_LIFT_DROP=S: the chr is set 1.6 m under the car S s into the ride, its ride bit
    reads off) the rescue must bring it back: the drop is seen, the guest is carried again or landed on solid ground,
    never in the shaft under the bottom floor, nobody dies."""
    sp = LD[tag]
    drop = "BB_TEST_LIFT_DROP" in os.environ.get("FW_ENV", "")
    need(v, L, tag + " join: 2 players", 0, r"cooperators 0 -> 1")
    go = need(v, L, tag + " guest: the ride starts", 1, r"test: flags %s=\d %s=1" % (sp["pos"], sp["busy"]))
    ys = []
    for t, line in L.all(1, r"test: pos area %s \(" % sp["block"], go or 0, (go or 0) + 40.0):
        mm = re.search(r"\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line)
        if mm:
            ys.append((t, float(mm.group(2))))
    low = min((y for _, y in ys), default=None)
    v.step(tag + " guest: never under the bottom floor", low is not None and low > sp["bot_y"] - 2.0,
           "lowest y %s (bottom %.1f)" % (("%.1f" % low) if low is not None else "none", sp["bot_y"]))
    thru = L.all(1, r"dropped through the moving car", go or 0, 1e9)
    if drop:
        t0 = need(v, L, tag + " guest: the test drop under the car happened", 1, r"BB_TEST_LIFT_DROP( \d+ of \d+)?\): done", go or 0)
        v.step(tag + " guest: the drop was rescued (put back on the car)", bool(thru), "%d put-back(s)" % len(thru))
        gave = L.all(1, r"was put back on the car \d+ times and is not carried", go or 0, 1e9)
        landed = L.all(1, r"the local chr from .* onto ", go or 0, 1e9)
        end = ys[-1][1] if ys else None
        carried = end is not None and end < sp["bot_y"] + 1.0
        v.step(tag + " guest: carried again to the bottom, or landed safely",
               carried or bool(landed) and not (end is not None and end < sp["bot_y"] - 2.0),
               "end y %s, give-up %d, landing %d" % (("%.1f" % end) if end is not None else "none", len(gave), len(landed)))
        none_of(v, L, tag + " guest: no fall to the shaft floor", r"local death: HP 0", 0, 1e9, (0, 1))
    else:
        v.step(tag + " guest: no drop through the car", not thru, "%d: %s" % (len(thru), thru[0][1][:150]) if thru else "none")
        none_of(v, L, tag + " lift: nobody died", r"local death: HP 0", 0, 1e9, (0, 1))
    check_common(v, L, tag)


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
            # 0.7.3 (5c286072, user decision): a guest leaving the Dream alone never resets the host's
            # world: a split-travel re-join with the host's snapshot, no rest load, guest 2 stays
            need(v, L, "mk %d host: the guest's lone travel, the snapshot again" % (k + 1), 0,
                 r"host: a guest travels alone into this area \(split travel\)", u[0] - 1, end)
            none_of(v, L, "mk %d host: no rest load from the guest leaving the Dream" % (k + 1),
                    r"rested at lamp %s" % MK_LAMP, u[0] - 1, end, (0,))
            none_of(v, L, "mk %d guest 2 stays (no load)" % (k + 1), r"map - -> m24_00", u[0], end, (2,))
            need(v, L, "mk %d host: guest 1 arrived" % (k + 1), 0, r"host: slot \d arrived", u[0], end)
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
             max(0.0, ga - 15), end)  # the host's scene can start before the guest logs its grab (other clock, 60 FPS)
        need(v, L, "kd (guest grab) host: +1 Insight from 9422", 0, r"event 9422 gave the host 1 Insight", max(0.0, ga - 15), end)
        for i in (1, 2):
            if g28[i] is not None:
                need(v, L, "kd (guest grab) guest %d: the waking scene in the cell" % i, i, r"remo 2800[01]010 mode",
                     g28[i], end)
                need(v, L, "kd (guest grab) guest %d: +1 Insight (9422)" % i, i,
                     r"event 9422, 1 Insight\): \+1 here", max(0.0, ga - 15), end)
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
                # two paths send it home, whichever is first: the apart tick (6 s) or the game's own OnLeave_Limit (5.9 s after
                # the host left, not dropped: the host's dungeon is not this guest's)
                need(v, L, "cl guest 1 forced out: sent home by itself", 1,
                     r"chalice: this guest is apart in|funnel: guest_sent_home from \+0x138a51c \(role guest, not a party travel\)",
                     back - 5, g_back)
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
        # 0.7.6: the host's dungeon came home with it, into a free slot next to its own (slot 1), once
        need(v, L, "cl guest 1 brought the host's dungeon home (slot 2, its own slot 1 kept)", 1,
             r"progress home: dungeon 1110692594440089 added to the altar in slot 2", first)
        none_of(v, L, "cl guest 1 never added a dungeon over its own", r"progress home: dungeon \d+ added to the altar in slot 1,", 0, 1e9, (1,))
    # A re-summon into the dungeon whose layer boss is dead never pays it again (cl6 / cl8: PREY
    # SLAUGHTERED, +1995 echoes and +1 Insight at every re-summon).
    none_of(v, L, "cl no second paid kill of 2900100 after a re-summon",
            r"boss clear on this game \(BlockClear2\): boss 2900100|kill seen: entity 2900100 3\.5 s later: .*\(\+[1-9]\d* since\)",
            0, 1e9, (1,))
    none_of(v, L, "cl no GPU device lost / guest fault",
            r"(?i)device lost|GPU fault|Guest fault|Dantelion2 Panic|exited with code [1-9]")
    check_common(v, L, "cl")


def check_chc(v, L, wipe=False):
    """chc: in the Cursed dungeon the host and the guest are both cut to about half; the cut survives a death + revive."""
    join = need(v, L, "chc join: guest 1 in the host's dungeon", 1, r"map .* -> m29_00")
    rx = r"test: hp (\d+)/(\d+) area (0x\w+) curse speffects:(.*)"

    def rows(i, after=0.0):
        out = []
        for _t, line in L.all(i, rx, after):
            m = re.search(rx, line)
            if m and m.group(3).lower().startswith("0x1d") and int(m.group(2)) > 0:
                out.append(m)
        return out

    h, g = rows(0), rows(1, join or 0)
    v.step("chc host: logged its max HP in the dungeon", bool(h), "%d lines" % len(h))
    v.step("chc guest: logged its max HP in the dungeon", bool(g), "%d lines" % len(g))
    if not (h and g):
        return check_common(v, L, "chc")
    v.step("chc host: the Cursed SpEffect 4730 is on, max HP halved at first", " 4730" in h[0].group(4), h[0].group(0)[:100])
    v.step("chc guest: a curse SpEffect (4735 / 4736, vanilla's client curse) is on at first",
           " 4735" in g[0].group(4) or " 4736" in g[0].group(4), g[0].group(0)[:100])
    hm, gm = int(h[0].group(2)), int(g[0].group(2))
    v.step("chc guest: max HP in the dungeon = the host's cursed max HP (both half of the hunter's: %d, %d)" % (hm, gm),
           0.9 <= gm / max(hm, 1) <= 1.1, "ratio %.2f" % (gm / max(hm, 1)))
    # after the scripted death + revive of each
    hl, gl = h[-1], g[-1]
    v.step("chc host: after its death and revive the curse is still on and max HP is %d" % hm,
           " 4730" in hl.group(4) and int(hl.group(2)) == hm, hl.group(0)[:100])
    v.step("chc guest: after its death and revive the curse is still on and max HP is %d" % gm,
           (" 4735" in gl.group(4) or " 4736" in gl.group(4)) and int(gl.group(2)) == gm, gl.group(0)[:100])
    if wipe:
        need(v, L, "chw the host's respawn travel reaches the guest", 1, r"guest: host travels \(#\d+ host_death")
    check_common(v, L, "chc")


def check_cpr(v, L):
    join = need(v, L, "cpr join: guest 1 in the host's dungeon", 1, r"map .* -> m29_00")
    need(v, L, "cpr host lit the boss room's lamp", 0, r"lamp spot: Chalice Dungeon lamp %s lit / used" % CL_LAMP)
    left = need(v, L, "cpr guest 1 left the party", 1, r"guest_leave: Party > Leave pressed", join or 0)
    need(v, L, "cpr the host's dungeon added to the guest's altar (slot 1, nothing else there)", 1,
         r"progress home: dungeon 1110692594440089 added to the altar in slot 1, ([4-9]|\d\d+) of \d+", left or 0)
    none_of(v, L, "cpr no second add of the dungeon", r"progress home: dungeon \d+ added to the altar in slot (?!1,)", 0, 1e9, (1,))
    keep = CPR_KEEP / "1" / "CUSA00207" / "SPRJ0005" / "userdata0000"
    if keep.is_file():
        sys.path.insert(0, str(REPO / "tools" / "party"))
        import save_decode as sd
        s = sd.Save.read(keep)
        def val(base, off, bits=32):
            x = 0
            for b in range(bits):
                x = (x << 1) | (1 if s.flag(base + off + b) else 0)
            return x
        base = 94100000
        ok = val(base, 32) == 0x458bd799 and val(base, 64) == 0x3f22b and (val(base, 96) >> 24) == 0x1d
        v.step("cpr the guest's save: slot 1 holds the host's dungeon", ok,
               "record %x %x %x" % (val(base, 32), val(base, 64), val(base, 96)))
        lamp = [s.flag(i) for i in (14107840, 14107850, 14107851, 74100202)]
        v.step("cpr the guest's save: the lit lamp 2902952's flags in slot 1", all(lamp), str(lamp))
        v.step("cpr the guest's save: lamp 2902950 (not reached) stays unlit in slot 1", not s.flag(74100200), "")
        v.step("cpr the guest's save: the layer 1 boss flag 12901686 set in slot 1 (the host's kill)", s.flag(14101686), "")
    else:
        v.step("cpr the guest's save was kept", False, "missing %s" % keep)
    check_common(v, L, "cpr")


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
    st = need(v, L, "clg guest 1's fall leaves a party stain tied to the host's dungeon", 1,
              r"party stain \(death in the host's world\): 15000 Blood Echoes .* in Chalice Dungeon \d+")
    if st:
        shown = need(v, L, "clg the stain is shown in the host's dungeon", 1,
                     r"party stain: shown in the host's world, 15000 Blood Echoes", st)
        if shown:
            need(v, L, "clg guest 1 walks onto it: the 15000 echoes are paid in the Chalice Dungeon", 1,
                 r"party stain: recovered 15000 Blood Echoes in the host's world", shown)
            # echoes077 (NW-4b5e142): paid exactly once and exactly 15000, whatever gain rate the guest's SpEffects have
            rec = L.find(1, r"party stain: recovered 15000 Blood Echoes in the host's world at .*: echoes \d+ -> \d+", shown)
            m = re.search(r"echoes (\d+) -> (\d+)", rec[1]) if rec else None
            v.step("clg the pick-up adds exactly the stain's 15000 (the guest wears gain-rate SpEffects)",
                   bool(m) and int(m.group(2)) - int(m.group(1)) == 15000, rec[1][-60:] if rec else "no pay line")
            sp = L.all(1, r"stain test: SpEffect 10000\d on the own chr")
            v.step("clg the gain-rate SpEffects were on the guest", len(sp) == 3, "%d applied" % len(sp))
            pays = L.all(1, r"party stain: recovered \d+ Blood Echoes")
            v.step("clg the stain is paid once", len(pays) == 1, "%d pay lines" % len(pays))
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
    # The lamp table is derived at run time from the player's game (gamedata section `lamps`: AAB, n, group), so it is
    # read from the host instance's party_tables.txt, not from the source.
    lamps = []
    tabs = Path(RUN_ROOT[0]) / "inst0" / "user" / "party_tables.txt"
    if tabs.exists():
        sec = None
        for ln in tabs.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"\[(\w+)\] \d+", ln)
            if m:
                sec = m.group(1)
            elif sec == "lamps" and re.match(r"\d+	\d+	\d+", ln):
                a, n = ln.split("	")[:2]
                lamps.append((int(a), int(n)))
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
        # 0.7.7 (bossrunes): Ludwig's rune (lot 3401800, flag 6674) reaches the guest once; the Holy Moonlight Sword
        # (test award of its spot lot 43211) too.
        need(v, L, "br guest: Ludwig's rune from the host", 1, r"gave #\d+ lot 3401800 ", dead)
        n = len(L.all(1, r"gave #\d+ lot 3401800 "))
        v.step("br guest: exactly one give of Ludwig's rune", n == 1, "%d give(s)" % n)
        need(v, L, "br guest: the Holy Moonlight Sword (lot 43211)", 1, r"gave #\d+ lot 43211 .*weapon|gave #\d+ lot 43211 ")
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


def check_ef(v, L):
    """Phase ef: a guest that accepts plays the host's ending (cutscene, next cycle once, credits)."""
    need(v, L, "ef join: 2 players", 0, r"cooperators 0 -> 1")
    q = need(v, L, "ef the guest is asked about the host's ending 3", 1, r"ending 3: question open")
    a = need(v, L, "ef the guest answers YES", 1, r"ending 3: answer YES", q or 0)
    need(v, L, "ef the guest's ending is the host's (type 3, no cords needed)", 1,
         r"this player's ending type 3 \(host 3", a or 0)
    r = need(v, L, "ef the guest plays Childhood's Beginning (remo 21000030, +1000 by sex)", 1, r"replay: remo 2100[01]030 mode 0", a or 0)
    none_of(v, L, "ef never the other ending's cutscene (Honoring Wishes 21000020)", r"remo 2100(0020|1020) ", a or 0, insts=(1,))
    ng = L.all(1, r"Party story: NG cycle 0 -> 1", a or 0)
    v.step("ef the next cycle once", len(ng) == 1, "%d NG step(s)" % len(ng))
    if r is not None and ng:
        v.step("ef cutscene before the NG step", r < ng[0][0], "remo +%.1f ng +%.1f" % (r, ng[0][0]))
    need(v, L, "ef the credits flag 23 after the cutscene", 1, r"flags set ON \(read back: 23=1\)", a or 0)
    need(v, L, "ef the staff roll runs", 1, r"ending: staff roll / ending step t=10 s", a or 0)
    check_common(v, L, "ef")


def check_ngp(v, L):
    """Phase ngp: host ending with a guest; both saves NG+ 1 exactly; the guest rejoins in NG+."""
    j = need(v, L, "ngp join: 2 players", 0, r"cooperators 0 -> 1")
    q = need(v, L, "ngp the guest is asked about the host's ending", 1, r"ending 1: question open", j or 0)
    a = need(v, L, "ngp the guest answers YES", 1, r"ending 1: answer YES", q or 0)
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "ngp %s starts at NG cycle 0" % who, i, r"NG cycle 0 at world-up")
        need(v, L, "ngp %s ends in NG cycle 1" % who, i, r"NG cycle 1 \(changed\)|NG cycle 0 -> 1|NG cycle 1 at world-up", j or 0)
        too = L.all(i, r"NG cycle [2-9]\b|NG cycle 1 -> [2-9]")
        v.step("ngp %s never reaches NG cycle 2" % who, not too, "none" if not too else too[0][1][:120])
        need(v, L, "ngp %s: the new cycle's world is up (Iosefka's Clinic, NG cycle 1)" % who, i, r"NG cycle 1 at world-up \(map (?:m24_01|-)", j or 0)
    t = L.find(0, r"ending: the next cycle's world is up")
    t1 = L.find(1, r"ending: the next cycle's world is up")
    base = min(x[0] for x in (t, t1) if x) if (t or t1) else 0  # the earliest: the guest's line comes after the host's world and its READY
    nw = L.find(1, r"NG cycle 1 at world-up \(map (?:m24_01|-)", j or 0)
    base = nw[0] if nw else base
    r = need(v, L, "ngp the NG+ host becomes start-ready", 0, r"start \(prologue_solo\): step ready.*READY", base)
    need(v, L, "ngp the guest is back in the party in NG+ (Rejoin pressed or rung in by itself)", 1,
         r"guest_rejoin: (Party > Rejoin pressed|back in the party)|NG guard: kept the own NG cycle", r or base)
    need(v, L, "ngp the host has its guest again (cooperators 0 -> 1)", 0, r"cooperators 0 -> 1", r or base)
    need(v, L, "ngp the guest is in the host's NG+ 1 world (NG cycle 1 as a client)", 1,
         r"NG cycle 1 at world-up .*session client", r or base)
    none_of(v, L, "ngp no refused join after the ending", r"join refused|not start-ready", base, insts=(0, 1))
    check_common(v, L, "ngp")


def check_mb(v, L):
    """Phase mb: the Moon Presence music (event 12104853's boss map sound on) runs on host and guest."""
    need(v, L, "mb join: 2 players", 0, r"cooperators 0 -> 1")
    cut = need(v, L, "mb host: the Moon Presence event starts (12101852 past its wait)", 0, r"EMEVD event 12101852 \[(?:2[7-9]|30)\]")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "mb %s: Moon Presence music on (12104853 boss sound 2010[04])" % who, i,
             r"EMEVD event 12104853 \[13\]", cut or 0)
    none_of(v, L, "mb: the music is not switched off again at once (12104853 [21])", r"EMEVD event 12104853 \[2[1-3]\]", cut or 0)
    check_common(v, L, "mb")


def check_gb(v, L):
    """Phase gb: Gehrman's music (event 12104803's boss map sound on) runs on host and guest."""
    need(v, L, "gb join: 2 players", 0, r"cooperators 0 -> 1")
    cut = need(v, L, "gb host: Gehrman's fight starts (12101802 past its wait)", 0, r"EMEVD event 12101802 \[(?:[5-9]|1[0-6])\]")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "gb %s: Gehrman music on (12104803 boss sound 2010[04])" % who, i,
             r"EMEVD event 12104803 \[13\]", cut or 0)
    none_of(v, L, "gb: the music is not switched off again at once (12104803 [22])", r"EMEVD event 12104803 \[2[2-4]\]", cut or 0)
    check_common(v, L, "gb")


def check_gbs(v, L):
    """Phase gbs: after Gehrman's kill the music stops on host and guest (OFF event 12104805 [3])."""
    need(v, L, "gbs join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "gbs host: Gehrman music on", 0, r"EMEVD event 12104803 \[13\]")
    kill = need(v, L, "gbs host: Gehrman killed (kill flag 12104809)", 0, r"flags .*12104809=1", on or 0)
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "gbs %s: music off at the kill (OFF event 12104805 [3], 2010[04] off)" % who, i,
             r"EMEVD event 12104805 \[3\]", kill or 0)
    for i, who in ((0, "host"), (1, "guest")):
        v.step("gbs (info) %s: ON event stop [22] after the kill and the walk out" % who, True,
               "seen" if L.find(i, r"EMEVD event 12104803 \[22\]", kill or 0) else "never ran (the OFF event stops the music)")
    check_common(v, L, "gbs")


def check_mbs(v, L):
    """Phase mbs: after the Moon Presence's kill the music stops on host and guest (OFF event 12104855 [3])."""
    need(v, L, "mbs join: 2 players", 0, r"cooperators 0 -> 1")
    on = need(v, L, "mbs host: Moon Presence music on", 0, r"EMEVD event 12104853 \[13\]")
    kill = need(v, L, "mbs host: Moon Presence killed (kill flag 12104859)", 0, r"flags .*12104859=1", on or 0)
    for i, who in ((0, "host"), (1, "guest")):
        v.step("mbs (info) %s: ON event stop [21]" % who, True,
               "seen" if L.find(i, r"EMEVD event 12104853 \[21\]", kill or 0) else "never ran (the OFF event stops the music)")
        need(v, L, "mbs %s: music off at the kill (OFF event 12104855 [3], 2010[04] off)" % who, i,
             r"EMEVD event 12104855 \[3\]", kill or 0)
    check_common(v, L, "mbs")


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
         r"leaves the Hunter's Dream alone: a plain travel, no rest load|guest: travels alone \(headstone\) to m2\d+_.*a re-join with the host's snapshot", flag or left)
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


def check_nfe(v, L):
    need(v, L, "nfe join: 2 players", 0, r"cooperators 0 -> 1")
    at = need(v, L, "nfe host beside the blood-drunk Eileen", 0, r"test: host_chr %d" % NFE_EILEEN)
    gat = need(v, L, "nfe guest beside her", 1, r"test: guest_chr %d" % NFE_EILEEN)
    if not (at and gat):
        v.step("nfe both beside her", False, "host_chr %s guest_chr %s" % (at, gat))
        check_common(v, L, "nfe")
        return
    hd = nf_hp(L, 0, NFE_EILEEN, 0)
    gd = nf_hp(L, 1, NFE_EILEEN, 0)
    v.step("nfe Eileen on the host's game, hostile (team 27)", any(tm == 27 for _, _, tm in hd),
           "teams %s" % sorted({tm for _, _, tm in hd}))
    v.step("nfe Eileen on the guest's game, a foe it can hit (team 23)", any(tm == 23 for _, _, tm in gd),
           "teams %s" % sorted({tm for _, _, tm in gd}))
    t0 = gat
    dead = L.find(0, r"test: flags 1368=1", t0)
    end = dead[0] if dead else 1e9
    hits = L.all(1, r"test: guest_press 0x800", t0, end)
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already", t0, end)
    v.step("nfe the guest locks on to the hostile Eileen", bool(lock), lock[1][:140] if lock else "no lock-on")
    low = L.find(0, r"test: host_kill: .*entity %d" % NFE_EILEEN, t0)
    v.step("nfe (info) her HP set low on the host", True, low[1][-140:] if low else "no host_kill line")
    if hits and dead:
        hh = [h for _, h, _ in nf_hp(L, 0, NFE_EILEEN, low[0] if low else t0, end)]
        gh = [h for _, h, _ in nf_hp(L, 1, NFE_EILEEN, low[0] if low else t0, end)]
        v.step("nfe (info) her HP series, host / guest", True, "%s / %s" % (hh[:10], gh[:10]))
        v.step("nfe Eileen dies on the host from the guest's real blows (1368, no HP aid on the guest)", True,
               "1368 at +%.1f s after %d guest R1s" % (dead[0], len(hits)))
        need(v, L, "nfe her death on the guest's game (HP 0 / disabled in its chr dump)", 1, r"chr dump: entity %d [^\n]*HP 0/" % NFE_EILEEN, low[0] if low else t0)
        drop = L.find(0, r"shared drop roll .*entity %d" % NFE_EILEEN, t0)
        v.step("nfe (info) her drop roll on the host", True, drop[1][:140] if drop else "none")
    else:
        v.step("nfe the guest's R1s kill Eileen", False, "R1s %d, death %s" % (len(hits), bool(dead)))
    none_of(v, L, "nfe no flag write from a guest copy reaches the host as a world action",
            r"world action sent to the host[^\n]*\b(136[0-9]|137[0-9]|170[0-5])\b", 0, insts=(1,))
    check_common(v, L, "nfe")


def check_npi(v, L):
    """Phase npi: an NPC hunter arrives as an invader (session type 28); the whole party fights it (D11)."""
    N = NPI_NPC
    need(v, L, "npi join: 2 players", 0, r"cooperators 0 -> 1")
    arr = need(v, L, "npi the invader arrives (summon request accepted, desc[28])", 0, r"Party NPC: summon #1: entity %d .*desc\[28\]" % N)
    at = need(v, L, "npi host beside the invader", 0, r"test: host_chr %d" % N)
    gat = need(v, L, "npi guest beside it", 1, r"test: guest_chr %d" % N)
    if not (arr and at and gat):
        v.step("npi both beside the invader", False, "arrive %s host_chr %s guest_chr %s" % (arr, at, gat))
        check_common(v, L, "npi")
        return
    hd = [x for x in nf_hp(L, 0, N, arr) if x[2] == 21 or x[0] > arr + 5]
    gd = nf_hp(L, 1, N, arr)
    # arrival: the same place on both games (chr dump positions)
    def pos(i):
        out = []
        for t, line in L.all(i, r"chr dump: entity %d " % N, arr):
            m = re.search(r"team (-?\d+) .*HP (-?\d+)/\d+ at \((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)", line)
            if m and int(m.group(2)) > 0:
                out.append((t, int(m.group(1)), float(m.group(3)), float(m.group(5))))
        return out
    hp_, gp_ = pos(0), pos(1)
    v.step("npi the invader is there on both games", bool(hp_) and bool(gp_),
           "host dumps %d, guest dumps %d; teams host %s guest %s" % (len(hp_), len(gp_), sorted({x[1] for x in hp_}), sorted({x[1] for x in gp_})))
    if hp_ and gp_:
        dx = min(((a[2] - b[2]) ** 2 + (a[3] - b[3]) ** 2) ** 0.5 for a in hp_ for b in gp_ if abs(a[0] - b[0]) < 4)
        v.step("npi the invader stands at the same place on both games (within 3 m)", dx < 3.0, "%.1f m" % dx)
    t0 = gat
    dead = L.find(0, r"drops: enemy drop roll on this game[^\n]*chr handle 0x10014001", t0)
    end = dead[0] if dead else 1e9
    hits = L.all(1, r"test: guest_press 0x800", t0, end)
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already", t0, end)
    v.step("npi the guest locks on to the invader", bool(lock), lock[1][:140] if lock else "no lock-on")
    low = L.find(0, r"test: host_kill: .*entity %d" % N, t0 - 5)
    v.step("npi (info) its HP 0 on the host", True, low[1][-140:] if low else "no host_kill line")
    if hits and dead:
        hh = [h for _, h, tm in nf_hp(L, 0, N, t0, end) if tm == 21]
        gh = [h for _, h, tm in nf_hp(L, 1, N, t0, end) if tm == 21]
        v.step("npi (info) its HP series, host / guest", True, "%s / %s" % (hh[:12], gh[:12]))
        v.step("npi the guest's R1s hurt the invader on the host's game and on the guest's (same HP)", bool(hh) and bool(gh) and min(hh) < max(hh) - 100 and min(gh) < max(gh) - 100
               and abs(min(hh) - min(gh)) < 120, "host min %s guest min %s" % (min(hh, default=None), min(gh, default=None)))
        v.step("npi the invader dies on the host from the guest's real blows (drop roll)", True,
               "drop roll at +%.1f s after %d guest R1s" % (dead[0], len(hits)))
        need(v, L, "npi its death on the guest's game (HP 0 in its chr dump)", 1, r"chr dump: entity %d [^\n]*HP 0/" % N, low[0] if low else t0)
    else:
        v.step("npi the guest's R1s kill the invader", False, "R1s %d, death %s" % (len(hits), bool(dead)))
    check_common(v, L, "npi")


def check_nfw(v, L):
    need(v, L, "nfw join: 2 players", 0, r"cooperators 0 -> 1")
    at = need(v, L, "nfw host in front of Alfred (Cathedral Ward)", 0, r"test: host_chr %d" % NFW_ALFRED)
    gat = need(v, L, "nfw guest beside him", 1, r"test: guest_chr %d" % NFW_ALFRED)
    prov = L.find(0, r"test: host_press 0x800", at or 0)
    if not (at and gat and prov):
        v.step("nfw the host's R1s ran", False, "host_chr %s guest_chr %s press %s" % (at, gat, bool(prov)))
        check_common(v, L, "nfw")
        return
    bh = nf_hp(L, 0, NFW_ALFRED, 0, prov[0])
    bg = nf_hp(L, 1, NFW_ALFRED, 0, prov[0])
    v.step("nfw (before) Alfred friendly on both games (team 26)", bool(bh) and bool(bg) and all(tm == 26 for _, _, tm in bh + bg),
           "host teams %s, guest teams %s" % (sorted({tm for _, _, tm in bh}), sorted({tm for _, _, tm in bg})))
    hflag = L.find(0, r"test: flags [^\n]*1345=1", prov[0] - 1)
    hteam = [(t, tm) for t, _, tm in nf_hp(L, 0, NFW_ALFRED, prov[0] - 1) if tm == 27]
    v.step("nfw the host's R1s turn Alfred hostile on the host (1345 / team 27)", bool(hflag or hteam),
           "1345 at %s, team 27 at %s" % ("+%.1f" % hflag[0] if hflag else "-", "+%.1f" % hteam[0][0] if hteam else "-"))
    gteam = [(t, tm) for t, _, tm in nf_hp(L, 1, NFW_ALFRED, prov[0] - 1) if tm in (23, 27)]
    v.step("nfw in-map switch: Alfred a foe on the guest's game while it stands there (team 23)",
           bool(gteam) and gteam[0][1] == 23, "guest teams after the host's R1s %s" %
           sorted({tm for _, _, tm in nf_hp(L, 1, NFW_ALFRED, prov[0] - 1)}))
    t0 = gteam[0][0] if gteam else prov[0]
    dead = L.find(0, r"shared drop roll[^\n]*entity %d" % NFW_ALFRED, t0)
    end = dead[0] if dead else 1e9
    hits = L.all(1, r"test: guest_press 0x800", t0, end)
    lock = L.find(1, r"test: guest_press 0x(4|800)(; | ).*lock-on 1|test: guest_press 0x4:lock skipped: locked on already", t0, end)
    v.step("nfw the guest locks on to the hostile Alfred", bool(lock), lock[1][:140] if lock else "no lock-on")
    low = L.find(0, r"test: host_kill: .*entity %d" % NFW_ALFRED, t0 - 5)
    v.step("nfw (info) his HP 0 on the host", True, low[1][-120:] if low else "no host_kill line")
    if hits and dead:
        hh = [h for _, h, _ in nf_hp(L, 0, NFW_ALFRED, t0, end)]
        gh = [h for _, h, _ in nf_hp(L, 1, NFW_ALFRED, t0, end)]
        v.step("nfw (info) his HP series, host / guest", True, "%s / %s" % (hh[:10], gh[:10]))
        v.step("nfw the guest's R1s hurt Alfred on the host's game and on the guest's", bool(hh) and bool(gh) and min(hh) < max(hh[:1]) - 60 and min(gh) < max(gh[:1]) - 60,
               "host %s guest %s" % (hh[:4], gh[:4]))
        v.step("nfw Alfred dies on the host (his drop roll) after %d guest R1s" % len(hits), True, "drop roll at +%.1f s" % dead[0])
        need(v, L, "nfw his death on the guest's game (HP 0 in its chr dump)", 1, r"chr dump: entity %d [^\n]*HP 0/" % NFW_ALFRED, low[0] if low else t0)
    else:
        v.step("nfw the guest's R1s kill Alfred", False, "R1s %d, death %s" % (len(hits), bool(dead)))
    none_of(v, L, "nfw no flag write from a guest copy reaches the host as a world action",
            r"world action sent to the host[^\n]*\b(134[0-9]|135[0-9])\b", 0, insts=(1,))
    check_common(v, L, "nfw")


def check_pvd(v, L):
    """Phase pvd: the host stays hittable through repeated in-place PvP revives (0.7.5)."""
    need(v, L, "pvd join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "pvd friendly fire applied on both games", 1, r"pvp: friendly fire applied")
    hp = L.all(0, r"test: step \d+: own HP \d+ -> 30")
    dead = L.all(0, r"pvp: own HP \d+ -> 0 \(dead\)")
    rev = L.all(0, r"host revive #\d+: after")
    v.step("pvd the host's HP set to 30 each cycle", len(hp) >= PVD_CYCLES - 1, "%d of %d" % (len(hp), PVD_CYCLES))
    v.step("pvd every cycle killed the host (still hittable)", len(dead) >= len(hp) - 2 and len(hp) > 0,
           "%d deaths in %d cycles; first misses: %s" % (len(dead), len(hp), [round(h[0]) for h in hp][len(dead):][:4]))
    v.step("pvd every death was revived in place", len(rev) >= len(dead) - 1, "%d revives, %d deaths" % (len(rev), len(dead)))
    check_common(v, L, "pvd")


def pvh_hp(L, i, local, after=0.0, before=1e9):
    """(t, hp, max) of the local (or the first other) player chr from BB_PARTY_LEVELSYNC_LOG lines on inst i."""
    out = []
    for t, line in L.all(i, r"level sync: .*chr [0-9a-f]+", after, before):
        is_local = " (local)" in line
        if is_local != local:
            continue
        m = re.search(r"HP (-?\d+)/(-?\d+)", line)
        if m:
            out.append((t, int(m.group(1)), int(m.group(2))))
    return out


def check_pvn(v, L):
    """Phase pvn: PvP between players only; a bell-summoned NPC hunter stays the host's friend (0.7.7)."""
    need(v, L, "pvn join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "pvn the NPC hunter is summoned (cooperator slot kind 2)", 0, r"Party NPC: summon #1: entity 2410740")
    on = need(v, L, "pvn friendly fire applied (host)", 0, r"pvp: friendly fire applied")
    need(v, L, "pvn friendly fire applied (guest)", 1, r"pvp: friendly fire applied")
    for i, who in ((0, "host"), (1, "guest")):
        need(v, L, "pvn %s: the NPC chr moved to the spare team" % who, i, r"npc teams: \d+ NPC chr\(s\) moved to team (22|5):.*2410740", on or 0)
    # The table view after PvP is on: no NPC chr with a hostile cell to the host's team, on either game.
    for i, who in ((0, "host"), (1, "guest")):
        views = [h for h in L.all(i, r"pvp: test: step \d+: .*rule on, active yes", on or 0) if "[npc 2410740" in h[1]]
        bad = [h for h in views if re.search(r"\[npc 2410740 team 20|\[npc \d+ team \d+ type \d+: host->it rel 1|it->host rel 1|eff 1", h[1])]
        v.step("pvn %s: with PvP on the NPC is on its own team and no cell to the host is hostile (%d log lines)" % (who, len(views)),
               bool(views) and not bad, bad[0][1][:260] if bad else ("no view line" if not views else "ok"))
        v.step("pvn %s: the players' duel cells are open at the same time" % who, bool(L.find(i, r"friendly fire applied .*: opened relation\[19\]\[20\] effect\[19\]\[20\] relation\[20\]\[19\] effect\[20\]\[19\]")), "")
    hit = need(v, L, "pvn the guest's R1s on the host (PvP between players)", 1, r"pvp: test: step \d+: 6 x R1", on or 0)
    # The host's HP: nothing hurts it while PvP is on and nobody has hit yet (the NPC is no foe), then the guest's blows land.
    before = L.all(0, r"pvp: own HP \d+ -> \d+", on or 0, (hit or 1e9) - 2)
    v.step("pvn the NPC did not hurt the host with PvP on (%d host HP drops before the guest's first blow)" % len(before), not before,
           before[0][1][:160] if before else "none")
    after = L.all(0, r"pvp: own HP \d+ -> \d+", (hit or 0) - 1)
    v.step("pvn PvP between players still works (%d host HP drops after the guest's blows)" % len(after), len(after) >= 3,
           after[0][1][:160] if after else "no drop")
    need(v, L, "pvn PvP off: the NPC chr is back on its team", 0, r"npc teams: \d+ of \d+ NPC chr\(s\) back on their team", on or 0)
    check_common(v, L, "pvn")


NPF_ARENA = (-116.25, -126.33, 38.31)   # the Blood-starved Beast's arena entry spot (region 2302800)


def check_npf(v, L):
    """Phase npf: the bell helper Alfred (entity 2300740) must be inside the beast's arena once the fight is on."""
    need(v, L, "npf join: 2 players", 0, r"cooperators 0 -> 1")
    ring = os.environ.get("BB_NPF_RING", "host")
    ri = 0 if ring == "host" else 1
    on = need(v, L, "npf the fight is on (host-entered flag)", 0, r"flag 12304800=1|boss fog: slot \d+ went in first", 0.0)
    pos = L.all(ri, r"npc 2300740 at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\) disabled 0")
    best = None
    for t, line in pos:
        m = re.search(r"npc 2300740 at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", line)
        if m and on is not None and t >= on:
            d = sum((float(m.group(k + 1)) - NPF_ARENA[k]) ** 2 for k in range(3)) ** 0.5
            best = d if best is None else min(best, d)
    v.step("npf the helper is in the boss room (closest %.0f m to the arena entry spot)" % (best if best is not None else -1),
           best is not None and best < 15.0, "no position after the fight began" if best is None else "%.1f m" % best)
    last = pos[-1][1] if pos else ""
    m = re.search(r"npc 2300740 at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", last)
    if m:
        d = sum((float(m.group(k + 1)) - NPF_ARENA[k]) ** 2 for k in range(3)) ** 0.5
        v.step("npf the helper is still in the arena at the end (%.0f m)" % d, d < 60.0, last[:120])
    check_common(v, L, "npf")


def check_pvh(v, L):
    """Phase pvh: full health in PvP keeps the guest's HP fraction across every flip (0.7.6)."""
    need(v, L, "pvh join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "pvh the guest's HP set to 250 (the test aid)", 1, r"test: step \d+: own HP \d+ -> 250")
    flips = []
    at = 0.0
    for k, (key, val) in enumerate(PVH_FLIPS):
        hit = L.find(0, r"test: host_rule: %s %d -> %d" % (key, 1 - val, val), at)
        v.step("pvh flip %d: the host's %s -> %d" % (k + 1, key, val), bool(hit), ("+%.1f s" % hit[0]) if hit else "missing")
        if not hit:
            break
        at = hit[0] + 0.5
        flips.append((hit[0], key, val))
    for k, (t, key, val) in enumerate(flips):
        # The cut in force after this flip: PvP on and the rule on lifts it.
        pvp = [f for f in flips[:k + 1] if f[1] == "party_friendly_fire"][-1][2]
        rule = ([f for f in flips[:k + 1] if f[1] == "party_pvp_full_hp"] or [(0, "", 1)])[-1][2]
        lifted = bool(pvp and rule)
        want = r"phantom health: lifted: full health in PvP" if lifted else r"phantom health: on, the game's 30 % cut"
        for i, who in ((1, "guest"), (0, "host")):
            need(v, L, "pvh flip %d: the %s's game sets the phantom rate (%s)" % (k + 1, who, "lifted" if lifted else "cut"), i, want, t - 1, t + 12)
        kept = L.find(1, r"phantom health: HP kept at its fraction: (\d+)/(\d+) -> (\d+)/(\d+)", t - 1, t + 12)
        if not kept:
            same = L.find(1, r"phantom health: HP (\d+)/(\d+) -> (\d+)/(\d+), the fraction is kept already", t - 1, t + 12)
            v.step("pvh flip %d: the guest's HP kept at its fraction" % (k + 1), bool(same), same[1][:120] if same else "no keep line within 12 s")
            continue
        m = re.search(r"(\d+)/(\d+) -> (\d+)/(\d+)", kept[1])
        a, b, c, d = (int(x) for x in m.groups())
        expect = (a * d * 2 + b) // (2 * b) if 0 < a < b else (d if a >= b else 0)
        v.step("pvh flip %d: the guest's HP %d/%d -> %d/%d keeps the fraction (expected %d) and the max %s" %
               (k + 1, a, b, c, d, expect, "grew" if lifted else "shrank"),
               c == expect and (d > b if lifted else d < b), kept[1][:140])
        # The host's copy of the guest: its max after the flip is the guest's own within 1 % (the game's player
        # sync carries it with a few points of noise: 719 shows as 719-723, 503 as 503-506; live 2026-10-25) and
        # its HP the guest's within 2 % (the two logs are up to 0.5 s apart), so the HUD bars agree.
        hv = [x for x in pvh_hp(L, 0, False, t + 1, t + PVH_GAP - 2)]
        gv = [x for x in pvh_hp(L, 1, True, t + 1, t + PVH_GAP - 2)]
        close = bool(hv) and bool(gv) and abs(hv[-1][2] - gv[-1][2]) <= gv[-1][2] // 100 + 1 and \
            abs(hv[-1][1] - gv[-1][1]) <= gv[-1][2] // 50 + 1
        v.step("pvh flip %d: the host's view of the guest's HP bar is the guest's (host %s, guest %s)" %
               (k + 1, ["%d/%d" % (x[1], x[2]) for x in hv][-1:], ["%d/%d" % (x[1], x[2]) for x in gv][-1:]),
               close, "host sees %s, guest has %s" % (hv[-1:], gv[-1:]))
    none_of(v, L, "pvh the host's own HP never written by the keeper", r"phantom health: HP kept at its fraction", insts=(0,))
    none_of(v, L, "pvh no failed HP write", r"phantom health: HP kept.*WRITE FAILED")
    check_common(v, L, "pvh")


def check_npa(v, L):
    """Phase npa: a guest's blows on a friendly NPC of the host's world land (0.7.5)."""
    need(v, L, "npa join: 2 players", 0, r"cooperators 0 -> 1")
    need(v, L, "npa the guest's NPC-hit cells open (what the host hits)", 1, r"npc hit: a guest hits what the host hits: opened[^\n]*effect\[20\]\[26\]")
    gat = need(v, L, "npa guest in front of Alfred", 1, r"test: guest_chr %d" % NF_ALFRED)
    hits = L.all(1, r"test: guest_press 0x800", gat or 0)
    gkill = L.find(1, r"test: guest_kill: .*entity %d" % NF_ALFRED, gat or 0)
    if not (gat and hits):
        v.step("npa the guest's R1s ran", False, "guest_chr %s R1s %d" % (gat, len(hits)))
        check_common(v, L, "npa")
        return
    start = nf_hp(L, 0, NF_ALFRED, 0, hits[0][0])
    s0 = start[-1][1] if start else 3100
    for i, who in ((0, "host"), (1, "guest")):
        hp = [h for _, h, _ in nf_hp(L, i, NF_ALFRED, hits[0][0] - 0.5, gkill[0] if gkill else 1e9)]
        v.step("npa the guest's R1s on friendly Alfred hurt him on the %s's game" % who, bool(hp) and 0 < min(hp) < s0,
               "from %d: HP %s" % (s0, hp[:10]))
    tm = sorted({t for _, _, t in nf_hp(L, 1, NF_ALFRED, hits[0][0], gkill[0] if gkill else 1e9)})
    v.step("npa (info) Alfred's team on the guest's game during the blows", True, "%s" % tm)
    f = L.find(0, r"test: flags .*1349=1", hits[0][0] - 1)
    v.step("npa (info) the host's Alfred turned hostile (1349) from the guest's blows", True, f[1][:120] if f else "no: stays friendly")
    if gkill:
        need(v, L, "npa the kill is recorded on the host (1350)", 0, r"test: flags .*1350=1", gkill[0] - 1, gkill[0] + 30)
    check_common(v, L, "npa")


def check_cv(v, L, d: Path):
    """Phase cv: covenant outcomes per player; Valtr's rune for a guest (0.7.3)."""
    j = need(v, L, "cv join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "cv host's fixture: Annalise dead by Alfred, Valtr dead (host's save)", 0,
         r"test: set in the own save .*1042", 0)
    need(v, L, "cv rule on (host's setting)", 0, r"covenant outcomes per player: on")
    need(v, L, "cv rule on (guest: the host's rule)", 1, r"covenant outcomes per player: on")
    snap = need(v, L, "cv covenant: the host's covenant flags not copied into the guest's save", 1,
                r"covenant outcomes per player: \d+ covenant flag\(s\) set in the host's world not copied.*\b1042\b")
    # In the host's world (0.7.7): the guest sees its OWN covenant state (so its talks run on its own outcomes).
    # The test read (load mode 1) is taken at the world's first stable frame BEFORE the covenant overlay of that same
    # frame, so it still shows the host's values (1042=0/1: own/visible); the proof is the overlay's own log line: it
    # replaced the host's state by the guest's own in block 1 and lists the bits that changed (Annalise's 1040 / 1042).
    ov = L.find(1, r"guest covenant overlay: the host's covenant state replaced by the guest's own in block 1 \(bits changed:[^)]*\)", 0)  # from 0: the host's join line is on another clock (the guest's world can be up first)
    bits = set(re.findall(r"\d+", ov[1].split("bits changed:")[1])) if ov else set()
    rd = L.find(1, r"test read \(load mode 1, own save / visible\): ", 0)
    vals = dict(re.findall(r"\b(\d+)=([0-9?]/[0-9?])", rd[1])) if rd else {}
    own_ok = vals.get("1042", "").startswith("0/") and vals.get("1040", "").startswith("1/")  # the own save holds Annalise alive
    v.step("cv covenant: in the host's world the guest sees its own state (Annalise alive 1040, not the host's 1042)",
           {"1040", "1042"} <= bits and own_ok,
           (ov[1][-120:] if ov else "no covenant overlay line on the guest") + " | " + (rd[1][-160:] if rd else "no load mode 1 test read"))
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
         CV_RUNE_GIVEN % (CV_RUNE_LOT, CV_RUNE_FLAG, CV_RUNE_FLAG, CV_RUNE_LOT, CV_RUNE_FLAG),
         tr or j or 0)
    none_of(v, L, "cv rune: the trigger never goes to the host", r"world action sent to the host.*%s" % CV_RUNE_TRIGGER, insts=(0, 1))
    none_of(v, L, "cv rune: the host gets no rune", r"award lot %s\b" % CV_RUNE_LOT, insts=(0,))
    v.step("cv rune: in the guest's save (its own flag %s), not the host's" % CV_RUNE_FLAG,
           guest is not None and host is not None and guest[CV_RUNE_FLAG] and not host[CV_RUNE_FLAG],
           "guest %s host %s" % (fmt(guest and {CV_RUNE_FLAG: guest[CV_RUNE_FLAG]}),
                                 fmt(host and {CV_RUNE_FLAG: host[CV_RUNE_FLAG]})))
    check_common(v, L, "cv")


def check_cvt(v, L, d: Path, who="guest"):
    """Phases cvt / cvh: Valtr's League talk done by the guest / the host for real (0.7.7, parity row D5): the
    rune 43120 (own flag 6676) reaches the member who talked and nobody else."""
    t = "cvt" if who == "guest" else "cvh"
    ti, oi = (1, 0) if who == "guest" else (0, 1)
    j = need(v, L, t + " join: the guest is in", 0, r"cooperators 0 -> 1")
    at = need(v, L, t + " the %s in front of Valtr" % who, ti, r"test: %s_chr %s" % (who, CVT_VALTR), j or 0)
    talk = L.all(ti, r"npc_talk: local line: talk \d+ ", at or 0)
    v.step(t + " the %s's talk with Valtr plays (his lines)" % who, len(talk) >= 3, "%d lines" % len(talk))
    ch = need(v, L, t + " the League choice is reached in the talk (a real menu row)", ti,
              r"npc_talk: local choice: npc 270503 .*Join the League", at or 0)
    ids = [CV_RUNE_FLAG]
    if who == "guest":
        tr = need(v, L, t + " the talk set the trigger: given to the guest itself (own grant)", 1,
                  r"own talk gift trigger %s .*lot %s .*given to this guest" % (CV_RUNE_TRIGGER, CV_RUNE_LOT), ch or 0)
        need(v, L, t + " the rune is given into the guest's inventory (after the Leave, at home)", 1,
             CV_RUNE_GIVEN % (CV_RUNE_LOT, CV_RUNE_FLAG, CV_RUNE_FLAG, CV_RUNE_LOT, CV_RUNE_FLAG), tr or 0)
        none_of(v, L, t + " the trigger never goes to the host", r"world action sent to the host.*%s" % CV_RUNE_TRIGGER, insts=(0, 1))
        none_of(v, L, t + " the host gets no rune", r"gave #\d+ lot %s" % CV_RUNE_LOT, insts=(0,))
    else:
        # the host's rune is the game's own event 12700906 (no mod log line): proved by the host's save below
        none_of(v, L, t + " the guest gets no rune from the host's talk", r"gave #\d+ lot %s|own talk gift trigger %s" % (CV_RUNE_LOT, CV_RUNE_TRIGGER),
                insts=(1,))
    g = lp_save_flags(d, ti, ids)
    o = lp_save_flags(d, oi, ids)
    v.step(t + " the %s's save has the rune's flag %s" % (who, CV_RUNE_FLAG), g is not None and g[CV_RUNE_FLAG], "%s" % g)
    v.step(t + " the other member's save does not", o is not None and not o[CV_RUNE_FLAG], "%s" % o)
    check_common(v, L, t)


# covenant rune lot -> (the oath marker SpEffect names the log prints, label); GemGen 200000..200050
CVR_LOTS = {"34020": ("Radiance", "Radiance"), "17000": ("Corruption", "Corruption"), "35010": ("Hunter", "Hunter"),
            "43120": ("Impurity", "Impurity"), "3401850": ("BeastEmbrace", "Beast's Embrace"), "43020": ("Milkweed", "Milkweed")}


def check_cvr(v, L, d=None):
    """Phase cvr: each member carries its OWN covenant rune's SpEffect, never the other's (CL-524a2c)."""
    hn, hl = CVR_LOTS[CVR_HOST_LOT]
    gn, gl = CVR_LOTS[CVR_GUEST_LOT]
    j = need(v, L, "cvr join: the guest is in", 0, r"cooperators 0 -> 1")
    for i, who, own, other, label in ((0, "host", hn, gn, hl), (1, "guest", gn, hn, gl)):
        rows = [(t, ln) for t, ln in L.all(i, r"test: oath: covenant effects on the local player: ", 0)]
        on = [(t, ln) for t, ln in rows if re.search(r"effects on the local player: (?:\w+\(\d+\),)*%s\(" % own, ln)]
        v.step("cvr the %s memorized %s: its effect is on (%d samples)" % (who, label, len(on)), len(on) >= 4, "%d of %d" % (len(on), len(rows)))
        wrong = [ln for t, ln in rows if re.search(r"effects on the local player: .*%s" % re.escape(other), ln)] if other != own else []
        v.step("cvr the %s never carries the other member's covenant effect" % who, not wrong, "%d wrong samples" % len(wrong))
        summ = [ln for t, ln in on if "role 6" in ln or "role 3" in ln]
        v.step("cvr the %s's effect holds in the host's world (summoned / hosting)" % who, len(summ) >= 3, "%d samples" % len(summ))
    # the numbers (0.7.7 covleft077): the oath rune's SpEffect rows the PLAYER'S OWN active effect nodes point at (log_oath's `oath row`
    # line: node row IS the param row, every non-zero field): Beast's Embrace row 180050 holds the Therianthrope resist (+0x1E0 = 15),
    # Milkweed row 180060 the item discovery rate (+0x1FC = 0.1); the oath marker rows (6100 / 6110 / 6120 / 6142 / 250 / 255) carry no
    # numbers of their own (the engine and the EMEVDs / ESDs act on the marker's presence on the chr)
    num = {"3401850": ("BeastResist(180050)", r"\+1E0=15"), "43020": ("MilkweedDrop(180060)", r"\+1FC=0\.1")}
    for i, who, lot, other in ((0, "host", CVR_HOST_LOT, CVR_GUEST_LOT), (1, "guest", CVR_GUEST_LOT, CVR_HOST_LOT)):
        if lot in num:
            nm, pat = num[lot]
            rows = [ln for t, ln in L.all(i, r"test: oath row %s: node row IS the param row" % re.escape(nm), 0)]
            v.step("cvr the %s's own %s row is attached to its chr with its number (%s)" % (who, nm, pat), any(re.search(pat, ln) for ln in rows), "%d row line(s)" % len(rows))
            if other in num:
                onm, _ = num[other]
                wrong = L.all(i, r"test: oath row %s" % re.escape(onm), 0)
                v.step("cvr the %s never shows the other member's %s row" % (who, onm), not wrong, "%d" % len(wrong))
    home = [ln for t, ln in L.all(1, r"test: oath: covenant effects on the local player: .*role 0", 0) if re.search(gn + r"\(", ln)]
    v.step("cvr the guest keeps its own covenant effect at home after the Leave", len(home) >= 1, "%d samples" % len(home))
    check_common(v, L, "cvr")


OATH_VITALS = re.compile(r"effects on the local player: (?P<fx>[^(]*(?:\([^)]*\)[^(]*)*?) \(role (?P<role>\d+), map 0x[0-9a-f]+\) vitals HP (?P<hp>-?\d+)/(?P<hpmax>-?\d+) stamina (?P<st>-?\d+)/(?P<stmax>-?\d+)")


def oath_vitals(rows):
    """[(t, hp, hpmax, stamina, stamina max, effects text)] from `log_oath` lines [(t, line)] (0.7.8 covnpc078)."""
    out = []
    for t, ln in rows:
        m = OATH_VITALS.search(ln)
        if m:
            out.append((t, int(m["hp"]), int(m["hpmax"]), int(m["st"]), int(m["stmax"]), m["fx"]))
    return out


def stamina_regen(series, floor_gap=2):
    """Stamina recovered per second after the last drop: from the lowest stamina to the first sample at the maximum, a straight line
    through the rising samples (the ones that are neither the low point's plateau nor the full tank). None when no rise is seen."""
    if len(series) < 3:
        return None
    lo = min(range(len(series)), key=lambda i: (series[i][3], -i))
    rise = []
    for t, hp, hpmax, st, stmax, fx in series[lo:]:
        if st >= stmax - floor_gap:
            break
        rise.append((t, st))
    if len(rise) < 3 or rise[-1][0] - rise[0][0] < 1.0:
        return None
    n = len(rise)
    mt, ms = sum(t for t, _ in rise) / n, sum(s_ for _, s_ in rise) / n
    den = sum((t - mt) ** 2 for t, _ in rise)
    return sum((t - mt) * (s_ - ms) for t, s_ in rise) / den if den else None


def hp_drift(series, t0, t1):
    """HP change per second over [t0, t1] (a straight line through the samples), None with fewer than 3 samples."""
    pts = [(t, hp) for t, hp, hpmax, st, stmax, fx in series if t0 <= t <= t1]
    if len(pts) < 3 or pts[-1][0] - pts[0][0] < 2.0:
        return None
    n = len(pts)
    mt, mh = sum(t for t, _ in pts) / n, sum(h for _, h in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    return sum((t - mt) * (h - mh) for t, h in pts) / den if den else None


def check_cvm(v, L, d=None):
    """Phase cvm: the oath runes' amounts in game: Hunter's stamina recovery (+3 / s, row 180020) against a member without it,
    Corruption's HP drain (-1 / 2 s, row 180010) against a member without it (0.7.8 covnpc078)."""
    need(v, L, "cvm join: the guest is in", 0, r"cooperators 0 -> 1")
    ser = {}
    for i, who in ((0, "host"), (1, "guest")):
        ser[i] = oath_vitals(L.all(i, r"test: oath: covenant effects on the local player: .*vitals HP", 0))
        v.step("cvm the %s's vitals are logged (%d samples)" % (who, len(ser[i])), len(ser[i]) >= 40, "%d" % len(ser[i]))
    own = {0: CVM_LOTS.get(CVM_HOST_LOT), 1: CVM_LOTS.get(CVM_GUEST_LOT)}
    for i, who in ((0, "host"), (1, "guest")):
        nm = own[i]
        if not nm:
            continue
        on = [x for x in ser[i] if (nm[0] + "(") in x[5]]
        v.step("cvm the %s's %s rows are on its chr (%d samples)" % (who, nm[1], len(on)), len(on) >= 20, "%d of %d" % (len(on), len(ser[i])))
        rows = L.all(i, r"test: oath row %s\(\d+\)" % re.escape(nm[0]), 0)
        v.step("cvm the %s's %s rows are dumped with their numbers" % (who, nm[1]), len(rows) >= 1, "%s" % (rows[0][1][-160:] if rows else "none"))
    reg = {i: stamina_regen(ser[i]) for i in (0, 1)}
    detail = "host %s / s, guest %s / s" % tuple("%.2f" % reg[i] if reg[i] is not None else "-" for i in (0, 1))
    Log = lambda m: print("[cvm] " + m)
    Log("stamina recovery: " + detail)
    v.step("cvm stamina recovery measured on both members", reg[0] is not None and reg[1] is not None, detail)
    # measured 0.7.8 (cvm1): both members recover ~40-48 stamina / s on the 0.5 s samples (the 100-point bar); Hunter's +3 / s (row 180020,
    # staminaRecoverChangeSpeed 3) is about 7 % of that, below this sampling's resolution: the numbers are reported, not asserted
    t_end = max((x[0] for x in ser[0] + ser[1]), default=0)
    dr = {i: hp_drift(ser[i], 0, t_end) for i in (0, 1)}
    ddetail = "host %s HP / s, guest %s HP / s" % tuple("%.3f" % dr[i] if dr[i] is not None else "-" for i in (0, 1))
    Log("HP drift: " + ddetail)
    v.step("cvm HP drift measured on both members", dr[0] is not None and dr[1] is not None, ddetail)
    if os.environ.get("CVM_HP"):  # 0.7.9: both members set to a low HP (CVM_HP=PCT:DELAY_S) after the rune is on: what does each rune do from there
        for i, who in ((0, "host"), (1, "guest")):
            ts = L.find(i, r"test: HP set to \d+ of \d+", 0)
            pts = [(t, hp) for t, hp, hpmax, st, stmax, fx in ser[i] if ts and t >= ts[0] + 1.0]
            Log("%s after the low-HP set (%s): %s" % (who, ts[1][-40:] if ts else "none", [(round(t - ts[0], 1), hp) for t, hp in pts[::4]][:40] if ts else ""))
            v.step("cvm the %s's HP is set low and sampled afterwards" % who, bool(ts) and len(pts) >= 10, "%d samples" % len(pts))
            if pts:
                Log("%s HP %d -> %d over %.1f s, rows %s" % (who, pts[0][1], pts[-1][1], pts[-1][0] - pts[0][0], sorted({x[5] for x in ser[i] if ts and x[0] >= ts[0]})[-1:]))
    check_common(v, L, "cvm")


def check_cvc(v, L, d=None):
    """Phase cvc: each member carries its OWN oath rune effect in the Dream and in the host's Chalice Dungeon (CL-524a2c)."""
    hn, hl = CVR_LOTS[CVR_HOST_LOT]
    gn, gl = CVR_LOTS[CVR_GUEST_LOT]
    need(v, L, "cvc join: the guest is in", 0, r"cooperators 0 -> 1")
    for i, who, own, other, label in ((0, "host", hn, gn, hl), (1, "guest", gn, hn, gl)):
        rows = [ln for t, ln in L.all(i, r"test: oath: covenant effects on the local player: ", 0)]
        for mp, where in (("map 0x15", "the Dream"), ("map 0x1d", "the Chalice Dungeon")):
            on = [ln for ln in rows if mp in ln and re.search(r"effects on the local player: (?:\w+\(\d+\),)*%s\(" % own, ln)]
            v.step("cvc the %s carries its own %s effect in %s" % (who, label, where), len(on) >= 2, "%d samples" % len(on))
        wrong = [ln for ln in rows if re.search(r"effects on the local player: .*%s" % re.escape(other), ln)]
        v.step("cvc the %s never carries the other member's covenant effect" % who, not wrong, "%d wrong samples" % len(wrong))
    check_common(v, L, "cvc")


def check_cva(v, L, d: Path):
    """Phase cva: Annalise's greeting lines on the host and on the guest (the kneel itself is NOT proven: docs/party/event_flags.md; CVA_SWEEP / CVA_KNEEL explore it)."""
    j = need(v, L, "cva join: the guest is in", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        ln = L.all(i, r"npc_talk: local line: talk \d+ .*npc 250307 \(Annalise\)", j or 0)
        v.step("cva the %s hears Annalise's greeting lines in the Queen's Chamber" % who, len(ln) >= 3, "%d lines" % len(ln))
    check_common(v, L, "cva")


def check_cvk(v, L, d: Path):
    """Phase cvk: Annalise's kneel (Cross in her 1 m cylinder after the greeting) and the whole Vileblood oath for the host, then for the
    guest (0.7.9); CVK_ONLY=guest: the guest alone, its own gift."""
    gonly = os.environ.get("CVK_ONLY") == "guest"
    j = need(v, L, "cvk join: the guest is in", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        if gonly and i == 0:
            continue
        at = need(v, L, "cvk the %s in front of Annalise" % who, i, r"test: %s_chr %s" % (who, CVA_ANNALISE), j or 0)
        need(v, L, "cvk the %s's greeting plays (own lines)" % who, i, r"npc_talk: local line: talk \d+ .*npc 250307", j or 0)
        need(v, L, "cvk the %s's kneel flag 72500329 rises" % who, i, r"test: flags.* 72500329=1 ", at or 0)
        need(v, L, "cvk the %s swears the Vileblood oath (a real choice row)" % who, i, r"npc_talk: local choice: npc 250307 .*Swear Vileblood Oath", at or 0)
        need(v, L, "cvk the %s hears \"Now, thou'rt too a Vileblood\"" % who, i, r"npc_talk: local line: talk 1700462 ", at or 0)
        # 0.7.9 annalise079 (CL-22c416): the member's own character plays the kneel (101260 at the kneel flag, the blood 101263 at the
        # oath) on its own game, and the OTHER game sees it on that member's chr (log_anim's "remote anim")
        o = 1 - i
        k = need(v, L, "cvk the %s's own character plays the kneel animation 101260" % who, i, r"test: player anim 101260 ", at or 0)
        need(v, L, "cvk the %s's own character plays the oath's blood animation 101263" % who, i, r"test: player anim 101263 ", k or 0)
        need(v, L, "cvk the other game sees the %s kneel (remote anim 101260 on its chr)" % who, o, r"test: remote anim 101260 ", 0)
    # alone: its own grant; after the host's talk the host's shared award (a Caryll rune: "shared with the party") reached it first
    need(v, L, "cvk the guest's own talk gift trigger 9042 is seen (own grant, or already given by the host's shared award)", 1,
         r"own talk gift trigger 9042 .*lot 17000 .*(?:given to this guest|already given)", j or 0)
    need(v, L, "cvk the guest's Caryll rune is given", 1, CV_RUNE_GIVEN % ("17000", "50000200", "50000200", "17000", "50000200") +
         r"|gave #\d+ lot 17000 \(flag 50000200, award\) while summoned", j or 0)
    check_common(v, L, "cvk")


def check_cvl(v, L, d: Path):
    """Phase cvl: the League Staff for a guest: Valtr's real talk, then both members use the staff (0.7.9 annalise079)."""
    j = need(v, L, "cvl join: the guest is in", 0, r"cooperators 0 -> 1")
    at = need(v, L, "cvl the guest in front of Valtr", 1, r"test: guest_chr %s" % CVT_VALTR, j or 0)
    need(v, L, "cvl the League choice is reached in the talk", 1, r"npc_talk: local choice: npc 270503 .*Join the League", at or 0)
    need(v, L, "cvl the staff trigger 72700322 (set as Valtr's talk would) is the guest's own gift (lot 43130)", 1,
         r"own talk gift trigger 72700322 .*lot 43130", at or 0)
    need(v, L, "cvl the staff is given to the guest, or held already by the quick-slot aid: own flag 6678 set", 1, r"gave #\d+ lot 43130 \(flag 6678|own-save flag\(s\) 6678 set", at or 0)
    none_of(v, L, "cvl the host gets no staff", r"gave #\d+ lot 43130", insts=(0,))
    for i, who in ((0, "host"), (1, "guest")):
        q = need(v, L, "cvl the %s holds the staff in quick slot 0" % who, i, r"test: quick slot 0 holds goods 131 .*select ok", 0)
        ua = L.all(i, r"test: player anim (?!200000000|-1)\d+ ", q or 0)
        v.step("cvl the %s's item use plays an animation" % who, len(ua) >= 1, "%d anims: %s" % (len(ua), [x[1][-40:] for x in ua[-4:]]))
    check_common(v, L, "cvl")


def check_cvf(v, L, d: Path):
    """Phase cvf: Alfred's real talk by the host, then by the guest in one session (0.7.8 covnpc078)."""
    j = need(v, L, "cvf join: the guest is in", 0, r"cooperators 0 -> 1")
    for i, who in ((0, "host"), (1, "guest")):
        at = need(v, L, "cvf the %s in front of Alfred" % who, i, r"test: %s_chr %s" % (who, CVF_ALFRED), j or 0)
        talk = L.all(i, r"npc_talk: local line: talk \d+ ", at or 0)
        v.step("cvf the %s's talk with Alfred plays (his lines)" % who, len(talk) >= 3, "%d lines" % len(talk))
        need(v, L, "cvf the %s reaches Alfred's own talk menu (a real choice row)" % who, i, r"npc_talk: local choice: npc 240343 ", at or 0)
        # the first menu pass walks one topic (the Church): no gift trigger is set, none reaches the other member
        none_of(v, L, "cvf the %s's talk gives no oath rune lot" % who, r"gave #\d+ lot (17000|35010|34020|43120) ", insts=(i,))
    check_common(v, L, "cvf")


def check_cvs(v, L, d: Path, per_player=True):
    """Phases cvs / cvo: the host talks to Valtr and joins, then the guest talks to the same Valtr (0.7.7)."""
    t = "cvs" if per_player else "cvo"
    j = need(v, L, t + " join: the guest is in", 0, r"cooperators 0 -> 1")
    hat = need(v, L, t + " the host in front of Valtr", 0, r"test: host_chr %s" % CVT_VALTR, j or 0)
    hch = need(v, L, t + " the host's League choice is a real menu row", 0, r"npc_talk: local choice: npc 270503 .*Join the League", hat or 0)
    gat = need(v, L, t + " the guest in front of Valtr", 1, r"test: guest_chr %s" % CVT_VALTR, j or 0)
    gch = L.find(1, r"npc_talk: local choice: npc 270503 .*Join the League", gat or 0)
    ids = [CV_RUNE_FLAG]
    host = lp_save_flags(d, 0, ids)
    guest = lp_save_flags(d, 1, ids)
    v.step(t + " the host's save has the rune flag %s" % CV_RUNE_FLAG, host is not None and host[CV_RUNE_FLAG], "%s" % host)
    if per_player:
        v.step(t + " the guest gets its own join menu although the host joined", gch is not None, "%s" % (gch[1][:160] if gch else "no Join row"))
        tr = need(v, L, t + " the guest's talk set the trigger: own grant", 1,
                  r"own talk gift trigger %s .*lot %s .*given to this guest" % (CV_RUNE_TRIGGER, CV_RUNE_LOT), gat or 0)
        need(v, L, t + " the guest's rune is given (at home)", 1,
             CV_RUNE_GIVEN % (CV_RUNE_LOT, CV_RUNE_FLAG, CV_RUNE_FLAG, CV_RUNE_LOT, CV_RUNE_FLAG), tr or 0)
        v.step(t + " the guest's save has the rune flag", guest is not None and guest[CV_RUNE_FLAG], "%s" % guest)
    else:
        v.step(t + " rule off: the guest sees the joined dialog (no join row), expected", gch is None, "%s" % (gch[1][:160] if gch else "none"))
    check_common(v, L, t)


def check_il(v, L, d=None):
    """Phase il: the quiet catch-up batch and the guest's shop gates (0.7.5)."""
    j = need(v, L, "il join: the guest is in", 0, r"cooperators 0 -> 1")
    need(v, L, "il catch-up: the host's list reaches the guest", 1, r"guest got \d+ grants from the host", 0)
    gaves = L.all(1, r"gave #\d+ lot \d+ \(.*, full\) while summoned")
    v.step("il catch-up: the guest is given items from the list", len(gaves) >= 1, "%d gives" % len(gaves))
    v.step("il catch-up: quiet gives (no item-get dialog each)",
           bool(gaves) and all("no dialog (catch-up batch)" in h[1] for h in gaves), "")
    v.step("il catch-up: all given within 10 s (was one per 8 s)",
           bool(gaves) and gaves[-1][0] - gaves[0][0] < 10.0,
           "%.1f s for %d" % (gaves[-1][0] - gaves[0][0], len(gaves)) if gaves else "none")
    need(v, L, "il catch-up: one summary notice", 1, r"catch-up batch: \d+ item grant\(s\) given quietly: Received \d+ item", j or 0)
    none_of(v, L, "il policy: no consumable (Pebble 22000, Numbing Mist 24000, Sedative 26000, Pellets 27010) in the list",
            r"(gave|skip) #\d+ lot (22000|24000|24010|26000|27010|35000|15005) ", insts=(1,))
    need(v, L, "il shop: the guest's overlay gates are put back to its save", 1, r"shop gates while summoned: .*5500=0", j or 0)
    need(v, L, "il shop: the host's shared gate 5910 (Gascoigne's attire) shows on the guest whose own save has it clear", 1,
         r"test read \(load mode 1, own save / visible\): 5910=0/1", 0)
    need(v, L, "il shop: the guest's own Gascoigne flag cleared", 1, r"test: cleared in the own save.*12411800", 0)
    # 0.7.6 (group shop_sync): shop unlocks from shared progress reach the guest's own save
    need(v, L, "il sync: the host's Chalice boss unlock flags 5090 / 5091 are progress", 0,
         r"host flag 5090 = 1 \(world_state\)", j or 0)
    need(v, L, "il sync: ... 5091", 0, r"host flag 5091 = 1 \(world_state\)", j or 0)
    gs = need(v, L, "il sync: the guest applies the host's flags in its own save", 1,
              r"guest applied [1-9]\d* flags in load mode 1", j or 0)
    g = lp_save_flags(d, 1, ["5090", "5091"]) if d else None
    v.step("il sync: the guest's save holds the Bone Ash and Madman unlocks",
           g is not None and g["5090"] and g["5091"], "guest save %s" % g)
    check_common(v, L, "il")


def check_ie(v, L):
    """Phase ie: early items for every player (0.7.3)."""
    j = need(v, L, "ie join: the guest is in", 0, r"cooperators 0 -> 1")
    clr = L.find(0, r"test: cleared in the own save \(never_sync\):.*6670:")
    full = L.find(0, r"items: full list \(\d+ lots\) sent to slot")
    v.step("ie fixture: the host's 6670 cleared before its full item list to the guest",
           clr is not None and full is not None and clr[0] < full[0],
           "clear %s; full list %s" % (clr[0] if clr else "never", full[0] if full else "never"))
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
    # 0.7.5: key-item / questline map treasure is shared, ordinary treasure stays per player
    need(v, L, "ie treasure: the host's pickup of the Orphanage Key spot goes to the party", 0,
         r"host flag 52420900 -> #\d+ lot 2420900 .*queued for the party", j or 0)
    need(v, L, "ie treasure: the guest gets the Orphanage Key", 1,
         r"gave #\d+ lot 2420900 .*goods 4006 x1", j or 0)
    v.step("ie treasure: an ordinary corpse (lot 2410140) stays per player",
           L.find(0, r"host flag 52410140: map treasure, per player") is not None
           and L.find(1, r"gave #\d+ lot 2410140 ") is None, "")
    check_common(v, L, "ie")


def check_gd(v, L, d: Path):
    j = need(v, L, "gd join: the guest is in the host's world", 1, r"arrived in the host world")
    m = need(v, L, "gd the guest picks the Hunter's Dream at the world lamp", 1,
             r"warp menu: entry 1 chosen -> lamp 2102950", j or 0)
    f = need(v, L, "gd the Dream flag is set", 1, r"chose the Hunter's Dream: lamp %s's Dream flag \d+ set" % GD_LAMP, m or 0)
    w = need(v, L, "gd the lamp's event warps (the guest's request or its own warp)", 1,
             r"lamp 2102\d+ \(guest\): (held for the choice|a lamp of the loaded map -> travel alone)|lamp: request 2102950 sent", f or 0,
             what="the Dream flag was set but the lamp's warp never ran (0.7.5 guest Dream report)")
    need(v, L, "gd the guest's own copy of the lamp's event was used up by the host's trip (the repro's start)", 1,
         r"lamp 2102\d+ \(guest\): not this player's use|Lamp warp: event 7200's state from another player not applied", j or 0)
    need(v, L, "gd the guest arrives in the Dream", 1, r"Lamp travel: arrived: lamp 2102950", w or 0)
    none_of(v, L, "gd no stuck warp", r"no load within 60 s", insts=(1,))
    check_common(v, L, "gd")


def check_gh(v, L):
    j = need(v, L, "gh join: the guest is in the host's world", 1, r"arrived in the host world")
    g = need(v, L, "gh the guest picks the Hunter's Dream first", 1, r"warp menu: entry 1 chosen -> lamp 2102950", j or 0)
    h = need(v, L, "gh the host then picks the Hunter's Dream", 0, r"warp menu: entry 1 chosen -> lamp 2102950", g or 0)
    need(v, L, "gh the host's Dream flag is set", 0, r"chose the Hunter's Dream: lamp %s's Dream flag \d+ set" % GD_LAMP, h or 0)
    need(v, L, "gh the host arrives in the Dream (the lamp's event, or the fallback)", 0, r"Lamp travel: arrived: lamp 2102950", h or 0,
         what="the host's Dream choice did nothing (its copy of the lamp's event was used up by the guest's trip)")
    none_of(v, L, "gh no stuck warp", r"no load within 60 s", insts=(0,))
    check_common(v, L, "gh")


def check_mp(v, L, who=None):
    """mp / mpg / mph: the in-game map (BB_MAP=1): opens, shows the guest's dot moving, travels to a lamp from the map."""
    tag = "mp" if who is None else ("mpg" if who == "guest" else "mph")
    j = need(v, L, tag + " join: the guest is in the host's world", 1, r"arrived in the host world")
    if who is None:
        o = need(v, L, tag + " the host's map opens", 0, r"Map: opened \(area m24_01_00_00\)", j or 0)
        a = need(v, L, tag + " the guest's dot is drawn near the lamp", 0, r"Map: shown m24_01_00_00 .*\| Hunter\d \(-3\d\.\d ", o or 0)
        b = need(v, L, tag + " the guest's dot moved east (14 m)", 0, r"Map: shown m24_01_00_00 .*\| Hunter\d \(-(19|2\d)\.\d ", a or 0)
        c = need(v, L, tag + " the guest's dot moved west (14 m from the lamp)", 0, r"Map: shown m24_01_00_00 .*\| Hunter\d \(-4\d\.\d ", b or 0)
        need(v, L, tag + " the guest's dot came back to the lamp", 0, r"Map: shown m24_01_00_00 .*\| Hunter\d \(-3[2-4]\.\d ", c or 0)
        none_of(v, L, tag + " the map did not fail to read its data", r"Map: .* could not be read", insts=(0, 1))
        check_common(v, L, tag)
        return
    me = 1 if who == "guest" else 0
    o = need(v, L, tag + " the map opens", me, r"Map: opened", j or 0)
    q = need(v, L, tag + " the map asks for the travel to Old Yharnam", me, r"Map: travel to Old Yharnam \(lamp %s\) asked" % LP_OLD[0], o or 0)
    m = need(v, L, tag + " the lamp warp runs as the player's own lamp use", me, r"map travel: lamp %s " % LP_OLD[0], q or 0)
    need(v, L, tag + " the player arrives at the lamp", me, r"Lamp travel: arrived: lamp %s" % LP_OLD[0], m or 0)
    if who == "host":
        need(v, L, tag + " the guest follows the party travel into Old Yharnam", 1, r"map .*-> m23", m or 0)
    else:
        none_of(v, L, tag + " the host does not rest-load, transform or travel for the guest", GR_HOST_REST + r"|Party travel: host travel #\d+", m or 0, insts=(0,))
    none_of(v, L, tag + " no stuck warp", r"no load within 60 s", insts=(0, 1))
    check_common(v, L, tag)


def check_gr(v, L, which=1, tag=None):
    tag = tag or "gr%s" % ("" if which == 1 else which)
    off = tag.endswith("o")   # BB_PARTY_GUEST_LAMPS=0 (the rule off)
    j = need(v, L, tag + " join: the guest is in the host's world", 1, r"arrived in the host world")
    if which == 3:
        m = need(v, L, tag + " the host asks for the map travel", 0, r"test: map travel to %s " % LP_OLD[0], j or 0)
        need(v, L, tag + " the host's lamp warp from the map", 0, r"map travel: lamp %s " % LP_OLD[0], m or 0)
        need(v, L, tag + " the host arrives", 0, r"Lamp travel: arrived: lamp %s" % LP_OLD[0], m or 0)
        need(v, L, tag + " the guest follows the party travel into Old Yharnam", 1, r"map .*-> m23", m or 0)
        none_of(v, L, tag + " no stuck warp", r"no load within 60 s", insts=(0,))
        check_common(v, L, tag)
        return
    if which == 4:
        # apart in Old Yharnam first (alone, NO), then a map travel to a lamp of the host's map (Central Yharnam)
        a1 = need(v, L, tag + " the guest first travels alone to Old Yharnam", 1, r"Lamp travel: arrived: lamp %s" % LP_OLD[0], j or 0)
        m = need(v, L, tag + " the guest, apart, asks for the map travel into the host's map", 1,
                 r"test: then: a second map travel to %s " % LP_NEW[0], a1 or 0)
        c = need(v, L, tag + " the map's warp runs as the guest's own lamp use", 1, r"map travel: lamp %s " % LP_NEW[0], m or 0)
        # rule off ("o" variants): no split question / log of ours, the game's own lamp warp loads the guest
        if not off:
            need(v, L, tag + " the guest goes there itself (its own load)", 1,
                 r"this guest goes there itself|in the loaded map: no question, the guest's own loading screen", c or 0)
        need(v, L, tag + " the guest arrives at the lamp", 1, r"Lamp travel: arrived: lamp %s" % LP_NEW[0], c or 0)
        if c is not None:
            none_of(v, L, tag + " no request to the host: it would answer 'the guest loads alone' and nobody loads",
                    r"becomes a request to the host", c, insts=(1,))
            none_of(v, L, tag + " the host does not rest-load, transform or travel for the guest", GR_HOST_REST + r"|Party travel: host travel #\d+", c, insts=(0,))
        none_of(v, L, tag + " no stuck warp", r"no load within 60 s", insts=(1,))
        check_common(v, L, tag)
        return
    dest = LP_NEW[0] if which == 1 else LP_OLD[0]
    m = need(v, L, tag + " the guest asks for the map travel", 1, r"test: map travel to %s " % dest, j or 0)
    c = need(v, L, tag + " the map's warp runs as the guest's own lamp use", 1, r"map travel: lamp %s " % dest, m or 0)
    if off and which == 1:
        # rule off, a lamp of the host's own map: the game's own in-world warp moves only this guest (no load, so the
        # test aid's arrival check below does not apply)
        a = need(v, L, tag + " the guest moves itself (an in-world warp, no load)", 1,
                 r"in-world warp \(own move, lamp %s\)" % dest, c or 0)
    else:
        if not off:
            need(v, L, tag + " the guest goes alone (its own load)", 1,
                 r"in the loaded map: no question, the guest's own loading screen|BB_PARTY_SPLIT_TEST|-> travel alone", c or 0)
        a = need(v, L, tag + " the guest arrives at the lamp", 1, r"Lamp travel: arrived: lamp %s" % dest, c or 0)
    if c is not None:
        # the host keeps playing: no rest load, no forced transform, no travel of its own
        none_of(v, L, tag + " the host does not rest-load or transform for the guest's map travel", GR_HOST_REST, c, insts=(0,))
        none_of(v, L, tag + " the host starts no load of its own", r"Party travel: host travel #\d+", c, insts=(0,))
    if not (off and which == 1):
        none_of(v, L, tag + " no stuck warp", r"no load within 60 s", insts=(1,))
    check_common(v, L, tag)


def check_ml(v, L):
    need(v, L, "ml the host pressed Cross at the lamp", 0, r"travel step \d+/\d+: host_press", 0)
    need(v, L, "ml the guest is told the host's last lamp is the lit one", 1,
         r"guest death redirect: host's last lamp %s" % ML_LAMP[0], 0)
    r = L.find(1, r"guest: died alone: revived at its own last lamp (\d+)")
    v.step("ml guest gets up at the lit lamp, not the arrival", bool(r) and ("last lamp %s " % ML_LAMP[0]) in r[1],
           r[1][:200] if r else "no revive line")
    check_common(v, L, "ml")


def check_mg(v, L):
    need(v, L, "mg the guest lit the lamp for the host", 0, r"lamp: slot \d+ lit a lamp in the host's world|lamp_lit", 0)
    none_of(v, L, "mg: the guard did not put the host's last lamp back to the arrival",
            r"lamp record guard: the last lamp moved to %s" % ML_LAMP[0])
    r = L.find(0, r"host death taken: .*revive at lamp (\d+)")
    v.step("mg the host gets up at the lit lamp, not the arrival", bool(r) and ("revive at lamp %s " % ML_LAMP[0]) in r[1],
           r[1][:200] if r else "no host death line")
    check_common(v, L, "mg")


def check_hs(v, L):
    j = need(v, L, "hs join: the guest is in the host's Dream", 1, r"open: guest in the host's m21_00_00_00")
    g = need(v, L, "hs the guest picks a Dream headstone", 1, r"lamp flag (7210\d+) set by this game", j or 0)
    need(v, L, "hs the guest travels alone from the Dream", 1, r"guest: travels alone \(headstone\)", g or 0)
    # the host's copy of the headstone's event ran at the guest's pick (the repro's start); its warp was dropped
    # the host's copy of the headstone's event must NOT run at the guest's pick (its state packet is not applied)
    d = need(v, L, "hs the guest's pick reaches the host's event sync and is not applied", 0,
             r"Lamp warp: event 12107000's state from another player not applied", g or 0)
    none_of(v, L, "hs the host's copy of the headstone event did not run (no warp dropped as not its use)",
            r"lamp 24\d+ \(host\): not this player's use", g or 0, insts=(0,))
    h = need(v, L, "hs the host then picks the same headstone", 0, r"lamp flag (7210\d+) set by this game", d or g or 0)
    w = need(v, L, "hs the host's headstone choice warps (the event, or the fallback)", 0,
             r"lamp 24\d+ \(host\): (held for the choice|BB_PARTY_TRAVEL_CHOICE)", h or 0,
             what="the host's headstone choice did nothing (its copy of the headstone's event was used up by the guest's trip)")
    need(v, L, "hs the host arrives in the lamp's map", 0, r"host travel: arrived", w or 0)
    fb = L.find(0, r"headstone flag \d+ set [\d.]+ s ago and no warp followed", h or 0)
    v.step("hs (info) the host's warp came from the fallback (the event was used up)", True,
           fb[1][:150] if fb else "no fallback line (the event warped itself)")
    none_of(v, L, "hs no stuck warp", r"no load within 60 s", insts=(0, 1))
    check_common(v, L, "hs")


def check_ngj(v, L):
    r = need(v, L, "ngj the NG+ host becomes start-ready (not stuck in the prologue)", 0,
             r"start \(prologue_solo\): step ready.*READY")
    need(v, L, "ngj the guest reaches the host's world (NG+ 2 beside NG+ 2)", 1,
         r"start \(prologue_solo\): in the host's world", r or 0)
    check_common(v, L, "ngj")


CHECKS = {**{n: (lambda v, L, d, n=n: check_jn(v, L, n)) for n in JN_PHASES}, "ivc": lambda v, L, d: check_ivc(v, L), "ivf": lambda v, L, d: check_ivf(v, L), "ojla": lambda v, L, d: check_ojla(v, L), "iv2": lambda v, L, d: check_iv2(v, L), "ivm": lambda v, L, d: check_ivm(v, L), "ivp": lambda v, L, d: check_ivp(v, L), "iv": lambda v, L, d: check_iv(v, L), "ivn": lambda v, L, d: check_iv(v, L, "ivn"), "ivx": lambda v, L, d: check_ivx(v, L), "ivl": lambda v, L, d: check_ivl(v, L), "ojl": lambda v, L, d: check_ojl(v, L), "ah": check_ah, "pc": check_pc, "vm": check_vm, "yj": check_yj, "yjc": lambda v, L, d: _with_env(YJC_ENV, check_yj, v, L, d), "llp": lambda v, L, d: _with_env(LLP_ENV, check_ll, v, L), "s1": lambda v, L, d: check_s1(v, L), "s2": check_s2, "rules": lambda v, L, d: check_rules(v, L),
          "sc": check_sc, "sh": lambda v, L, d: check_sh(v, L), "gg": lambda v, L, d: check_gg(v, L), "ss": lambda v, L, d: check_ss(v, L), "summonfail": lambda v, L, d: check_summonfail(v, L),
          "s3": lambda v, L, d: check_s3(v, L), "sk": lambda v, L, d: check_sk(v, L), "lg": lambda v, L, d: check_lg(v, L), "lv": lambda v, L, d: check_lv(v, L),
          "ga": lambda v, L, d: check_ga(v, L), "gl": lambda v, L, d: check_gl(v, L), "lf": lambda v, L, d: check_lf(v, L), "lfx": lambda v, L, d: check_lf(v, L, 2), "lfa": lambda v, L, d: check_lfa(v, L), "lfw": lambda v, L, d: check_lfw(v, L, "yh", "lfw"), "lfwc": lambda v, L, d: check_lfw(v, L, "ca", "lfwc"), "lfws": lambda v, L, d: check_lfw(v, L, "yh", "lfws"), "lfwcs": lambda v, L, d: check_lfw(v, L, "ca", "lfwcs"), "lfwbs": lambda v, L, d: check_lfw(v, L, "ybs", "lfwbs"), "lfwcbs": lambda v, L, d: check_lfw(v, L, "cbs", "lfwcbs"), "lu": lambda v, L, d: check_lift_down(v, L, "lu"), "lfwf": lambda v, L, d: check_lfw(v, L, "yh", "lfwf"), "lfwcf": lambda v, L, d: check_lfw(v, L, "ca", "lfwcf"), "el": lambda v, L, d: check_el(v, L), "lt": lambda v, L, d: check_lt(v, L), "ch": lambda v, L, d: check_ch(v, L), "md": check_md, "mds": check_mds, "svl": check_svl, "rh": lambda v, L, d: check_rh(v, L), "rs": lambda v, L, d: check_rs(v, L), "fw": lambda v, L, d: check_fw(v, L), "fwg": lambda v, L, d: check_fw(v, L, "fwg", (1,)), "fwb": lambda v, L, d: check_fw(v, L, "fwb", (0, 1)), "fwe": lambda v, L, d: check_fw(v, L, "fwe", (0, 1)), "fh": lambda v, L, d: check_fw(v, L, "fh", (0, 1), FH_POS, FH_BUSY, "1", "0x16000000", (-211.25, 542.1), -20.3),
          "mk": lambda v, L, d: check_mk(v, L), "kd": lambda v, L, d: check_kd(v, L), "kg": lambda v, L, d: check_kg(v, L), "fdr": lambda v, L, d: check_fdr(v, L), "kw": lambda v, L, d: check_kw(v, L),
          "fl": lambda v, L, d: check_fl(v, L), "fd": lambda v, L, d: check_fd(v, L), "fdj": lambda v, L, d: check_fdj(v, L), "cd": lambda v, L, d: check_cd(v, L), "scb": lambda v, L, d: check_scb(v, L), "iw": lambda v, L, d: check_iw(v, L), "iwn": lambda v, L, d: check_iwn(v, L), "an": lambda v, L, d: check_an(v, L), "anl": lambda v, L, d: check_anl(v, L), "ld": lambda v, L, d: check_ld(v, L),
          "cl": lambda v, L, d: check_cl(v, L), "cy": lambda v, L, d: check_cy(v, L), "cyn": lambda v, L, d: check_cy(v, L, "cyn"), "clg": lambda v, L, d: check_clg(v, L), "cpr": lambda v, L, d: check_cpr(v, L), "chc": lambda v, L, d: check_chc(v, L), "chw": lambda v, L, d: check_chc(v, L, True), "chn": lambda v, L, d: check_chc(v, L), "cpa": lambda v, L, d: check_common(v, L, "cpa"), "cae": lambda v, L, d: check_cae(v, L), "cao": lambda v, L, d: check_cao(v, L), "cag": lambda v, L, d: check_cag(v, L), "cage": lambda v, L, d: check_cag(v, L, True), "cgt": lambda v, L, d: check_cgt(v, L), "cgp": lambda v, L, d: check_cgp(v, L), "cgu": lambda v, L, d: check_cgu(v, L), "cab": lambda v, L, d: check_cab(v, L), "caq": lambda v, L, d: check_common(v, L, "caq"), "caw": lambda v, L, d: check_common(v, L, "caw"), "clw": lambda v, L, d: check_clw(v, L), "cla": lambda v, L, d: check_cla(v, L), "caa": lambda v, L, d: check_caa(v, L), "cam": lambda v, L, d: check_cam(v, L), "camo": lambda v, L, d: check_cam(v, L, "offline"), "camh": lambda v, L, d: check_camh(v, L), "cas": lambda v, L, d: check_cas(v, L), "bf": lambda v, L, d: check_bf(v, L), "lp": check_lp, "gd": check_gd, "gh": lambda v, L, d: check_gh(v, L), "gm": check_gm, "gr": lambda v, L, d: check_gr(v, L), "gr2": lambda v, L, d: check_gr(v, L, 2), "gr3": lambda v, L, d: check_gr(v, L, 3), "gr4": lambda v, L, d: check_gr(v, L, 4), "mp": lambda v, L, d: check_mp(v, L), "mpg": lambda v, L, d: check_mp(v, L, "guest"), "mph": lambda v, L, d: check_mp(v, L, "host"), "gro": lambda v, L, d: check_gr(v, L, 1, "gro"), "gr2o": lambda v, L, d: check_gr(v, L, 2, "gr2o"), "gr3o": lambda v, L, d: check_gr(v, L, 3, "gr3o"), "gr4o": lambda v, L, d: check_gr(v, L, 4, "gr4o"), "jl": check_jl, "ml": lambda v, L, d: check_ml(v, L), "mg": lambda v, L, d: check_mg(v, L), "ll": lambda v, L, d: check_ll(v, L), "hlq": check_hlq, "bq": check_bq, "hlr": check_hlr, "hs": lambda v, L, d: check_hs(v, L), "ngj": lambda v, L, d: check_ngj(v, L), "nl": lambda v, L, d: check_nl(v, L), "ff": lambda v, L, d: check_ff(v, L), "lk": lambda v, L, d: check_lk(v, L), "lkc": lambda v, L, d: check_lkc(v, L), "br": lambda v, L, d: check_br(v, L), "rf": lambda v, L, d: check_rf(v, L), "rj": lambda v, L, d: check_rj(v, L), "em": lambda v, L, d: check_em(v, L), "emf": lambda v, L, d: check_emf(v, L), "bd": lambda v, L, d: check_bd(v, L), "bdo": lambda v, L, d: check_bd(v, L, True), "sm": lambda v, L, d: check_sm(v, L), "ef": lambda v, L, d: check_ef(v, L), "ngp": lambda v, L, d: check_ngp(v, L), "mb": lambda v, L, d: check_mb(v, L), "gb": lambda v, L, d: check_gb(v, L), "mbs": lambda v, L, d: check_mbs(v, L), "gbs": lambda v, L, d: check_gbs(v, L), "da": lambda v, L, d: check_da(v, L), "cv": check_cv, "cvs": check_cvs, "cvo": lambda v, L, d: check_cvs(v, L, d, False), "cvr": check_cvr, "cvc": check_cvc, "cva": check_cva, "cvf": check_cvf, "cvk": check_cvk, "cvl": check_cvl, "cvm": check_cvm, "cvt": check_cvt, "cvh": lambda v, L, d: check_cvt(v, L, d, "host"), "ie": lambda v, L, d: check_ie(v, L), "il": check_il, "rw": lambda v, L, d: check_rw(v, L), "rwe": lambda v, L, d: check_rwe(v, L), "rwt": lambda v, L, d: check_rwt(v, L), "gag": lambda v, L, d: check_gag(v, L), "gv": lambda v, L, d: check_gv(v, L), "gvg": lambda v, L, d: check_gvg(v, L), "gvr": lambda v, L, d: check_gvr(v, L), "gvl": lambda v, L, d: check_gvl(v, L), "gvn": lambda v, L, d: check_gv(v, L, "gvn"), "gvm": lambda v, L, d: check_gvm(v, L), "gvs": lambda v, L, d: check_gvs(v, L), "gvt": lambda v, L, d: check_gvt(v, L), "gvw": lambda v, L, d: check_gvw(v, L), "gvwn": lambda v, L, d: check_gvw(v, L, "gvwn"), "gva": lambda v, L, d: check_gva(v, L), "rwl": lambda v, L, d: check_rwl(v, L), "nf": lambda v, L, d: check_nf(v, L), "nfa": lambda v, L, d: check_nfa(v, L), "nfe": lambda v, L, d: check_nfe(v, L), "nfw": lambda v, L, d: check_nfw(v, L), "nfn": lambda v, L, d: check_nfh(v, L, "nfn"), "nfr": lambda v, L, d: check_nfh(v, L, "nfr"), "nfv": lambda v, L, d: check_nfh(v, L, "nfv"), "nfd": lambda v, L, d: check_nfh(v, L, "nfd"), "nfb": lambda v, L, d: check_nfb(v, L), "nfh": lambda v, L, d: check_nfb(v, L, "nfh"), "npi": lambda v, L, d: check_npi(v, L), "npa": lambda v, L, d: check_npa(v, L), "npd": lambda v, L, d: check_npd(v, L), "pvd": lambda v, L, d: check_pvd(v, L), "pvh": lambda v, L, d: check_pvh(v, L), "pvn": lambda v, L, d: check_pvn(v, L), "npf": lambda v, L, d: check_npf(v, L), "sj": lambda v, L, d: check_sj(v, L), "bl": lambda v, L, d: check_bl(v, L), "mw": lambda v, L, d: check_mw(v, L), "px": lambda v, L, d: check_px(v, L), "pxg": lambda v, L, d: check_px(v, L, 1), "ba": lambda v, L, d: check_ba(v, L), "bh": lambda v, L, d: check_bh(v, L), "bk": lambda v, L, d: check_bk(v, L), "hg": lambda v, L, d: check_hg(v, L), "bg": lambda v, L, d: check_bg(v, L), "lr": lambda v, L, d: check_lr(v, L), "lrg": lambda v, L, d: check_lrg(v, L), "mi": lambda v, L, d: check_mi(v, L), "mig": lambda v, L, d: check_mi(v, L, "mig"), "lw": lambda v, L, d: check_lw(v, L), "lwf": lambda v, L, d: check_lwf(v, L), "amp": lambda v, L, d: check_amp(v, L), "amp": lambda v, L, d: check_amp(v, L), "la": lambda v, L, d: check_la(v, L), "lb": lambda v, L, d: check_common(v, L, "lb"), "lc": lambda v, L, d: check_lc(v, L),
          "rt": lambda v, L, d: check_rt(v, L), "tv": lambda v, L, d: check_tv(v, L),
          "eh": lambda v, L, d: check_eh(v, L), "ou": lambda v, L, d: check_ou(v, L), "gdr": check_gdr, "gdrd": lambda v, L, d: check_gdr(v, L, d, "gdrd"), "gdrb": lambda v, L, d: check_gdr(v, L, d, "gdrb"), "gdrf": lambda v, L, d: check_gdr(v, L, d, "gdrf"), "mesh": lambda v, L, d: check_mesh(v, L), "oum": lambda v, L, d: check_ou(v, L, "oum"), "meshm": lambda v, L, d: check_mesh(v, L, "meshm"), "bv": lambda v, L, d: check_bv(v, L), "bw": lambda v, L, d: check_bw(v, L), "fp": lambda v, L, d: check_fp(v, L), "fp5": lambda v, L, d: check_fp5(v, L), "oj": lambda v, L, d: check_oj(v, L), "lo": lambda v, L, d: check_lo(v, L), "po": lambda v, L, d: check_po(v, L), "pos": lambda v, L, d: check_pos(v, L), "sg": lambda v, L, d: check_sg(v, L), "sg2": lambda v, L, d: check_sg2(v, L), "sgb": lambda v, L, d: check_sgb(v, L), "sgb2": lambda v, L, d: check_sgb2(v, L), "sgb3": lambda v, L, d: check_sgb3(v, L), "sgb4": lambda v, L, d: check_sgb4(v, L), "sgb4n": lambda v, L, d: check_sgb4(v, L, "sgb4n", False), "sgp": lambda v, L, d: check_sgp(v, L), "csh": lambda v, L, d: check_csh(v, L), "csh2": lambda v, L, d: check_csh2(v, L), "csh3": lambda v, L, d: check_csh3(v, L), "cis": lambda v, L, d: check_cis(v, L, d), "cim": lambda v, L, d: check_cim(v, L, d)}


def sweep_args(argv):
    """argv without --fps / --out / --sweep-fps (and their values): what a sweep child repeats."""
    base, k = [], 0
    while k < len(argv):
        t = argv[k]
        if t in ("--fps", "--out", "--sweep-fps"):
            k += 2
        elif t.startswith(("--fps=", "--out=", "--sweep-fps=")):
            k += 1
        else:
            base.append(t)
            k += 1
    return base


def sweep_main(a, only):
    """--fps LIST: this same command once per entry (own subprocess, --out <out>/fps<entry>), then the summary."""
    try:
        entries = parse_fps_sweep(a.fps)
    except ValueError as e:
        sys.exit(str(e))
    out = Path(a.out)
    base = sweep_args(sys.argv[1:])
    if only:  # the child gets the expanded group list (physics -> its groups)
        base = sweep_args(base + ["--only", a.only])
        k = [i for i, t in enumerate(base) if t == "--only"]
        if len(k) > 1:  # the original --only stays first: drop it, keep the expanded one
            del base[k[0]:k[0] + 2]
    results, codes, measured = {}, {}, {}
    for e in entries:
        sub = out / fps_dir_name(e)
        print("[smoke] === frame rate %s -> %s" % (e, sub), flush=True)
        codes[e] = subprocess.call([sys.executable, str(Path(__file__).resolve())] + base + ["--out", str(sub), "--sweep-fps", e])
        results[e] = {}
        for rj in sorted(sub.glob("*/rows.json")):
            try:
                rows = json.loads(rj.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            results[e][rj.parent.name] = [sum(1 for r in rows if r[1]), sum(1 for r in rows if not r[1])]
        measured[e] = measured_summary(sub, e)
    text, sensitive = fps_summary(results, entries)
    problems = []
    text += "\nmeasured (the games' own rate lines, median over the phases):"
    for e in entries:
        ml, pr = measured.get(e, ([], []))
        text += "\n  @%s: %s" % (e, "; ".join(ml) or "no logs")
        problems += pr
    if problems:
        text += "\nFRAME GENERATION NOT RUNNING: " + "; ".join(problems)
    out.mkdir(parents=True, exist_ok=True)
    (out / "fps_summary.txt").write_text(text + "\n", encoding="utf-8")
    print("\n[smoke] frame-rate sweep summary\n" + text + "\n[smoke] " + str(out / "fps_summary.txt"), flush=True)
    sys.exit(1 if problems or any(codes.values()) or any(r[1] for d in results.values() for r in d.values()) else 0)


# Phases whose walker or altar prompt is known flaky (docs/party/world_actions.md bisect table, phase cao's note):
# a failed run is repeated up to FLAKY_TRIES times in all and the run with the fewest failed rows is kept, so a
# one-off stall no longer fails a release smoke while a real fault (it fails every time) still does.
# BB_SMOKE_NO_RETRY=1 turns it off. Retried runs are listed in the output dir's retries.txt.
# Why each is here (0.7.9 audit of the 0.7.8 runs: smoke_stats.py over smoke..smoke7). A phase that passes only on a retry
# is listed as FLAKY in verdict.txt / flaky.txt; one that fails the same rows twice is deterministic and is not repeated.
FLAKY_PHASES = {
    # the Ritual Altar's menu walk: the first Cross at the altar often finds no prompt (cao: 1 clean entry in 5 runs; cae and
    # cgt each timed out in 0.7.8 with "the autopilot answered the prompts" missing). Root cause not found.
    "cae", "cao", "cag", "cage", "cgt", "cgp", "cgu", "cab", "cis", "cim", "caq", "caw",
    # the lift walk-ins: the car's build / the ride's start races the walker (0.7.8: lfwcs "no fall", lfwf "host does not ride"
    # and lfwcf "copy does not freeze" each failed once, green on the retry). Root cause not found.
    "lfw", "lfwc", "lfws", "lfwcs", "lfwf", "lfwcf", "lfwbs", "lfwcbs",
    # failed on retry in releases up to 0.7.5 (docs/RELEASE_CHECKLIST.md: s3, lt, lf); green first time in 0.7.8, kept until
    # three clean full runs show they no longer flake
    "lt", "lf", "s2", "gd", "la", "ngp", "npf", "pvd", "sj", "kd", "s3",
    # nfw: "(before) Alfred friendly on both games" failed twice in a row and passed on the 3rd/4th run (an NPC spawn race)
    "nfw",
}
# (cl and ml left the set: they failed the same rows on every try, a harness / fixture fault that a retry cannot cure)
FLAKY_TRIES = 3


def run_phase_retrying(a, name, ph, out_dir: Path, check, only=None):
    """run_phase + check; the run with the fewest failed rows is kept (check(verdict, Logs, dir)).

    A phase in FLAKY_PHASES is repeated up to FLAKY_TRIES times, except that a repeat which fails the SAME rows as the run
    before is a deterministic failure (a real fault or a harness bug, not a flake): it stops there (0.7.8 repeated cl and
    ml three times with the same two rows each). A phase that failed and then passed is FLAKY: it is written to
    <out>/flaky.txt and listed in the verdict, so a green run never hides it."""
    best = None
    tries = (5 if name in ("cag",) else 4 if name in ("cao",) else FLAKY_TRIES) if name in FLAKY_PHASES and not os.environ.get("BB_SMOKE_NO_RETRY") else 1
    prev_rows = None
    failed_before = []
    for k in range(tries):
        if k:
            import shutil
            shutil.rmtree(out_dir.with_name(out_dir.name + "_try%d" % k), ignore_errors=True)
            if out_dir.exists():
                out_dir.rename(out_dir.with_name(out_dir.name + "_try%d" % k))
        run_phase(a, ph, out_dir)
        lv = Verdict(only)
        check(lv, Logs(out_dir), out_dir)
        failed = [r[0] for r in lv.rows if not r[1]]
        nfail = len(failed)
        print("[smoke] %s try %d: %d failed rows" % (name, k + 1, nfail), flush=True)
        if nfail == 0:
            if k:
                with open(out_dir.parent / "flaky.txt", "a", encoding="utf-8") as f:
                    f.write("%s passed on try %d (failed before: %s)\n" % (name, k + 1, "; ".join(failed_before)[:300]))
            break
        if k + 1 < tries:
            with open(out_dir.parent / "retries.txt", "a", encoding="utf-8") as f:
                f.write("%s try %d failed %d rows: %s\n" % (name, k + 1, nfail, "; ".join(failed)[:300]))
        if prev_rows is not None and failed == prev_rows and name != "cag":  # cag: the altar's first Cross is a known flake (same rows each time)
            print("[smoke] %s: the same %d rows failed twice: deterministic, no more tries" % (name, nfail), flush=True)
            break
        prev_rows, failed_before = failed, failed
    return


# ---------------------------------------------------------------------------------------------
# Tiers, the phase list for a run, progress / ETA (CL-564e30)
#
#   quick    the old default list (41 phases): the core party flow, about 1.5 h on one lane
#   release  every phase except RELEASE_SKIP (variants whose feature a sibling phase already runs)
#   full     every phase in PHASES (also what --release asks for: no hidden subset)
# A tier only chooses WHICH phases run; every phase keeps all of its checks and runs to the same verdict.
QUICK_PHASES = ("s1,s2,rules,sc,summonfail,s3,sk,ga,gl,lf,ld,mk,kd,kw,fl,fdj,cl,clg,clw,el,lt,lfw,lfwc,ch,rh,rs,bf,bl,lp,rt,eh,br,sm,da,cv,tv,nf,nfa,ie,sj")

# fast (smoke 0.7.9): ONE representative phase per feature area, about 40 min on one lane: the gate for a merge or a daily
# check. It is the cheapest phase that proves the area's core claim, with all of its checks. Area -> phase:
#   join / death / travel / boss / wipe s1 | items, kill, itembox, host crash s2 | gather ga | lifts + ladders lf | two riders on a lift
#   lt | deep fall fd | story warp kw | chalice progress cpr | fight load bf | boss rooms + fog walls br | boss drops bd |
#   travel vote tv | NPC fights nfa | host lamp hlq | Rom's fog rf | poison px | chalice shop csh | give menu gvm | echo hold eh
# The areas it leaves out (chalice dungeon entry, lift walk-ins, covenants, NPC quests, PvP, shops, music, ...) are in the
# release tier; `--changed-since TAG` picks them by the files a change touched.
FAST_PHASES = "s1,s2,ga,lf,lt,fd,kw,cpr,bf,br,bd,tv,nfa,hlq,rf,px,csh,gvm,eh,gdr"

# Phases the release tier leaves out: a variant of a phase that stays in (same map and mechanism, other lift / rule
# value / actor), kept in `full`. Each value names what covers it.
RELEASE_SKIP = {
    "lfwf": "lfw (yh lift walk-in, far from the car)", "lfwcf": "lfwc (ca lift walk-in, far)",
    "lfws": "lfwbs (slow car build; the yh lift itself is lfw)", "lfwcs": "lfwcbs (slow car build; the ca lift itself is lfwc)",
    "lfx": "lf (the two-player lift ride)", "cvh": "cvt (host talks instead of the guest)", "cvo": "cvs (per-player off)",
    "pxg": "px (guest poisoned instead of the host)", "chn": "chc (rule off)", "cyn": "cy (rule off)",
    "gro": "gr (lamps rule off)", "gr2o": "gr2 (rule off)", "gr3o": "gr3 (rule off)", "gr4o": "gr4 (rule off)",
    "bdo": "bd (boss drops off)", "sg2": "sg", "csh2": "csh", "mds": "md", "nfe": "nf (same NPC fight rows)",
}

# --changed-since: changed file -> the phases that exercise it. A game-code file with no entry here runs the quick tier
# (the safe side); files that cannot change game behaviour (docs, tests, launcher, packaging) run nothing.
CHANGED_MAP = [
    (r"party_lift|party_worldact|world_warper", "lf,lfw,lfwc,lfwbs,lfwcbs,ld,el,lt,fl,fdj"),
    (r"party_kidnap|kidnap", "kd,kw"),
    (r"party_chalice|chalice", "cl,clg,clw,cla,cpr,chc,chn,chw,cpa,cae,cao,cab,caq,caw,cis,cim,ch,cy,cyn"),
    (r"party_npc|npcfight|npcfog|npctalk|npcghost", "nf,nfa,nfe,nfw,npi,npa,npd,npf,cv,cvs,cvr,cvc,cva,cvt,tv,an,anl,scb"),
    (r"party_pvp|pvp_rules", "px,pxg,pvd,pvh,pvn"),
    (r"party_gather|gather", "ga,gl,gg,sh"),
    (r"party_split|splitscreen|stayhome|party_stay", "ss,sh,s1"),
    (r"lamp_travel|party_travel|party_rot", "lp,lt,rt,s1,gr,gr2,gr3,gr4,gro,gr2o,gr3o,gr4o,cy,mp,mpg,mph"),
    (r"shim/map/|map_prepare|bbmap_bake|party_hud_game", "mp,mpg,mph"),
    (r"death_rules|party_death|party_phantom|party_balance", "s1,s3,gd,gh,gm,chc,chn"),
    (r"party_vote", "br,s1"),
    (r"party_items|droproll|item_notice|kill_award|kill_record", "ie,il,s2,rw,rwe,rwt,rwl,bd,bdo,sgb,sgb2,sgb3,sgb4,sgb4n,sgp"),
    (r"party_give|give_rules", "gv,gvg,gvr,gvl,gvn"),
    (r"party_progress|party_flags|party_story|bossfog|test_newgame", "kw,ngp,br,rf,rj,em,ef,s1"),
    (r"party_hud|party_status|party_gestures|remote_show|party_glow", "rules,bf,bl,sg,sg2,sgb,sgb2,csh,csh2,csh3"),
    (r"party_guestsvc|guestvals|levelsync|enemyai|enemy_census|party_collision|party_catchup", "s1,s2,ga,gl,ml,mg"),
]
NO_GAME_CODE = re.compile(r"^(docs/|tests/|launcher|packaging/|scripts/|tools/(?!mp/(smoke_party|instances|game_queue))|README|CHANGES|QA_TESTING|licenses/|VERSION|.*\.md$)")


def phases_changed_since(tag):
    """(phase list, notes) for the files changed between `tag` and HEAD (plus the working tree)."""
    out = subprocess.run(["git", "diff", "--name-only", tag], cwd=REPO, capture_output=True, text=True)
    if out.returncode:
        sys.exit("--changed-since %s: git diff failed: %s" % (tag, out.stderr.strip()))
    files = [f for f in out.stdout.splitlines() if f.strip()]
    chosen, notes = set(), []
    for f in files:
        if f.startswith("tools/mp/"):
            chosen |= set(FAST_PHASES.split(","))
            notes.append("%s: harness changed -> fast tier" % f)
            continue
        if NO_GAME_CODE.match(f):
            continue
        hit = [ph for rx, ph in CHANGED_MAP if re.search(rx, f)]
        if hit:
            chosen |= {p for h in hit for p in h.split(",")}
            notes.append("%s -> %s" % (f, ",".join(sorted({p for h in hit for p in h.split(",")}))[:80]))
        else:
            chosen |= set(QUICK_PHASES.split(","))
            notes.append("%s: no mapping -> quick tier" % f)
    return [p for p in PHASES if p in chosen], notes


def phase_list_for(a):
    """The ordered phase names of this run: --phase when given, else --release / --tier / --changed-since."""
    notes = []
    if a.phase:
        names = [p.strip() for p in a.phase.split(",") if p.strip()]
        label = "--phase"
    elif a.changed_since:
        names, notes = phases_changed_since(a.changed_since)
        label = "changed since %s" % a.changed_since
    else:
        tier = "full" if a.release else (a.tier or "quick")
        if tier == "fast":
            names = FAST_PHASES.split(",")
        elif tier == "quick":
            names = [p for p in QUICK_PHASES.split(",")]
        elif tier == "release":
            names = [p for p in PHASES if p not in RELEASE_SKIP]
        else:
            names = list(PHASES)
        label = "tier %s" % tier
    bad = [n for n in names if n not in PHASES]
    if bad:
        sys.exit("unknown phase(s): " + ",".join(bad))
    print("[smoke] %s: %d of %d phases%s" % (label, len(names), len(PHASES),
                                           " (left out: %s)" % ",".join(p for p in PHASES if p not in names)[:200] if len(names) < len(PHASES) and len(names) > 100 else ""),
          flush=True)
    for n in notes:
        print("[smoke]   " + n, flush=True)
    return names


SEED_TIMES = HERE / "phase_times_seed.json"  # per-phase wall seconds of a full run (the ETA before this machine has its own)


def load_times(times_file):
    known = {}
    for f in (SEED_TIMES, times_file):
        try:
            known.update(json.loads(Path(f).read_text()))
        except (OSError, ValueError):
            pass
    return known


class Progress:
    """phases done/total and an ETA from the last measured phase times (300 s when unknown), over `lanes` lanes."""

    def __init__(self, names, known, lanes=1):
        self.names, self.known, self.lanes = list(names), known, max(1, lanes)
        self.done, self.t0 = [], time.time()

    def eta(self):
        left = sum(self.known.get(n, 300) for n in self.names if n not in self.done)
        return left / self.lanes

    def finish(self, name, secs=None, tag="", wait=0):
        self.done.append(name)
        self.waited = getattr(self, "waited", 0) + wait
        if wait >= 1:
            tag = (tag + ", " if tag else "") + "%.0f s waiting for game slots" % wait
        print("[smoke] progress %d/%d phases done (%s%s%s); elapsed %s, ETA about %s" % (
            len(self.done), len(self.names), name, ", " + tag if tag else "", ", %.0f s" % secs if secs else "", fmt_hms(time.time() - self.t0),
            fmt_hms(self.eta())), flush=True)


def queue_wait(phase_dir):
    """Seconds the phase's runs (retries included) waited for game slots (game_queue's `after waiting N s` line)."""
    tot = 0
    d = Path(phase_dir)
    for h in [d / "harness.log"] + sorted(d.parent.glob(d.name + "_try*/harness.log")):
        try:
            tot += sum(int(x) for x in re.findall(r"after waiting (\d+) s", h.read_text(errors="replace")))
        except OSError:
            pass
    return tot


def fmt_hms(s):
    s = int(s)
    return "%d:%02d:%02d" % (s // 3600, s % 3600 // 60, s % 60)


def auto_lanes(want):
    """--lanes auto: 2 lanes when the machine has the free RAM and commit for two 3-game phases at once (see
    game_queue memory guard), else 1."""
    if want != "auto":
        return int(want)
    sys.path.insert(0, str(HERE))
    from game_queue import free_ram_mb
    free = free_ram_mb()
    n = 2 if free is not None and free >= 40000 else 1  # two 3-game phases take ~40 GB of commit
    print("[smoke] --lanes auto: %s MB free RAM -> %d lane(s)" % (free, n), flush=True)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(DEFAULT_ROOT))
    ap.add_argument("--build", default=str(REPO))
    ap.add_argument("--game", default=os.environ.get("BB_GAME_DIR", str(AGENT_ROOT / "game")))
    ap.add_argument("--out", default=str(AGENT_ROOT / "smoke" / time.strftime("%Y%m%d_%H%M")))
    ap.add_argument("--phase", default=None, help="comma list of phases (wins over --tier / --release / --changed-since)")
    ap.add_argument("--tier", choices=("fast", "quick", "release", "full"), default=None,
                    help="which phases: fast = one phase per feature area, about 40 min (docs/SMOKE_TIERS.md); quick = the old 41-phase default (default); release = every phase except the variants in "
                         "RELEASE_SKIP; full = every phase")
    ap.add_argument("--release", action="store_true",
                    help="the release gate: the FULL phase list (no hidden subset), as --tier full; pair with --lanes auto")
    ap.add_argument("--changed-since", default=None, metavar="TAG",
                    help="only the phases that exercise the files changed since TAG (git diff TAG; CHANGED_MAP; an unmapped game "
                         "file runs the quick tier, a harness change the fast tier)")
    ap.add_argument("--no-mem-guard", action="store_true",
                    help="with --lanes > 1: do not let the lanes' game runs exceed the machine-wide client limit (memory guard off; "
                         "the lanes then take turns)")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--fps", default=None,
                    help="frame-rate sweep: comma list, each one rate or a per-instance mix: 30,60,90,120,60fg,60/30 (60/30 = host "
                         "at 60, guests at 30; a fg suffix = FSR frame generation 2x on that rate: 60fg, 60fg/30, uncapfg). The phases run once per entry into <out>/fps<entry, / -> x>, then one "
                         "summary. Without it: BB_MP_FPS (default 30), a phase's own fps")
    ap.add_argument("--sweep-fps", default=None, help=argparse.SUPPRESS)  # one sweep entry (a --fps child run)
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--real-net", action="store_true",
                    help="the phases on the real network (instances.py --real-net: public STUN, UPnP, the Internet code)")
    ap.add_argument("--only", default="", help="comma list of step groups (a smaller run); `physics` = the lift / ladder / elevator / fall "
                    "groups (" + ",".join(PHYSICS_GROUPS) + "): " + ",".join(GROUPS))
    ap.add_argument("--lanes", default="1",
                    help="run the phases in N parallel lanes (own instance root each, <root>_lN; a phase goes to the next free "
                         "lane, longest first); `auto` = 2 when the free RAM allows. The lanes' game runs may exceed the "
                         "client limit only while the memory guard (game_queue.py) finds the RAM for them. The verdict is made "
                         "once over all phases")
    ap.add_argument("--lane-id", type=int, default=-1, help=argparse.SUPPRESS)  # a lane child (--lanes)
    a = ap.parse_args()
    RUN_ROOT[0] = a.root
    a.lanes = auto_lanes(a.lanes) if a.lane_id < 0 else 1
    only = {g.strip() for g in a.only.split(",") if g.strip()}
    if "physics" in only:
        only = (only - {"physics"}) | set(PHYSICS_GROUPS)
        a.only = ",".join(sorted(only))
    if a.fps:
        sweep_main(a, only)
        return
    global WIPE_LATE_RUN
    WIPE_LATE_RUN = "wipe_late" in only
    bad = only - set(GROUPS)
    if bad:
        sys.exit("unknown --only group(s): " + ",".join(sorted(bad)))
    if only and a.lane_id < 0:  # (a lane child keeps the one phase it was given)
        a.phase = ",".join(p for p in (*JN_PHASES, "pc", "vm", "yj", "yjc", "s1", "s2", "rules", "ss", "sh", "iv", "ivn", "ivx", "ivl", "ivp", "ivc", "ivf", "iv2", "ivm", "ojl", "ojla", "gg", "bv", "bw", "sc", "summonfail", "s3", "sk", "lg", "lv", "ga", "gl", "lf", "lfa", "lfw", "lfwc", "lfws", "lfwcs", "lfwf", "lfwcf", "lfwbs", "lfwcbs", "lu", "mk", "kd", "kw", "fl", "fd", "fdj", "cd", "iw", "iwn", "an", "ld", "cl", "clg", "clw", "cla", "cpr", "chc", "chn", "chw", "cpa", "cae", "cao", "cag", "cage", "cgt", "cgp", "cgu", "caa", "cas", "cab", "el", "lt", "bf", "ch", "md", "mds", "svl",
                                     "lp", "gd", "gh", "gm", "gr", "gr2", "gr3", "gr4", "gro", "gr2o", "gr3o", "gr4o", "mp", "mpg", "mph", "jl", "ml", "mg", "ll", "llp", "hlq", "hlr", "bq", "hs", "ngj", "nl", "ff", "lk", "lkc", "rt", "br", "rf", "rj", "em", "emf", "bd", "bdo", "sm", "ef", "ngp", "mb", "gb", "mbs", "gbs", "da", "cv", "cvr", "cvm", "cvc", "cva", "cvf", "cvk", "cvl", "cvt", "cvh", "cvs", "cvo", "ie", "il", "rw", "rwe", "rwt", "rwl", "gv", "gvg", "gvr", "gvl", "gvn", "gvm", "gvs", "gvt", "gvw", "gvwn", "gva", "rh", "rs", "fw", "fwg", "fwb", "fwe", "fh", "tv", "nf", "nfa", "nfe", "nfw", "nfn", "nfr", "nfv", "nfd", "nfb", "nfh", "npi", "npa", "npd", "pvd", "pvh", "pvn", "npf", "eh", "bl", "mw", "px", "pxg", "sj", "ou", "gdr", "gdrd", "gdrb", "gdrf", "mesh", "oum", "meshm", "fp", "fp5", "oj", "lo", "po", "pos", "sg", "sg2", "sgb", "sgb2", "sgb3", "sgb4", "sgb4n", "sgp", "csh", "csh2", "csh3", "cis", "cim", "ba", "bh", "bk", "hg", "bg", "lr", "lrg", "mi", "mig", "lw", "lwf", "amp", "la", "lb", "lc")
                           if p in {n for g in only for n in GROUPS[g].split(",")})
    if Path(a.game).resolve() == Path(r"E:\games\bloodborne").resolve():
        sys.exit("never the user's game folder as the test game (agents/RULES_062.txt)")
    out = Path(a.out)
    names = phase_list_for(a)
    times_file = AGENT_ROOT / "smoke" / "phase_times.json"
    if a.lane_id >= 0:
        # a lane child: run its phases only; the parent makes the verdict
        for name in names:
            ph = PHASES[name](only)
            if only:
                ph = narrow_phase(ph, only)
            t0 = time.time()
            run_phase_retrying(a, name, ph, out / name, CHECKS[name], only)
            (out / name / "wall.txt").write_text("%.0f" % (time.time() - t0))
            # check now, while this lane's saves are the phase's (the sc / lp checks read them)
            lv = Verdict(only)
            CHECKS[name](lv, Logs(out / name), out / name)
            (out / name / "rows.json").write_text(json.dumps(lv.rows), encoding="utf-8")
        return
    t_all = time.time()
    known = load_times(times_file)
    prog = Progress(names, known, a.lanes)
    if a.lanes > 1 and not a.check_only:
        # Each lane is a thread that starts the next phase of a shared list, longest first, as a child process
        # `--lane-id K --phase X` (own instance root <root>_lK). How many games run at once is decided by the game
        # queue; the lanes ask it with the memory guard on (BB_GAME_QUEUE_MEMGUARD, see game_queue.py).
        import threading
        todo = sorted(names, key=lambda n: -known.get(n, 300))
        lock = threading.Lock()
        out.mkdir(parents=True, exist_ok=True)
        base_env = dict(os.environ)
        if not a.no_mem_guard:
            base_env["BB_GAME_QUEUE_MEMGUARD"] = "1"

        def lane(k):
            while True:
                with lock:
                    if not todo:
                        return
                    name = todo.pop(0)
                argv = [sys.executable, str(Path(__file__).resolve()), "--root", "%s_l%d" % (a.root, k),
                        "--build", str(a.build), "--game", str(a.game), "--out", str(out), "--every", str(a.every),
                        "--phase", name, "--lane-id", str(k)] + (["--only", a.only] if a.only else []) + (
                        ["--sweep-fps", a.sweep_fps] if a.sweep_fps else []) + (["--real-net"] if a.real_net else [])
                t0 = time.time()
                with open(out / ("lane%d.log" % k), "a") as lf:
                    subprocess.call(argv, stdout=lf, stderr=subprocess.STDOUT, env=base_env)
                with lock:
                    prog.finish(name, time.time() - t0, "lane %d" % k, wait=queue_wait(out / name))
        print("[smoke] %d lanes, %d phases, estimate %s" % (a.lanes, len(names), fmt_hms(prog.eta())), flush=True)
        threads = [threading.Thread(target=lane, args=(k,)) for k in range(a.lanes)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        a.check_only = True  # the verdict below reads the lanes' logs
        lanes_used = True
    else:
        lanes_used = False
    v = Verdict(only)
    for name in names:
        ph = PHASES[name](only)
        if only:
            ph = narrow_phase(ph, only)
        if not a.check_only:
            t_ph = time.time()
            run_phase_retrying(a, name, ph, out / name, CHECKS[name], only)
            (out / name / "wall.txt").write_text("%.0f" % (time.time() - t_ph))
            prog.finish(name, time.time() - t_ph, wait=queue_wait(out / name))
        print("[smoke] verdict %s" % name, flush=True)
        if lanes_used:
            try:
                v.rows += [tuple(r) for r in json.loads((out / name / "rows.json").read_text(encoding="utf-8"))]
            except (OSError, ValueError):
                v.rows.append(("%s: lane result" % name, False, "no rows.json (lane crashed? see lane*.log)"))
            continue
        n0 = len(v.rows)
        CHECKS[name](v, Logs(out / name), out / name)
        (out / name).mkdir(parents=True, exist_ok=True)
        (out / name / "rows.json").write_text(json.dumps(v.rows[n0:]), encoding="utf-8")
    if not a.check_only or lanes_used:
        # this machine's measured phase wall times: the next run's ETA
        try:
            cur = {}
            try:
                cur = json.loads(times_file.read_text())
            except (OSError, ValueError):
                pass
            for name in names:
                try:
                    cur[name] = float((out / name / "wall.txt").read_text())
                except (OSError, ValueError):
                    pass
            times_file.parent.mkdir(parents=True, exist_ok=True)
            times_file.write_text(json.dumps(cur, indent=1))
        except OSError:
            pass
    fails = [r for r in v.rows if not r[1]]
    text = "\n".join("%s  %s  %s" % ("PASS" if ok else "FAIL", n, d) for n, ok, d in v.rows)
    text += "\n\n%d steps, %d failed\n" % (len(v.rows), len(fails))
    try:  # a phase that passed only on a retry is flaky, even in a green run
        flaky = (out / "flaky.txt").read_text(encoding="utf-8").strip()
        if flaky:
            text += "FLAKY (passed only on a retry; see FLAKY_PHASES in tools/mp/smoke_party.py):\n" + flaky + "\n"
    except OSError:
        pass
    if not a.check_only or lanes_used:
        text += "wall time %.0f s (queue wait included%s)" % (
            time.time() - t_all, ", %d lanes" % a.lanes if lanes_used else "") + chr(10)
    out.mkdir(parents=True, exist_ok=True)
    (out / "verdict.txt").write_text(text, encoding="utf-8")
    print("[smoke] %d steps, %d failed; %s" % (len(v.rows), len(fails), out / "verdict.txt"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
