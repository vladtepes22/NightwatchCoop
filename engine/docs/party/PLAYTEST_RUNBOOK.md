# Playtest runbook: the 4-PC session on 0.8.0

A script for one real session of host + 3 guests (4 PCs, Max players 4), 60-90 minutes of play, that gives
every row of [ERSC_PARITY.md](ERSC_PARITY.md) a chance to be proven from the logs. After the session the
logs go through `tools/party/session_score.py`, which marks each of the 49 rows PROVEN, FAILED or NOT
EXERCISED and cites the log line. The rows are in route order (fewest travels and loading screens), not
in row order; each block is one row. Plan: Setup (15 min, not counted), then Stop 1 to Stop 7.

Players: **Host**, **Guest 1**, **Guest 2**, **Guest 3**. Best: Guest 3 plays from a second household on
a plain home connection (no VPN) - that alone is what proves F1 and F2. Everyone uses the same release,
0.8.0, and different player names.

## Setup (every PC, before the game starts)

1. **Version.** Everyone installs the same 0.8.0 release (the launcher header shows the version; the host
   reads it out). A different build is a refusal, which is the point of step A9, so do not test it by accident.
2. **Characters.** Anyone with a character past the clinic keeps it. For a brand-new guest character the
   host turns on *System > Party rules: Phantoms > Skip the prologue*, so the character starts at the Central
   Yharnam lamp (the real prologue is not part of this run). The host's character has the Cleric Beast and
   Father Gascoigne alive and the Central Yharnam and Cathedral Ward lamps lit (back up the saves first,
   as for any playtest). Leave the Hunter's Dream's other lamps alone.
3. **Max players.** Host: launcher *Party* tab, *Max players* = **4**. Guests take the host's value when they
   join; check that the Party page shows "4 players" on every PC.
4. **Diagnostics (launcher > Advanced / Debug).** On **every PC**: *Party logging* = **Verbose** (the same level
   everywhere). Also on: *Boss log* on the host, *Network receive gap log* on Guest 3, *Event sync log* on
   the host. Nothing else; these are off by default and cost almost nothing.
5. **Toggles for this session.**

| Who | Where | Switch |
|---|---|---|
| Host | System > Party rules: Rot | **Rot on death** ON (C4; leave the three cleanse options on) |
| Host | System > Party rules: World | **Party votes on travel** ON; the host turns it OFF again after block B3 |
| Guest 1 | System > My party: Chalice | **Ritual Altar as a guest** ON (G1) |
| Guest 2 | System > My party: Display (or the launcher's game settings) | **Auto-hide HUD** ON and **Party name tags** = name + level (E2) |

6. **Save hashes (A2), on every PC, before the game starts.** From the bbport folder:
   `python\python.exe -I scripts\save_hash.py . -o savehash_pre.json`. Keep the file. Do not start the game
   before this, and run the same command with `savehash_post.json` after the last quit (the final step).
7. **Screenshots.** Any tool. Name files with the row id at the start: `a8_system_menu.png`,
   `e2_party_hud.png`. Put them in the folder you send.
8. **Mark odd moments.** Press **F9** (or *Party tab > Mark an issue*) the moment something looks wrong.
   Every player's log gets the same numbered line, which makes the logs easy to match up.
9. The host starts the party from the title screen (*Party > New party game / Continue party game*), copies the
   Internet code and password into the group chat. Guests join with *Party > Join party game*
   after the host's game runs. See [COOP.md](../COOP.md) "Start from the title screen".

## Stop 1: the Hunter's Dream (about 20 min, no travel)

Everyone is in the host's Dream. Order below is the order to do things in.

### A9 Version check (mismatched guest, 2 min)
- **Who:** Guest 3, before joining.
- **Do:** In the launcher switch one gameplay patch or mod away from the host's setting (any option that
  changes gameplay; the launcher's party help lists them), then press *Join party game*. Read the refusal. Switch the patch back and join
  properly in the next block.
- **Look for:** a refusal "version mismatch with the host" that names the patch, not a silent hang.
- **Proof marker:** Guest 3's log `refused by the host: version mismatch ...` (scorer: a version-class
  refusal, net_doctor `refused`); every log also has the `version check: eboot ...` line.

### F3 Security: a stranger is refused (2 min)
- **Who:** Guest 3, right after A9 (this stands in for a stranger: wrong password, once or twice only).
- **Do:** Join with a wrong password. Then the right one.
- **Look for:** "wrong password or party code", the host's game unchanged.
- **Proof marker:** Guest 3's log `refused by the host: wrong password or party code` (a password-class
  refusal). Do not try more than 3 times: 6 wrong tries lock the address out for 30 s.

### A1 Friends-only, password, no randoms (3 min)
- **Who:** Guests 1 and 2 first, then Guest 3 with the right patch and password.
- **Do:** Join by party code + password. Guest 3 joins from the second household.
- **Look for:** each guest lands beside the host in the Dream; nobody random appears.
- **Proof marker:** guest log `Party: link: connected: slot N`; host log `Party: link: <name> joined (slot N, from ...)`.

### A7 Player count, 4 PCs (while joining)
- **Who:** all.
- **Do:** all three guests in the Dream together. Open *System > Party > Members*.
- **Look for:** four rows, slots 0-3, pings and maps.
- **Proof marker:** host log `Party: cooperators N -> 3` and a roster line with `(slot 3, ...)`.

### A4 Join speed and E4 Fast summon messages (while joining)
- **Who:** Host watches a stopwatch for each guest.
- **Do:** note seconds from "guest world loaded" to "guest beside the host".
- **Look for:** a few seconds, no repeated bell banners.
- **Proof marker:** guest `Party B2: guest: re-join done after N s`; host `direct summon: slot N (...) reports it joined`.
  E4 also: `Fast multiplayer messages: applied` in every log.

### A8 Settings in game (every System / Party page, 3 min)
- **Who:** Host and Guest 1, one page each at a time, then swap.
- **Do:** open every System and Party page (Party rules: World, PvP & lamps, Deaths, Rot, Enemies, ...;
  My party: Giving, Display, Chalice, Chat & markers, ...; Advanced: Shaders). Screenshot each page.
  Host changes one rule per page and sets it back; guests see the host's rules as read-only rows.
- **Look for:** no blank rows, no cut text, no crash, a guest cannot edit the host's rules.
- **Proof marker:** screenshots named `a8_*.png` (scorer), plus `Game menu: page N rows` lines when the menu
  trace is on.

### E2 Party HUD and names
- **Who:** Guest 2 (Auto-hide HUD, name + level tags), everyone looks.
- **Do:** stand in the Dream, then walk next to the others; screenshot the HUD on every PC once.
- **Look for:** names above companions, the party panel, HUD fades by itself (Guest 2), nothing over the
  boss bar later.
- **Proof marker:** `Party HUD: tag <name> out from behind this player` lines; screenshots `e2_*.png`.

### E1 Rest / level / shop for guests, and H5 character freedom
- **Who:** Guest 1 and Guest 2 together, then Guest 3.
- **Do:** each guest talks to the Doll (level up if you have echoes), buys something from a messenger, uses
  the workshop and the storage, changes a gesture or clothes.
- **Look for:** each guest has its own shop stock ("already bought" never comes from another player's
  purchase), levels and items stay in the guest's own character.
- **Proof marker:** guest log `Party guest services: ...` service lines (Doll level-up, shop), host log
  `Party gestures: the host's gestures for the party` (H5).

### D8 NPC dialogue synchronised
- **Who:** Guest 1 talks to Gehrman and the Doll; the host talks to Gehrman.
- **Do:** one full conversation each, including a choice.
- **Look for:** the others hear the lines ("shared line" captions), the talk state does not reset.
- **Proof marker:** host `npc_talk: line from <name> (slot N)`; guests `npc_talk: shared line from <name>`.

### G2 Insight and the Messengers' bath
- **Who:** Guest 2.
- **Do:** after the host's first Insight gain later (Stop 3) check the guest's Insight went up; here just open the
  Insight messenger and buy one thing with Insight.
- **Look for:** Insight shared as set (host *Insight for bosses* rule), purchase works for the guest.
- **Proof marker:** guest log `Party: Insight N -> M`.

### A3 Start / join in game (Leave and Rejoin), A6 Leave cleanly
- **Who:** Guest 2 presses, Host watches and times it.
- **Do:** *System > Party > Leave and Rejoin* (Leave). Wait until the host's log shows it (host counts seconds). Then
  rejoin from the same page.
- **Look for:** no "Session lost" message, the guest is home in a few seconds, the host sees it leave in
  under 3 s, the rejoin lands beside the host.
- **Proof marker:** guest `Leave: left the party (BYE sent; no reconnects)`; host `link: <name> left the party`,
  then `link: <name> rejoined` or `direct summon: ... reports it joined`.

### H2 No repeated summons (all stop long)
- **Who:** everyone; read once at the end of Stop 4.
- **Do:** nothing extra: after each travel nobody should have to ring a bell again.
- **Look for:** no bell banners after travels, only after a crash.
- **Proof marker:** `banner N hidden (a re-join, not a new summon)` in the host log.

## Stop 2: Central Yharnam (about 15 min, one party lamp travel)

### B3 Fast travel (lamp to lamp, travel vote) and B4 Loading screens
- **Who:** Host presses the lamp in the Dream (travel vote ON), then everyone votes.
- **Do:** Host takes the party to Central Yharnam. First round: the guests vote yes. Second round: Guest 1 votes
  no (travel cancelled with a notice), then a third time everyone yes. Afterwards the host turns **Party
  votes on travel** off. Count the loading screens on every PC for the one travel that happened.
- **Look for:** exactly one loading screen per travel per PC, a cancel notice when someone says no,
  nobody left behind.
- **Proof marker:** `Lamp travel: arrived: lamp N`; guests `guest: host travels (#N lamp ...`; `Party vote: round N
  over (...)`; `Party: loading screen up` counts per PC (the scorer prints them).

### D2 World state shared (lamps, shortcuts, doors)
- **Who:** Guest 3 lights the Central Yharnam lamp if it is unlit, the Host opens the shortcut door near the
  lamp, Guest 2 watches.
- **Do:** open one shortcut door and light one lamp as a guest.
- **Look for:** the guest gets a *Light lamp* prompt at an unlit lamp; the door is open for everyone on
  screen, not only for the opener.
- **Proof marker:** host `Party progress: host flag N = 1 (shortcut_door|lamp_unlocked)`; guest `guest applied N flags in load mode`.

### D3 Guest actions change the host's world (levers, ladders)
- **Who:** Guest 1 pulls Central Yharnam's lever ladder; Host stands far away.
- **Do:** guest lowers the ladder; everyone climbs it.
- **Look for:** the ladder is down on every screen at once.
- **Proof marker:** host `Party: world: host: slot N changed the host's world`. (Failure marker for the lift
  part, Stop 6: `lift ride: riding an object ... with no table car playing`.)

### D4 Items and pickups (once each)
- **Who:** all; Guest 2 picks up a world item, the Host picks up a different one.
- **Do:** each player collects the same item pickup once; open an item chest if there is one.
- **Look for:** each player gets its own copy exactly once, an item-get dialog, no "given again" message.
- **Proof marker:** `Party items: gave #N lot N ...` on the guest, `Party items: award lot N` on the host.

### B1 No tether and B5 Independent travel
- **Who:** Guest 3 walks off alone to the far end of the street; Guest 1 uses the lamp to go to the Dream
  alone and comes back to the host's area through the lamp.
- **Do:** split the party on purpose for two minutes, then merge.
- **Look for:** no invisible walls, no forced loading screen on the others; the lone traveller loads once,
  alone; the host's world does not load.
- **Proof marker:** host `host: slot N travels alone into (another|this) area`; guest `guest: travels alone into the host's area` or `arrived alone at`.

### B6 Scripted transitions and the Hunter's Mark
- **Who:** Guest 2 uses a headstone; Host uses the Hunter's Mark from the Dream or a story warp.
- **Do:** accept "Travel with the party?" once, decline once.
- **Look for:** one question per transition, one loading screen when you accept, the session stays up.
- **Proof marker:** `host travels (#N transform` or `loading #N transform` in a guest log.

### C1 Guest dies alone, H1 No unnecessary loading screens, H4 No unnecessary respawns
- **Who:** Guest 3 dies to an enemy, then falls off a ledge (Great Bridge side); Guest 2 dies on a ladder.
- **Do:** let it happen once each, away from the host.
- **Look for:** the guest gets up at its lamp with **no loading screen**; the camera follows; the enemies killed
  before stay dead for everyone; the host has no load.
- **Proof marker:** guest `guest: died alone: revived at its own last lamp N`; host `slot N was revived in place (no load)`
  (C1, H1); host `kill record: N kill(s) seen on slot N` (H4).

### H3 No progress lost, and D9 A rest resets the world
- **Who:** Guest 1 rests at the lamp; the host walks on.
- **Do:** one rest at the Central Yharnam lamp by a guest in the party's area.
- **Look for:** enemies respawn in the host's world once (one load for everyone); gestures, items, flags
  all still there afterwards.
- **Proof marker:** guest `guest: HP N after the load (a rest travel ...`; host `kill record`; guest `guest applied N flags in load mode` (H3).

## Stop 3: the Cleric Beast (about 15 min)

Enter like players do: the host walks in; guests go through the fog. Use the host's Boss log.

### A11 Joining during a boss fight (Guest 3 arrives late)
- **Who:** Guest 3 takes the lamp to the Dream during the fight, then asks to come back after 1 minute.
- **Do:** Guest 3 tries to rejoin while the boss bar is up, again after the kill.
- **Look for:** Guest 3 waits outside the fight (a notice); nobody is dragged into the boss room; after the
  kill it arrives beside the host.
- **Proof marker:** host `the host's hold: boss` then `the host's hold: none (its bell rings again)`;
  guest `direct summon: ... refused (the host is in a boss fight ...`. (A5: `host: slot N arrived` for the
  late arrival.)

### B2 Fog and boss gates, a guest first
- **Who:** Guest 1 walks through the fog before the host.
- **Do:** first attempt: Guest 1 steps through the fog first.
- **Look for:** the fight starts once, for everyone, the host is not shut out, no fight starts twice.
- **Proof marker:** host `boss fog: slot N went in`; guest `went through boss fog (event N)`.

### D7 Enemy scaling and C4 Death penalty (rot is ON)
- **Who:** everyone fights; Guest 2 dies once.
- **Do:** note the boss HP versus a solo fight (a bit more with 4); on a death a rot stack appears; kill
  the boss to cleanse.
- **Look for:** rot counted on the dead member, cleansed at the boss kill (notice "Rot cleansed: boss defeated").
- **Proof marker:** `Party balance: boss chr N: SpEffect N ...` (D7); `death N; rot on, N stack(s)` and
  `rot: a boss died ...` (C4).

### C5 Spectate while dead and C2 Host dies alone
- **Who:** Guest 2 dies mid-fight (spectates). In the second attempt the Host dies mid-fight while the guests live.
- **Do:** let both happen.
- **Look for:** the dead guest's camera follows a living member and does not hop; the host's death waits
  in limbo, the boss turns to the living hunters, nobody is stuck in a menu; the host gets up in the arena
  after the kill, with no load.
- **Proof marker:** `spectating: following slot N` and `spectating ended`; host `host death taken: no stage
  request, no party move` and `host revive #N: after: hp N`.

### C3 Party wipe
- **Who:** all die on purpose in a second attempt (or let the boss win).
- **Do:** wipe once, then walk back through the fog.
- **Look for:** one loading screen for the party at the host's lamp, the boss reset, nobody hit by a boss
  restarted by itself at the lamp, no stuck "Host dead".
- **Proof marker:** host `host death: every member went down` or `... nobody fights`.

### D1 Boss kills count for everyone
- **Who:** all; kill the Cleric Beast.
- **Do:** the host or a guest lands the kill.
- **Look for:** every player gets the kill, the echoes, the reward item and the lamp; nobody is sent home.
- **Proof marker:** `boss fight: the boss was killed` and guest `boss-clear leave suppressed (guest; party stays)`.
  (Failure marker: `may be stuck`.)

### C7 Rest while another member is in a boss room
- **Who:** Guest 3 stays at the lamp; the others fight Father Gascoigne (Stop 4).
- **Do:** Guest 3 rests at the lamp while the others are in the fog.
- **Look for:** a real load for the party, the fight and the enemies reset, everyone taken out of the room.
- **Proof marker:** a `(a rest travel` line while a boss fight is on (scorer: co-occurrence only).

## Stop 4: Father Gascoigne (about 10 min, same map, no travel if the lamp is adjacent)

### D6 Campaign, as far as it goes
- **Who:** all.
- **Do:** kill Gascoigne with the whole party; nothing special.
- **Look for:** a second boss flag set for everyone.
- **Proof marker:** host `host flag N = 1 (boss_defeated)` three times in all (a single session is partial
  evidence only; the multi-session soak is the real proof).

### G3 Blood Moon / world phase
- **Who:** Host, only if the story reaches it.
- **Do:** skip unless the Blood Moon happens on its own.
- **Proof marker:** `Party story: host #N cutscene 32000000`.

## Stop 5: the Cathedral Ward (about 12 min, one lamp travel)

### D3 Lifts (the Upper Cathedral Ward elevator, car 2421250)
- **Who:** Guest 1 first and alone, then Guests 2 and 3 together with the host.
- **Do:** ride the elevator outside the Altar of Despair up and down, once alone, once together;
  call it from both ends.
- **Look for:** nobody falls through the car; everyone arrives at the same end and nobody dies.
- **Proof marker:** none (success is silence). **Failure marker:** `lift ride: riding an object ...
  with no table car playing` or `the local chr dropped through the moving car`. Press F9 if a rider drops.

### D10 Cutscenes play for the whole party
- **Who:** Host opens the Cathedral Ward door; Guest 3 starts one by itself if possible.
- **Do:** watch a cutscene together; the host skips one.
- **Look for:** all see it, a host skip ends it for all, the HUD hides.
- **Proof marker:** host `Party story: host #N cutscene N (mode N ...)`; guests `Party story: guest #N`.

### D5 Quests, NPCs and story, D11 NPC hunter fights
- **Who:** Guest 2 talks to Alfred (or Henryk at the Tomb of Oedon if time); the whole party fights a hostile hunter.
- **Do:** accept one quest choice; fight Henryk or Alfred with all present.
- **Look for:** the quest state follows the host into each guest's own save; a guest can lock on and hurt
  the hunter; his death counts for everyone.
- **Proof marker:** host `host flag N = 1 (npc_quest)`, guests `npc_talk: shared line from <name>` (D5);
  `Party: npc fights ...` start lines (D11, only when the fight happens).

## Stop 6: link loss and crash (about 10 min, at a lamp)

### C6 Rejoin after a crash and A5 Join any time
- **Who:** Guest 3 kills its game (Task Manager), restarts it, uses *Continue party game*.
- **Do:** crash once while the party stands at a lamp.
- **Look for:** the host holds the slot, Guest 3 returns beside the host in well under a minute, no manual
  code entry.
- **Proof marker:** host `C6: <name> crashed: looking for its game connection` then `link: <name> rejoined (slot N`;
  host `host: slot N arrived` (A5).

### F5 Watchdog and link loss
- **Who:** Guest 3 turns Wi-Fi (or pulls the cable) for 30 seconds, then reconnects.
- **Do:** one real cut, 20-40 s.
- **Look for:** "connection unstable" notice, no instant send-home, the party goes on when the link is back.
- **Proof marker:** host `link: lost <name> (slot N): ...; keeping the slot 130 s`, then `rejoined` or `outage resume: ... heard again after N s`.

### F1 Real internet play and F2 NAT traversal
- **Who:** nothing extra; scored from the whole session with Guest 3 on its own household.
- **Look for:** no freezing, no repeated "link keeps dying" after the cut above.
- **Proof marker (scorer):** a guest joined from a public address in the host log, and net_doctor reports no
  freezing / dying link. F1 is FAILED if net_doctor finds one.

## Stop 7: a chalice dungeon (about 8 min, two loads)

### G1 Chalice dungeons in party
- **Who:** Guest 1 (Ritual Altar as a guest is ON) at the Makeshift Altar in the Dream; everyone present.
- **Do:** perform the ritual with a chalice the guest owns; enter the dungeon; one lamp-to-lamp walk, return.
- **Look for:** the dungeon starts for everyone with one load, nobody left in the Dream, the guest's chalice
  is the one used.
- **Proof marker:** host `altar: the party's dungeon is loading` and guest `map ... -> m29_00`.

## Finish (every PC)

1. Quit the game; wait for the launcher.
2. `python\python.exe -I scripts\save_hash.py . -o savehash_post.json` (A2 needs both hash files).
3. Launcher > Advanced / Debug > **Save support bundle**. The zips carry the logs with passwords, codes and
   addresses removed; they do **not** carry saves.
4. Send one folder per PC (support bundle zip, `savehash_pre.json`, `savehash_post.json`, screenshots)
   to the person who runs the scorer.

### A2 Separate co-op save (no game step)
- **Who:** every PC, with the two hash commands of Setup 6 and Finish 2.
- **Look for:** nothing in the game; the single-player save was never touched.
- **Proof marker:** `savehash_pre.json` / `savehash_post.json` pair (scorer): every file under
  `savedata` identical, `savedata_party` allowed to change. A changed single-player file is FAILED.

## Scoring (the person who collects the logs)

```
python -I tools/party/session_score.py <folder with all PCs' folders> --md score.md --json score.json
python -I scripts/net_doctor.py --redactnames --redactips <the logs>      # the network view
```

The scorer lists every row as PROVEN, FAILED or NOT EXERCISED with file:line. A FAILED row is a bug report:
attach its log lines (IPs and names are masked in the scorer's output). An unexercised row means the step
was skipped or its marker never appeared; run that block again in the next session. Scores in
ERSC_PARITY.md are changed by hand from the table, never by the scorer.

### Row list (every row has its block above)

A1 A2 A3 A4 A5 A6 A7 A8 A9 A11, B1 B2 B3 B4 B5 B6, C1 C2 C3 C4 C5 C6 C7, D1 D2 D3 D4 D5 D6 D7 D8 D9 D10 D11,
E1 E2 E4, F1 F2 F3 F5, G1 G2 G3, H1 H2 H3 H4 H5.
