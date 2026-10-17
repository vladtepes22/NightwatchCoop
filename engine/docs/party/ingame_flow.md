# The in-game party flow

Starting, joining and continuing a party (co-op) game from the game's own menus, without the
launcher's Party tab. Code: `gpu/shim/party/party_flow.{h,cpp}` (party.json, the action planner,
the relaunch file; unit-tested by `tests/test_party_flow.cpp`, target `party-flow-test`),
`party_flow_game.cpp` (game glue), `party_flow_rows.cpp` (the Party page rows),
`gpu/shim/bbport_game_menu.cpp` (the title entry), `run.bat` (the relaunch loop),
`src/probe.c` `runtime_relaunch`, `src/runtime_savepolicy.c` (the party saves).

## 1. What the player does

**First time (everyone makes a fresh character):**

1. Host: title screen → **Party** (right after *New Game*) → **New party game** (press Cross
   twice: the first press asks *Press again to confirm* on the help line). The game restarts by itself (a few seconds) into the party game: online title, party
   saves (`user\savedata_party`, empty the first time), hosting. Choose **New Game**, make the
   character. The party code is on *System → Party* (*Copy party code*) and in
   `user\party_code.txt`; it stays the same in later sessions (§4).
2. Friends: title → **Party** → **Host address** (type or paste the host's party code or
   `host:port`, Enter) → **Password** (if the host set one) → **Join party game** (Cross twice). The game
   restarts by itself, joined to the host; choose **New Game** and make the character.
3. Everyone plays the prologue on their own (`BB_PARTY_START=prologue_solo`, `party_start.h`);
   once a hunter has the first Dream behind them and both starting weapons, the party gathers
   automatically (the host's Beckoning Bell, the guests' Small Resonant Bell, as before).

**Later sessions:** the host: title → **Party** → **Continue party game** (Cross twice; shown once the party saves hold a character, also for
launcher users): restart, the
party character loads on its own, hosting starts. Friends: title → **Party** → **Continue party
game** (their last role was *join*: the saved address and password are used) – restart, the party
character loads, the game joins the host and the summon follows by itself. *Continue* /
*New Game* on the title stay the single-player game.

**In the world (party game):** *System → Party* has the party status, *Copy party code* (a host:
*Copy Internet code* and *Copy LAN code*; the help line shows the code, the value *Copy* /
*Copied*), *Summon again* / *Rejoin
the host* and *Leave the party*; *System → Party mode* has **Host party** / **Join party** (the
other role: a restart, the character continues; or hosting / joining again after *Leave*) and
**Single player** (restart into the single-player game and saves). In a launcher-started party
game the title has *Party* (the status: role and "not open yet" / "not joined yet" before the
link runs) and *Party mode* after *New Game*. With a single-player character
loaded the Party page only explains that party games start on the title screen: no party action
can touch a single-player save.

Text entry is the window's text box (the same as the character name): type on the keyboard,
**Ctrl+V** pastes, Enter confirms, Esc (or Circle) cancels. The pad cannot type; the address and
password are typed once and remembered. The text rows (*Host address*, *Password*, *Party name*)
are buttons showing the current value; Cross opens the box (every press).

**Rows are buttons** (0.5.9): every action and read-only line on the port's pages is the game's
choice row (ComboBox 0x1B2A100) built with ONE value, as droogie/bbhost's key binding rows:
Left/Right have nothing to step to (the game still draws its arrows on the focused row, dimmed; a
true arrowless row would need bbhost's command rows, which only exist in the System list itself). The pad sampling counts Cross presses
(`runtime_pad_cross_presses`); the present thread (`PollButtons`) reads the open page's cursor
(dialog + 0xA70 rows list, cursor +0x74, override +0x78, from bbhost's menu_pointer.cpp) and runs
that row's button (`Pages::Press`); a press while the page was being opened, or while the text box
is open, does not count. Confirmed buttons (New / Join / Continue party game, Host / Join party,
Single player, Leave, Kick, Restart, Quit) arm on the first press (the help line: *Press again to
confirm*) and run on a second press within 10 s. The page layout ("ControllSetting") has six widget
slots and does not scroll: every page keeps its visible rows within six in every state
(`tests/test_game_menu_rows.cpp` walks 3840 party states), which is why the role switches moved to
the *Party mode* page and the solo title's Party page has no Status line. The value column follows
at once (a saved address, *Join* instead of *Set address first*, the party status): the rows'
widgets (kept with a reference from page+0xE50's rows) redraw their items (0x1ACFB20) after a
run-time text changed, from the game's menu thread (`bbport_game_menu.cpp` live value columns;
`BB_GAME_MENU_LIVE_VALUES=0` turns it off).

**Leave in the world (guest)**: *System > Party > Leave the party* (Cross twice) first runs the game's own
voluntary leave and stops the party link once the game's session is idle (`party_status_bridge.cpp`).
The vanilla path (Ghidra): `OnLeaveMenu_Yes 0x1389FB0` = session leave (0x13CDDA0: BeginLeaveSession
0x1ED07A0 + drop) and, for a phantom, `OnLeave_Limit 0x138A4A0` at once (pre-summon spot, the funnel
0x13CDE30, the load home). The party now calls `OnLeave_Limit` for a summoned guest
(`coop::LeaveGameSession`; party_travel's send-home retarget lets this one through,
`AllowNextSendHome`). With the session leave alone the game noticed the empty room 10 s later and
showed *Session lost* (message 0xcc/5001, found with `BB_GAME_MENU_TRACE_TEXT`) before going home. A
host's Leave with guests in ends the game session first (0x13CDDA0), then the link. After Leave the page shows *Status (Not running), Host party, Join
party, Single player* - Copy code / Rejoin / Leave need a running link. **Join party** then starts a
fresh link; the guest registers its NP context with the host again (`party_runtime.cpp`
`fresh_link`) and a pending signaling resolve is retried every 2 s (`np_signaling.cpp`): before,
the host's game kept summoning the rejoined guest and the summons never arrived.

**Party name** (title page, before the first party character: the page shows six rows): every
player needs their own - the host admits one player per name. Without one, the first party action
picks `<Windows user name>-xxxx` (`DefaultName`: the name's `[A-Za-z0-9_-]`, at most 11, plus four
random base-36 digits) and keeps it in party.json. (Live, 2026-10-11: before this every menu player
was "Hunter" and the host refused the second friend: `rejected: the name Hunter is already in the
party`.)

## 2. Design decision: a quick restart instead of switching in place

The party layer is chosen when the game starts, and the single-player game stays strictly as it
was (no sockets, no party hooks, the offline title, `user\savedata`). Switching a running game
between the two was examined and rejected; every boot-time choice below would need a live switch
inside a running game:

| Boot-time choice | Where | Why it cannot change later |
|---|---|---|
| The party patches (*Bells anywhere*, *Keep session on map reload*, *SOS sign timeout*, *Bells without Insight*) and their hash in the party version check | `scripts/patches.py` → `out/patches.bin`, applied to the image before any game code runs | Byte patches in code the game is executing; the hash must be identical on every player (party_runtime HELLO). |
| The online title (*Skip Online/Offline Choice (Online)*) | the same patches; FrpgNetMan+0xa | The online/offline decision exists only in the title's dialog: the step chain 0x1C36A80 (online sign-in 0x1C37B30) has exactly one caller, the title functor 0x1B48D40 (Ghidra refs), reached from the title constructor 0x1B384F0 and the title re-entry 0x1B4BC30 only. There is no in-game online switch to call after *Continue*. |
| Import routing (sceNp / sceNet / sceHttp → the party library) | `bbnet_resolve`, at link time | The game's import slots are bound once at load; a dormant party library in single-player would replace the offline stubs single-player has always used. |
| The save folder (`savedata` / `savedata_party`) | `runtime_savepolicy_dir()`, read once; the game mounts its saves at the title | The game caches the mounted save data and the slot list; a remount under a running title is not something the game expects. |
| The party runtime's hooks (`PartyInit`) | `bbgpu_patch_image`, while the image is writable | Installed before the game runs, byte-verified; installing later means patching live code on other threads. |

All of these are already right at boot when the process starts with `BB_PARTY` set, so the flow
**restarts the game automatically** with the new environment. The restart takes a few seconds
(the shader cache is warm) and needs no input: the new process continues the party character on
its own (the director's auto-Continue, as after a crash) or waits at the title for *New Game*. A
return-to-title plus an online *Continue* would still have needed the patches, the routing and the
save folder switched, so it gains nothing.

### How the restart works

1. The Party page's action (`party::flow::Run`) plans it (`PlanAction`): `BB_PARTY=host|join`,
   `BB_PARTY_SAVE=separate`, `BB_PARTY_MENU=1` (this run came from the menu: party.json supplies the
   address, password and host secret), `BB_PARTY_AUTOCONTINUE=0|1`, `BB_SKIP_NETWORK_CHOICE=online`,
   `BB_PARTY_NAME` / `BB_PARTY_PORT` when saved, and unsets `BB_PARTY_CODE`, `BB_PARTY_PASSWORD`,
   `BB_PARTY_RESTARTED`. *Single player* unsets the party variables.
2. It stores `user\party.json`, writes `user\party_relaunch.bat` (`set "K=V"` lines; values limited
   to `[A-Za-z0-9_.:@-]`), shuts the party link down cleanly (`bbnet_shutdown`: BYE, UPnP unmapped,
   the crash marker removed – so the next run is not taken for a crash restart) and ends the game
   with exit code **75** (`runtime_relaunch`, the same TerminateProcess path as the overlay's
   restart).
3. `run.bat` starts the game with `BB_RELAUNCH_LOOP=1`; on 75 it calls and deletes
   `party_relaunch.bat`, runs `scripts\patches.py` again (party patches, online title) and starts
   the game again in the same console – the launcher's log and the test harness keep following
   it. Without the loop (an older `run.bat`, `run.sh` on Linux) `runtime_relaunch` falls back to
   `runtime_restart`, which starts `run.bat` / `run.sh` with the environment already set in the
   process.

The launcher's Party tab keeps working unchanged: it sets `BB_PARTY` before the first start (the
party game from the beginning; the in-game rows then switch role or back to single player). Both
paths use the party saves; the menu path keeps its settings in party.json, the launcher path in
`launcher_settings.json` (a launcher run never reads party.json's password or code).

### Why "Continue party game" and not the title's Continue + "Host party"

The first idea was: press the game's own *Continue*, then *Host party* / *Join party* in the
world. With the party save as the only co-op path, the title's *Continue* loads a single-player
character (it reads `user\savedata`); hosting or joining from there would sync the party's
progress and items into that save. So the party entries live next to *Continue* instead: *Continue
party game* is the one press that picks the party save, the role and the saved address, and the
world's *Host party* / *Join party* are offered only once the party character is loaded (switching
role, or hosting / joining again after *Leave*).

## 3. The title entry

The title's main menu is built by 0x1B4A3D0 (Ghidra): command items through
**AddCommandItem 0x1B4C9A0** (`(list, name+help texts, functor, 0)`, returns the list) – Continue
0x61F95 (only when the last slot is valid), Load Game 0x61F96, New Game 0x61F97, 0x61F98, and
online-only items – and page items through AddPageItem 0x1B4E2A0 (0x61F99, 0x61F9B). The game menu
already hooks AddPageItem (System pages); the flow adds a byte-verified detour on AddCommandItem
(prologue `55 48 89 E5 41 57`, the same 6 bytes the System hook moves): right after the item whose
name text is New Game's (the pointer the message lookup returned for 200/0x61F97), it adds the
port's **Party** page as a title page item, with the same page factory as *System → Party*. So the
title shows *Continue / Load Game / New Game / Party / ...*; *Party* opens the same page as
*System → Party*. `BB_PARTY_TITLE_ENTRY=0` leaves the title menu alone (*System → Party* still has
everything).

## 4. user\party.json

```json
{ "version": 1, "name": "Hunter", "role": "join", "host_address": "BBP3-....",
  "password_dpapi": "<base64>", "port": 0, "has_character": true, "slot": 2,
  "host_secret": "0123456789abcdef" }
```

* **password**: Windows DPAPI (`CryptProtectData`, this Windows user, fixed extra entropy) as
  `password_dpapi`; only this Windows user on this PC can read it back. A hand-written plain
  `"password"` is accepted and re-stored protected. Never in the environment, `launcher.log` or
  `party_relaunch.bat`. (On Linux it is stored in clear text; the page says "stored protected"
  only where it is.)
* **host_address**: a party code (BBP3-, or an older BBP2- / BBP1-) or `host[:port]` (checked with `decode_party_code` when typed).
* **host_secret**: the host's party-code secret, reused by menu runs (`BB_PARTY_MENU=1`) so the
  code friends saved stays valid next session (as long as the public address and port stay the
  same). A plain `host:port` address carries no secret: it joins a host **that has a password**
  with that password alone (PartyLink: the host also accepts the key derived from the password and
  an all-zero secret, only when its password is set; `party-link-test`
  `test_plain_address_password`). Without a password a guest needs the party code.
* **has_character / slot**: set when a party character reaches the world in a party session
  (the game's last-used slot, `[[0x553B130]+0x50]+2000`); *Continue party game* is offered once it
  is true. The game's own *Continue* in the party process loads the party save's last-used slot,
  which is the party character (the party saves hold no other).
* **role**: the last party role; *Continue party game* uses it.
* **name**: the party name (online id); chosen automatically on the first party action (section 1).

## 5. Saves: the party save is the only co-op path

* `runtime_savepolicy_dir()`: every party session (any non-empty `BB_PARTY`: host, join or a
  code) plays from `user\savedata_party`; `BB_PARTY_SAVE=shared` is ignored (logged). The
  single-player *Continue* / *New Game* always use `user\savedata`.
* The party save is mounted from the start of the party process (the save root is chosen before
  the title mounts the saves), so a party action never sees a solo character, and the
  progress / item sync (C2/C3) can only write into party characters.
* Party actions in the world are offered only with the party save loaded (`PartyActionsAllowed`).
* Backups of the party save at every party start and every N minutes stay on (save policy C5).
* The launcher's *Separate party saves* is always on (the checkbox is kept, disabled); the
  "copy single-player save" button and first-run offer remain for players who want their solo
  hunter in the party.
* The test harness copies its test saves into `savedata_party` too (`instances.py`), so
  `--party` runs keep their characters.

## 6. Tests

* `party-flow-test`: party.json round trip (DPAPI on Windows, plain fallback, bad values dropped),
  every action's plan in each context (solo title / solo world / party host / party guest),
  the relaunch file's escaping.
* `BB_PARTY_FLOW_TEST=<action>[@seconds]` (`new`, `join`, `continue`, `host`, `leave`, `solo`;
  `world:` prefix = in the world): runs that action once, as the row would – the in-game
  verification below uses it besides driving the menu with the pad.
* **The real menus, live** (`tools/mp/flow_live.py fresh|continue --root DIR --game DIR
  [--guest-world SCRIPT]`): two instances start as the single-player game; `BB_TEST_MENU_FILE`
  gives each process start one line (`title|world <seconds> <MenuScript>`, or `-`; the run.bat
  relaunch loop's next process takes the next line) and the walk goes through the game's own pad
  path (`runtime_pad_director_press`). MenuScript (`test_newgame.h`): `~button@text` presses once
  `text` was looked up since the previous press (the title menu and the page rows look their texts
  up when built - the help lines too, so the focused row cannot be told from its help: the
  walks count rows, the screenshots check them), `type=TEXT` / `enter` / `esc` reach the
  window's text box as SDL key events (`WindowSDL::TestType`: no OS input, no focus), `options`,
  `mark=NAME` (a named pause: screenshot points). Character creation after the restart:
  `BB_TEST_NEWGAME=party` / `BB_TEST_PROLOGUE_SKIP=party` (only in the party process) with
  `BB_TEST_NAME` (the name box). Screenshots: `ROOT/shots/<phase>/` (`shot.py --watch`).
  `joined <s>`: in the world once the party is together (summoned / members in). Navigation
  (English, 1.09): title of an empty save *New Game / Party / System / Log In*; the
  solo title's Party page opens on *New party game*; in the world *Options, Right, Right, Cross*
  (System) and *Up* x8 = *Party* (the list ends *Party, Party mode, Party members, Party rules,
  Enemy scaling, Brightness, Network, Exit Game*; `flow_live.to_party_page` counts them).

Evidence of the in-game runs: §7.

## 7. Verification

**Single instance, the real menu** (`instances.py run --count 1`, single-player start, pad driven
through `BB_PAD_FILE`, 2026-10-10): the title menu shows *Continue / Load Game / New Game /
**Party** / System*; *Party* opens the page with *Status: Single player*, *New party game*, *Join
party game* (*Set address first*), *Continue party game*, *Host address*, *Password*. Two presses on
*Continue party game*:

```
Party flow: continue (solo, single-player save, title): Restarting into the party game (host)
Party flow: relaunching (exit code 75, .\user\party_relaunch.bat)
Runtime: ending with exit code 75: run.bat starts the game again
Party: starting the party game (host)
Patches: party co-op: Party: Skip Online/Offline Choice (Online), Party: Bells anywhere, ...
Saves: party session with separate saves: using .\user/savedata_party (the single-player saves in .\user/savedata are not touched)
Party: hosting on port 48000 as Hunter0; code (Internet): BBP3-NJR0-...
Party: auto-continue: party link up; confirming Continue on the title (party mode)
Party: auto-continue: world up after 2 press(es)
Party flow: party character in the world (save slot 0): Continue party game will load it
```

party.json afterwards: the hand-written plain `"password"` became `password_dpapi`, `host_secret`
was stored, `slot` 0. The first run (single-player) checked `user/savedata`; the restarted one
`user/savedata_party` only.

**Two instances, later session** (`tmp/flow2.py`: both start single-player with a party.json -
host: role host, password; guest: role join, `host_address` `127.0.0.1:<port>`, the same password -
and `BB_PARTY_FLOW_TEST=continue@1`, the same code path as the *Continue party game* row). The
first attempt was refused (`wrong password or party code`: a plain address carries no code
secret, and the link key needed it) - fixed in PartyLink (§4). Then:

```
inst1: Party flow: continue (solo, single-player save, title): Restarting to join the party
inst1: Party: starting the party game (join)
inst1: Party: connected to the host as slot 1
inst0/1: Party: auto-continue: world up after 7 / 3 press(es)
inst0: Party: ringing the Beckoning Bell (host ready, a ready party member waits)
inst1: Party: session trying-to-join (4, sub 0) -> client (6, sub 0)        <- summoned
```

**Two instances, a fresh party** (`tmp/flow4.py`: empty party saves; host role host, guest role
join; `BB_PARTY_FLOW_TEST=auto@1` = *New party game* / *Join party game*; the test aids
`BB_TEST_NEWGAME=1`, `BB_TEST_PROLOGUE_SKIP=1` make the characters and skip the prologue):

```
inst0: Party flow: new (solo, single-player save, title): Restarting into the party game (host): choose New Game
inst1: Party flow: join (solo, single-player save, title): Restarting to join the party: choose New Game
both:  Saves: .\user/savedata_party has no saves yet: the party game starts without characters
both:  Party: newgame: ... contract -> finish: name entered ... -> opening -> done
inst0: Party: session trying-to-host (1, sub 0) -> host (3, sub 4)
inst1: Party: session trying-to-join (4, sub 0) -> client (6, sub 0)        <- summoned
```

With the event-driven new-game driver (screen changes from the message lookups) the same run
reached the summon in 61 s of harness time instead of 90 s, without a retried press.

**The real menus, two instances, 2026-10-11** (`tools/mp/flow_live.py`, no BB_PARTY_FLOW_TEST, no
OS input; screenshots in `<root>/shots/<phase>/`):

* *fresh* - both start single-player on empty saves. Host: title *Party* -> *Password* (text box,
  shown as asterisks) -> *New party game* (twice) -> exit 75 -> run.bat restarts it hosting
  (`party name vp797-rz2z (automatic)`) -> New Game (test driver) -> hosting. Guest: *Party* ->
  *Host address* `127.0.0.1:<port>` -> *Password* -> *Join party game* (twice) -> restart ->
  `joining 127.0.0.1:48660 as vp797-jime` -> New Game -> `trying-to-join -> client` (summoned,
  52.9 s after its restart). party.json: `password_dpapi` only (no clear `password`), role,
  host_address, name, has_character, slot, the host's host_secret.
* *continue* - both start single-player again: *Party -> Continue party game* (twice; the guest first
  reopens *Host address* twice: Enter keeps the saved address, Esc cancels) -> restart -> the party
  character loads by itself -> summoned 23 s after the restart.
* *in the world* (guest, same run): *Options, System, Party, Leave the party* (twice) ->
  `the game's session leave: role leaving`, `client -> leaving -> idle` in 0.4 s, the load into its
  own world; reopened Party page: *Status (Not running), Host party, Join party, Single player* ->
  *Join party* (twice) -> `Rejoin: connecting`, `context registered again with the host`, on the
  host `vp797-jime registered with the host: resolving its pending connection again`, `signaling
  resolved` -> summoned again (t 135 s).

Bugs found and fixed on the way: every menu player was "Hunter" (the second friend refused);
Once rows needed two presses for a second edit; a guest's Leave cut the link under a live session;
after Leave the page had seven rows (Single player cut, Rejoin next to Join party); Join party after
Leave was summoned in vain (no NP context re-registration; a resolving signaling connection killed
by the room's end, and a single resolve attempt).
