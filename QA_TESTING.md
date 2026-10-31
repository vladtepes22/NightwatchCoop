# QA and testing

How Nightwatch Co-op checks a change before it ships, modelled on visy's
[QA_TESTING.md](https://github.com/visy/bloodborne_pc_windows_port/blob/master/QA_TESTING.md). Everything
here runs on one Windows PC with the real game (your own dump, never shipped); nothing needs a second
machine or an online service.

The rule behind all of it: **a change is only as good as the layer that checked it.** A unit test proves
logic, not that the game behaves; a smoke row proves one scripted path; a playtest finds the rest. Every
change report says which layers ran and what stayed unproven.

## 1. Layers

| Layer | Catches | Tool | When |
|---|---|---|---|
| `native` | logic in pure code: party rules, codes, NAT, lifts, menus, settings, shop gates (`*_rules.h`) | `ninja -C out/gpu seamless-tests` + `ctest -j8` (65 tests) | every engine change |
| `python` | patch compiler, scripts, save tools, net_doctor, the test harness | `python -m unittest discover -s tests` in `engine/` (CLANG64 Python 3.12) | scripts or tools touched |
| `launcher` | co-op settings, host/join validation, updates, every translated string, display options | `dotnet run --project SeamlessTests`, `tests/LocalizationChecks`, `tests/DisplayChecks` | launcher touched |
| `build` | the release configuration: LTO, the party build ID re-hash | `cmake out/gpu` + `build.sh` | release, or hooks / start-up changed |
| `smoke` | the game boots, reaches the world, ticks the party code, no fault line | `tools/smoke-game.py --new-game` (headless, muted, isolated save) | engine hooks, start-up, packaging |
| `coop` | host + guest in one session; scripted co-op rows judged from both logs | `tools/smoke-coop.py --scenario <row>` | party / net code touched; release |
| `privacy` | user names, friends' names, addresses, game-derived files in a package | `tools/privacy-scan.py` + file check | every package |
| visible | what a player sees (headless draws nothing) | one numbered checklist for the batch | only what no layer above covers |
| playtest | everything the scripts never thought of | testers with logs, `net_doctor` | after a pre-release |

`tools/qa.py` runs the layers for you and prints one table:

```
python tools/qa.py plan --level change          # which layers the touched files need (vs fork/main)
python tools/qa.py run  --level change          # ... and run them
python tools/qa.py run  --level merge           # native + python + launcher
python tools/qa.py run  --level release --package <package dir>   # everything, every co-op row
python tools/qa.py run  --only coop --scenario core
```

Each run writes `work/qa/<run>/report.md` (the table to paste into a change report), `report.json` and a
log per layer. Exit code 1 when any layer fails. The release level refuses to build without a `subst`
drive mapped to the tree: exes built under `C:\Users\...` embed the user name.

## 2. Co-op rows: the game plays itself

There is no external input injection. visy's party code has a built-in **director**
(`BB_PARTY_DIRECTOR_TEST`, reference in `engine/docs/party/DEBUG_SWITCHES.md`) that runs a chain of steps
inside each game: `guest_leave@10`, `guest_rejoin@5`, `guest_die@12`, `host_warp=LAMP@S`,
`host_kill=ENTITY@S`, `guest_press=BUTTONS@S`, conditions such as `^N` (the Nth cooperator) or `?FLAG=V`.

`tools/smoke-coop.py` starts a host and a guest (headless, muted, loopback, isolated copies of the smoke
saves), gives each its own chain and judges **log lines, in order, per role**. The rows live in
`tools/qa_scenarios.json`:

```json
"core": {
  "host":  "host_warp=2412953@25^1*2",
  "guest": "echoes=5000,guest_leave@10,guest_rejoin@5,guest_die@12",
  "expect": [["host", "cooperators 0 -> 1"], ["guest", "guest_leave: Party > Leave pressed"], ...]
}
```

- `fiverejoin` (four guests, the fourth Leaves and Rejoins) is run by hand only: five games need about 40 GB of free commit.
- Rows today: `join` (session up), `core` (Leave / Rejoin, guest death + bloodstain + respawn, party lamp
  travel), `deaths` (host and guest deaths outside a fight: revive at the lamp, session kept), `boss`
  (Gascoigne: the guest goes in first, the host dies in the fight and spectates, then the wipe). A row may
  carry `env` switches for both games (the boss row clears Gascoigne's progress flags).
- A run ends as soon as every expected line is there, or at its deadline; each row prints PASS / FAIL and
  `result.json` keeps the matching line. Any engine fault, `C++ exception`, `VK_ERROR` or `STOP:` line in
  either log fails the run.
- Logs are the oracle: every party subsystem writes a plain line per state change (`Party: cooperators
  0 -> 1`, `Party travel: ...`, `Party B2: ...`). Rows match those lines; nothing is judged from memory
  reads in the script.
- **Every co-op fix adds or extends a row** so the bug stays covered. A row that only passes sometimes is
  not evidence: rerun it (`--only coop --scenario X` a few times) and say how many passed.
- visy's own `engine/tools/mp/smoke_party.py` has many more phases, but it drives his single-exe build
  (`bbport.exe`); its phases are the source for our rows (same director steps, same log markers).
- **Scope (decided 26 Oct): our rows check our port, not visy's co-op logic.** visy runs his full suite
  (19 phases, ~1 h) after every merge to his master, so what reaches us has already passed it. What his runs
  can't see is our tree: Lantern's engine, our launcher and `bbport.ini`, his overlay rows placed by hand,
  the conflicts we resolved. The basic loop (`join`, `core`, `deaths`, `boss`: ~8 min) catches a port that
  broke that. Don't port all his phases or rerun his suite on our side; port one phase as a row only when we
  change that co-op behaviour ourselves (the "every co-op fix adds a row" rule above).

Test switches are for tests only. Player-facing behaviour is never controlled by them.

## 3. The change gate

Before a change is committed:

1. `python tools/qa.py run --level change` green (or the layers it names, run by hand).
2. A co-op behaviour change: a row in `tools/qa_scenarios.json` that shows it, or a note why none can.
3. Anything a player sees: on the visible checklist for the batch, not claimed as tested.
4. The change report: root cause with evidence, the QA table, what stayed unproven.

Rules every change follows:

- **New behaviour is a toggle**: a `bbport.ini` key, a row on the matching in-game page and in the launcher,
  zh-CN strings (LocalizationChecks enforces them). Host-decided rules show read-only on guests. Fixes get
  no player toggle, at most a `BB_*` switch for testing.
- **New diagnostics are opt-in**: off by default, behind a `BB_*_LOG` switch, rate-limited.
- **Correct over seamless**: if a seamless path glitches, use the game's own loading screen.
- **Any engine change makes a new party build ID**: players on different builds can't join each other; the
  notes say "update together".
- **Privacy**: player logs hold names and IP addresses. Nothing from them goes into commits, docs or notes;
  examples use `203.0.113.x`. Packages pass the privacy layer and carry no game-derived files (eboot.elf,
  boot-linked.bin, saves).

## 4. Porting visy's co-op

visy's tree maps onto `engine/`. Each batch since the last merge point is applied with `git apply --3way
--directory=engine`, slice by slice; each slice is committed only after its proof (the layers its files
need, plus `native` always). visy's tests come with his code, and his tests passing in our tree is part of
the proof. The label (`visy vX+<commit>` in `tools/package-seamless.ps1` and `BB_ID_CONTENT`) names the
last visy commit merged as a whole; hand-ported pieces are named in the notes.

After a batch, `python tools/visy-port-check.py <last merge point> <visy commit>` checks that every hunk
visy changed in the range is in our tree: identical file, or his patch reverse-applies onto ours. It lists
the rest with visy's added lines. Each one is either our glue on purpose (his party menu rows live in our
`bbport_game_menu_pages.cpp`, not `bbport_overlay.cpp`; his test targets in `gpu/cmake/SeamlessTests.cmake`)
or a missed piece. There is no allowlist on purpose: new rows land in exactly those files.

What visy has not run live is not claimed as tested here either: it goes on the playtest list.

## 5. Player reports

1. Keep the files private, outside the repo (`work/player_logs/<date>/<short-name>/` is git-ignored), with a
   `README.txt`: what was reported, build and role (host / guest), first findings with log line numbers.
2. Skim first: role, build (`visy vX+...` and the party build ID), area, the lines around the event. First
   checks: `party networking on (host|guest)`, `Party: roster`, `cooperators`, `Guest fault`, `STOP:`,
   `uncaught C++ exception`, `GPU memory:`, `device lost`.
3. Classify: co-op side (ours or visy's party code: fix or report to visy), or engine / GPU side (one-line
   verdict, Lantern's code). The Launcher's Logs button and Check my connection give the player's logs with
   IPs hidden.
4. Severity: S1 blocks play or loses progress, S2 wrong but playable, then features and polish.
5. Reply with a workaround if one exists and what evidence would help (both players' logs).
6. The fix adds a co-op row or a native test that reproduces the report.

## 6. A release

1. Freeze: everything planned is merged, or moved to the next version. Nothing half-merged.
2. `cmake out/gpu` (re-hashes the party build ID), then `python tools/qa.py run --level release` on the
   final commit. Triage every failure: regression (fix, rerun), flake (rerun once, write it down), stale
   check (fix the check). To keep the co-op rows (~8 min) off the critical path: once the release build
   and the `smoke` layer are done, start `python tools/qa.py run --only coop` in the background and do
   steps 3-5 meanwhile; the release waits only for its PASS (the rows run every release, never skipped).
3. Package into a **fresh** folder (`tools/package-seamless.ps1 ... -Release`), never one a game ran from;
   rerun the `privacy` layer on it (`--only privacy --package <dir>`).
4. `tools/release.ps1 -Package <dir> -Notes <notes> -DryRun`.
5. Notes in player language (Added / Changed / Fixed / Known issues / Not played live yet), the Lantern
   attribution line first, "update together" when the party build ID changed.
6. Pre-release to testers first (`python tools/gofile-upload.py <zip>` prints the download page for the
   Discord post); publishing (tag, push, GitHub release) is a separate, explicit decision.
7. One numbered visible checklist for the batch, after the headless layers are green.
