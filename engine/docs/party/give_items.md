# Give items to a party member (0.7.8, giveitem078, CL-63e235)

A player gives consumables or materials from the own inventory to another party member. Host to guest, guest to host
and guest to guest. Code: `gpu/shim/party/party_give_rules.h` (pure rules, wire, the two state machines; unit test
`tests/test_party_give.cpp`, `ninja -C out/gpu party-give-test`) and `party_give.{h,cpp}` (game side). The receiving
half of the inventory work is `ReadGiveFacts` / `GiveGoodsToOwn` / `TakeGoodsFromOwn` at the end of `party_items.cpp`.

## 1. Player view

* Switch `party_give_items` (System > My party: Your items, the launcher's My Party tab, bbport.ini, the Insert menu's Party
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

## 2. What may be given (`GoodsAllowed`, `GiverVerdict`, `ReceiverVerdict`)

Goods only (weapons, armor and rings are shared by the key / unique rules of items.md section 11 and are not offered).

| Rule | Reason shown |
|---|---|
| EquipParamGoods `goodsType` must be 0 (key items are 1) | key items cannot be given |
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

## 4. Tests

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
