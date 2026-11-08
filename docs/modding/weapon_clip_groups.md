# Feature `weapon_clip_groups` (Nightwatch 0.3.0, gameplay)

A weapon's attack clips come from its **clip group** (`aGGG_` in the character's animations), and the game takes that
group from the weapon's `wepmotionCategory`. But the category also drives game code: Blades of Mercy's trick (the blades
split into both hands, the tricked R1 / R2 chain) only works with category 27. A new weapon on that base that needs its
own moveset therefore could not have both. `weapon_clip_groups` gives a weapon its own clip group while it keeps its
category, so everything keyed on the category still runs.

## Table: `nightwatch/weapon_clip_groups.json`

```json
{
  "version": 1,
  "rows": [
    {"weapon": 27330000, "clip_group": 161},
    {"weapon": 27340000, "clip_group": 161}
  ]
}
```

| Field | Meaning |
|---|---|
| `weapon` | an EquipParamWeapon id, a multiple of 10000; the row covers that weapon's +0..+10 rows (`weapon` .. `weapon`+9999) |
| `clip_group` | 1..999: the group the weapon's attacks play (`a161_030000` ...); the mod ships those clips and the graph's selectors for them |

The manifest must list `"weapon_clip_groups"` in `features` ([manifest](manifest.md)). Key: `weapon`; a later mod's row
replaces an earlier one. A table that lists a weapon twice is refused.

## What changes in game

The anim id offset of the behaviour selectors that read a hand's weapon (offsetType 13: right hand, 14: left hand) is
`clip_group * 1000000` while that hand holds a listed weapon, exactly what a weapon of category `clip_group` would
give. Nothing else changes: the hks, the trick code and every other reader still see the row's `wepmotionCategory`.
The weapon of a hand is the one the game reads (right: the overriding weapon when one is set, else the selected slot).

A private clip group adds clips to the player's graph. The game gives a character's clip collection room for 8192 clips
and drops the rest without an animation (the selector then plays its first child: the wrong swing, or a freeze). Vanilla
has 7784. Nightwatch 0.3.0 raises the room to 16384 for every player, with or without a table (log line
`behaviour clip capacity: 16384 clips per player`), so a mod with more than 8192 clips must say `requires` 0.3.0.

## Co-op

Gameplay: the merged rows are part of the party version check (`out/party_mods.txt`), so players with different
tables are refused like players with different gameplay files.

## Checking it

- `Mods: weapon_clip_groups: N rows from M mods` in the start log, and `out/nightwatch_weapon_clip_groups.txt`.
- `feature weapon_clip_groups: installed (N rows ...)` in the session log; the first 16 remaps log one line each
  (`chr ... right weapon 27330000, anim 30000: offset 113000000 -> 161000000 (clip group 161)`);
  `BB_WEAPON_CLIP_GROUPS_LOG=1` logs 200. `BB_WEAPON_CLIP_GROUPS=0` turns the feature off.
