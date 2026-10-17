# Playtest 1 save audit (the guest's save folder after 0.6.0)

Input: the guest's only party save folder (`user\savedata_party`, 15 files, 2 character slots) after the
2.5 h Internet playtest on 0.6.0 (8143fda) and a later local solo run in Join-a-party mode with no host
(2026-10-14, two launches: one in the world on slot 0, one that stayed at the title), the later run's
launcher log, and the playtest logs (H1..H5 / G1..G5 as in `playtest1_triage.md`). The save files are
untrusted data: decoded offline with `python -I`, never committed. Tool: `tools/party/save_decode.py`
(new, see the layout notes in its docstring; `tests/test_save_decode.py`).

Coverage gap (as in the triage): no guest logs after G5 (19:49): Gascoigne, Cathedral Ward, the Oedon
Tomb Key, the Blood Gem Workshop Tool and Old Yharnam are known from the host's logs and the save only.

## How a save is read

`userdataNNNN` is the plain character save (version 0x41, no encryption in this port). Its header has
the packed map id and play time, then a table of `{size, end}` pairs; the section of 150,001 bytes is the
event flag store: the 1200 x 125-byte pool of `SprjEventFlagMan` (group key = pool index, keys as in
`event_flags.md` 2) plus the revive-wait byte. Goods are 16-byte entries `{handle 0xb.., 0x4.. | id,
count, index}`; key items (goodsType 1) live in their own list (a count + 64 entries). Mapping checked
against the user's test save (Cleric Beast, Gascoigne, 7 key items) and against the host's logged flag
changes (175 host flags, all but 3 match, below). The `party_*.txt` files were not in the zip; their
state is read from the later run's log.

## The two characters

| Slot | Which game | Play time | Map | Level / Echoes / Insight | Key items |
|---|---|---|---|---|---|
| 0 | the first game (abandoned in its prologue at the playtest, #19; played solo again in the later local run) | 0.28 h | m24_01 | 10 / 0 (HP 277 of 659, a 420-echo bloodstain in m24_01) / 0 | none |
| 1 | the playtest character (played to Old Yharnam) | 1.85 h | m24_00 | 22 / 3993 / 8 | **none** |

Slot 1 against the host's world at the end of the session (every `host flag N = V (category)` line of
H1..H5, last value of each): **172 of 175 match**. Both bosses the host killed are dead in the save, the cutscenes seen
match, and the **Blood-starved Beast was not killed** (as in the host's logs: only two boss kills). The 5 lamps the host
lit are lit; shortcuts, NPC quest flags and story flags are as the host's. Time of day: evening, no night / Blood Moon. No
stuck state: no lamp warp request, no cutscene-running flag, revive-wait 0, the prologue chain complete. The area gates of
the Central Yharnam area are 0 although both bosses are dead: never_sync by design (they only gate the bell in that area).

Slot 0 is a normal early save: the Notebook messenger not done yet, which matches #19 (the guest was pulled out of its
first Dream). The later run used its headstone and lamp normally. No boss, lamp or ledger flag from slot 1.

## Issues

Seven findings (S1 to S7), each with its cause and what the head did about it:

* **S1, key items lost (major, fixed in saveaudit, live).** Slot 1 held no key item at all, although its save says it
  got the Sword Hunter Badge and the Oedon Tomb Key (both through the ledger bits) and the Blood Gem Workshop Tool
  (through its treasure flag). The key-item list count was 0; the guest log shows the gives (held 0 to 1, own-save flag
  set in the pool, load mode 1) and the host log the awards and the pickup, and the guest played about 50 minutes more
  with saves after that. Cause: a key item given to, or picked up by, a *summoned* guest goes into the summoned
  inventory, which the save does not keep, while the flag or ledger bit is written into the own pool and is saved. The
  earlier check "the save written while summoned keeps the gives" came from a base save that already held both keys.
  On the head before this audit the Tomb Key and the badge were re-given only if the join's full item list was applied
  while the guest stood in its own world, or deferred to a home trip in the same run; a title join straight into the
  host's world and quitting from there never did it, and the Workshop Tool never (a flagged lot whose flag is set counts
  as already given). Severity: the Tomb Key gates the guest's own Cathedral Ward, and no gem workbench.
* **S2, the re-give list is per user folder (minor, fixed).** `party_summoned_gives.txt` is one file per user folder,
  not per character, and this guest has two characters in one folder, so a key given while summoned to one character
  would be re-given to whichever loads next. Entries now carry the save slot.
* **S3, Doll level-up handshake flags left set (minor, not changed).** In slot 1 the three handshake flags were 1
  while the host's were 0, and one of them in slot 0. The host cleared them; the NPC quest sync delivered the sets to
  the guest's pool but not the clears. They are never_sync since pt1npc, so no new ones appear, but the old bits stay:
  the next level-up at the guest's own Doll finds two of them already set and opens the menu without the kneel and
  gesture; the talk clears them.
* **S4, rot crosses characters (minor, fixed in slotfiles).** A later run's slot 0 character, level 10, got slot 1's
  rot ("1 stack(s) kept from the last game", rot factor HP 90 %), cleansed at the next lamp. `party_rot.txt` was per user
  folder and was dropped only for a save in its prologue; it is now keyed by save slot and an old unkeyed file is dropped.
* **S5, `party_prologue.txt` is per user folder too (minor, fixed in slotfiles).** A title join might be held once for
  a character past its prologue after the other one was in its prologue. Now per save slot; an old unkeyed file reads as
  the last character's state.
* **S6, Old Hunter Bell never given (none, ok).** The gift is per player and 0.6.0 had no path for it; pt1items gives it
  to a guest in the host's Dream (the vanilla game also gives it in the guest's own Dream). Not lost, still to be
  collected.
* **S7, the save files were rewritten by a launch that stayed at the title (none seen, note).** The play time moved by
  8 ms between the two files; the content was intact.

Not found: duplicated items, two states of one NPC, a half-done death or send-home, a missing host lamp /
boss / shortcut, a Blood-starved Beast kill, cross-contamination of flags between the two slots (slot 0
has none of slot 1's progress; S3's 72100102 in slot 0 came from the host's session, S4 / S5 are folder
files).

## The fix (S1, S2)

`party_items.cpp` `KeyCatchupTick`: 4 s into every steady own world (load mode 0, own world), from the
save alone (no host, no party list): every ledger lot whose ledger bit **and** boss flag are set in this
save (`KeyCatchupLots`, boss_defeated done flags only) and every `SharedKeyTreasureLot` whose treasure
flag is set, whose slots are all unique goods not held with at least one key item
(`KeyCatchupNeeded`), is given (`GiveItemDirect`) and a save is requested; log `key item catch-up: gave
goods N xC (lot L, ...; held 0 -> 1)`. Consumed / NPC-handed items are out of scope (boss rewards and
the shared keys are never used up). `party_summoned_gives.txt` lines get the save slot
(`party::flow::CurrentSaveSlot()`, GameDataMan +0x50 -> +2000); another character's entries are kept
for it. Unit: party-items-test `test_key_catchup`.

Live (one instance, own copy of the guest's save folder, slot 1 loaded by Continue, no host):
`key goods held: ... 4000 x0 ... 4114 x0`, then `key item catch-up: gave goods 4000 x1 (lot 31000, ledger
25 = flag 60009025 set with boss flag 12411800 in this save; held 0 -> 1)`, the same for 4114 (lot
50000010) and 4103 (lot 2411000, shared key treasure 52411000), `save requested (C3 key item catch-up)`,
`key goods changed: 4000 0 -> 1, 4114 0 -> 1`; the instance's `userdata0001` on disk then had the three
key items. The same build on the slot 0 character gave nothing (no ledger bits).

## What the guest should do

Nothing by hand. With 0.6.1, load the playtest character once in his own world (Continue; a solo
Continue in Join mode is enough): the Oedon Tomb Key, the Sword Hunter Badge and the Blood Gem Workshop
Tool come back within a few seconds and the game saves. The Old Hunter Bell is still waiting at the
Messenger in the Hunter's Dream. The Doll's stale handshake bits clear at his next level-up. The rot of
the other character was already cleansed.

## The fix (S4, S5)

Both files now carry the save slot (`party::flow::CurrentSaveSlot()`, GameDataMan +0x50 -> +2000), as
`party_summoned_gives.txt` does since S2. Pure parts with unit tests: `party_balance_rules.h`
(`RotFile`, party-balance-test) and `party_start.h` (`SavedPrologueFile`, party-start-test).

- `party_rot.txt` (S4): one line per character with rot, `slot S kinds H ST SC R D` (stacks per kind:
  health, stamina, stamina cost, rally, discovery); a cleansed character's line goes, an empty file is
  deleted. Nothing applies at start; at the first in-world tick the loaded character's slot is read
  and its own stacks come in (log `rot: save slot N loaded: its own K stack(s)`); loading another
  character keeps the first one's and switches. **Old unkeyed file (`stacks N` + `kindK N`): dropped**,
  logged once (`rot: N stack(s) in an old party_rot.txt without a save slot dropped`). Its owner cannot
  be told apart, giving it to the wrong character was the bug, and dropping a penalty costs nobody
  anything (the next lamp would cleanse it anyway); the safe choice. The prologue rule
  (`RotFromAnotherCharacter`, 87709d03) stays for a new game made in a slot whose old character was
  deleted.
- `party_prologue.txt` (S5): `last V` (the character seen in its own world last) plus `slot S V` per
  character. A guest at its title asks for the slot it continues; with that slot unknown or never seen
  the `last` value counts (the behaviour before). **Old unkeyed file (`1` / `0`): read as `last`**, so the
  answer is unchanged until each character has been seen once; the safe direction here is to keep it,
  because a `1` holds a title join (plain Continue, the bell summons later) rather than load a prologue
  save into the host's world (#19).
- `instances.py --fresh-saves` already deletes both files. Not checked live (pure logic; the slot read
  is the one S2 uses).
