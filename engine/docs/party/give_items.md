# Give items to a party member (0.7.8, giveitem078, CL-63e235; weapons and armor 0.7.9, givegear079, CL-1b3b98)

A player gives consumables or materials (and, from 0.7.9 with a second switch, weapons and armor: section 5) from the own
inventory to another party member. Host to guest, guest to host
and guest to guest. Code: `gpu/shim/party/party_give_rules.h` (pure rules, wire, the two state machines; unit test
`tests/test_party_give.cpp`, `ninja -C out/gpu party-give-test`) and `party_give.{h,cpp}` (game side). The receiving
half of the inventory work is `ReadGiveFacts` / `GiveGoodsToOwn` / `TakeGoodsFromOwn` at the end of `party_items.cpp`.

## 1. Player view

* Switch `party_give_items` (System > My party: Giving, the launcher's My Party tab, bbport.ini, the Insert menu's Party
  tab). **Off by default.** It is per player and has two meanings: with it on, an item's command menu in the inventory has a
  *Give* entry and you can send; with it off other hunters' gives are refused ("does not take gifts") - the consent.
  `BB_PARTY_GIVE_ITEMS=1` forces it on (tests).
* The player opens the inventory, an item, and its command menu reads *Use / Discard / Give* (the game's own menu). *Give*
  opens a native command menu of the other connected hunters (shown names go through `bbname::Display`), then a native
  amount menu (1, 2, 5, 10, 20, 50 below the most that can be given, then "All (n)"), and the give is requested. *Give* is
  absent for items the rules deny (keys, unique and story items, equipped / quick-slot last copy) and outside a party.
* Notices (the party notice corner): giver "Gave Blood Vial x3 to *name*", receiver "Blood Vial x3 from *name*",
  refusals "Give Blood Vial to *name*: *reason*". The receiver also gets the game's item-get dialog.

### 1.1 How the native menu is extended (bbport_game_menu.cpp)

RE (Ghidra, `deps/ghidra-export`; `tools/re/ghidra_scripts/FindScalar.java` finds code by an FMG text id): the item command
menu is `FUN_01ba0e70`, which builds a "CommandList" with `AddCommandItem` (0x1b4c9a0; Use 102010, Discard 102011) and
finishes it with `FUN_01bea630` (builder, finalizer functor) which returns the ref-counted menu object. A command's functor is a
polymorphic object (vtable: 0/1 clone, 2 operator(), 4/5 destructors) with the item context copied at +8 (inventory index at
+0x50, type | id at +0x54). `operator()(out, self, event)` returns the next menu object in `*out`.

* At Discard's `AddCommandItem` (caller checked inside the builder) a Give functor is made from Discard's (context copied with
  the game's own copy constructor 0x1c5da00, vtable = a copy of Discard's with slots 0/1/2 ours) when `MenuOffers(item)` says
  so; the finalize call (hooked) adds the entry just before the menu is finished, so Discard's own set-up is untouched, and
  keeps a clone of the finalizer (it carries the item context) and the owner field for the pickers.
* The pickers are fresh builders (`FUN_01be8a30` constructor, layout "CommandList", owner = the item menu's) with our own
  functors (a 0x88-byte object with no refcounted members: kind, member slot / amount at +0x78..) finished with a clone of the
  item menu's finalizer; their `operator()` returns the amount menu, or requests the give and returns no menu. Texts are
  run-time port texts (`Pages::PickerText`, 8 slots).
* The game calls a command's `operator()` twice for one press (seen in the logs); a repeat of the same command within 0.5 s is
  ignored.
* Every call into the game is behind `VtableOk` (readable vtable slots); a failure leaves the entry out or closes the picker
  (null menu object), never a crash. `BB_PARTY_GIVE_MENU_OFF=1` installs nothing.
* Known: after a give the inventory list shows the old count until the cursor moves (the list is not told to refresh).
* Cross on a row (found 2026-11-05, give079): the command menu view (class 0x539be10) handles Cross in vtable slot 11
  (`FUN_01ba2600`), and it is text based. A row whose text is "Discard" activates at once; every other row first asks
  `FUN_016e4f00(item index)` ("can this item be used now", the same test that greys Use out) and, when it says no, only plays the
  error sound: the row's functor is never called. On the consumables tab the answer is yes (Pebble worked), on materials, key items
  and the like it is no (Use is greyed, the cursor still starts on it; Down twice reaches Give). `CrossHook` replaces slot 11: when
  the highlighted row (`FUN_01bea2e0` copies it) is ours (Give, or a picker entry: vtable = ours) it calls `FUN_01beaad0`, the
  handler's own activation, and skips the check; every other row goes to the original. The functor's `operator()` is called by that
  activation (and once more by the per-frame highlight code, hence the repeat guard above). `BB_GIVE_VTRACE=1` logs every
  virtual call of the Use / Discard functor, predicate, finalizer and view classes (the way this was found).

## 2. What may be given (`GoodsAllowed`, `GiverVerdict`, `ReceiverVerdict`)

Goods only in this section (weapons and armor: section 5).

| Rule | Reason shown |
|---|---|
| EquipParamGoods `goodsType` must be 0 (consumables) or 2 (materials: the Blood Stone Shard / Twin / Chunk / Rock 3000-3030 and the ritual materials 7000-7200; 27 rows, all maxNum 99). Key items are 1, the unique rows 6 | key items cannot be given |
| `maxNum` must be above 1 (unique goods: badges, the Eye, tools, keys) | unique items cannot be given |
| goods 800-822 (gestures) and 4000-4999 (the key / story band: keys, hunter badges 4110-4119, Eye 4311, Old Hunter Bell 4312, workshop tools ...) | story items cannot be given |
| unknown param row or count | could not be given |
| a quick-slot item keeps at least one (the slot points at the inventory entry; a removed entry would leave it dangling) | keep one of the item in the quick slot |
| more than held / more than 99 per give | you do not hold that many |

The receiver applies the same goods rules to what the giver names, on its own param data, so a modified peer cannot make
a game take a key or unique item. It cannot check what the giver holds: a modified giver can conjure plain consumables
(Blood Vials, bullets) for a friend; that is within the trust of a private party and not worth a protocol.

The moment (`Gate`): nothing while the world is not up or loading, a party travel / re-join runs, the player is dead or
spectating, a boss health bar is up (fights and fog walk-ins), or a cutscene plays. Checked by the receiver at OFFER and
again at COMMIT, and by the giver at the offer and again right before the removal.

Room (`ReceiverVerdict`): the receiver's inventory up to `maxNum`, then its storage box (`StorageRoom`, the game's own
check); the give is refused with "no room in their inventory and storage" if the whole count does not fit, so nothing
ever lands on the ground and the game's blocking "inventory or storage max" box never opens. The apply goes through the
award's give list (`GiveGoodsViaList`) when it passes the inventory maximum, else `GiveItemDirect` + `ShowItemGet`.

## 3. The transfer (atomic across the link)

```
giver                            host (relay)               receiver
OFFER {id, goods, n}  ---------> sets from = sender ------> gate, goods rules, room
                      <--------- relays back <------------- READY {room} | REFUSE {why}      (nothing moved)
remove n; the count must drop
by exactly n (else put back, ABORT)
COMMIT {id}           ---------> ---------------------->    reservation live? give n, count verified
                      <--------- <----------------------   DONE {got, why}
refund n - got (a short DONE)
```

* The giver removes only after READY, verifies the removal (inventory count before and after; `TakeGoodsFromOwn`), and
  puts back whatever it removed if the count is not exactly n (it removed nothing or too many) and sends ABORT.
* The receiver gives only on a COMMIT of a live reservation (`kReserveS` 20 s), from the member that made it, and
  records the outcome of the last 32 transfers: a repeated COMMIT (a replay after a reconnect) or an ABORT after the
  apply answers the same DONE, a COMMIT after an ABORT or an expired reservation gives nothing (DONE got 0). So a lost
  or late message can never apply twice.
* No DONE within `kDoneWaitS` (10 s): the giver asks (ABORT). The receiver answers with the truth: applied (DONE {got}) or
  "never, and never now" (DONE 0, the id is dead). The giver refunds exactly `n - got`.
* If the receiver cannot be asked - its link is down for `kLinkDownS` (90 s) after COMMIT, or the ABORT stays unanswered
  `kAbortWaitS` (20 s) with the link up - the giver rolls the whole amount back ("no confirmation, the item was put
  back"). Link back in time: the wait starts afresh and the queued DONE arrives.
* The host is the relay: a guest's events go to the host, which sets `from` to the sender's slot (a guest cannot sign for
  another) and forwards to `to`; an offer to a missing member is answered REFUSE at once. Events are reliable and
  ordered per stream, and queued for a member that is reconnecting.
* One transfer per giver at a time; the receiver holds one reservation at a time (Busy otherwise); 1 s between gives.

Residual risks (not covered, by design): a crash of the giver's game between the removal and its next save loses the
removed stack (the game saves on its own events; the removal also asks for a save at COMMIT); a receiver that applied,
saved and then vanished for good before the DONE reached the giver, while the giver's rollback timer ran out, would
leave both with the items (needs the link down for 90 s exactly in that window).

## 4. Bug fixed in 0.7.9: no Give on materials (CL-063e51)

Player report: "I cant get it to show up for upgrade stuff". The menu hook was fine (it is the one item command menu of every
inventory tab); `GoodsAllowed` refused every goods row whose `goodsType` is not 0, and the upgrade materials (Blood Stone Shard,
Twin Blood Stone Shards, Blood Stone Chunk, Blood Rock) and the ritual materials are `goodsType` 2 (checked in the game's
EquipParamGoods: 27 rows of type 2, all maxNum 99; type 0 = 189 rows, type 1 = 54 key items, type 6 = 25 unique rows). The rule now
allows type 0 and 2; key (1), unique (maxNum 1) and the story bands stay out. Smoke `gvt` (both players, real menu presses since give079) opens the Materials tab, shows Give on
Blood Stone Shard and completes a give.

## 5. Weapons and armor (0.7.9, givegear079, CL-1b3b98)

### 5.1 Player view

* Switch `party_give_gear` (System > My party: Giving: *Give weapons and armor*, the launcher's My Party tab, bbport.ini,
  the Insert menu's Party tab; `BB_PARTY_GIVE_GEAR=1` forces it on). **Off by default.** It is separate from `party_give_items`
  (a hunter may take bullets and refuse gear): the own switch adds *Give* to a weapon's / armor's command menu in the inventory,
  and with it off other hunters' gear gifts are refused ("does not take weapons and armor") - the consent.
* The command menu is the same native one; Give opens the member list and gives at once (one entry: no amount list).
* Notices: "Gave Saw Cleaver +4 (2 blood gems) to *name*", "Saw Cleaver +4 (2 blood gems) from *name*".

### 5.2 What may be given (`GearAllowed`, `GearGiverVerdict`, `GearReceiverVerdict`)

| Rule | Reason shown |
|---|---|
| the id is on the giveable list (built once at run time from the loaded params, `EnsureGearTable`): an EquipParamWeapon / Protector row with `isDeposit` 1 (a normal inventory row; NPC / internal rows are 0) that no *shared* lot gives | unique and shared gear cannot be given |
| not worn or wielded (the 32 equip handles of the equip record) | take it off first |
| the giver holds that entry (by GaItem handle, so two copies of one weapon are told apart) | you do not hold that many |
| receiver: the weapon / armor row and every gem's GemGenParam / GemeffectParam rows exist, the gem data is plausible (slots 0..4 once each, flags 0..3, rank 0..1000, effects -1 or an id) | its blood gems could not be read |
| receiver: a free inventory entry (the normal section) | no room in their inventory and storage |

"Shared" is the party sharing rule of items.md sections 11 / 12: the lots of `items_audit.md` with the verdict *shared* reach every
member exactly once, so the weapons and armor they give (Kos Parasite, the story weapons, Gascoigne's set ...) are unique party gear
and are not offered. A weapon's family is its 10000-id block (`base + 100 * level`, variants in the next blocks), so a shared weapon
is excluded at every upgrade level (an over-exclusion is the safe side). 741 weapon rows and 115 armor rows are giveable. Both peers
use the same table on their own data, so a modified giver cannot make a game take a forbidden item (`GearReceiverVerdict` on the
OFFER and again on the COMMIT). DLC gear is an ordinary row of the same param tables and follows the same rules (no separate flag
exists in the params). The table is built from the loaded EquipParamWeapon / EquipParamProtector / ItemLotParam (and the party's own share rules, `ItemLotSharePath`), so a mod that changes them is respected; nothing from the game data is committed. The counts are logged once (`gear table: ...`).

### 5.3 What moves with a weapon

A weapon's upgrade level is part of its item id (`base + 100 * level`), so the OFFER's `goods` carries it. Its blood gems are not
items in the inventory: each is a GaItem instance referenced only by one of the weapon instance's five slots (shape mask at
`+0x38 + 0x10 * i`, the gem's handle at `+0x3C + 0x10 * i`; the game itself writes it through the refcounted assign `0x1A8BED0`, as
`0x1A876D0` does when it builds a weapon). **Decision: the gems travel with the weapon as data** (slot, GemGenParam id, the 8
instance ints = flags, rank, six effect ids), in the OFFER (`kind`, `gems`), and the receiver builds each as a new instance and
assigns it to the same slot of the new weapon. The alternative (return the gems to the giver first) would need the game's gem
detach function and would change two inventories for one give; moving them keeps the weapon exactly as it was. A gem is never in
two places: the giver's weapon (and with it its slot references) is removed first, exactly like goods (section 3), the receiver builds
the copy only on COMMIT, and a refund after a failed transfer builds the same weapon with the same gems again (`RefundGear`, the
OFFER's snapshot). If a slot does not take its gem on the receiver (mask mismatch) the gem goes into the inventory as a loose gem:
never lost. The receiver reads the gems back after building and logs `gems placed / loose / lost`.

Armor has no state (an OFFER with gems for armor is malformed and dropped).

### 5.4 The transfer

The same two-phase protocol as goods (section 3, same state machines, `n` always 1, `kind` in the OFFER). Differences: the giver's
re-check before the removal compares the whole entry (same handle, id and gems; a gem socketed meanwhile or a sold weapon cancels),
the removal is by index of that handle (`0x14D0310`, never "the first entry of that id") and must leave the handle gone, and the
refund is the rebuild above. The receiver applies on COMMIT only: new entry = the handle absent before and present after, exactly
one (else nothing is claimed).

### 5.5 Tests (gear)

* `party-give-test`: the giveable table (Kos Parasite at every level, Bare Fists, unknown ids, armor vs weapon rows), giver and
  receiver verdicts (held, worn, room, gem plausibility), the gear gate (own switch / consent), the wire (gems round trip, five gems
  inside the body limit, malformed and implausible input) and the machines (kind carried, replay, ABORT before COMMIT).
* Smoke group `give_gear` = phase `gvw` (host fixture: Saw Cleaver +4 with 2 gems, armor, a Kos Parasite; guest fixture: a Chikage +2
  with a gem): the Kos Parasite refused, weapon with gems host -> guest, armor, the guest's switch off (refused, giver told) and on
  again, guest -> host and the same weapon back (the same two gems after two moves, read back from the inventory); `gvwn` the same
  under `BB_NET_SIM=dsl`. Test aids: `BB_PARTY_GEAR_SETUP` (fixture), `BB_PARTY_GEAR_PROBE`, `BB_PARTY_GEAR_WATCH`, and the
  `w<id>:<slot>@s` / `a<id>:<slot>@s` / `gon@s` / `goff@s` tokens of `BB_PARTY_GIVE_TEST`.
* Smoke group `give_armor` = phase `gva` (0.7.9): the Armor tab by REAL presses only. The test save wears Hat / Garb / Gloves / Trousers; the fixture
  adds an unworn second Hunter Garb (same id as the worn one) and a Yharnam Garb. The menus of the worn Hat and worn Garb offer no *Give*;
  the unworn Garb's menu (Use / Discard / Give: Down Down, the cursor starts on the disabled Use) gives that inventory entry (its handle, not
  the worn one's): READY, then the giver removes it, COMMIT, the guest builds it, DONE; probes show the guest with both Garbs and the host
  with the worn one only. Press timing is 6 s a key (`GVA_GAP`); the Armor tab is Right x5 (`GVA_TAB`).

## 6. Tests (goods)

* `party-give-test` (unit): allow / deny table, room, the gate, the wire (range checks, malformed input), the giver and
  receiver machines (happy path, refusal, no answer, removal not verified, short DONE, lost COMMIT, lost DONE, ABORT
  unanswered, link down and back, expired reservation, wrong member).
* Smoke (`tools/mp/smoke_party.py`, groups `give_items` = phases `gv` + `gvg`, `give_rollback` = `gvr` + `gvl`,
  `give_netsim` = `gvn`): `gv` host to guest and back, a refused badge, both switches (giver off, receiver off), storage
  overflow, a give of more than held, and conservation of both inventories; `gvg` guest to guest through the relay;
  `gvr` the receiver ignores COMMIT and ABORT (timeout roll-back); `gvl` the giver drops its link right after COMMIT
  (whatever happens, both inventories sum to the same); `gvn` = `gv` under `BB_NET_SIM=dsl`; `gvm` the native item menu: the
  director presses Options > Inventory > Pebble > Give > Hunter1 > 2 and the transfer runs; `gvs` opens and closes the item
  menu 14 times and the pickers 6 times, then gives once (group `give_menu`).
* Test aids: `BB_PARTY_GIVE_TEST`, `BB_PARTY_GIVE_FAULT`, `BB_PARTY_GIVE_WATCH` (DEBUG_SWITCHES.md).
