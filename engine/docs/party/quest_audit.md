# Quest audit: the whole campaign against the party sync (2026-10-14)

Static audit of every co-op relevant event of Bloodborne 1.09 (EMEVD event scripts, ItemLotParam,
ObjActParam, NpcParam, talk ESDs, MSBs) against what the party code does today
(`windows-upstream-0.5` after 0.6.1 packaging). Goal: find before the next Internet session
(the host and the guest, continuing from Old Yharnam) the key items, doors, warps, fog walls and
NPC steps that can break co-op. Nothing here was run live (except P1-P7 and P9, marked live); "verified" means checked in the data
and the code, not on screen.

How it was made (repeatable, game data read-only from an agent game copy):

* `python -I tools/party/flag_tool.py emevd GAME/event --all-ops --json OUT` (every event, slot,
  flag write, item lot, cutscene, map move), plus a scratch pass that also records item checks
  `3[4]`/`3[16]`, item removal `2003[24]`, chr warps `2004[3/40/41/42]` and insight checks `3[26]`.
* ItemLotParam rows whose items are goods 4000-4999 / chalices (key items), joined with the
  awarding events and `party_items.inc` (ledger / lot flag tables) and goods names from
  `msg/engus/item.msgbnd.dcx`.
* ObjActParam rows with `spQualifiedType = 1` (doors that need a key item).
* MSB enemy parts with a talk id (talk NPCs) against NpcParam byte 0x145 (ghost bit) and
  `party_npcghost.h`.
* Talk ESDs scanned for the NPC gift trigger flags 9042-9077 (common event 9040).
* Code read: `party_flags.inc` categories and the C2 policy (`party_progress.cpp`), the world
  mirror, C3 items (`party_items.cpp`), world actions (`party_worldact.cpp`), split travel
  (`party_split.cpp`), travel capture/replay (`party_travel.cpp`), host death (`party_b2.cpp`),
  story dedupe (`party_story.cpp`).

---------------------------------------------------------------------------------------------

## Next session checklist (Old Yharnam -> Blood-starved Beast -> Cathedral Ward -> Hemwick / Forbidden Woods -> Byrgenwerth)

The ten things most likely to go wrong in the next ~5 hours, and what to do in game.

1. **Snatchers (kidnappers) appear as soon as the Blood-starved Beast dies** (m24_00 12400791 /
   m27 1270501: on while flag 9453 "Blood-starved Beast killed" is set and the Blood Moon 9802 is
   not), in Cathedral Ward and the Forbidden Woods. In the game data only the **host** can be
   kidnapped (12400780 / 12700500 need chr type 0); since 0.7.3 (kidnapall, host rule
   `party_any_kidnap` "Anyone can be abducted", on) a guest's grab takes the party too (below).
   The host's grab-death sets 9420, common 9421 moves the host's respawn point to the Hypogean Gaol
   (ReturnPoint 2802959, Yahar'gul) and the host's death becomes the party's respawn load; the
   guests replay it as a lamp warp to the host's last lamp record, which is then the Gaol
   (**P7 done**: `2003[23]` is the dispatcher case 0x17C10A5, it writes GSM +0x1528 = {row +4, row +8}
   of the ReturnPointParam row; live: host `respawn load for the whole party at lamp 2802959`, guest
   `replay ... host_death ... last lamp 0x2ac50f via lamp warp`, one loading screen, guest placed
   0.5 m beside the host in the cell).
   *In game:* everyone wakes up in the Hypogean Gaol (a cell, Yahar'gul) with one loading screen;
   the host sees the waking scene, the guest's loading screen waits for it ("Waiting: the host is
   watching a cutscene"). Avoid the grab if you do not want to go to Yahar'gul yet: kill Snatchers
   from range or let the guest tank them.
   *Fixed here:* 9420 is no longer copied into the guest's own save (it would have sent the
   guest's next solo death to the Gaol). The guest's own save gets 9421 / 9423 (key_event: "kidnap
   done") and, once it goes home and rings again, the Gaol as its own last lamp (the bell's gather
   lamp warp to the host's lamp) - the same state as a solo kidnap, never a pending one.
   *Fixed (kidnap071, 2026-10-21, live, smoke group `kidnap`):* 0.7.0's default host revive
   (`BB_PARTY_B2_HOST_REVIVE`) had turned the kidnap into a lone load (the Gaol is in another map:
   `the host loads it alone (the guests stay)`) and the guests, still in the Cathedral Ward, got
   the host's waking scene 28000010 as a live mirror without the cell's scenery (a player report;
   reproduced on master). A scripted death warp (`deathrules::ScriptedDeathWarp`, see "Scripted
   warps into another map") is now always the party's respawn load, and a live mirror plays only
   in its own map (`MirrorMapOk`; a missed scene's summoned replay waits for its map too).
   *Fixed (parg, live):* the host's +1 Insight at the end of 9422 (SpEffect 4680, host only) now
   reaches every guest present (`DirectInsight`, party_phantom.h): live, host 6 -> 7 at the end of
   the waking scene, `event 9422 gave the host 1 Insight`, guest `+1 here`, 14 -> 15.
   *Added (kidnapall, 0.7.3): anyone can be abducted.* The grab event, decoded
   (`m24_00 12400780`, 4 slots for Snatchers 2400872..875; `m27 12700500` for 2700680): `4[3]` chr
   10000 type 0 or end; `4[0]` the Snatcher alive, `4[1]` chr 10000 damaged by it, `4[2]` its HP
   ratio 0, `3[0]` 9401 and 9404 on; then `2003[2] 9420`. So the grab is a death by the Snatcher's
   hit that the event recognises; on a guest (type 1) the copy ends at the gate and the death is an
   ordinary phantom death. With the host's rule on, a summoned guest's copy runs past the gate (its
   own chr type is written into the 4[3] argument, party_b2 `KidnapInstr`, rules in
   `party_kidnap_rules.h`), its own 9420 write is skipped (never into the guest's flags or save, F1)
   and the host is told (`b2` op `kidnap`). The host (9423 off, in m24/m27, alive, not in a boss
   fight) sets its own 9420; its common 9421 moves the host's respawn point to the Gaol 2802959 and
   clears 9420, as for its own grab; then the host makes the party's travel there (a decided lamp
   warp to 2802959, no rest): one loading screen for everyone. The grabbed guest's follow waits for
   its own death as at a wipe (`PlanWipeFollow`: its bloodstain where it fell, then a dead follow to
   the cell). 9422 then plays the waking scene on the host's arrival. Off: as before (the gate is the
   data's, the guest's death a plain death). Smoke `kd` (group `kidnap`): the guest grab first
   (test aid `guest_die:kidnap`, what the copy does at the grab), then the host's grab as before.
   *Changed (kidnapall, user decision 2026-10-22): the waking scene for everyone.* 0.7.1's "guests
   skip 28000010" is gone: every guest plays the scene as its summoned replay once it stands in the
   cell (its loading screen waits through the host's copy; a live mirror is still dropped outside
   m28); 9423 with it, the +1 Insight by the host's relay as before.
2. **Blood-starved Beast reward (Pthumeru Chalice)**: lot 80000000, flag 5000 (global) -> shared
   with every member through C3; the guest's item box waits for the boss banner to end. No intro
   cutscene, so the guest may go through the fog first (B2). *If the chalice is missing on the
   guest:* Leave + Join party (the rejoin resends the full item list); check the guest's log for
   `lot 80000000`.
3. **NPC gifts when the guest is the one who talks** (Djura's Powder Keg Hunter Badge in Old
   Yharnam, Eileen's badges, Alfred, Gilbert's Flamesprayer, the Oedon Chapel Dweller, the old
   woman's / old man's gifts, Patches). **P1 done (live):** the talk script's trigger flag
   (9042-9077) set on the guest goes to the host as a world action; the host's common event 9040
   awards the lot and C3 gives it to every member once. Live (Eileen, Cathedral Ward, test save):
   before the fix the guest heard "Here, to welcome the new hunter", got only the gesture, no Bold
   Hunter's Marks, and Eileen's state 72400520 moved on in the host's world (gift lost for
   everyone); with the fix guest log `NPC gift trigger 9074 set by its talk: to the host`, host
   `award lot 35000`, guest `gave ... lot 35000 ... goods 1400 x4 (held 2 -> 6)`, both saves 1400 x6,
   the host's own talk afterwards gave nothing more. *In game:* either player may talk.
4. **Vicar Amelia: either player may start the first fight (P5, live).** Her intro (12401802,
   remo 24000060) runs only on the host (chr type 0). A guest may now go through her fog first:
   the host's game runs the intro event at once (the host's character visits the event's region
   for a moment and is put back), the scene plays on every screen, then the fight is on. *In
   game:* whoever is at the fog goes in; the host walks in through its own fog after the scene.
   Live 2026-10-15 (pard S1d): guest through the fog 165.6 s, host scene 166.0 s, guest's live
   mirror 166.0 s, guest in the fight with Amelia's health bar while the host stood 217 m away.
   Reward lot 50000001 (goods 4001) is a ledger lot -> shared.
5. **Amelia's skull (evening -> night)**: 12401803 runs only on the host. *In game:* the **host**
   examines the skull at the altar; the guest sees the scene as a live mirror and its time of day
   is caught up. If the guest's world stays evening, it changes at its next summon (time-of-day
   catch-up).
6. **Hunter Chief Emblem** (Cathedral Ward corpse, lot 2400450) opens the plaza gate (12400760,
   gatekeeper; flag 12400160). Map treasure used to be per player. *Fixed here:* when the host
   picks it up, every member gets it (same for the Rune Workshop Tool in Hemwick and the Lecture
   Theatre Key in Byrgenwerth). The gate opened by anyone stays open for everyone.
7. **Scripted area changes now ask "Travel with the party?"** Every EMEVD `2003[49]` warp goes
   through the lamp warp hook, so the Hemwick carriage to Cainhurst, the Rom -> Blood Moon warp to
   Yahar'gul, the Lecture Building doors, the Amygdala grab and the DLC grab all open the travel
   question on the player who triggered them (after the cutscene). *In game:* answer **YES**. A NO
   leaves the others where they were; recover with the bell or a party lamp travel.
   *Fixed + live (parg, Blood Moon):* these warps set no lamp flag, so since bug 20's "whose lamp"
   rule the host's Blood Moon warp was dropped as another member's lamp (the host stayed at
   Moonside Lake). Story warps (`deathrules::StoryWarpEvent`) are now the triggering player's own
   travel (host-only ones only on the host: a guest's stray copy after its load stays dropped), and
   the question waits 2 s after the scene (opened in the scene's last frame it never showed). Live:
   question, YES, host and guest in Yahar'gul with one load each.
8. **One-off host-only story steps**: the Blood Moon (Rom dead + host near Queen Yharnam,
   13201803), Willem's memory (5), first fights with an intro (Amelia, One Reborn, Wet Nurse,
   Micolash, Logarius), the Yahar'gul altar warp to the Lecture Building after One Reborn
   (12800430, `End if Client`). *In game:* the host does them; guests wait or watch.
9. **Lifts and two-state gates** (Forbidden Woods 12700131/132 lift and 12700171/172 gate,
   Hemwick 12200122/123 lift, Cathedral Ward 12400147/148 and 12400824/825 lifts): the guest sees
   the host's lift position (world mirror) and its own lever kicks go to the host. *If a lift
   looks stuck on one screen:* the other player rides it once or pulls the lever; a rest fixes
   everything.
10. **Insight differs per player** (decided per player, as in vanilla co-op; parg audit of every
    `3[26]` Insight check in the EMEVD, 21 events):
    * *Scenery, per player:* Central Yharnam's big Amygdala (12410815, Insight >= 20 or Blood Moon)
      and the Yahar'gul Amygdala (12800401, >= 40 or Blood Moon) are event characters (display
      mask + enable, SpEffect 5686), not fights (no defeat event, no reward; the grab is an event):
      each game shows its own. The
      Yahar'gul grab (12800431) needs the Tonsil Stone on the local player (P4). Whispers (common
      9200, Insight >= 60) and the Dream's music (12100310, >= 50) are sound only.
    * *Decided by the host for everyone:* the Witch of Hemwick checks (12204844 / 12204010), the
      Cathedral Ward checks (12404000 -> 12404001-003, the DLC Amygdala), the Upper Cathedral check
      (12425290) and the Bath Messenger's Insight animation (12105043) run only for chr type 0 / not
      on a client; their zone flags come from the host's world, so both games agree.
    * *Real enemies:* the only Insight-gated enemies are the "madness spirits" (`SAN値で狂気の悪霊出現`,
      12414000 / 12504000 / 12604000 / 12905406 with their despawn twins). Their spawn waits for
      chr type 0 (the host) as well as the Insight, so they exist only in the host's game; a guest
      can neither see, hit nor be hit by them (hits are decided on the victim's game); their kill
      echoes go to the host only (the guest's game has no such enemy to see die), as in vanilla. Only 12905406 is ever started
      (chalice layout m29_01_00_56); the m24_01 / m25 / m26 ones are cut content (no
      InitializeEvent anywhere). Nothing to fix: no enemy whose existence or HP differs while both
      can fight it.
    * *Insight gains:* every host source reaches the guests present: 9350 boss kills (N - 1 on top
      of the native +1) and first encounters / story (N), the kidnap's +1 (9422) and the Chalice
      Dungeon boss events (12901588/589/591, 12901708/712/718/722), which give Insight with a bare
      SpEffect 4680 outside 9350 (parg). A player's own items (Madman's Knowledge, Great One's
      Wisdom) and the Insight shop are local. Not shared on purpose: the NPC summon cost (xx04410,
      SpEffect 4682) and its refund (9183) are the summoner's own.
    *In game:* nothing to do; one player may hear whispers or see an Amygdala the other does not.

Also keep in mind: the guest should pick up its own copies of ordinary treasure (instanced, D4);
boss rewards, key items from bosses and NPC event awards arrive by themselves.

---------------------------------------------------------------------------------------------

## How the sync handles each kind of event (summary of the code)

| Kind | Host -> guest | Guest -> host | Code |
|---|---|---|---|
| Boss defeated (1AAB1800, 9452-9479) | world mirror (host's world live) + C2 set-only into the guest's own save; a boss of the map the guest stands in is held back in mode 0 | no (only the host's kill counts) | party_progress |
| Boss fight / fog state (T1 zone 4, `boss_area`) | not mirrored (vanilla snapshot only at join) | B2 fog report (`world_action fog`) | party_worldact |
| Doors, ladders, shortcuts (`shortcut_door`), area objects (`world_state`) | mirror + C2 set-only (two-state lifts: `State`) | world action set/clear (lifts: the ObjAct kick only) | party_worldact |
| Lamps (7AAB02xx, 1AAB78x0) | mirror + C2 | lit through the host | party_progress, guest services |
| Key events (9400-9439 minus 9420, KEY_KW events) | mirror + C2 set-only | world action, set only | |
| Cutscenes | live mirror on the guest (C4) + story dedupe; a guest's own scene goes up (`story_up`) | yes | party_story |
| Time of day 9800-9802 | only through the cutscene path and the summon catch-up, upward only | no | party_catchup |
| NPC quest 1000-1999, 9360-9399, 9480-9499, 9700-9799, 7AAB0nnn | Follow (set and clear), whole 20-flag NPC blocks | world action, set and clear | party_progress, party_worldact |
| Item pickups 5AABZnnn (map treasure) | per player (instanced) except `SharedKeyTreasureLot` (new) | the guest's pickup goes to its own save only | party_items, party_worldact |
| Global item flags 5000-5299 / 500000nn, boss and event awards (`2003[4]`) | C3 replay (flag or ledger 60009000+) | gift_up only for hostOnly 0 awards with a flag | party_items |
| Scripted warps `2003[49]` / `2003[14]` | split question (Ask), YES = party travel | a guest's warp becomes a lamp request | party_split, party_travel |
| Host death | party respawn load at the host's last lamp record | guests replay as a lamp warp | party_b2, party_travel |

### Rule audit: "party quest progress is shared by everyone" (user decision 2026-10-15; pard3)

Every kind of progress any member makes must end up in every member's own save, both ways, and stay
there (owed when a summoned save cannot take it yet). Checked kind by kind against the code and the
live runs; lamps are agent guestvals' (not repeated here).

| Kind | Host's progress -> each guest's save | A guest's progress -> host's world and save | Status |
|---|---|---|---|
| Boss kills in the host's world (anyone's hit) | the host's game has the kill; C2 writes the boss flag into each guest's own pool (live O2, 3 players: the headless guest's own save `12801800=1` after it went home) | it is the host's world: the host's own kill | shared |
| Boss kills by a guest alone in another map (split travel) | - | **was lost** (LB2: a lone guest's game ran no defeat - the boss's start and defeat events are the host's, and the host's game does not have that area loaded). pard4 (correct over seamless): such a fight cannot happen any more - the fog keeps the vanilla phantom rule while the host's area is another one (notice "Boss fight: the host is in another area. Travel together to fight this boss"), a guest in a boss room while the host is elsewhere is put back outside (no fight) or loads to its own last lamp (fight on) | prevented, live A2 / A3 / A4 (split_travel.md 6); the boss stays for the party |
| First fights started by a guest (intro, boss start) | host's event, scene mirrored (P5) | the guest's fog report | shared (live: Ludwig, the One Reborn, Amygdala, Wet Nurse, Amelia) |
| Doors, levers, shortcuts, one-way doors | C2 + mirror | world action + ObjAct kick replay | shared (live: Lecture Building door, lever 2411310, P9 doors) |
| Key-item gates (the plaza gate's Hunter Chief Emblem) | C2 (12400160) | **a guest could not open it** (its key items read 0 summoned). NEW: `KeyItemGate`, the guest's own emblem opens it; the host's copy opens by its far-side ObjAct | fixed, live run P1b; again with the own key view (0.7.1, items.md 10.7: the check is no longer inverted when the emblem reads held; run8: "A special emblem opened the main gate" on both, 12400160 = 1 on the host and in the guest's save) |
| NPC quest steps, talk choices | Follow (set and clear) | world action npc_quest (live: Chapel Dweller 72400310, Eileen 72400520); the host saves at once | shared |
| NPC gifts | C3 | P1 / P2 (gift_up, the guest's own grant for repeatable gifts) | shared |
| Story scenes and their flags | live mirror + C4 replay into the own save | story_up; NEW: host-only trigger spots the guest reaches (the Blood Moon, `StoryTrigger`) run on the host's chr | shared; Amelia's skull by a guest live (pard4 B2: the guest's copy runs with its client / chr-type checks skipped, the game passes its press to the host's event, scene 24000030 and night on the host and both guests) |
| Story notices (2007[1] dialogs only the host's event shows) | NEW: relayed (`story_notice`, Yahar'gul's "Ritual secret broken") | - | shared (display only, no flag) |
| Key items, workshop tools, boss key rewards | C3, owed lots for title joiners | shared key treasure (`SharedKeyTreasureLot`); pard4: a guest's own pickup of one goes up as gift_up (the host's world awards it, C3 gives it to every member) | shared |
| Time of day / Insight-world state | C4 (upward), zone flags of the host's world through C2 | the host's own clock / Insight decide | host-led by design (one world) |
| Treasure pickups (non-key) | per player (instanced) | own save only | per player by decision ("World item pickups"), not progress |
| Endings / NG+ | NEW: each guest is asked (YES: the ending in its own save; NOT NOW: home, save unchanged) | the host's own | per player by decision (2026-10-15) |
| Progress a member makes playing solo in its own world (no party) | - | - | not shared: outside a party session each save is its own (as in vanilla and ERSC) |

---------------------------------------------------------------------------------------------

## Fixes in this change (data / tables, unit-tested)

| # | What | Before | After | Where |
|---|---|---|---|---|
| F1 | Snatcher kidnap trigger 9420 | `key_event`: host -> guest set-only. A host kidnap (or any 9420 the C2 scan caught) wrote 9420 into the guest's own save; the guest's next own-world load ran common 9421 (it ends only for clients), set its respawn point to the Hypogean Gaol and the guest's next solo death woke it in Yahar'gul | `never_sync` (override row); 9421-9439 stay `key_event` | `tools/party/flag_tool.py` OVERRIDE_ROWS, `party_flags.inc` (regenerated), tests/test_party_progress.cpp, tests/test_party_worldact.cpp |
| F2 | Progression keys and workshop tools found as map treasure | instanced: only the player who walked to the corpse got it; a guest without the emblem / key / tool was locked out in its own world (and could not open the plaza gate in the host's world either) | `SharedKeyTreasureLot`: Hunter Chief Emblem 2400450, Upper Cathedral Key 2800290, Iron Door Key 2600550 / 2600610, Lecture Theatre Key 3200720, Underground Cell Key 3500800, Rune Workshop Tool 2200360, Blood Gem Workshop Tool 2411000: the host's pickup is replayed to every member (C3, also in the rejoin full list) | `party_items.{h,cpp}` `InstancedTreasureFlag`, tests/test_party_items.cpp; 0.7.5: every key-item treasure and the daughter questline, generated, items.md 10.9 |
| F3 | Eye of a Blood-drunk Hunter (lot 10040, flag 50000100, DLC entry) | denied as a per-player Messenger gift, but its event 12101028 needs chr type 0, so a guest who lives in the host's Dream never got it | replayed from the host like any key item | `party_items.cpp` `ItemLotDenied`, tests |

Badges (Saw, Radiant Sword, Cosmic Eye Watcher, Old Hunter) and messenger cosmetics stay per
player: they only unlock shop stock or looks.

---------------------------------------------------------------------------------------------

## Per area

Columns: **ids** = event / flag / lot; **host** = what the host experiences; **guest** = what a
guest summoned in the host's world experiences under the current sync; **risk** = blocker /
major / minor / ok; **fix** = done here (F#), proposal (P#, list at the end) or none.

### Old Yharnam (m23_00) - current position

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Lamps | Old Yharnam 2302950, Church of the Good Chalice 2302951, Graveyard of the Darkbeast 2302952 (objects 12307800/820/840) | lights | lamps via the host, mirror + C2 | ok | |
| 12 ladders | 12301000-12301011 (2009[0]) | kicks down | mirrored; own-save set-only | ok | |
| Djura (tower) | talk t230323, NpcParam 6300 (solid); NPC state block 1xxx; gift trigger 9071 -> lot 33000 Powder Keg Hunter Badge + Bold Hunter's Mark (flag 50001700) | talk, gift through common 9040 | can talk (6300 has no ghost bit); its 9040 instance ended at load, so its view says "gift given"; its talk's trigger 9071 now goes to the host (P1) | ok (P1 fixed; same path as Eileen, live) | either talks (checklist 3); P1 |
| "Beast guardian" talk suppression | 12300750 / 12300751 (world_state) | | mirrored | ok | |
| Blood-starved Beast | defeat 12301800, host entry 12301802 (no remo), guest entry 12304811, late guest 12301803; sets 2300 (area gate, never_sync), 9453 | fight | first fight: the host's entry is a region (no fog for the host, single-player only `3[6]` state 0 = host), the guest's fog waits for it; P5 (2026-10-15): a guest may go through first, the host's game runs 12301802 (region visit) and the boss starts; re-fights: B2 guest fog first | ok | P5 |
| Blood-starved Beast reward | lot 80000000 Pthumeru Chalice, flag 5000 | gets it | C3 replay (flag 5000), popup after the banner | ok | checklist 2 |
| 9453 side effects | Snatchers (12400791, 1270501), Hunter's Dream / Messenger changes | | C2 sets 9453 in the own save | ok | checklist 1 |
| Darkbeast Paarl | defeat 12301700 (2003[12]), host entry 12301702 (no remo); reward lots 50800000 Spark Hunter Badge (ledger 38), 50800005 | fight | first fight: the host comes within 16 m (3[3]) or a guest goes through the fog first (P5); re-fights: B2; ledger replay | ok | P5 |
| NPC summons (Alfred, Yamamura, Yahar'gul hunter) | 12304400-12304432, need the Old Hunter Bell | signs only in single player | none | minor | none (vanilla) |

### Central Yharnam / Iosefka's Clinic / Great Bridge (m24_01) - done, kept for reference

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Oedon Tomb Key | lot 31000, ledger 25 (Gascoigne 12411800) | boss award | C3 + re-give when held 0 (playtest #16 fix) | ok | |
| Oedon Tomb doors | ObjAct 2410001 / 2410080 need goods 4000 | opens | can open with its own key; door state mirrored | ok | |
| Cainhurst Summons letter | 12410510 (clinic), lot 2410990 flag 52410990, `2003[4]` | award | captured award -> shared | ok | |
| Blood Gem Workshop Tool | lot 2411000 (treasure) | picks up | was per player | minor | F2 |
| Iosefka's Blood Vial (real Iosefka, clinic door) | t241313 -> 72410310 (common 9100 slot 1, lot 23000, ledger 5), gated by 72410301 (npc_quest, shared; cleared by 12410736 at an own-world clinic load) | talk: own vial, C3 to the guests | talk: own vial + gift_up, the host's world awards it, C3 to every member | ok (0.7.3) | items.md 10.8 |
| Fake Iosefka gifts | t241314 -> 9047-9053 (common 9040 slots 7-13, ledger lots) | | same as Djura | ok (P1 fixed, live with Eileen) | P1 |
| Gilbert (window) | t241319 -> 9066 Flamesprayer (lot 29000, ledger 23) | | same as Djura | ok (P1 fixed, live with Eileen) | P1 |
| Gascoigne's daughter | t241320 -> 9067 Tiny Music Box (lot 30000, ledger 24); Red Messenger Ribbon / brooch (12410713, lots 2410910/2410920) | | window talk works (D8) | ok (P1 fixed, live with Eileen) | P1 |
| Central Yharnam lift | 12410321-12410325, state 12410350/351 (`State`) | | mirror; world action refused (lift kick only) | ok | |
| Big Amygdala on the bridge | 12410815 (Insight >= 20, local) | | per player | minor | checklist 10 |
| Moon-presence / Arianna's child (night 3) | 12410833/834 | | npc_quest Follow | ok | |

### Cathedral Ward, Oedon Chapel, Grand Cathedral (m24_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Day -> evening on entering | 12400750 (ObjAct 7030 on door 2401210), remo 24000020 with warp 2402200 and tod 1 | done in playtest 1 | done | ok | |
| Hunter Chief Emblem | lot 2400450, flag 52400450 (treasure) | picks up | was per player | major | F2 |
| Plaza gate (gatekeeper) | 12400760: goods 4011 held by the local player + action 2400030 at 2401220; sets 12400160 (world_state); clears area gates 2400/2401/2405/2406 | opens | opens with its own emblem (now shared); a host-opened gate is mirrored and goes into the own save | ok after F2 | |
| Church lifts / trick doors | 12400147/148 (state 12400150/151), 12400824/825 (12400827/828), trick doors 12400155/156 (12400157), 12400125/126 (12400177/178), stair door 12400174/175 | rides | mirrored; doors that close again stay open in the own save (set-only) | minor | none |
| Vicar Amelia | defeat 12401800 (sets 2400-2402 never_sync), host entry 12401802 remo 24000060 (chr type 0), 9455 | intro + fight | either goes in first: a guest's fog starts the host's intro event (P5, live 2026-10-15); intro on every screen (live mirror) | ok | checklist 4; P5 |
| Amelia reward | lot 50000001 goods 4001 (ledger 35); ObjAct 2400080 needs goods 4001 | gets it | ledger replay | ok | |
| Amelia's skull -> night | 12401803 (host only), remo 24000030 tod 2 | plays, world -> night | live mirror; time-of-day catch-up into the own save (upward) | minor | checklist 5 |
| Snatchers | spawn 12400791 / 12400797 (9453 and not 9802); kidnap 12400780 (chr type 0) sets 9420 | kidnapped -> Gaol | grabbed: the party to the Gaol through the host (0.7.3 `party_any_kidnap`; off: dies normally); follows the host's respawn (live, P7) | ok | F1, checklist 1 |
| Insight checks | 12404000 (host only): Insight >= 15 -> 12404002, >= 40 -> 12404001, >= 50 -> 12404003 (Cathedral Ward variants, DLC Amygdala) | | zone 4 flags: the host's values via the vanilla join snapshot | minor | |
| Oedon Chapel NPCs | Dweller 113010 (t240312 -> gift 9045 Pebble), lonely old woman 6090 (t240316 -> 9054-9057 sedatives), old man 6080 (t240317/t240338 -> 9059-9064 pellets), Arianna 6110-6113 (Blood of Arianna: common 9100, repeatable, denied), Adella (9100, denied), Alfred 6151/6155 (t240333/t240343 -> 9072/9073), Eileen 6161 (t240325/t240341/t240342 -> 9074/9075 Crow Hunter Badge) | talk, gifts | solid since playtest 1; gifts as Djura; Blood of Arianna / Adella: the guest who talks gets its own (P2, 2026-10-15) | ok | P1, P2 |
| Beast beggar (suspicious beggar) | NpcParam 504020 (only this talk NPC uses the row, every MSB checked), t240217 | talk | solid for the guest since P3 (2026-10-15): can talk; turned hostile he is a normal enemy on its screen | ok | P3 |
| NPC "scared" flag | 9432 (key_event; cleared at every m24_00 load by ev 50) | | harmless | ok | |
| Upper Cathedral Ward gate | ObjAct 2400100 needs Upper Cathedral Key (Yahar'gul treasure 2800290) | | key now shared (F2) | ok after F2 | |
| DLC grab | 12400765 shows Amygdala 2400899 at Blood Moon or 12404001; 12405263 needs goods 4311 on the local player; host: 12401000 + `2003[49] 3402959` | grab -> travel question | grab cutscene only (chr type != 0 ends) and needs its own Eye (F3) | minor | checklist 7 |

### Hemwick Charnel Lane (m22_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Lamps | 2202950, 2202951 | | via host | ok | |
| Lift | 12200122/123 (state 12200125/126) | | mirrored | minor | |
| Rune Workshop Tool | lot 2200360 (treasure) | picks up | was per player -> no Memory Altar for the guest | major | F2 |
| Witch of Hemwick (Eye Collector) | defeat 12201800 (2200 never_sync, 9452), host entry 12201802 (no remo), insight checks 12204844 / 12204010 host only; lot 21002950 (ledger 31) | fight | first fight: host region or a guest's fog (P5); re-fights: B2; ledger replay | ok | P5 |
| Cainhurst carriage | 12200130 (appears: local player holds goods 4003 + 12201800 + in region 2202410), 12200131 (action, remo 22000040, `2003[49] 2502959`) | travel question | with its own summons (shared) it can call the carriage too -> its question -> party request | minor | checklist 7 |
| Hostile crowd woman back throw | 12205080 | | | ok | |

### Forbidden Woods (m27_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Lamps | 2702950, 2702951 | | via host | ok | |
| Lifts / gate | lift 12700130-132 (12700134/135), lift 12700140-142 (12700144/145), gate 12700170-172 + 12705490 (12700173/174) | | mirrored, State | minor | |
| Snatchers | 1270501 (spawn), 12700500 (kidnap, chr type 0) | | | major | F1 |
| Shadows of Yharnam | defeat 12701800 (2700/2701 never_sync, 9463), host entry 12701802 (no remo); lots 2700990 (flag 52700950), 2700995 | fight | first fight: host region or a guest's fog (P5); re-fights: B2; captured award (map flag) -> shared | ok | P5 |
| Valtr / League (t270503, 6500), beggar 504030 (t270217, ghost), old man 6100 (t270317 -> 9058 Pungent Blood Cocktail) | Valtr: state 1790-1809, m27 12700901-12700910, T7 72700300-72700339 (covenant_outcome, 0.7.3); Impurity rune 43120 / League Staff 43130 by m27 12700906 (triggers 72700321 / 72700322, personal flags 6676 / 6678) | talk, award | Valtr's quest per player (rule *Covenants per player*); the rune / staff: the guest's own grant (worldact OwnTalkGiftLot, items OfferOwnGift), never a world action; old man gifts as Djura; beggar solid since P3 | ok (0.7.3) | P1, P3 |
| NPC summons (Henryk, executioner) | 12704400-12704431 | | | minor | none |

### Byrgenwerth, Lecture Building, Moonside Lake (m32_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Lamps | 3202950-3202953 (+ script points 3202956-3202959) | | via host | ok | |
| Lecture Theatre Key | lot 3200720 (treasure); ObjAct 3200030 | picks up | was per player | major | F2 |
| Lunarium Key | 13200400 award lot 3200810 (flag 53200810); ObjAct 3200040 | award | captured -> shared | ok | 0.7.6: live in a party (smoke lk); key catch-up on (lkc), items.md 10.9 |
| Willem | 513000 (talk NPC + two cutscene dummies, every MSB checked), t320310; 13200121 points at the moon | | solid since P3 (2026-10-15): the guest can talk | ok | P3 |
| Rom | defeat 13201800 (3200 never_sync, 9465), lot 51001900 (ledger 40); fog door 3201800 (action 3200800, host event 13204830, guest event 13204831 waits on 13201802 + 13204800; the landing in the lake 13204820 / 13204821 region 3202800, sets 13204801); first fight = anyone's first hit on Rom (13201802, 3[23] attacker -1), the wall rises after it | fight (no intro) | guest fog first (0.7.6: the gate was missing from BossGates, a guest had no prompt after the host's first hit; fixed, smoke phase rf) | ok | |
| Blood Moon | 13201803 (host only: Rom dead + near Queen Yharnam 3200801), remo 32000000 tod 3, sets 70002802, `2003[49] 2802958` | cutscene -> travel question | live mirror (a mirror cut by the guest's own load is replayed in full); time of day into the own save; the host's question after the scene, the party travels to Yahar'gul (parg, live) | ok | checklist 7/8 |
| Lecture Building doors | 13200310 (ObjAct 13200124 -> `2003[49] 2602959` Nightmare of Mensis), 13200311 (ObjAct 13200123 -> `2003[49] 3302959` Nightmare Frontier) | question | its own question (both may be asked) | minor | P4 |
| Lecture Building arrival via Amygdala | 13200950 (clears 12800434) | | | ok | |

### Yahar'gul, Unseen Village (m28_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Hypogean Gaol (kidnap) | common 9421 (respawn 2802959; ends once 9423 is on; since 0.7.8 BB_PARTY_GAOL_ALWAYS the host of a party writes the Gaol respawn record itself on every later grab of a GUEST and takes the party there, no 9420, the waking scene only for a guest that has not seen it; a host's own later grab is still the game's), 9422 (remo 28000010 in m28, sets 9423, SpEffect 4680; `End if Client`) | wakes in a cell | follows the respawn with one loading screen, beside the host (live, P7; again live in 0.7.x, kidnap071: the host's revive no longer takes it alone); its loading screen holds through the host's waking scene (the mirror of 28000010 is dropped while it is not in m28, then it plays the scene as its own replay in the cell); the host's +1 Insight of 9422 reaches the guest (parg, live) | ok | F1, checklist 1, "Scripted warps" |
| First-sacrifice multiplayer block | 12800160: SpEffects 9120/9121 while 9802 is off | | cutscenes_endings.md 5.2: unknown effect on summons in m28 | major (untested) | P6 |
| Time of day | 12800400 (sets 2800 never_sync, 70002802), gates 12800402/12800403 by 9802 | | mirrored | ok | |
| Upper Cathedral Key | lot 2800290 (treasure) | picks up | was per player | major | F2 |
| One Reborn | defeat 12801800 (2800, 9464), host entry 12801802 remo 28000000; lot 50700000 (ledger 37) | intro + fight | either goes in first (P5: a guest's fog runs the host's intro event 12801802, region visit) | ok | checklist 8, P5 |
| Altar warp to the Lecture Building | 12800430 (`End if Client`, action 2800020 at 2801500, `2003[49] 3202959`) | question | cannot use it | minor | checklist 8 |
| Amygdala grab (Tonsil Stone) | 12800401 (visible at Insight >= 40 or Blood Moon, local), 12800431 (event message 10 of 2800745 + goods 4310 on the local player, remo 28000040/28001040, `2003[49] 3202958`; **no client guard**) | question | a guest holding the Tonsil Stone runs its own copy too (its own question) | minor | P4 |
| Tonsil Stone | lot 39000 (common 9040 slot 36, ledger 28, done 9076); its trigger 9076 is in no talk ESD (set by another path; Patches' state 9782 checks the stone) | award | ledger replay | ok (who triggers it: unverified) | |
| Patches | t280322 -> 9069 (Madman's Knowledge) | | gifts as Djura (P1 fixed) | minor | P1 |
| NPC summons | 12804400-12804431 | | | minor | |

### Upper Cathedral Ward, Lumenwood Garden, Altar of Despair (m24_02)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Orphanage Key | 12420700 (brain eater drop) lot 2420900 flag 52420900, `2003[4]` | award | captured -> shared | ok | |
| Orphanage door | ObjAct 2420020 needs goods 4006 | | own key (shared) | ok | |
| Lift to Ebrietas | 12420150-12420156 (State 12420154/155) | | mirrored | minor | |
| Celestial Emissary | defeat 12421700 (2420), entry 12421702 (no remo, region 2422815: a box at the lower end of the arena, it enables the small emissaries; a guest's fog first runs it on the host, emissary079); lots 25700000 / 25700005 (ledger 33/34) | | guest fog first | ok | |
| Ebrietas | defeat 12421800 (2421, 9459), entry 12421802 (no remo); lot 80000300 Great Isz Chalice flag 5009 | | shared | ok | |
| Queen Annalise revival | 12420500 (removes goods 4305 Queenly Flesh, remo 24020000) | | covenant_outcome (0.7.3): per player, with Annalise's state 1040-1059 and 72500300-72500339 | minor | |
| Insight check | 12425290 (host only, >= 10) | | | ok | |
| Cosmic Eye Watcher Badge | lots 2420270 / 2420280 (treasure, per player) | | per player | ok | |

### Forsaken Cainhurst Castle (m25_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Arrival | carriage `2003[49] 2502959` (from Hemwick) | party travel | follows | minor | checklist 7 |
| Lift | 12500074 (12500076/077) | | | minor | |
| Martyr Logarius | defeat 12501800 (2500, 9460), host entry 12501802 remo 25000020 | intro + fight | either goes in first (P5, region visit) | ok | P5 |
| Unopened Summons | 12500600 lot 2500910 flag 52500910 (award) | | shared | ok | |
| Queenly Flesh | 12500020, lot 2500920 | | | ok | |
| Annalise / Yurie / Alfred | 6040 (t250307 -> 9042 lot 17000 Cainhurst Badge), 6150 (t250324) | | solid; gifts as Djura; Annalise 1040-1059 / m25 12500010-12500021, Alfred 1340-1359 / 12400590-12400595 / 12500050-12500054 / 72400460-72400479 / 72500350-72500359 / 9770 are covenant_outcome (0.7.3): Alfred's kill of the queen stays in the world it happened in | ok (P1 fixed, live with Eileen) | P1 |
| Church marks | 12500206 / 12500220 (world_state toggles) | | | minor | |

### Nightmare Frontier (m33_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Arrival | Lecture Building door 13200311 (`2003[49] 3302959`) | question | | minor | |
| Patches' push | 13300200 (remo 33000000/33001000 + warp 3302171 in the same map) | | local player | minor | |
| Lift | 13300110-13300113 (13300115/116) | | | minor | |
| Amygdala | defeat 13301800 (3300, 9466), entry 13301802 (no remo); lot 80000200 Ailing Loran Chalice flag 5006 | | shared | ok | |

### Nightmare of Mensis (m26_00)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Arrival | Lecture Building door 13200310 (`2003[49] 2602959`) | question | | minor | |
| Iron Door Key | lots 2600550 / 2600610 (treasure); ObjAct 2600020/2600021 | | was per player | major | F2 |
| Lifts | 12601320/330/340/352 (12601250-12601256) | | | minor | |
| Micolash | defeat 12601850 (2600, 9461; clears 1080-1099), host entry 12601852 remo 26000060; post scene 12601854 host only; lot 21000 Mensis Cage (ledger 3) | intro + fight | either goes in first (P5: the host's chr visits 2 m from Micolash's MSB spot for the 3[3] 32 m check) | ok | P5 |
| Mergo's Wet Nurse | defeat 12601800 (2601, 9462 final night), entry 12601802 remo 26000010, spawn needs the host in region 2602805; lot 55100000 One Third of Umbilical Cord flag 50000305 (global -> shared) | | either goes in first (P5, region visit into 2602805) | ok | P5 |
| 9462 "final night" | Hunter's Dream burns, Gehrman's offer | | C2 | ok | |

### Hunter's Dream (m21_00, m21_01)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Doll level-up | 12100112-12100118, 72100100-72100113 never_sync (playtest 1) | | per player | ok | |
| Messenger gifts | Old Hunter Bell 10050 (replayed from the host since 0.7.3, flag 6670 per player; a guest's talk 12101027 goes up and the host's world awards it), Beckoning Bell 10010 (director grant), Notebook 10000 | | C3 / own grant | ok | items.md 10.8 |
| Starting weapons | messengers 12101020 / 12101021 (never_sync since 0.7.3), stock flags 72101000-72101002 / 72101010-72101011 (per player) | own pick | own pick in its own Dream (a summoned guest cannot pick in the host's Dream) | ok (0.7.3) | items.md 10.8 |
| Eye of a Blood-drunk Hunter | 12101028 (chr type 0, night 9801, DLC flag), lot 10040 flag 50000100 | gets it | never for a guest | minor | F3 |
| Badge shops / workshop tools | 12101000 (badges held), 12100450 (tools held) | | own inventory | ok | F2 for the tools |
| Tear Stone | 12100123 lot 14000 (ledger 0) | | ledger | ok | |
| Abandoned Old Workshop | 12110300 Small Hair Ornament, 12110301 umbilical cord (lot 2110810, flag 52110810, award) | | shared | ok | |
| Gehrman / Moon Presence / endings | cutscenes_endings.md 3.3 / 4.4; 72100130/131, 21-23 never_sync | | C4 replay | ok (soaked) | |

### DLC: Hunter's Nightmare, Research Hall, Clocktower, Fishing Hamlet (m34, m35, m36)

| Event | ids | Host | Guest | Risk | Fix |
|---|---|---|---|---|---|
| Entry | 12405263 grab (host warp 3402959); arrival 13401000; multiplayer wall 13400010 until Ludwig | question | follows the party; needs the DLC (6899) | minor | F3 |
| Ludwig / Laurence | 13401800 / 13401850 (personal flags 6673/6674, exceptions in ItemLotDenied); Laurence needs goods 4014 Laurence's Skull (13404861 / 13401851) | | | minor | |
| Research Hall keys | Underground Cell Key 3500800 (treasure, F2), Inner Chamber Key 43220 (Simon, flag 50002350 global), Balcony Key 43010 / 43030 (flag 50002250 global), Astral Clocktower Key 3501850 (award), Celestial Dial 3501800 (award), Eye Pendant 3401810 (flag 9470) | | shared | ok after F2 | |
| Clocktower altar | 13501105 (goods 4017, phantom-aware), 13501140 | | | minor | |
| Research Hall talk NPCs | NpcParam 402080, 402090, 402280, 402281, 402290, 402300 (talks 3505xx), Fishing Hamlet 405100 (t360507), 409000 (t340540), Ludwig's head 451100 (t340508) keep the ghost bit | | solid since P3 (2026-10-15; only these talk NPCs use the rows): the guest can talk | ok | P3 |

### Scripted warps into another map (kidnap071, 2026-10-21)

Every EMEVD instruction that moves a player to another map, from the game data (`flag_tool.py
emevd` with 2003[23] added as a marker; every 2002[4] / [6] cutscene warp stays in its own map, so
none is listed). Rule for all of them: the whole party is taken together with one loading screen
(a death's respawn load or a party travel), and no guest mirrors a cutscene of a map it has not
loaded (`MirrorMapOk`, party_story.h: the remo id AABBnnnn names its map; a missed scene's
summoned replay waits for that map or the own world).

| Warp | Event (trigger, gate) | Destination | Who can trigger it | The rest of the party | Status |
|---|---|---|---|---|---|
| Snatcher kidnap | 12400780 (m24_00, 4 slots) / 12700500 (m27): local chr type 0, its death by the Snatcher, 9401 + 9404 -> 9420; common 9421 (`End if Client`, until 9423) -> `2003[23] 2802959` | the Gaol cell (m28) on the death's respawn | the host; a guest too since 0.7.3 (its copy past the type gate, the host sets 9420 and moves the party: `party_any_kidnap`) | the host's death is a scripted death warp: the party's respawn load (`host death: a scripted death warp: the Snatcher's kidnap ...`, then `the respawn load for the whole party at lamp 2802959`); every guest wakes beside the host (live: smoke `kidnap`) | fixed (0.7.0 regression) |
| First death with Insight | common 9404 (`End if Client`): 9401 on, Insight >= 1, the player's death -> `2003[23] 2102961` (12100110 sets 9404 at the Doll's awakening) | the Hunter's Dream (m21) on the death's respawn | the host only | the same rule: the respawn record moved between the host's last frame alive and the funnel to another map (`RespawnMovedAtDeath`), so the party's respawn load; unit-tested, not reached live (no fixture for a first death with Insight) | fixed (unit) |
| Prologue first death | common 9400 (`End if Client`, ends on 9401 / 12410999) -> `2003[23] 2102962` | the first Dream | the host only | the clinic has no multiplayer (campaign_start.md): never in a party | n/a |
| DLC entry (the Eye of a Blood-drunk Hunter) | 12405263 (m24_00, Oedon Chapel: Amygdala 2400899 shown at the Blood Moon or 12404001; its grab = event message 710 + goods 4311 held by the local player): remo 24000000 (m24_00), then chr type 0 only: 12401000, `2003[49] 3402959`; arrival 13401000 (no scene, respawn 3402958) | the Hunter's Nightmare (m34) | the host's grab warps; a guest's grab plays the m24 scene only and stays | story warp (P8): "Travel with the party?", YES (default) = the party travel with one load each; the grab scene plays in the map everyone has loaded (live kw: `host_lamp 3402959 as a story warp`, every game in m34_00) | ok |
| Yahar'gul Amygdala grab (Tonsil Stone) | 12800431 (m28: Amygdala 2800745 shown at Insight >= 40 or the Blood Moon; event message 10 + goods 4310 held; **no client guard**): remo 28000040 / 28001040 (m28), 12800434, `2003[49] 3202958`; arrival 13200950 (no scene) | the Lecture Building (m32) | any player holding the Tonsil Stone | story warp: the triggering player's question, YES = the party travel (live kw: guest 1's `guest_lamp 3202958 as a story warp`, every game in m32_00) | ok |
| One Reborn altar | 12800430 (`End if Client`) -> `2003[49] 3202959` | the Lecture Building | the host only | story warp question | ok (P8) |
| Rom -> Blood Moon | 13201803 (host) -> remo, `2003[49] 2802958` | Yahar'gul (m28) | the host only | story warp question | ok (P8, live parg) |
| Hemwick carriage | 12200131 (action button) -> remo, `2003[49] 2502959` | Cainhurst (m25) | any player | story warp question | ok (P8) |
| Lecture Building doors | 13200310 -> `2003[49] 2602959` (Mensis), 13200311 -> `3302959` (Nightmare Frontier) | m26 / m33 | any player | story warp question (P4: one question) | ok (P8, parab) |
| DLC cleanup | common 3503 (chr type 0, 6899 off) -> `2003[49] 2102961` | the Dream | the host only | story warp question | ok |
| Chalice leftovers | m29 12900990 (`2003[14]` to m20_01 / m29_01 / m29_02 points), m29_03 12900780 ("December version" return statue, `2003[14]` 2100980) | | no object of a released dungeon runs them | | n/a |

There is no Amygdala grab in Lumenwood Garden or anywhere in the Upper Cathedral Ward (m24_02 has
no warp instruction); the Nightmare Frontier is reached through the Lecture Building's door
13200311. The grab attacks themselves are not scripted in the tests (an Amygdala's grab hit is the
trigger): phase `kw` runs the same warps the events run (`host_lamp=ID:story`) and plays the Gaol
scene on the host outside m28 (`BB_PARTY_STORY_TEST=host:28000010`) to show no guest mirrors it.

### Chalice dungeons

Out of scope here (chalice.md, bossk10): ritual chalices from bosses are global flags 5000-5053
(shared), dungeon flags are never_sync.

---------------------------------------------------------------------------------------------

## Proposals (not done here)

* **P1 NPC gifts triggered by a guest. DONE (kidnap agent, 2026-10-14, live).** Common event 9040
  slots 2-37 award the NPC gifts when the talk ESD sets the slot's own flag (9042-9077; each id is
  in its ESD exactly once, as the SetEventFlag argument: the gift branch is chosen by the NPC's own
  state flags, not by this one). On a client 9040 ends at load (chr type 0 only) and its slot flag
  reads 1 in the guest's overlay from then on (watch: `9074: visible 1, own save 0, host image 0`),
  so the guest's talk set nothing new, awarded nothing, and the NPC state (npc_quest) still moved
  on in the host's world. Fix: `party_worldact` observes this game's own SetEventFlag calls
  (`progress::AddLocalFlagSetObserver`); a gift trigger (`NpcGiftTrigger`) the guest's talk sets
  while it stands in the host's world goes up as a set-only world action
  (`WorldActionAllowed`), unless the host's image already has it; the host's 9040 awards the lot,
  C3 hands it to every member (ledger / lot flag), the guest's own save gets the trigger. Live:
  see checklist 3.
* **P2 Repeatable NPC gifts for guests. DONE (pard, 2026-10-15).** Common event 9100
  (`NPCアイテム入手_何度でも`, slots 0-4: 72410359 -> 24060 Blue Elixir x3, 72410310 -> 23000
  Iosefka's Blood Vial, 72400398 -> 32020 Blood of Adella, 72400489 -> 28000 Blood of Arianna, 9043
  -> 17010 Queenly Flesh) awards the lot when the talk sets the trigger and clears it again. It
  ends at once on a client, and the trigger used to go to the host as an npc_quest world action,
  so the HOST's 9100 gave the guest's Blood of Arianna to the host. Now the trigger is never a world
  action (`RepeatGiftLot`, `WorldActionAllowed`); the guest's own talk set (SetEventFlag observer)
  queues its own grant (`ItemSource::Repeat`, a key per grant: every talk gives again), given while
  summoned like any grant, and clears the trigger as 9100 would; a trigger the host's world has set
  (the host's own talk) gives the guest nothing. Unit: party-worldact-test, party-items-test. Live (pard S2b,
  `guest_flag` test steps): Blood of Arianna given while summoned with the item box (0 -> 1; consumable
  uniques are given there, key items such as Queenly Flesh wait for home), Blue Elixir twice (0 -> 3 -> 6),
  the host's own Blood of Adella trigger awarded the host only. Queenly Flesh (a key item) live
  (pard2 qf2): the guest's trigger 9043 in the host's world queued lot 17010 ("waits for the own
  world"), the guest left the party 25 s later and its own world gave goods 4305 7 s after the
  Leave (`inventory add 1/1: item 4305`), before the bell summoned it again; the host got nothing.
* **P3 Remaining ghost NPCs. DONE (pard, 2026-10-15).** Every MSB part (all maps, chalice
  layouts too) that uses NpcParam 504020 / 504030 (the suspicious beggar, m24_00 / m27), 513000
  (Willem and two m32 cutscene dummies), 402080 / 402090 / 402280 / 402281 / 402290 / 402300 (the
  Research Hall's talking patients, heads and Adeline), 405100 (the Fishing Hamlet priest), 409000
  (the corpse in Ludwig's arena) and 451100 (Ludwig's head) is that talk NPC: no hostile enemy shares
  the rows, so all twelve lose the ghost bit on clients (`party_npcghost.h`). The beggar turned
  hostile is then a solid enemy on the guest's screen like any other. Live (pard S3): the guest
  talked to Willem at Byrgenwerth (prompt at the first spot, his line, solid on the screenshot).
* **P4 Double travel questions. DONE (parab, 2026-10-15, live b6a).** A scripted warp that runs on
  two games (Lecture Building doors via the replicated ObjAct, the Tonsil Stone grab of every
  player holding the stone) asks once: each game tells the others when it opens the question
  (`split_choosing`) and the lower party slot keeps it (the host first,
  `deathrules::DropSameWarpQuestion`); the host also takes no second question for a warp whose
  party travel a member's request already started. Live (parab b6a, Lecture Building 3202959 by
  the test aid): both asked at once, the guest's question closed, one party travel. The real
  door's ObjAct by a guest is still not run live (pard2 c7 travelled to the Lecture Building
  3202959 with the party, but its fixture named the doors' SFX entity 3202000, not the door object).
* **P5 First fights. DONE (pard, 2026-10-15, live with Amelia).** Every boss's FIRST fight
  starts with the host's first-fight event (`ホストがボス部屋入場_初戦/初回`: the host enters a region
  or comes within a distance, many with an intro scene), and each guest-entry event waits for it
  (3[0] on that event's flag), so a guest stood at a fog with no prompt. For the 12 gates whose
  first-fight event waits only on the host's position (Witch, Blood-starved Beast, Darkbeast
  Paarl, Amelia, Cleric Beast, Gascoigne, Logarius, Wet Nurse, Micolash, Shadows, One Reborn,
  Amygdala; `BossGateGuestStarts`) the guest's fog now has its prompt; its `fog` report makes the
  host's game run the first-fight event: EMEVD conditions are registered once (a late skip does
  not pass one already waiting: the first try armed a skip and nothing happened), so the host's
  character visits the event's region for a moment (`FirstFightSpotFor`, MSB data) and is put back
  as soon as the event went on (0.07 s live). Its scene plays for the whole party (live mirror),
  the boss starts. `BB_PARTY_GUEST_FIRST_FIGHT=0` off. Live S1d: see checklist 4.
  *pard2 (2026-10-15), the host's side, live with frame captures of the host's window (66 per s):*
  - Without a hold the visit showed: 2-4 frames of the arena's view between the host's own view
    and the intro (p5a2). Now the host's screen keeps its last picture while the chr is away
    (`bbport_present_hold.h`, the presenter shows the held frame again; not with frame
    generation; `BB_PARTY_FF_HOLD=0` off): p5d, 240 frames, the 3D view identical up to the
    intro's first frame (only the party panel's distance number changed for ~0.2 s).
  - A host put back before its intro scene ended was killed the moment the scene ended (p5a /
    p5b / p5d / p5e: `hit: HP 719 -> 0` at the scene's end, then the party's respawn load; the
    scene's player pose ends at the arena entrance). Gates with an intro scene (`FirstFightSpot::scene`:
    Amelia, Gascoigne, Logarius, Wet Nurse, Micolash, One Reborn, Ludwig, Laurence, the Orphan)
    now keep the host at the spot until the scene has started (9180 does not end the visit) and
    through it, and put it back 1 s after it (p5f: put back at 202.1 s, alive at its lamp to the
    end of the run with Amelia's bar up, the guest fighting her, screenshots; c2: the One Reborn
    the same, host alive).
  - Host dead when the guest's start arrives (p5c, `host_die` 2 s before): the start is dropped
    ("the host died: the party respawns at its lamp"), the party's death load takes the guest out
    of the arena; no visit. Another boss bar up or a scene playing: the visit waits.
  - Host in the game menu (p5b, Options pressed 3 s before): visited all the same, the menu stays
    open, the scene plays. Host mid-attack (p5a2 / p5d, R1 every 0.7 s): visited and put back.
  - No other event ran from the host's moment there: with `BB_PARTY_EMEVD_TRACE=1` the only new
    instructions in the visit window were the first-fight event 12401802's own and common 9280
    (slot 2, `出現寡婦_消滅`), which waits on that event's flags, not on a position.
  - DLC (`BossGateGuestStarts` 16 gates, 15 since 2026-10-22: Laurence left out, see below): Ludwig (13404811 + 13401801, both 3[2] 3402805),
    Laurence (13404861 + 13401851, 3[2] 3402855; the host must hold goods 4014 as in vanilla),
    the Orphan of Kos (13604811 + 13601801, 3[2] 3602805) are two-part first fights (the first part
    sets 9180 and its flag, the scene part needs that flag AND the host still in the region:
    `FirstFightSpot::chained`), the Living Failures one part (13501851, 3[2] 3502815). Spots from
    the MSB regions (origin + 0.5 m). *pard3 (2026-10-15), live, the test save owns the DLC (6899) and the Eye:* Ludwig (L1c: the guest's fog, host visit 0.1 s, scene 34000020 on both, Ludwig killed, the guest +3 Insight, Guidance given while summoned with its OK row), the Living Failures (L4: no scene, the fight started 0.1 s after the guest's fog, killed, +2 Insight), the Orphan of Kos (L5b: two-part start, scene 36000000 mirrored, killed through its second-phase entity 3600801, +5 Insight, the weapon reward given while summoned), Laurence (L3c, the host holding Laurence's Skull). A host standing in a first-fight region starts the fight itself (L3b / L5: the DLC lamps 3402953 and 3602952 stand inside Laurence's and the Orphan's regions), as in vanilla.
  - Laurence is host-first since 2026-10-22 (player report: without the skull a guest's walk through
    his fog was taken for the fight's start, the host's altar visit started nothing, and the guest
    stayed shut in behind the fog until a Hunter's Mark). `bossfog::FirstFights` marks the first
    fights started by an action in the room (`action`: Ebrietas, Laurence, Lady Maria;
    `tools/party/bossroom_tool.py scan` classifies every first-fight event); for those a guest's
    game skips the re-fight event's `1003[5] skip 2 if client` ([2] of 12424810 / 13404850 /
    13504800), so its fog is off until the intro flag is on, as on the host: the room is open until
    the fight really starts. The guest's fog notice says what starts the fight.
  - Host-first by design: Ebrietas (12421802: 3[23] damage to her by entity 10000, the HOST's own
    hit), Lady Maria (13501801 waits on 13500947, set by 13500948 when the host examines her in her
    chair, 3[1] 1650/1651), the Celestial Emissary (no first-fight event). Rom has no fog gate and
    her first fight starts on anyone's hit (13201802: 3[23] attacker -1), so a guest starts it
    already.
* **P6 Yahar'gul before and after the Blood Moon. DONE (pard2, 2026-10-15, live).** 12800160 puts
  SpEffects 9120/9121 on the local player in m28 while 9802 is off (the Hunter's Dream constructor
  does the same, and summons work there). Before (p6, test saves, 9802 = 0 on both, own save and
  view): the host in Yahar'gul at lamp 2802950, the guest rang from its own Dream: refused once
  (another map), gathered to 2802950 with a loading screen, rang again, direct summon joined 3.0 s
  after the request, arrived 0.5 m from the host and stayed 170 s to the end of the run
  (screenshot: both in the night, unreddened Yahar'gul). After (c2, the Blood Moon world of c1): the
  guest's title join loaded straight into Yahar'gul beside the host, and it started the One
  Reborn's first fight there.
* **P7 Kidnap respawn trace. DONE (kidnap agent, 2026-10-14, live).** The EMEVD dispatcher
  0x17C0420 switches on the 2003 instruction id (jump table 0x17C2C38); id 23 (`復活ポイント設定`,
  set respawn point) is the case at 0x17C10A5: row lookup 0x1F26B40, then `GSM +0x1528 =
  {row +4 | row +8 << 32}` (0x17C1145). (Id 20 writes +0x1530 the same way.) So the host's last
  lamp record is the Gaol before its death funnel, and the guests' host_death replay lands there.
  Live (2 instances, test save, host in Cathedral Ward, `host_flag=9420` then `host_die`):
  one loading screen for both, guest 0.5 m beside the host in the cell, no stuck or half state.
  A guest cannot be kidnapped in the host's world (12400780 / 12700500 end unless the local chr
  type is 0; a summoned guest is type 1 within 0.3 s of arriving).
* **P8 Scripted warp question. DECIDED (pard2, 2026-10-15): they keep asking.** Every story warp
  (`deathrules::StoryWarpEvent`) asks the player who triggered it "Travel with the party?" after
  its scene, like a lamp: YES (the cursor's default) moves the party with one loading screen, NO
  lets that player go alone (the vanilla game moves only the host and sends phantoms home; ERSC
  keeps the session and moves nobody else). Asking costs one press and keeps a way to split the
  party on purpose; P4 makes sure it is one question. `BB_PARTY_TRAVEL_CHOICE=party` never asks.
  Live: Blood Moon 2802958 (parg, pard2 c1: question 2.0 s after the scene, YES, guest beside the
  host in Yahar'gul), Cainhurst 2502959, Mensis 2602959, the Hamlet 3302959, the Nightmare
  3402959, the Lecture Building 3202959 (parab b6a-c).
* **P9 One-way doors that close again. DONE (pard2, 2026-10-15, live).** 12400157, 12400177 and
  12700173 were already in the C2 state list (`kStateIds`, party_progress.cpp, since 2026-10-10):
  they follow the host both ways, open and shut, in the guest's own save as the lifts do
  (party-progress-test now asserts it). Live (pard2 qf2, `BB_PARTY_WORLD_WATCH=12400157` on the
  guest): the host's world shut 12400157 at 69.5 s; the guest's own save read 0 at 69.9 s and its
  view 0 at 70.9 s.

## Regenerating

`python -I tools/party/flag_tool.py gen-inc GAME --out gpu/shim/party/party_flags.inc` (the 9420
override lives in `OVERRIDE_ROWS`); the key treasure list is `SharedKeyTreasureLot` in
`party_items.cpp`.
