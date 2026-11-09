# Feature `hit_effects` (Nightwatch 0.3.0, visual)

Index: [README.md](README.md). Co-op class: **visual** (not in the party hash; each player's game decides alone).

Minimal mod:

```
Mods/Gold Bolt Paper/
  dvdroot_ps4/...                 the mod's own files, incl. the effect files for the new ids
  nightwatch.json                 {"id": "gold-bolt-paper", "requires": "0.3.0", "features": ["hit_effects"]}
  nightwatch/hit_effects.json     the table below
```

The game draws the elemental on-hit look (the lightning crackle of Bolt Paper or a bolt weapon, the fire burst) with an
**overlay effect whose id is hardcoded** in the game's code; no param chooses it. `hit_effects` lets a mod replace that
overlay id while the **attacker** has a given SpEffect active, so a buff (a paper, a weapon state) can have its own hit
look without changing every Bolt hit in the game.

## Table: `nightwatch/hit_effects.json`

```json
{
  "version": 1,
  "rows": [
    {"attacker_speffect": 90002, "base_fx": 880,   "fx": 900880},
    {"attacker_speffect": 90002, "base_fx": 10880, "fx": 910880}
  ]
}
```

| Field | Meaning |
|---|---|
| `attacker_speffect` | a SpEffect id; the row applies while the attacking character has it active |
| `base_fx` | the overlay id the game picked for this hit (list below) |
| `fx` | the effect id drawn instead; the mod ships it (an `f<id>.fxr` in an ffxbnd the game loads, e.g. its dlceffects) |

All three are whole numbers 1..2147483647. The manifest must list `"hit_effects"` in `features`
([manifest](manifest.md)). Key: (`attacker_speffect`, `base_fx`); a later mod's row replaces an earlier mod's row with
the same key (inside one table the last row of a key wins). If one hit matches rows of several SpEffects, the row of
the latest mod in load order wins; inside one mod the first matching row in the file wins. At most 4096 rows per file
and after the merge.

## The overlay ids the game picks (1.09)

| Element (attack spAttribute) | Overlay family | Ids |
|---|---|---|
| Bolt (6) | slots 10-19 | 880, 10880, 10980 (+1 variants 881 / 10881 / 10981) |
| Fire (2) | slots 0-9 | 800, 10800, 10900 (+1 variants) |
| Fire (2) | slots 20-29 | 806, 10806, 10906 (+1 variants) |

Which id of a family is used depends on the hit character (a byte of one of its sub-objects: 0 -> 880 / 800 / 806, 1 -> 10880 / 10800 / 10806,
other -> 10980 / 10900 / 10906; no vanilla effect bank holds 10980); the +1 comes from a per-character check (881 exists only in a few map effect banks). A row matches the
**exact** id the game picked, so a mod that wants every variant lists each. Other elements (Arcane 3, physical...) draw no
overlay in the game, so there is nothing to replace for them: a **draw row** (next section) gives them one.

## Draw rows: an overlay for an element the game draws nothing for

```json
{"sp_attribute": 3, "fx": 203050, "sound": 203001}
```

| Field | Meaning |
|---|---|
| `sp_attribute` | the hit's spAttribute (AtkParam `spAttribute`, or the attacker's weapon-buff SpEffect's), whole number 1..255 but not 2 or 6 (the game draws Fire and Bolt itself; swap those with the rows above) |
| `fx` | the effect drawn on the hit character, started like the game starts the Bolt overlay (slots 10-19); whole number 1..2147483647 |
| `sound` | the sound played with it (`s<id>` in a loaded sound bank); required, whole number 1..2147483647, because the game plays the overlay's sound slot unconditionally |

A draw row must not also hold `attacker_speffect` or `base_fx`.

The game's own Fire / Bolt checks decide when it draws: the hit dealt damage, and the hit was not one the game skips
for overlays. Key: `sp_attribute`; a later mod's draw row for the same value replaces an earlier one. A draw row never
swaps a game overlay, and a swap row never matches a draw. Log (`BB_HIT_EFFECTS_LOG=1`): `hit_effects: draw:
spAttribute 3, dealt 112 -> overlay 203050, sound 203001 (...)`, then `store 10-19: draw overlay 880 -> 203050, sound
8800 -> 203001`.

## Behaviour

- The swap happens when the game starts the overlay for a hit, from the attacker the game passes to its hit handler.
  While an overlay of the same family is still running on that character, a new hit keeps the running overlay (the
  game's own rule), so the first hit's look stays until it ends.
- **Visual class:** each player's game swaps the overlays it draws itself, from the SpEffects it already knows. Players
  without the mod see the vanilla overlay; it does not count in the party check.
- No mod table: nothing changes. Nightwatch before 0.3.0: the table is ignored (vanilla overlay).
- A bad table is dropped with one log line: `Mods: <folder>: nightwatch/hit_effects.json: row 2 needs whole numbers
  1..2147483647 for attacker_speffect, base_fx and fx: its hit_effects table is not used`. For a draw row the text is
  `row N (a draw row) needs sp_attribute 1..255 but not 2 or 6, and whole numbers 1..2147483647 for fx and sound (no
  attacker_speffect or base_fx)`.
- If the engine cannot read the merged file (should not happen with bb-start's own output) it logs
  `hit_effects: <path> line N is not "<attacker_speffect> <base_fx> <fx>": no rows used` and uses none.
- With mods turned off (`BB_MODS_ENABLED=0`) no table is read.

## Log lines (`logs/last_session.log`)

- `Mods: hit_effects: 2 rows from 1 mods` (start: the merged table)
- `hit_effects: feature hit_effects: installed (3 of 3 id stores, 2 rows from ...)`, or `not installed (why)` when the
  game code differs from 1.09 (another patch wrote there) or `BB_HIT_EFFECTS=0` turned it off.
- `hit_effects: store 10-19: overlay 880 -> 900880 (attacker 0x..., local player has SpEffect 90002)`: a swap (the first 32 of a
  session; `local player` appears only when the attacker is the local player; the slot is `0-9`, `10-19` or `20-29`). `BB_HIT_EFFECTS_LOG=1` logs every overlay pick (first 200), also those without a row.

## Switches

- `BB_HIT_EFFECTS=0`: the feature is not installed (vanilla overlays).
- `BB_HIT_EFFECTS_LOG=1`: log every overlay pick.
